/* 
 * $Id: infinite_tcp.cc,v 1.12 2005-12-29 17:22:55 jsommers Exp $
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
#include <cstdlib>
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

    class InfTCPServerSock : public SharedPluginState
    {
    public:
        InfTCPServerSock(): SharedPluginState(), m_fd(0) {}
        virtual ~InfTCPServerSock() {}

        /*!
         * create the server socket.  we make it non-blocking so that we don't get stuck
         * at any point inside the user_server() routine.
         */
        void openSocket(struct in_addr ina, unsigned short port, int rbuf = 0, int sbuf = 0, int ip_tos = 0, int tcp_mss = 0)
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

                if (sbuf > 0)
                {
                    if (setsockopt(m_fd, SOL_SOCKET, SO_SNDBUF, &sbuf, sizeof(int)) < 0)
                        std::cerr << "error setting SNDBUF for server socket:" << errno << std::endl;
                }

                if (rbuf > 0)
                {
                    if (setsockopt(m_fd, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(int)) < 0)
                        std::cerr << "error setting RCVBUF for server socket:" << errno << std::endl;
                }

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
     * @class InfiniteTCPPlugin

     * A plugin implementing "infinite" TCP connection.
     *
     * a few general notes on this plugin:
     * - there are no distributions used.  the only thing that matters is
     *   the number of threads configured at client and server.  clients make
     *   connections, servers blast data as long as they can.
     *
     * - clients tear down connections at end of each interval duration.  to get very long duration connections, set the duration, uh, very long (-w flag).
     *
     * - as with the standard TCPPlugin, since there may be many server threads, but most operating systems don't support
     *   the SO_REUSEPORT setsockopt() argument, we must be sure to construct only one
     *   server socket.  all server threads subsequently call accept() on this socket.  we
     *   rely on the fact that the init() method is called in a single-threaded fashion: on
     *   the first call, we create the socket and initialize a private InfTCPServerSock
     *   structure.  on subsequent calls to init() (as server threads are created), we
     *   increment a reference count.  when threads are later cancelled, we decrement the
     *   reference count in the shutdown() method and close the socket on the last reference.
     *   this pattern is a useful one for one-time, global initialization for a plugin...
     *
     * - in the client, we must deal with handling concurrent connections.  we keep a list
     *   of all outstanding connection (an STL vector of InfTCPConnInfo structures) and multiplex
     *   these conections through use of poll() call.  (NB: poll is just a convenience method
     *   to select() on some systems, so performance may not be as good as it could be.  most
     *   systems natively support poll() and it is a much more scalable syscall therefore we
     *   prefer its use.)  
     *
     */
    class InfiniteTCPPlugin : public HarpoonPlugin
    {
    public:
        InfiniteTCPPlugin() : HarpoonPlugin() { }
    
    
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
                        struct in_addr ina; 
                        unsigned short port;

                        int rbuf = 0, sbuf = 0;
                        std::vector<float> *vf_buf = getDistribution("rcvbuf");
                        if (vf_buf && vf_buf->size())
                            rbuf = (unsigned short)((*vf_buf)[0]);

                        vf_buf = getDistribution("sndbuf");
                        if (vf_buf && vf_buf->size())
                            sbuf = (unsigned short)((*vf_buf)[0]);

                        int tos = 0;
                        vf_buf = getDistribution("ip_tos");
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


                        getAddress("server_pool",ina, port);
                        InfTCPServerSock *tss = new InfTCPServerSock();
                        tss->openSocket(ina, port, rbuf, sbuf, tos, mss);

                        int fd = tss->getSocket();
                        sbuf = rbuf = 0;
                        SOCKLEN_T optlen = sizeof(int);
                        // not all that important to error-check these calls
                        getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sbuf, &optlen);
                        getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rbuf, &optlen);

                        std::ostringstream ostr;
                        ostr << "opened server socket with RCVBUF " << rbuf << " bytes, SNDBUF " << sbuf << " bytes." << std::endl;
                        hlog->log(7, ostr.str());

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
    
                pollfd pfd;

                InfTCPConnInfo *this_conn = new InfTCPConnInfo();
    
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

#ifdef DEBUG
                std::cerr << "client started at " << curr_time.tv_sec << " ending at " << session_end.tv_sec << std::endl;
