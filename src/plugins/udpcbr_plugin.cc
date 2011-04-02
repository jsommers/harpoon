/* 
 * $Id: udpcbr_plugin.cc,v 1.25 2005-12-18 17:14:58 jsommers Exp $
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
#include <cstring>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <map>
#include <vector>
#include <string>
#include <sstream>

#include "config.h"
#include "configurator.hh"
#include "harpoon_plugin.hh"
#include "xmlrpc_util.hh"

namespace Harpoon
{
    /*!
     * @class UDPcbrPlugin
     *
     */
    class UDPcbrPlugin : public HarpoonPlugin
    {
    public:
        UDPcbrPlugin() : HarpoonPlugin() {}

        virtual bool init(HarpoonPluginConfig *hpc, HarpoonLog *hlog)
            {
                HarpoonPlugin::init(hpc, hlog);
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                {
                    ServerPluginState *sps = dynamic_cast<ServerPluginState*>(getSharedData());
                    if (!sps)
                    {
                        server_init();
                        sps = dynamic_cast<ServerPluginState*>(getSharedData());
                        assert (sps);
                    }
                    sps->incrRefCount();
                }
                return (true);
            }

        virtual void stats(std::ostream &os) 
            {
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                    server_stats(os);
                else if (hpp == plugin_personality_client)
                    client_stats(os);
            }

        virtual void client_session()
            {
                int recv_buf_size = getpagesize();
                char *recv_buf = new char[recv_buf_size];
                memset(recv_buf, 0, sizeof(recv_buf_size));

                unsigned short pkt_size = 1000;
                int br = 10000;

                std::vector<float> *datagram_size = getDistribution("datagram_size");
                std::vector<float> *bitrate = getDistribution("bitrate");
                if (datagram_size && datagram_size->size())
                    pkt_size = (unsigned short)((*datagram_size)[0]);
                if (bitrate && bitrate->size())
                    br = int((*bitrate)[0]);

                // how much to delay (millisec) between sends of 
                // datagrams of size pkt_size to achieve bitrate
                float fdel = float(pkt_size) * 8.0 / float(br);
                int delay = int(fdel * 1000.0);
#ifdef DEBUG
                std::cerr << "asking for delay of " << delay << " millisec between pkts." << std::endl;
#endif                 

                std::vector<float> *interconn = getDistribution("interconnection_times");
                assert (interconn && interconn->size());
                int next_ic = random() % interconn->size();


                std::map<int, UDPTransferInfo *> transfer_map;
                typedef std::map<int, UDPTransferInfo *>::iterator TMI;

                struct sockaddr_in src_addr;
                create_src(&src_addr);

                struct sockaddr_in dst_addr;
                create_dst(&dst_addr);

                int control_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (control_fd < 0)
                    throw errno;

                if (bind(control_fd, (const struct sockaddr*)&src_addr,
                         sizeof(struct sockaddr_in)))
                    throw errno;

                // no TIME_WAIT
                struct linger ling = {1,0};
                if (setsockopt (control_fd, SOL_SOCKET, SO_LINGER, 
                                &ling, sizeof(struct linger)) < 0)
                    throw errno;
        
                int max_attempts = 5;
                while (connect(control_fd, (struct sockaddr *) &dst_addr, 
                               sizeof(dst_addr)) != 0)
                {	    
                    std::cerr << "udp control connect failed to " <<
                        inet_ntoa(dst_addr.sin_addr) << '/' <<
                        ntohs(dst_addr.sin_port) << std::endl;

                    if (shouldExit())
                    {
                        close (control_fd);
                        throw -1;
                    }
                       
                    if (!max_attempts--)
                    {
                        close (control_fd);
                        throw -1;
                    }

                    sleep(1); 
                } 

                struct timeval curr_time;
                gettimeofday(&curr_time, 0);

                struct timeval session_end = curr_time;
                timeraddfloat(session_end, getIntervalDuration());

                struct timeval next_start = curr_time;
                timeraddfloat(next_start, (*interconn)[next_ic++]);
                next_ic = next_ic % interconn->size();

#ifdef DEBUG
                std::cerr << "setting next start to be " <<
                    (next_start.tv_sec - curr_time.tv_sec) << 
                    " secs from now" << std::endl;
#endif

                int pfd_length = 10;
                pollfd *pfd = new pollfd[pfd_length];
                assert (pfd);

                int next_req_id = 1;
                bool end_of_session = false;
                try
                {
                    while (!end_of_session || transfer_map.size())
                    {
                        // start new transfer - don't start any new one if we're just waiting
                        // for some stragglers to complete at the end of a session.
                        // if (next_start <= curr_time.tv_sec && curr_time.tv_sec < session_end)
                        if (timercmp(&curr_time, &next_start, >) &&
                            timercmp(&session_end, &curr_time, >))
                        {
                            m_num_sessions++;

                            next_start = curr_time;
                            timeraddfloat(next_start, (*interconn)[next_ic++]);
                            next_ic = next_ic % interconn->size();

#ifdef DEBUG
                            std::cerr << "setting next start to be " << 
                                (next_start.tv_sec - curr_time.tv_sec) <<
                                " secs from now" << std::endl;
#endif
                            end_of_session = (timercmp(&next_start, &session_end,  >));

#ifdef DEBUG
                            if (end_of_session)
                                std::cerr << "setting end-of-session flag" << std::endl;
#endif

                            UDPTransferInfo *new_transfer = new UDPTransferInfo();
                            int new_fd = -1;

                            if ((new_fd = socket (AF_INET, SOCK_DGRAM, 0)) < 0)
                                throw errno;

                            if (fcntl(new_fd, F_SETFL, O_NONBLOCK) < 0)
                                throw errno;

                            struct sockaddr_in bind_addr;
                            memset(&bind_addr, 0, sizeof(struct sockaddr_in));
                            bind_addr.sin_family = AF_INET;
                            bind_addr.sin_addr = src_addr.sin_addr;
                            bind_addr.sin_port = htons(0);
                            if (bind (new_fd, (const struct sockaddr*)&bind_addr,
                                      sizeof(struct sockaddr_in)) < 0)
                                throw errno;
            

                            // find out what port the system gave us
                            SOCKLEN_T addr_len = sizeof(struct sockaddr_in);
                            if (getsockname(new_fd, (struct sockaddr *)&bind_addr, 
                                            &addr_len) < 0)
                                throw errno;

                            new_transfer->m_ephem_port = bind_addr.sin_port;
                            new_transfer->m_started = time(NULL);

                            if (c_udp_file_request (control_fd, new_transfer->m_ephem_port,
                                                    next_req_id, pkt_size, delay) < 0)
                                throw errno;

                            m_num_req++;

                            new_transfer->m_request_id = next_req_id++;
                            new_transfer->m_fd = new_fd;
                            new_transfer->m_pfid = -1;

                            transfer_map.insert(std::pair<int, UDPTransferInfo*>(new_transfer->m_request_id, new_transfer));
                        }


                        /* 
                         * add all fds to poll vector.
                         * set time-flag on select to next start time 
                         * or nearest inter-file time.
                         */
                        struct timeval next_timer = next_start;
                        ssize_t nxfer = transfer_map.size() + 1;
                        if (nxfer > pfd_length)
                        {
                            delete [] pfd;
                            pfd_length = (transfer_map.size() * 2) + 1;
                            pfd = new pollfd[pfd_length];
                        }

                        memset(pfd, 0, sizeof(pollfd) * pfd_length);
                        pfd[0].fd = control_fd;
                        pfd[0].events = POLLIN;

                        int pfid = 1;
                        for (TMI iter = transfer_map.begin(); iter != transfer_map.end(); ++iter)
                        {
                            
                            pfd[pfid].fd = (iter->second)->m_fd;
                            pfd[pfid].events = POLLIN;
                            (iter->second)->m_pfid = pfid++;
                        }

                        struct timeval timeout = {1,0};
                        if (timercmp(&curr_time, &next_timer, <))
                        {
                            timerdiff(&next_timer, &curr_time, &timeout);

                            // XXX cap sleep at 1 sec?
                            if (timeout.tv_sec > 1)
                                timeout.tv_sec = 1;
                        }
                        int tmo = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
                        int nready = poll(pfd, pfid, tmo);
                        gettimeofday(&curr_time, 0);

                        if (shouldExit())
                            break;

                        if (nready < 0)
                        {
                            if (errno == EINTR)
                                continue;
                            else
                                throw errno;
                        }
                        else if (nready > 0)
                        {
                            if (pfd[0].revents & POLLIN)
                            {
                                UDPControlResponse uc_resp;
                                memset(&uc_resp, 0, sizeof(UDPControlResponse));

                                int tot_recv = 0;
                                while (tot_recv < UDP_CTRL_RESPONSE_LEN)
                                {
                                    int rlen = recv(control_fd, &uc_resp.raw_data[tot_recv],
                                                    (UDP_CTRL_RESPONSE_LEN - tot_recv), 0);
                                    if (rlen <= 0)
                                    {
                                        throw -1;
                                        break;
                                    }
                                    tot_recv += rlen;
                                }

                                uc_resp.s.request_id = ntohl(uc_resp.s.request_id);

                                TMI iter = transfer_map.find(uc_resp.s.request_id);
                                if (iter != transfer_map.end())
                                {
                                    UDPTransferInfo *uti = iter->second;
                                    close(uti->m_fd);
                                    delete (uti);
                                    transfer_map.erase(iter);
                                }
                                else
                                {
                                    std::ostringstream ostr;
                                    ostr << "Got server completion for request " << uc_resp.s.request_id << " but couldn't find matching request.";
                                    m_log->log(3, ostr.str());
                                }
                            }


                            // check fds ready to read
                            for (TMI iter = transfer_map.begin(); iter != transfer_map.end(); ++iter)
                            {
                                UDPTransferInfo *uti = iter->second;
                                int pfidx = uti->m_pfid;
                                assert (pfidx < pfid);
                                
                                if (pfidx == -1)
                                    continue;

                                if (pfd[pfidx].revents & POLLIN)
                                {
                                    while (1)
                                    {
                                        int bytes_read = recvfrom(uti->m_fd, recv_buf, recv_buf_size, 0, 0, 0);
                                        // we don't really care about error checking this, do we?
                                        // all udp is getting dropped on the floor anyway.
                                        if (bytes_read < 0)
                                        {
                                            if (errno != EAGAIN)
                                            {
                                                std::ostringstream ostr;
                                                ostr << "UDP receive failure: " << strerror(errno) << '/' << errno;
                                                m_log->log(7, ostr.str());
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                catch ( ... )
                {
                    std::ostringstream ostr;
                    ostr << "udp client " << pthread_self() << 
                        " abrupt session end " << strerror(errno) <<
                        '/' << errno;
                    m_log->log(1, ostr.str());
                }

                close (control_fd);
                // cleanup anything left over
                for (TMI iter = transfer_map.begin(); iter != transfer_map.end(); ++iter)
                {
                    UDPTransferInfo *uti = iter->second;
                    close(uti->m_fd);
                    delete (uti);
                }
                delete [] recv_buf;
                delete [] pfd;
            }


        virtual void server_session()
            {
                std::vector<UDPServerConnInfo*> session_list;
                typedef std::vector<UDPServerConnInfo*>::iterator UssiI;

                std::vector<UDPSessionControl*> request_list;
                typedef std::vector<UDPSessionControl*>::iterator UVI;

                int send_buf_size = getpagesize();
                char *send_buf = new char[send_buf_size];

                assert (getSharedData()); // assert that we have a server fd
                ServerPluginState *sps = dynamic_cast<ServerPluginState *>(getSharedData());
                int serv_fd = sps->m_server_fd;

                std::vector<float> *filesizes = getDistribution("file_sizes");
                assert(filesizes && filesizes->size());
                int next_fs = random() % filesizes->size();

                // udp socket for blasting files
                int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (udp_fd < 0)
                    throw -1;

                int ip_tos = 0;
                std::vector<float> *vf_buf = getDistribution("ip_tos");
                if (vf_buf && vf_buf->size())
                    ip_tos = (int)((*vf_buf)[0]);
                if (ip_tos != 0)
                    setsockopt(udp_fd, IPPROTO_IP, IP_TOS, &ip_tos, sizeof(ip_tos));

                struct sockaddr_in bind_addr;
                SOCKLEN_T addr_len = sizeof(struct sockaddr_in);
                memset(&bind_addr, 0, addr_len);
                bind_addr.sin_family = AF_INET;

                std::ostringstream ostr;
                std::string tmp;

                if (bind (udp_fd, (struct sockaddr*)&bind_addr, addr_len) < 0)
                {
                    ostr << "bind of UDP socket failed: " << strerror(errno) << '/' << errno;
                    m_log->log(1, ostr.str());
                    ostr.str(tmp);
                }

                getsockname(udp_fd, (struct sockaddr *)&bind_addr, &addr_len);
                ostr << "Server UDP socket bound to port: " << ntohs(bind_addr.sin_port);
                m_log->log(7, ostr.str());
                ostr.str(tmp);

                int pfd_length = 10;
                pollfd *pfd = new pollfd[pfd_length];

                try
                {
                    // server thread lifetime
                    while (1)
                    {
                        if (shouldExit())
                            throw -1;

                        struct timeval curr_time;
                        gettimeofday(&curr_time, 0);

                        int csize = int(session_list.size());
                        if ((csize+1) >= pfd_length)
                        {
                            delete [] pfd;
                            pfd_length = session_list.size() + 1;
                            pfd = new pollfd[pfd_length];
                        }
                        
                        memset(pfd, 0, sizeof(pollfd) * pfd_length);
                        pfd[0].fd = serv_fd;
                        pfd[0].events = POLLIN;

                        int pfid = 1;
                        for (UssiI iter = session_list.begin(); iter != session_list.end(); ++iter)
                        {
                            pfd[pfid].fd = (*iter)->m_fd;
                            pfd[pfid].events = POLLIN;
                            (*iter)->m_pfid = pfid++;
                        }

                        int tmo =  1000;
                        struct timeval nextsend = { 0, 0};

                        // set timeout for next udp pkt send
                        for (UVI iter = request_list.begin(); iter != request_list.end(); ++iter)
                        {
                            UDPSessionControl *usc = *iter;
                            if (nextsend.tv_sec == 0 ||
                                timercmp(&usc->m_next_send, &nextsend, <))
                                nextsend = usc->m_next_send;
                        }
            
                        if (timercmp(&nextsend, &curr_time, >))
                        {
                            struct timeval timeout;
                            timerdiff(&nextsend, &curr_time, &timeout);
                            tmo = timeout.tv_sec * 1000 + timeout.tv_usec / 1000;
                            tmo = std::min(tmo, 1000);
                        }
                        else if (nextsend.tv_sec != 0)
                        {
                            // overran next time to send
                            tmo = 0;
                        }
                        int nready = poll(pfd, pfid, tmo);
                        gettimeofday(&curr_time, 0);

                        if (shouldExit())
                            throw 0;

                        if (nready < 0 && errno == EINTR)
                            continue;
                        else if (nready < 0 && errno != EINTR)
                            throw errno;

                        if (pfd[0].revents & POLLIN)
                        {
                            struct sockaddr_in dst_addr;
                            SOCKLEN_T dst_addr_len = sizeof(dst_addr);
                            int cli_fd = accept(serv_fd, (struct sockaddr *) &dst_addr, &dst_addr_len);
                            if (cli_fd > 0)
                            {
                                ostr << "UDP CBR server got connection from " << inet_ntoa(dst_addr.sin_addr) << ':' << ntohs(dst_addr.sin_port);
                                m_log->log(7, ostr.str());
                                ostr.str(tmp);

                                UDPServerConnInfo *new_session = new UDPServerConnInfo();
                                new_session->m_fd = cli_fd;
                                new_session->m_pfid = -1;
                                memcpy(&new_session->m_peer, &dst_addr, sizeof(struct sockaddr_in));
                                session_list.push_back(new_session);
                            }
                        }

                        UssiI sessiter = session_list.begin(); 
                        while (sessiter != session_list.end())
                        {
                            UDPServerConnInfo *ussi = *sessiter; 

                            if (ussi->m_pfid > 0 && 
                                pfd[ussi->m_pfid].revents & POLLIN)
                            {
                                // if new request, add control block and send first packet
                                // of data (what about degenerate case of one pkt per file?)
                                struct UDPControlRequest ucr;
                                int tot_recv = 0;
                                bool disconnect = false;
                                while (tot_recv < UDP_CTRL_REQUEST_LEN)
                                {
                                    int rlen = recv(ussi->m_fd, &ucr.raw_data[tot_recv],
                                                    (UDP_CTRL_REQUEST_LEN - tot_recv), 0);
                                    if (rlen < 0)
                                    {
                                        if (errno == EAGAIN)
                                            continue;
                                        else
                                        {
                                            disconnect = true;
                                            break;
                                        }    
                                    }

                                    if (rlen == 0)
                                    {
                                        disconnect = true;
                                        break;
                                    }    

                                    tot_recv += rlen;
                                }

                                if (!disconnect && tot_recv == UDP_CTRL_REQUEST_LEN)
                                {
                                    UDPSessionControl *usc = new UDPSessionControl();
                                    usc->m_ussi = ussi;
                                    memcpy(&usc->m_dst_addr, &ussi->m_peer, sizeof(sockaddr_in));
                                    usc->m_dst_addr.sin_port = ucr.s.response_port;
                  
                                    // get next file size and
                                    // atomically increment file size pointer
                                    usc->m_bytes_remain = int((*filesizes)[next_fs++]);
                                    next_fs = next_fs % filesizes->size();

                                    usc->m_send_interval.tv_sec = 0;
                                    usc->m_send_interval.tv_usec = ntohl(ucr.s.ms_wait) * 1000;
                                    timeradd (&curr_time, &usc->m_send_interval, &usc->m_next_send);
                                    usc->m_request_id = ntohl(ucr.s.request_id);
                                    usc->m_pkt_size = ntohs(ucr.s.pkt_size);

                                    request_list.push_back(usc);

#ifdef DEBUG
                                    ostr << "udp request id " <<
                                        usc->m_request_id << 
                                        " sending " <<
                                        usc->m_bytes_remain << " to " << 
                                        inet_ntoa(usc->m_dst_addr.sin_addr)
                                              << ':' << 
                                        ntohs(usc->m_dst_addr.sin_port) 
                                        << " psize<" << usc->m_pkt_size 
                                        << ">interval<" << usc->m_send_interval.tv_sec 
                                        << '.' << std::setw(6) << std::setfill('0') << usc->m_send_interval.tv_usec << ">"; 
                                    m_log->log(9, ostr.str());
                                    ostr.str(tmp);
#endif
                                    m_num_transfer++;
                                }
                                else 
                                    disconnect = true;

                                if (disconnect)
                                {
                                    ostr << "disconnected client.  (errno=" << strerror(errno) << '/' << errno << ')';
                                    m_log->log(7, ostr.str());
                                    ostr.str(tmp);

                                    UVI reqiter = request_list.begin();
                                    while (reqiter != request_list.end())
                                    {	
                                        if ((*reqiter)->m_ussi == ussi)
                                        {
                                            UDPSessionControl *usc = *reqiter;
                                            delete (usc);
                                            reqiter = request_list.erase(reqiter);
                                            continue;
                                        }
                                        reqiter++;
                                    }

                                    close (ussi->m_fd);
                                    delete (ussi);
                                    sessiter = session_list.erase(sessiter);     
                                    continue;
                                }
                            }    
                            sessiter++;
                        }


                        // check whether we should send some data
                        UVI reqiter = request_list.begin();
                        while (reqiter != request_list.end())
                        {
                            UDPSessionControl *usc = *reqiter;
                            if (timercmp(&curr_time, &usc->m_next_send, >))
                            {
                                if (usc->m_bytes_remain < usc->m_pkt_size)
                                    usc->m_pkt_size = usc->m_bytes_remain;

                                int s = sendto(udp_fd, send_buf,
                                               std::min(send_buf_size, usc->m_pkt_size), 0,
                                               (struct sockaddr *)&usc->m_dst_addr,
                                               sizeof(struct sockaddr_in));
                                if (s <= 0)
                                {
                                    ostr << "error in udp sendto? " << strerror(errno) << '/' << errno;
                                    m_log->log(3, ostr.str());
                                    throw errno;
                                }

                                m_bytes_xfered += s;
                                m_bytes_xfered_recent += s;

                                usc->m_bytes_remain -= usc->m_pkt_size;
                                if (usc->m_bytes_remain <= 0)
                                {
                                    usc->m_bytes_remain = 0;
#ifdef DEBUG
                                    ostr << "completed udp request " << usc->m_request_id;
                                    m_log->log(9, ostr.str());
                                    ostr.str(tmp);
#endif

                                    UDPControlResponse uc_resp;
                                    uc_resp.s.request_id = htonl(usc->m_request_id);

                                    int tot_sent = 0;
                                    bool ok_to_remove = true;
                                    while (tot_sent != UDP_CTRL_RESPONSE_LEN)
                                    {
                                        int slen = send(usc->m_ussi->m_fd, &uc_resp.raw_data[tot_sent],
                                                        UDP_CTRL_RESPONSE_LEN, 0);
                                        if (slen < 0 && errno == EAGAIN)
                                            continue;
                                        else if (slen < 0 && errno != EAGAIN)
                                        {
                                            ostr << "couldn't send response message - need to destroy session block and any active transfer blocks - " << strerror(errno) << '/' << errno;
                                            m_log->log(3, ostr.str());
                                            ostr.str(tmp);

                                            ok_to_remove = false;
                                            UDPServerConnInfo *ussi = (*reqiter)->m_ussi;
                                            reqiter = request_list.begin();
                                            while (reqiter != request_list.end())
                                            { 
                                                UDPSessionControl *usc = *reqiter;
                                                if (usc->m_ussi == ussi)
                                                {
                                                    delete (usc);
                                                    reqiter = request_list.erase(reqiter);
                                                    continue;
                                                }
                                                reqiter++;
                                            } 
                                            // reset reqiter to begin, so we
                                            // can semi-reasonably continue
                                            // in the current request-processing
                                            // loop.
                                            reqiter = request_list.begin();


                                            UssiI sess_iter = session_list.begin();
                                            while (sess_iter != session_list.end())
                                            { 
                                                UDPServerConnInfo *tmp_ussi = *sess_iter; 
                                                if (tmp_ussi == ussi)
                                                {
                                                    close(tmp_ussi->m_fd);
                                                    delete (tmp_ussi);
                                                    sess_iter = session_list.erase(sess_iter);
                                                    break;
                                                }
                                                sess_iter++;
                                            }

                                            // break out of control-send
                                            // loop
                                            break;
                                        }
                                        tot_sent += slen;
                                    }

                                    if (ok_to_remove)
                                    {
                                        UDPSessionControl *usc = *reqiter;
                                        delete (usc);
                                        reqiter = request_list.erase(reqiter);
                                        continue;
                                    }
                                }
                                else
                                {
                                    struct timeval last_send = usc->m_next_send;
                                    timeradd (&last_send, &usc->m_send_interval, &usc->m_next_send);
                                }
                            }
                            reqiter++;
                        }
                    }
                }
                catch ( ... )
                {
                    ostr << "abrupt end in udp server " << pthread_self() << ": " << strerror(errno) << '/' << errno;
                    m_log->log(1, ostr.str());
                }

                // disconnected - make sure request_list and session_list
                // are cleaned up.

                for (UssiI iter = session_list.begin();
                     iter != session_list.end(); ++iter)
                {
                    close ((*iter)->m_fd);
                    delete (*iter);
                }

                for (UVI iter = request_list.begin(); 
                     iter != request_list.end();
                     ++iter)
                {
                    UDPSessionControl *usc = *iter;
                    delete (usc);
                }
                close (udp_fd);
                delete [] send_buf;
                delete [] pfd;
            }

        virtual void shutdown()
            {
                std::ostringstream ostr;
                ostr << "udpcbr plugin shutdown " << pthread_self();
                m_log->log(5, ostr.str());

                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                {
                    // must be careful to do this in a sane order
                    ServerPluginState *sps = dynamic_cast<ServerPluginState*>(getSharedData());
                    if (sps)
                    {
                        sps->decrRefCount();
                        if (sps->isRefCountZero())
                        {
                            close(sps->m_server_fd);
                            setSharedData(0);
                            delete (sps);
                        }
                    }
                }
                return;
            }


        int c_udp_file_request (int control_fd, unsigned short port,
                                int request_id, unsigned short pkt_size,
                                int ms_delay)
            {
#ifdef DEBUG
                std::cerr << "sending udp file request" << std::endl;
#endif
                UDPControlRequest ucr;
                memset(&ucr, 0, sizeof(UDPControlRequest));
                ucr.s.response_port = port; // already in net byte order
                ucr.s.pkt_size = htons(pkt_size);
                ucr.s.request_id = htonl(request_id);
                ucr.s.ms_wait = htonl(ms_delay);

                int sent = 0; 
                while (sent < UDP_CTRL_REQUEST_LEN)
                {
                    int n = send(control_fd, &ucr.raw_data[sent],
                                 (UDP_CTRL_REQUEST_LEN - sent), 0);
                    if (n <= 0)
                        return -1;

                    sent += n;
                }
#ifdef DEBUG
                std::cerr << "udp request successfully sent" << std::endl;
#endif
                return 0;
            }


#ifndef __SUNPRO_CC  // sun compiler stupidity
    private:
#endif

        static const int UDP_CTRL_RESPONSE_LEN = 4;
        static const int UDP_CTRL_REQUEST_LEN = 12;


        /*!
         * create the server socket.  we make it non-blocking so that we don't get stuck
         * at any point inside the user_server() routine.
         */
        void server_init()
            {
                std::ostringstream ostr;
                ostr << "server_init from thread " << pthread_self();
                m_log->log(5, ostr.str());

                // make a server listener file descriptor that all threads can use
                ServerPluginState *sps = new ServerPluginState();

                sps->m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
                if (sps->m_server_fd < 0)
                    throw errno;

                if (fcntl(sps->m_server_fd, F_SETFL, O_NONBLOCK) < 0)
                    throw errno;

                int opt = 1;
                if (setsockopt(sps->m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt,
                               sizeof(int)) < 0)
                    throw errno;

                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(struct sockaddr_in));
                serv_addr.sin_family = AF_INET;
                getAddress("server_pool", serv_addr.sin_addr, serv_addr.sin_port);

                std::string tmp;
                ostr.str(tmp);
                ostr << "server binding to address " << inet_ntoa(serv_addr.sin_addr) << ':' << serv_addr.sin_port;
                m_log->log(7, ostr.str());

                if (bind(sps->m_server_fd, (const sockaddr *)&serv_addr,
                         sizeof(struct sockaddr_in)) < 0)
                    throw errno;

                if (listen(sps->m_server_fd, SOMAXCONN))
                    throw errno;

                setSharedData(sps);
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

        class ServerPluginState : public SharedPluginState
        {
        public:
            ServerPluginState() : SharedPluginState(), m_server_fd(0) { }
            virtual ~ServerPluginState() {}

            int m_server_fd;
        };

        struct UDPServerConnInfo
        {
            UDPServerConnInfo() : m_fd(0), m_pfid(0)
                {
                    memset(&m_peer, 0, sizeof(m_peer)); 
                }
            int m_fd;
            int m_pfid;
            struct sockaddr_in m_peer;
        };

        struct UDPTransferInfo
        {	
            UDPTransferInfo() : m_fd(0), m_pfid(0), m_ephem_port(0), m_request_id(0), m_started(0) {}

            int m_fd;
            int m_pfid;    
            int m_ephem_port; 
            int m_request_id;
            time_t m_started;
        };


        struct UDPControlRequest
        {
            union
            {
                char raw_data[UDP_CTRL_REQUEST_LEN];
                struct
                {
                    unsigned short response_port;
                    unsigned short pkt_size;
                    int ms_wait;
                    int request_id;
                } s;
            };
        };


        struct UDPControlResponse
        {
            union
            {
                char raw_data[UDP_CTRL_RESPONSE_LEN];
                struct
                {
                    int request_id;
                } s;
            };
        };

    
        struct UDPSessionControl
        {
            UDPSessionControl() : m_ussi(0), m_bytes_remain(0), m_pkt_size(0), m_request_id(0)
                {
                    memset(&m_dst_addr, 0, sizeof(m_dst_addr));
                    timerclear (&m_next_send);
                    timerclear (&m_send_interval);
                }

            UDPServerConnInfo *m_ussi;
            int m_bytes_remain;
            struct sockaddr_in m_dst_addr;
            struct timeval m_next_send;
            struct timeval m_send_interval;
            int m_pkt_size;
            int m_request_id;
        };

        static void client_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_sessions", m_num_sessions);
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_requests", m_num_req);
            }

        static void server_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_transfer", m_num_transfer);
 
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_total", m_bytes_xfered);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_recent", m_bytes_xfered_recent);

                time_t now = time (NULL);
                unsigned long interval = now - m_last_stats_retrieval;
                unsigned long up = now - m_started;
                double bw_total = (interval > 0 ? (m_bytes_xfered * 8.0 / up) : 0.0);
                double bw_recent = (interval > 0 ? (m_bytes_xfered_recent * 8.0 / interval) : 0.0);

                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_total_bps", bw_total);
                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_recent_bps", bw_recent);

                m_bytes_xfered_recent = 0;
                m_last_stats_retrieval = now;
            }

        static int m_num_sessions;
        static int m_num_req;
        static int m_num_transfer;
        static double m_bytes_xfered;
        static double m_bytes_xfered_recent;
        static time_t m_last_stats_retrieval;
        static time_t m_started;
    };

    int UDPcbrPlugin::m_num_sessions = 0;
    int UDPcbrPlugin::m_num_req = 0;
    int UDPcbrPlugin::m_num_transfer = 0;
    double UDPcbrPlugin::m_bytes_xfered = 0.0;
    double UDPcbrPlugin::m_bytes_xfered_recent = 0.0;
    time_t UDPcbrPlugin::m_last_stats_retrieval = time(NULL);
    time_t UDPcbrPlugin::m_started = time(NULL);
}


/*
 * factory function.  "factory_generator" is the symbol we look for
 * when loading harpoon plugins.
 */
extern "C"
{
#if STATIC_PLUGINS
  Harpoon::HarpoonPlugin *udpcbr_plugin_generator(void)
  {
     return dynamic_cast<Harpoon::HarpoonPlugin*>(new Harpoon::UDPcbrPlugin()); 
  }
#else
  Harpoon::UDPcbrPlugin *factory_generator(void)
  {
     return (new Harpoon::UDPcbrPlugin()); 
  }
#endif
}

