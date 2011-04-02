/* 
 * $Id: tcp_plugin.cc,v 1.23 2005-12-29 17:22:55 jsommers Exp $
 */

/*
 * Copyright 2004, 2005  Joel Sommers.  All rights reserved.
 * 
 * This file is part of Harpoon, a flow-level traffic generator.
 * 
 * Harpoon is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Harpoon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Harpoon; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include <iostream>
#include <iomanip>
#include <cstring>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <pthread.h>

#include "config.h"
#include "configurator.hh"
#include "harpoon_plugin.hh"
#include "xmlrpc_util.hh"

namespace Harpoon
{

    class TCPServerSock : public SharedPluginState
    {
    public:
        TCPServerSock(): SharedPluginState(), m_fd(0) {}
        virtual ~TCPServerSock() {}

        /*!
         * create the server socket.  we make it non-blocking so that we don't get stuck
         * at any point inside the user_server() routine.
         */
        void openSocket(struct in_addr ina, unsigned short port, int ip_tos = 0, int tcp_mss = 0)
            {
#ifdef DEBUG
                std::cerr << "server_init from thread " << pthread_self() << std::endl;
#endif
                // make a server listener file descriptor that all threads can use
                m_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (m_fd < 0)
                    throw errno;
    
                if (fcntl(m_fd, F_SETFL, O_NONBLOCK) < 0)
                    throw errno;
    
                int opt = 1;
                if (setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                               sizeof(int)) < 0)
                    throw errno;

                if (ip_tos != 0)
                {
#if __linux
                    int srv = setsockopt(m_fd, SOL_IP, IP_TOS, &ip_tos, sizeof(ip_tos));
#else
                    int srv = setsockopt(m_fd, IPPROTO_IP, IP_TOS, &ip_tos, sizeof(ip_tos));
#endif
                    // std::cerr << "set IP_TOS to 0x" << std::hex << ip_tos << std::dec << " rv: " << srv << std::endl;
                }


                if (tcp_mss != 0)
                {
#if __linux
                    int srv = setsockopt(m_fd, SOL_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#else
                    int srv = setsockopt(m_fd, IPPROTO_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#endif
                    // std::cerr << "set TCP_MAXSEG to " << tcp_mss << " rv: " << srv << std::endl;
                }


                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(struct sockaddr_in));
                serv_addr.sin_family = AF_INET;
                serv_addr.sin_addr = ina;
                serv_addr.sin_port = port;
    
                if (bind(m_fd, (const sockaddr *)&serv_addr,
                         sizeof(struct sockaddr_in)) < 0)
                    throw errno;
    
                if (listen(m_fd, SOMAXCONN))
                    throw errno;
            }
        int getSocket() const { return m_fd; }

    private:
        int m_fd;
    };



    /*!
     * @class TCPPlugin

     * A plugin implementing TCP users.  There are a few "interesting" features about this
     * plugin:
     *
     * - since there may be many server threads, but most operating systems don't support
     *   the SO_REUSEPORT setsockopt() argument, we must be sure to construct only one
     *   server socket.  all server threads subsequently call accept() on this socket.  we
     *   rely on the fact that the init() method is called in a single-threaded fashion: on
     *   the first call, we create the socket and initialize a private TCPServerSock
     *   structure.  on subsequent calls to init() (as server threads are created), we
     *   increment a reference count.  when threads are later cancelled, we decrement the
     *   reference count in the shutdown() method and close the socket on the last reference.
     *   this pattern is a useful one for one-time, global initialization for a plugin...
     * - in the client, we must deal with handling concurrent connections.  we keep a list
     *   of all outstanding connection (an STL vector of TCPConnInfo structures) and multiplex
     *   these conections through use of poll() call.  (NB: poll is just a convenience method
     *   to select() on some systems, so performance may not be as good as it could be.  most
     *   systems natively support poll() and it is a much more scalable syscall therefore we
     *   prefer its use.)  a TCPConnInfo only lasts as long as it takes to transfer a file.  we
     *   keep track of most recent activity on a TCPConnInfo so that we can destroy it if there's
     *   no activity for EV_TIMEOUT seconds (EV_TIMEOUT, by default, is 10 seconds.)
     *   all activity for a connection is non-blocking, which can make connections tricky since
     *   we need to figure out when a connection ends up succeeding (or failing.)  we keep
     *   state info on the connection to help out with that.
     * - statistics are held in static variables.  the non-static method stats() determines
     *   whether the called object is a client or server, and calls the appropriate static
     *   method to retrieve the appropriate statistics.  note that none of the statistics
     *   variables are protected by mutexes.  these are not crucial pieces of information and
     *   no one will get hurt if there is a little happy inconsistency.
     */
    class TCPPlugin : public HarpoonPlugin
    {
    public:
        TCPPlugin() : HarpoonPlugin() { }
    
    
        /*!
         * initialize the plugin.  here is where we deal with creating a server socket
         * or incrementing a reference count.  we \e know that we will get called in
         * a single threaded manner -- that's the only way this works.
         */
        virtual bool init(HarpoonPluginConfig *hpc, HarpoonLog *hlog)
            {
                HarpoonPlugin::init(hpc, hlog);
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                { 
                    SharedPluginState *sps = 0;
                    if (!getSharedData())
                    {
                        int tos = 0;
                        std::vector<float> *vf_buf = getDistribution("ip_tos");
                        if (vf_buf && vf_buf->size())
                        {
                            unsigned int index = random() % vf_buf->size();
                            tos = (int)((*vf_buf)[index]);
                        }

                        int mss = 0;
                        vf_buf = getDistribution("tcp_mss");
                        if (vf_buf && vf_buf->size())
                        {
                            unsigned int index = random() % vf_buf->size();
                            mss = (int)((*vf_buf)[index]);
                        }

                        struct in_addr ina; 
                        unsigned short port;
                        getAddress("server_pool", ina, port);
                        TCPServerSock *tss = new TCPServerSock();
                        tss->openSocket(ina, port, tos, mss);
                        setSharedData(tss);
                        sps = tss;
                    }
                    else
                    {
                        sps = dynamic_cast<SharedPluginState*>(getSharedData());
                    }
                    assert (sps);
                    sps->incrRefCount();
                } 
                return (true);
            }


        /*!
         * the method handling all the user-level client stuff: sessions, file transfers,
         * inter-file request times, etc.
         */
        virtual void client_session()
            {
                int recv_buf_size = BUFSIZ; // getpagesize()
                char *recv_buf = new char[recv_buf_size];
    
                int pfd_length = 10;
                pollfd *pfd = new pollfd[pfd_length];

                std::vector<TCPConnInfo *> conn_list;
                typedef std::vector<TCPConnInfo *>::iterator RtI;
    
                // single src/dst pair for life of this user/hostpair
                struct sockaddr_in dst_addr;
                create_dst(&dst_addr);
    
                struct sockaddr_in src_addr;
                create_src(&src_addr);
    
                usleep(random() % 1000000);

                struct timeval curr_time;
                gettimeofday(&curr_time, 0);
    
                // this session should last for a single interval duration
                struct timeval session_end = curr_time;
                timeraddfloat(session_end, getIntervalDuration());

                // get handle to inter-connection distribution
                std::vector<float> *interconn = getDistribution("interconnection_times");
                assert(interconn);
                int next_ic = random() % interconn->size();

                int tcp_mss = 0;
                std::vector<float> *vf_buf = getDistribution("tcp_mss");                
                if (vf_buf && vf_buf->size())
                {
                    unsigned int index = random() % vf_buf->size();
                    tcp_mss = (int)((*vf_buf)[index]);
                }


                //
                // next_conn is now the time of the next connection.
                //
                struct timeval next_conn = curr_time;
                timeraddfloat(next_conn, (*interconn)[next_ic++]);
                next_ic = next_ic % interconn->size();


                // set flag when next connection start is beyond the
                // interval end time.  we *still* wait for that next conn,
                // but stop after that.
                bool last_conn = timercmp(&next_conn, &session_end, >);
 
                std::ostringstream ostr;
                std::string tmp;
 
#ifdef DEBUG
                ostr << "client started at " << curr_time.tv_sec << " ending at " << session_end.tv_sec;
                m_log->log(9, ostr.str());
                ostr.str(tmp);

                ostr << "(begin) setting connection start to be "
                     << (next_conn.tv_sec - curr_time.tv_sec)
                     << " secs from now";
                m_log->log(9, ostr.str());
                ostr.str(tmp);
#endif

                int max_failed_conn = 10;
                std::vector<float> *mfconn_dist = getDistribution("max_failed_connect_attempts");
                if (mfconn_dist && mfconn_dist->size())
                {
                    max_failed_conn = int((*mfconn_dist)[0]);
                }

                ostr << "setting maximum failed connection attempts at " << max_failed_conn;
                m_log->log(7, ostr.str());
                ostr.str(tmp);
                
                
                try
                {
                    bool end_of_session = false;
                    while (!end_of_session || conn_list.size())
                    {
                        // start new connection - don't start any new connection if 
                        // we're just waiting for some stragglers to complete 
                        // at the end of a user.
            
                        if (!last_conn && timercmp(&curr_time, &next_conn, >))
                        {
                            TCPConnInfo *new_conn = new TCPConnInfo();

                            next_conn = curr_time;
                            double ictime = (*interconn)[next_ic++];
                            timeraddfloat(next_conn, ictime);
                            next_ic = next_ic % interconn->size();

                            last_conn = timercmp(&next_conn, &session_end, >);

                            ostr <<  "ic-time " << ictime;
                            if (last_conn)
                                ostr <<  " (last one)";
                            m_log->log(5, ostr.str());
                            ostr.str(tmp);

                            bool conn_ok = true;
                            do
                            {
                                new_conn->m_state = STATE_CONNECTING;
                                new_conn->m_start = curr_time;
                                new_conn->m_lastev = curr_time;
                                new_conn->m_fd = socket(AF_INET, SOCK_STREAM, 0);
                                if (new_conn->m_fd < 0)
                                {
                                    ostr << "can't open socket: " << errno << '/' << strerror(errno); 
                                    m_log->log(3, ostr.str());
                                    ostr.str(tmp);
                                    conn_ok = false;
                                    break;
                                }
                    
                                if (fcntl(new_conn->m_fd, F_SETFL, O_NONBLOCK) < 0)
                                {
                                    ostr << "couldn't set socket as non-blocking: " << errno << '/' << strerror(errno);
                                    m_log->log(3, ostr.str());
                                    ostr.str(tmp);
                                    conn_ok = false;
                                    close(new_conn->m_fd);
                                    break;
                                }

                                if (tcp_mss != 0)
                                {
#if __linux
                                    int srv = setsockopt(new_conn->m_fd, SOL_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#else
                                    int srv = setsockopt(new_conn->m_fd, IPPROTO_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#endif
                                    // std::cerr << "set TCP_MAXSEG to " << tcp_mss << " rv: " << srv << std::endl;
                                }

                                if (bind(new_conn->m_fd, (const struct sockaddr*)&src_addr, sizeof(struct sockaddr_in)))
                                {
                                    ostr << "couldn't bind socket to local address: " << errno << '/' << strerror(errno);
                                    m_log->log(3, ostr.str());
                                    ostr.str(tmp);
                                    conn_ok = false;
                                    close(new_conn->m_fd);
                                    break;
                                }
                    
                                // cause a RST on close.  just a trick to not let ports
                                // get into CLOSE_WAIT state and lets those ports get reallocated
                                struct linger ling = {1,0};
                                if (setsockopt (new_conn->m_fd, SOL_SOCKET, SO_LINGER, 
                                                &ling, sizeof(struct linger)) < 0)
                                {
                                    ostr << "couldn't set SO_LINGER on socket: " << errno << '/' << strerror(errno);
                                    m_log->log(3, ostr.str());
                                    ostr.str(tmp);
                                    conn_ok = false;
                                    close(new_conn->m_fd);
                                    break;
                                }
                    
                                if (connect(new_conn->m_fd, (struct sockaddr *) &dst_addr, sizeof(dst_addr)) < 0)
                                {
                                    if (errno != EINPROGRESS)
                                    {
                                        ostr << "connect returned: " << errno << '/' << strerror(errno);
                                        m_log->log(3, ostr.str());
                                        ostr.str(tmp);
                                        close(new_conn->m_fd);
                                        conn_ok = false;
                                        break;
                                    }
                                }
                                new_conn->m_bytes_remain = 0;
                                new_conn->m_pfdi = -1;
                            } while (0);

                            if (conn_ok)
                            {
                                conn_list.push_back(new_conn);
                            }
                            else
                            {
                                delete (new_conn);
                                max_failed_conn--;
                                if (!max_failed_conn)
                                {
                                    ostr << "thread reached limit of failed connections --- exiting (and restarting)";
                                    m_log->log(3, ostr.str());
                                    ostr.str(tmp);
                                    throw -1;
                                }
                            }
                        }
            
                        if (pfd_length < int(conn_list.size()))
                        {
                            pfd_length = conn_list.size();
                            delete [] pfd;
                            pfd = new pollfd[pfd_length];
                        }

                        int pfd_index = 0;
                        for (RtI iter = conn_list.begin(); iter != conn_list.end(); ++iter)
                        {
                            pfd[pfd_index].fd = (*iter)->m_fd;
                            pfd[pfd_index].events = POLLIN;
                            pfd[pfd_index].revents = 0;
                            (*iter)->m_pfdi = pfd_index;
                            if ((*iter)->m_state == STATE_CONNECTING)
                            {
                                pfd[pfd_index].events |= POLLOUT;
                            }
                            pfd_index++;
                        }
            
                        struct timeval timeout;
                        timerdiff(&next_conn, &curr_time, &timeout); 
                        int tmo_ms = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
                        // possible edge conditions with tmo less than 0.  this
                        // isn't really a problem - just means that we're at 
                        // the end of an interval and waiting for that final 
                        // session to fire
                        if (tmo_ms < 0 || tmo_ms > 1000)
                            tmo_ms = 1000;
                        int nsel = poll(pfd, pfd_index, tmo_ms);
            
                        gettimeofday(&curr_time, 0);

                        // end of sesson if 1) interval end time has passed AND
                        // 2) we're on the last connection AND 3) the next_conn
                        // time has passed.
                        end_of_session = timercmp(&session_end, &curr_time, <) &&
                            timercmp(&next_conn, &curr_time, <) &&
                            last_conn;
#ifdef DEBUG
                        if (end_of_session)
                            std::cerr << "endofsession flag set" << std::endl;
#endif

                        if (shouldExit())
                            throw 0;
            
                        if (nsel < 0)
                        {
                            if (errno == EINTR)
                                continue;
                            else
                                throw errno;
                        }
                        else if (nsel > 0)
                        {
                            // check fds ready to ready
                            RtI iter = conn_list.begin();
                            while (iter != conn_list.end())
                            {
                                errno = 0;
                                bool destroy_conn = false;
                                pfd_index = (*iter)->m_pfdi;
#ifdef DEBUG
                                assert(pfd[pfd_index].fd == (*iter)->m_fd);
#endif
                                TCPConnInfo *this_conn = (*iter);

                                if (this_conn->m_state != STATE_INVALID && (
                                        pfd[pfd_index].revents & POLLERR ||
                                        pfd[pfd_index].revents & POLLHUP ||
                                        pfd[pfd_index].revents & POLLNVAL ||
                                        ((curr_time.tv_sec - this_conn->m_lastev.tv_sec) >= EV_TIMEOUT)))
                                {
                                    if (!(this_conn->m_state == STATE_CONNECTING &&
                                          (pfd[pfd_index].revents & POLLHUP ||
                                           pfd[pfd_index].revents & POLLNVAL)))
                                    {
                                        ostr << "marking stale transfer connection for death (state: " << this_conn->m_state << ")";
                                        m_log->log(7, ostr.str());
                                        ostr.str(tmp);
                                        destroy_conn = true;
                                    }
                                }

                                if (!destroy_conn && this_conn->m_state == STATE_CONNECTING)
                                {
                                    if (pfd[pfd_index].revents & POLLOUT)
                                    {
#ifdef DEBUG
                                        ostr << "requesting new file transfer";
                                        m_log(9, ostr.str());
                                        ostr.str(tmp);
#endif
                                        if (request_file(this_conn->m_fd) < 0)
                                        {
                                            // check for false positive from poll().  we *should*
                                            // get write indication when TCP connection is
                                            // (asynchronously) completed, but OSes can be flaky...
                                            if (errno != EINPROGRESS && errno != EAGAIN && errno != EINTR)
                                            {
                                                ostr << "socket not connected after getting POLLOUT indication --- destroying the connection.";
                                                m_log->log(7, ostr.str());
                                                ostr.str(tmp);
                                                destroy_conn = true;
                                            }
                                        }
                                        else
                                        {
                                            m_num_req++;
                                            this_conn->m_lastev = curr_time;
                                            this_conn->m_state = STATE_FILE_REQUEST;
                                        }
                                    }
                                }

                                if (!destroy_conn && this_conn->m_state == STATE_FILE_REQUEST)
                                {
                                    if (pfd[pfd_index].revents & POLLIN)
                                    {
                                        int remain = get_filesize (this_conn->m_fd);
                                        if (remain < 0 && (errno != EAGAIN && errno != EINTR))
                                        {
                                            destroy_conn = true;
                                        }
                                        else
                                        {
                                            this_conn->m_state = STATE_TRANSFER;
                                            this_conn->m_bytes_remain = remain;
                                            this_conn->m_xfer_size = remain;
                                            // NB: if we want xfer time to
                                            // only include transfer (not conn)
                                            // this_conn->m_start = curr_time;
                                            this_conn->m_lastev = curr_time;
                                        }
                                    }
                                }
                                
                                if (!destroy_conn && this_conn->m_state == STATE_TRANSFER)
                                {
                                    while (this_conn->m_bytes_remain > 0)
                                    {
                                        int bytes_read = recv(this_conn->m_fd, &recv_buf[0], recv_buf_size, 0);
                                        if (bytes_read < 0)
                                        {
                                            if (errno != EAGAIN && errno != EINTR)
                                            {
                                                destroy_conn = true;
                                            }
                                            break;
                                        }
                                        else if (bytes_read == 0)
                                        {
#ifdef DEBUG
                                            ostr << "client got hangup from server? " << errno << '/' << strerror(errno);
                                            m_log->log(9, ostr.str());
                                            ostr.str(tmp);
#endif
                                            destroy_conn = true;
                                            break;
                                        }
                                        else
                                        {
                                            // something was read
                                            this_conn->m_bytes_remain -= bytes_read;
                                            this_conn->m_lastev = curr_time;
                                        }
                                    }
                            
                                    if (this_conn->m_bytes_remain <= 0)
                                    {
#ifdef DEBUG
                                        if (this_conn->m_bytes_remain < 0)
                                            ostr << "server sent more data than it told us?";
                                        else
                                            ostr << "done with connection - killing conn block";
                                        m_log->log(7, ostr.str());
                                        ostr.str(tmp);
#endif
                                        destroy_conn = true;
                                    } // end of file
                                }

                                if (destroy_conn)
                                {
#if DEBUG
                                    if (errno)
                                    {
                                        ostr << "connnection destroyed with non-zero errno: " << strerror(errno) << '/' << errno;
                                        m_log->log(5, ostr.str());
                                        ostr.str(tmp);
                                    }   
#endif

                                    struct timeval xfer_time;
                                    timerclear(&xfer_time);
                                    gettimeofday(&curr_time, 0);
                                    timerdiff(&curr_time, &(this_conn->m_start), &xfer_time);
                                    if (this_conn->m_xfer_size)
                                    {
                                        ostr << "transfer-complete " << this_conn->m_xfer_size << " bytes in " << xfer_time.tv_sec << '.' << std::setw(6) << std::setfill('0') << xfer_time.tv_usec;
                                        m_log->log(5, ostr.str());
                                        ostr.str(tmp);
                                    }

                                    close (this_conn->m_fd);
                                    delete this_conn;
                                    iter = conn_list.erase(iter);
                                }
                                else
                                {
                                    iter++;
                                }

                            } // while iterating through session blocks
                        } // nsel > 0
                    } // while !end of session
                }
                catch ( int &eno )
                {
                    if (eno > 0)
                    {
                        ostr << "tcp client thread " << pthread_self()
                             << " abrupt session end: " << eno << '/' 
                             << strerror(eno);
                        m_log->log(1, ostr.str());
                        ostr.str(tmp);
                    }
                }
    
#ifdef DEBUG
                ostr << "end of client session";
                m_log->log(9, ostr.str());
                ostr.str(tmp);
#endif
 
                delete [] recv_buf;
                delete [] pfd;
    
                // cleanup anything left over
                for (RtI iter = conn_list.begin(); iter != conn_list.end(); ++iter)
                {
                    close((*iter)->m_fd);
                    delete (*iter);
                }
            }


        /*!
         * method handling all session-level stuff for servers: just accepting file requests
         * and serving files - not much.  just remember that the server socket is in
         * non-blocking mode, so all client sockets are created non-blocking too.
         */
        virtual void server_session()
            {
                std::vector<TCPConnInfo *> conn_list;
                typedef std::vector<TCPConnInfo *>::iterator RtI;
    
                int send_buf_size = BUFSIZ; // getpagesize();
                char *send_buf = new char[send_buf_size];
		memset(send_buf, 0, send_buf_size);
    
                int pfd_length = 10;
                pollfd *pfd = new pollfd[pfd_length];
		memset(pfd, 0, sizeof(pollfd) * pfd_length);

                assert (getSharedData()); // assert that we have a server fd
                TCPServerSock *tss = dynamic_cast<TCPServerSock*>(getSharedData());
                int serv_fd = tss->getSocket();

                int tos = 0;
                std::vector<float> *vf_buf = getDistribution("ip_tos");
                if (vf_buf && vf_buf->size())
                    tos = (int)((*vf_buf)[0]);

                std::vector<float> *tcp_mss_distr = getDistribution("tcp_mss");

                std::vector<float> *filesizes = getDistribution("file_sizes");
                assert(filesizes);
                int fs_num = random() % filesizes->size();
    
                std::ostringstream ostr;
                std::string tmp;
                
                try
                {
                    // lifetime of server thread
                    while (1)
                    {
                        bool stuff_to_send = false;
                        if ((pfd_length - 1) < int(conn_list.size()))
                        {
                            pfd_length = conn_list.size() + 1;
                            delete [] pfd;
                            pfd = new pollfd[pfd_length];
		            memset(pfd, 0, sizeof(pollfd) * pfd_length);
                        }

                        pfd[0].fd = serv_fd;
                        pfd[0].events = POLLIN;

                        int pfd_index = 1;
                        for (RtI iter = conn_list.begin(); iter != conn_list.end(); ++iter)
                        {
                            (*iter)->m_pfdi = pfd_index;
                            pfd[pfd_index].fd = (*iter)->m_fd;
                            pfd[pfd_index].events = POLLIN;
                            stuff_to_send = (*iter)->m_bytes_remain != 0 || stuff_to_send;
                            pfd_index++;
                        }
            
                        struct timeval timeout = { 1, 0 };
                        if (stuff_to_send)
                        {
                            timeout.tv_sec = 0;
                            timeout.tv_usec = 1000;
                        }
                        int tmo_ms = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
            
                        int n = poll(pfd, pfd_index, tmo_ms);
            
                        if (shouldExit())
                            throw 0;
            
                        if ((n < 0 && errno == EINTR) || n == 0)
                            continue;
            
                        if (n < 0 && errno != EINTR)
                            throw errno;
            
                        if (pfd[0].revents & POLLIN)
                        {
                            struct sockaddr_in dst_addr;
                            SOCKLEN_T dst_addr_len = sizeof(dst_addr);
                            int cli_fd = accept(serv_fd, (struct sockaddr *) &dst_addr, &dst_addr_len);
                            if (cli_fd > 0)
                            {
#ifdef DEBUG
                                ostr << "tcp server got connection from " << inet_ntoa(dst_addr.sin_addr) << ':' << ntohs(dst_addr.sin_port);
                                m_log->log(9, ostr.str());
                                ostr.str(tmp);
#endif
                                TCPConnInfo *new_session = new TCPConnInfo();
                                new_session->m_fd = cli_fd;
                                conn_list.push_back(new_session);
                                new_session->m_pfdi = -1;

                                if (tcp_mss_distr && tcp_mss_distr->size())
                                { 
                                    unsigned int index = random() % tcp_mss_distr->size();
                                    int mss = (int)((*tcp_mss_distr)[index]);
#if __linux
                                    int srv = setsockopt(cli_fd, SOL_TCP, TCP_MAXSEG, &mss, sizeof(mss));
#else
                                    int srv = setsockopt(cli_fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
#endif
                                    // std::cerr << "set TCP_MAXSEG to " << mss << " rv: " << srv << std::endl;
                                }

                                if (tos != 0)
                                {
#if __linux
                                    int srv = setsockopt(cli_fd, SOL_IP, IP_TOS, &tos, sizeof(tos));
#else
                                    int srv = setsockopt(cli_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
                                    // std::cerr << "set IP_TOS to 0x" << std::hex << tos << std::dec << " rv: " << srv << std::endl;
                                }
                            }
                        }
            
                        RtI iter = conn_list.begin();
                        while (iter != conn_list.end())
                        {
                            if (shouldExit())
                                throw 0;

                            pfd_index  = (*iter)->m_pfdi;
                            
                            assert(pfd_index == -1 || pfd[pfd_index].fd == (*iter)->m_fd);

                            if (pfd_index == -1 || 
                                pfd[pfd_index].revents & POLLIN)
                            {   
                                char request_byte = 0x00;
                                int rlen = recv ((*iter)->m_fd, &request_byte, 1, 0);
                                if (rlen != 1 || request_byte != 0x7e)
                                {
                                    if (rlen > 0)
                                    {
                                        ostr << "not a correct tcp file request? (recv 0x" << std::hex << request_byte << ")";
                                        m_log->log(3, ostr.str());
                                        ostr.str(tmp);
                                    }
                                    
                                    close ((*iter)->m_fd);
                                    delete (*iter);
                                    iter = conn_list.erase(iter);
                                    continue;
                                }
                    
                                // only a broken client would send a new request
                                // while there's already one outstanding.
                                assert ((*iter)->m_bytes_remain == 0);
                    
                                (*iter)->m_bytes_remain = int((*filesizes)[fs_num++]);
                                fs_num = fs_num % filesizes->size();

                                ostr << "file-size " << (*iter)->m_bytes_remain;
                                m_log->log(5, ostr.str());
                                ostr.str(tmp);

                                int xfer_size = htonl((*iter)->m_bytes_remain);
                                if (send((*iter)->m_fd, &xfer_size, 4, 0) != 4)
                                {
                                    ostr << "client hung up before server sent file size: " << errno << '/' << strerror(errno);
                                    m_log->log(5, ostr.str());
                                    ostr.str(tmp);
                                    
                                    close ((*iter)->m_fd);
                                    delete (*iter);
                                    iter = conn_list.erase(iter);
                                    continue;
                                }
                            }
                
                            if ((*iter)->m_bytes_remain)
                            {
                                // blast as much of the file as we can - break if we finish sending
                                // or we get an EAGAIN
                                bool dead_client = false;
                                while ((*iter)->m_bytes_remain)
                                {
                                    int to_send = std::min((*iter)->m_bytes_remain, send_buf_size);
                                    int slen = send((*iter)->m_fd, send_buf, to_send, 0);
                                    if (slen < 0 && errno == EAGAIN)
                                        break;
                                    else if (slen <= 0)
                                    {
                                        close ((*iter)->m_fd);
                                        delete (*iter);
                                        iter = conn_list.erase(iter);
                                        dead_client = true;
                                        break;
                                    }
                        
                                    // 
                                    // some amount of data transferred
                                    // 
                                    (*iter)->m_bytes_remain -= slen;
                                    m_bytes_xfered += slen;
                                    m_bytes_xfered_recent += slen;
                                }

                                // incr number of files transferred
                                if ((*iter)->m_bytes_remain == 0)
                                {
                                    m_num_transfer++;
                                }
                    
                                if (dead_client)
                                    continue;
                            }
                            iter++;
                        }
                    }
                } 
                catch ( int &eno )
                {
#ifdef DEBUG
                    ostr << "tcp server thread " << pthread_self()
                              << " abrupt error: ";
                    if (eno > 0)
                        ostr << strerror(eno) << '/' << eno;
                    else
                        ostr << "unknown fault";
                    m_log->log(1, ostr.str());
                    ostr.str(tmp);
#endif
                }
    
                for (RtI iter = conn_list.begin(); iter != conn_list.end(); ++iter)
                {
                    close ((*iter)->m_fd);
                    delete (*iter);
                }
    
                delete [] send_buf;
                delete [] pfd;
            }



        /*!
         * routine called for every thread upon shutdown.  for clients, we don't do
         * anything.  for servers, we decrement a reference count and destroy the server
         * state and close the server socket on last reference.
         */
        virtual void shutdown()
            {
#ifdef DEBUG
                std::cerr << "tcp plugin shutdown " << pthread_self() << std::endl;
#endif
    
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                {
                    // must be careful to do this in a sane order
                    TCPServerSock *tss = dynamic_cast<TCPServerSock*>(getSharedData());
                    assert(tss);
                    tss->decrRefCount();
                    if (tss->isRefCountZero()) 
                        m_log->log(5, "server ref count dropped to zero.");
                }
                return;
            }


        /*!
         * externally-callable statistics gathering routine.  we check what personality
         * the current thread has, and call the appropriate static routine.  (NB: all our
         * stats are in static variables so the "real" stats routines must also be static.)
         */
        virtual void stats(std::ostream &os)
            {
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                    server_stats(os);
                else if (hpp == plugin_personality_client)
                    client_stats(os);
            }


        /*
         * stupid sun C++ compiler can't deal with nested private enumeration.
         */
#ifdef __SUNPRO_CC
    public:
#else
    private:
#endif
        //! state of a user session - can only be transferring or idle.
        enum TCPConnState { STATE_TRANSFER = 0, STATE_INVALID = 1, 
                            STATE_FILE_REQUEST = 2, STATE_CONNECTING = 3 };

    private:
        //! prevent copy construction
        TCPPlugin(TCPPlugin &t) {}

        static const int EV_TIMEOUT;

        static int m_num_req;       //!< number of file requests made by client
        static int m_num_transfer;  //!< number of file transfers made by server.

        static double m_bytes_xfered;  //!< number of octets sent by server
        // use a double to avoid overflow - kooky.  XMLRPC doesn't have 64bit 
        // ints, and this is only a coarse measure anyway
        static double m_bytes_xfered_recent;  //!< number of octets sent by server since last stats retrieval
        // same kooky thing as above

        static time_t m_last_stats_retrieval;  //!< time of last stats retrieval for rudimentary calculation of bandwidth
        static time_t m_started;    //!< time plugin was constructed.  used in cheap bandwidth calculation

        //! encode the number of files transferred by server threads
        static void server_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_transfer", m_num_transfer);
    
                time_t now = time(NULL);
    
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_total", m_bytes_xfered);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_recent", m_bytes_xfered_recent);
    
                unsigned long interval = now - m_last_stats_retrieval;
                unsigned long up = now - m_started;
                double bw_total = (interval > 0 ? (m_bytes_xfered * 8.0 / up) : 0.0);
                double bw_recent = (interval > 0 ? (m_bytes_xfered_recent * 8.0 / interval) : 0.0);
    
                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_total_bps", bw_total);
                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_recent_bps", bw_recent);
                m_bytes_xfered_recent = 0.0;
                m_last_stats_retrieval = now;
            }


        //! encode client-specific statistics
        static void client_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_requests", m_num_req);
            }


        /*!
         * utility routine for initializing a sockaddr_in with the next 
         * source address for a client session to use.
         * @param sin the sockaddr_in structure to fill.
         */
        void create_src(struct sockaddr_in *sin)
            {
                in_addr ipaddr;
                unsigned short port;             
                getAddress("client_source_pool", ipaddr, port);
                memset(sin, 0, sizeof(struct sockaddr_in));
                sin->sin_family = AF_INET;
                sin->sin_port = port;
                sin->sin_addr = ipaddr;
            }

        /*!
         * utility routing for initializing a sockaddr_in with the next
         * destination address for a client session to use.
         * @param sin the sockaddr_in structure to fill.
         */
        void create_dst(struct sockaddr_in *sin)
            {
                in_addr ipaddr;
                unsigned short port;             
                getAddress("client_destination_pool", ipaddr, port);
                memset(sin, 0, sizeof(struct sockaddr_in));
                sin->sin_family = AF_INET;
                sin->sin_port = port;
                sin->sin_addr = ipaddr;
            }

        /*!
         * routine called by client threads to send a file request to the server.  just
         * sends a single byte as a signal to the server, receives a network-byte-order
         * integer representing the file size, and returns the file size retrieved.
         * @param fd the client session socket
         * @return int size of the file the server is sending, or -1 on error.
         */
        int request_file(int fd)
            {
                char request_byte = 0x7e;
                return (send(fd, &request_byte, 1, 0));
            }

        int get_filesize(int fd)
            {
                int file_size;
                if (recv(fd, &file_size, 4, 0) < 0)
                    return -1;
    
                return ntohl(file_size);
            }

        /*!
         * structure holding connection state for client threads
         */
        struct TCPConnInfo
        {	
            TCPConnInfo() : m_fd(-1), m_state(STATE_INVALID), 
                            m_bytes_remain(0), m_xfer_size(0), m_pfdi(-1) 
            { 
                timerclear(&m_start);
                timerclear(&m_lastev);
            }
    
            int m_fd;                  //!< client session socket
            TCPConnState m_state;      //!< STATE_TRANSFER, STATE_INVALID, STATE_FILE_REQUEST, or STATE_CONNECTING
            int m_bytes_remain;        //!< number of octets remaining to read from server
            int m_xfer_size;           //!< total file/xfer size
            struct timeval m_start;    //!< starting time for this transfer
            struct timeval m_lastev;   //!< keep track of last activity on connection
            int m_pfdi;                //!< index into pollfd struct list for poll() call
        };


    };

    int TCPPlugin::m_num_req = 0;
    int TCPPlugin::m_num_transfer = 0;
    double TCPPlugin::m_bytes_xfered = 0.0;
    double TCPPlugin::m_bytes_xfered_recent = 0.0;
    time_t TCPPlugin::m_last_stats_retrieval = time(NULL);
    time_t TCPPlugin::m_started = time(NULL);
    const int TCPPlugin::EV_TIMEOUT = 30;
}



/*!
* factory function.  "factory_generator" is the symbol we look for
 * when loading harpoon plugins.  using a C symbol gets around weirdo
 * name-mangling problems with C++.
 */
extern "C"
{
#if STATIC_PLUGINS
    Harpoon::HarpoonPlugin *tcp_plugin_generator(void)
    {
        return dynamic_cast<Harpoon::HarpoonPlugin*>(new Harpoon::TCPPlugin()); 
    }
#else
    Harpoon::TCPPlugin *factory_generator(void)
    {
        return (new Harpoon::TCPPlugin()); 
    }
#endif
}