#endif 

                this_conn->m_state = STATE_CONNECTING;
                this_conn->m_lastev = curr_time;
                this_conn->m_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (this_conn->m_fd < 0)
                    throw errno;
                



                if (fcntl(this_conn->m_fd, F_SETFL, O_NONBLOCK) < 0)
                    throw errno;
                
                if (bind(this_conn->m_fd, (const struct sockaddr*)&src_addr, sizeof(struct sockaddr_in)))
                    throw errno;
                
                // cause a RST on close.  just a trick to not let ports
                // get into CLOSE_WAIT state and lets those ports get reallocated
                struct linger ling = {1,0};
                if (setsockopt (this_conn->m_fd, SOL_SOCKET, SO_LINGER, 
                    &ling, sizeof(struct linger)) < 0)
                    throw errno;
                
                if (connect(this_conn->m_fd, (struct sockaddr *) &dst_addr, sizeof(dst_addr)) < 0)
                {
                    if (errno != EINPROGRESS)
                    {
                        std::cerr << "connect returned: " << errno << '/' << strerror(errno) << std::endl;
                        throw errno;
                    }
                }
                this_conn->m_pfdi = -1;

                int tcp_mss = 0;
                std::vector<float> *vf_buf = getDistribution("tcp_mss");
                if (vf_buf && vf_buf->size())
                {
                    unsigned int index = random() % vf_buf->size(); 
                    tcp_mss = (int)((*vf_buf)[index]);
                }

                if (tcp_mss != 0)
                {
#if __linux
                    int srv = setsockopt(this_conn->m_fd, SOL_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#else
                    int srv = setsockopt(this_conn->m_fd, IPPROTO_TCP, TCP_MAXSEG, &tcp_mss, sizeof(tcp_mss));
#endif
                    // std::cerr << "set TCP_MAXSEG to " << tcp_mss << " rv: " << srv << std::endl;
                }


                try
                {
                    bool end_of_session = false;
                    while (!end_of_session)
                    {
                        pfd.fd = this_conn->m_fd; 
                        pfd.events = POLLIN;
                        pfd.revents = 0;
                        if (this_conn->m_state == STATE_CONNECTING)
                            pfd.events |= POLLOUT;

                        int nsel = poll(&pfd, 1, 1000);
            
                        gettimeofday(&curr_time, 0);

                        // end of sesson if 1) interval end time has passed AND
                        // 2) we're on the last connection AND 3) the next_conn
                        // time has passed.
                        end_of_session = timercmp(&session_end, &curr_time, <);
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
                            bool destroy_conn = false;

                            // check fds ready to ready
                            errno = 0;
                            if (pfd.revents & POLLERR ||
                                pfd.revents & POLLHUP ||
                                pfd.revents & POLLNVAL)
                            {
                                destroy_conn = true;
                            }

                            if (!destroy_conn && this_conn->m_state == STATE_CONNECTING && pfd.revents & POLLOUT)
                            {
                                this_conn->m_lastev = curr_time;
                                this_conn->m_state = STATE_SETUP;
                                unsigned char flag = 0x7e;
                                m_log->log(9, "client sending setup byte");
                                if (send(this_conn->m_fd, &flag, 1, 0) < 0)
                                {
                                    m_log->log(9, "client couldn't send flag byte - bailing out.");
                                    destroy_conn = true;
                                }
                            }


                            if (!destroy_conn && this_conn->m_state == STATE_SETUP && pfd.revents & POLLIN)
                            {
                                unsigned char flag = 0x7e;
                                if (recv(this_conn->m_fd, &flag, 1, 0) != 1)
                                {
                                    m_log->log(9, "client couldn't recv flag byte - bailing out.");
                                    destroy_conn = true;
                                }
                                else
                                {
                                    m_log->log(9, "client got flag byte from server - transitioning to transfer state.");
                                    this_conn->m_state = STATE_TRANSFER;
                                }
                            }


                            if (!destroy_conn && this_conn->m_state == STATE_TRANSFER)
                            {
                                    if ((curr_time.tv_sec - this_conn->m_lastev.tv_sec) > EV_TIMEOUT)
                                    {
#ifdef DEBUG
                                        std::cerr << "marking stale transfer connection for death" << std::endl;
#endif
                                        destroy_conn = true;
                                    }

                                    while (1)
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
                                            std::cerr << "client got hangup from server? " << errno << '/' << strerror(errno) << std::endl;
#endif
                                            destroy_conn = true;
                                            break;
                                        }
                                        else
                                        {
                                            // something was read
                                            this_conn->m_lastev = curr_time;
                                        }
                                    }
                             }
                             if (destroy_conn)
                                 break;

                        } // nsel > 0
                    } // while !end of session
                }
                catch ( int &eno )
                {
                    if (eno > 0)
                        std::cerr << "tcp user thread " << pthread_self()
                                  << " abrupt session end: "
                                  << strerror(eno) << '/' << eno << std::endl;
                }
    
#ifdef DEBUG
                std::cerr << "end of client session" << std::endl;
#endif
 
                delete [] recv_buf;
                close(this_conn->m_fd);
                delete this_conn;
            }


        /*!
         * method handling all session-level stuff for servers: just accepting file requests
         * and serving files - not much.  just remember that the server socket is in
         * non-blocking mode, so all client sockets are created non-blocking too.
         */
        virtual void server_session()
            {
                std::vector<InfTCPConnInfo *> conn_list;
                typedef std::vector<InfTCPConnInfo *>::iterator RtI;
    
                int send_buf_size = BUFSIZ; // getpagesize();
                char *send_buf = new char[send_buf_size];
    
                int pfd_length = 10;
                pollfd *pfd = new pollfd[pfd_length];

                assert (getSharedData()); // assert that we have a server fd
                InfTCPServerSock *tss = dynamic_cast<InfTCPServerSock*>(getSharedData());
                int serv_fd = tss->getSocket();

                try
                {
                    // lifetime of server thread
                    while (1)
                    {
                        if ((pfd_length - 1) < int(conn_list.size()))
                        {
                            pfd_length = conn_list.size() + 1;
                            delete [] pfd;
                            pfd = new pollfd[pfd_length];
                        }

                        pfd[0].fd = serv_fd;
                        pfd[0].events = POLLIN;

                        int pfd_index = 1;
                        for (RtI iter = conn_list.begin(); iter != conn_list.end(); ++iter)
                        {
                            (*iter)->m_pfdi = pfd_index;
                            pfd[pfd_index].fd = (*iter)->m_fd;
                            pfd[pfd_index].events = (POLLIN | POLLOUT);
                            pfd_index++;
                        }
            
                        int n = poll(pfd, pfd_index, 1000);
            
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
                                std::cerr << "tcp server got connection from " << inet_ntoa(dst_addr.sin_addr) << ':' << ntohs(dst_addr.sin_port) << std::endl;
#endif
                                InfTCPConnInfo *new_session = new InfTCPConnInfo();
                                new_session->m_state = STATE_SETUP;
                                new_session->m_fd = cli_fd;
                                conn_list.push_back(new_session);
                                new_session->m_pfdi = -1;

                                int tos = 0;
                                std::vector<float> *vf_buf = getDistribution("ip_tos");
                                if (vf_buf && vf_buf->size())
                                {
                                    unsigned int index = random() % vf_buf->size(); 
                                    tos = (int)((*vf_buf)[index]);
                                }
                                if (tos != 0)
                                {
#if __linux
                                    int srv = setsockopt(cli_fd, SOL_IP, IP_TOS, &tos, sizeof(tos));
#else
                                    int srv = setsockopt(cli_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
#endif
                                    std::cerr << "set IP_TOS to 0x" << std::hex << tos << std::dec << " rv: " << srv << std::endl;
                                }

                                int mss = 0;
                                vf_buf = getDistribution("tcp_mss");
                                if (vf_buf && vf_buf->size())
                                {
                                    unsigned int index = random() % vf_buf->size(); 
                                    mss = (int)((*vf_buf)[index]);
                                }
                                if (mss != 0)
                                {
#if __linux
                                    int srv = setsockopt(cli_fd, SOL_TCP, TCP_MAXSEG, &mss, sizeof(mss));
#else
                                    int srv = setsockopt(cli_fd, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss));
#endif
                                    std::cerr << "set TCP_MAXSEG to " << mss << " rv: " << srv << std::endl;
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

                            bool dead_client = false;
                            if (pfd_index == -1)
                            {
                                iter++;
                                continue;
                            }


                            if (pfd[pfd_index].revents & POLLIN)
                            {   
                                unsigned char flag_byte = 0x7e;
                                if (recv ((*iter)->m_fd, &flag_byte, 1, 0) == 1)
                                    m_log->log(9, "Received flag byte in inftcp server session.");
                                else
                                    dead_client = true;

                                if ((*iter)->m_state == STATE_SETUP)
                                {
                                    flag_byte = 0x7e;
                                    if (send ((*iter)->m_fd, &flag_byte, 1, 0) != 1)
                                    {
                                        m_log->log(9, "server failed to send flag.");
                                        dead_client = true;
                                    }
                                    else
                                    {
                                        m_log->log(9, "server sent response flag.");
                                        (*iter)->m_state = STATE_TRANSFER;
                                    }
                                }

                            }
                
                            // blast as much data as we can
                            // or we get an EAGAIN
                            while (!dead_client && (*iter)->m_state == STATE_TRANSFER)
                            {
                                int to_send = send_buf_size;
                                int slen = send((*iter)->m_fd, send_buf, to_send, 0);
                                if (slen < 0 && errno == EAGAIN)
                                    break;
                                else if (slen <= 0)
                                {
                                    dead_client = true;
                                    break;
                                }
                        
                                // 
                                // some amount of data transferred
                                // 
                                m_bytes_xfered += slen;
                                m_bytes_xfered_recent += slen;
                            }

                            if (dead_client)
                            {   
                                close ((*iter)->m_fd);
                                delete (*iter);
                                iter = conn_list.erase(iter);
                                continue;
                            }   

                            iter++;
                        }
                    }
                } 
                catch ( int &eno )
                {
#ifdef DEBUG
                    std::cerr << "server thread " << pthread_self()
                              << " abrupt error: ";
                    if (eno > 0)
                        std::cerr << strerror(eno) << '/' << eno;
                    else
                        std::cerr << "unknown fault";
                    std::cerr << std::endl;
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
                std::cerr << "inftcp plugin shutdown " << pthread_self() << std::endl;
#endif
    
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                {
                    // must be careful to do this in a sane order
                    InfTCPServerSock *tss = dynamic_cast<InfTCPServerSock*>(getSharedData());
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
        enum InfTCPConnState { STATE_TRANSFER = 0, STATE_INVALID = 1, STATE_CONNECTING = 2, STATE_SETUP = 3 };

    private:
        //! prevent copy construction
        InfiniteTCPPlugin(InfiniteTCPPlugin &t) {}

        static const int EV_TIMEOUT;

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
         * structure holding connection state for client threads
         */
        struct InfTCPConnInfo
        {	
            InfTCPConnInfo() : m_fd(-1), m_state(STATE_INVALID), m_pfdi(-1) { }
    
            int m_fd;                  //!< client session socket
            InfTCPConnState m_state;   //!< STATE_TRANSFER, STATE_INVALID, STATE_SETUP, or STATE_CONNECTING
            struct timeval m_lastev;   //!< keep track of last activity on connection
            int m_pfdi;                //!< index into pollfd struct list for poll() call
        };


    };

    double InfiniteTCPPlugin::m_bytes_xfered = 0.0;
    double InfiniteTCPPlugin::m_bytes_xfered_recent = 0.0;
    time_t InfiniteTCPPlugin::m_last_stats_retrieval = time(NULL);
    time_t InfiniteTCPPlugin::m_started = time(NULL);
    const int InfiniteTCPPlugin::EV_TIMEOUT = 10;
}



/*!
* factory function.  "factory_generator" is the symbol we look for
 * when loading harpoon plugins.  using a C symbol gets around weirdo
 * name-mangling problems with C++.
 */
extern "C"
{
#if STATIC_PLUGINS
    Harpoon::HarpoonPlugin *infinite_tcp_plugin_generator(void)
    {
        return dynamic_cast<Harpoon::HarpoonPlugin*>(new Harpoon::InfiniteTCPPlugin()); 
    }
#else
    Harpoon::InfiniteTCPPlugin *factory_generator(void)
    {
        return (new Harpoon::InfiniteTCPPlugin()); 
    }
#endif
}


