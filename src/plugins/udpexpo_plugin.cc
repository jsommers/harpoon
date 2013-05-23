/* 
 * $Id: udpexpo_plugin.cc,v 1.1 2005-11-07 03:40:15 jsommers Exp $
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
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <math.h>
#include <set>

#include "config.h"
#include "configurator.hh"
#include "harpoon_plugin.hh"
#include "xmlrpc_util.hh"

namespace Harpoon
{

    /*!
     * @class UDPExpoPlugin
     *
     */
    class UDPExpoPlugin : public HarpoonPlugin
    {
    private:
        struct DnsSessionBlock
        {
            DnsSessionBlock()
                {
                    timerclear(&m_nextsend);
                    memset(&m_sasrc, 0, sizeof(struct sockaddr_in));
                    memset(&m_sadst, 0, sizeof(struct sockaddr_in));
                    m_period = 0;
                }

            struct timeval m_nextsend;
            struct sockaddr_in m_sasrc;
            struct sockaddr_in m_sadst;
            int m_period;
        };

    public:
        UDPExpoPlugin() : HarpoonPlugin() {}

        virtual bool init(HarpoonPluginConfig *hpc, HarpoonLog *hlog)
            {
                HarpoonPlugin::init(hpc, hlog);
                HarpoonPluginPersonality hpp = getPersonality();
                bool rv = true;
                if (hpp == plugin_personality_client)
                {
                    std::vector<float> *vu = getDistribution("virtual_users");
                    std::vector<float> *np = getDistribution("expo_means");
                    rv = (vu && np && vu->size() && np->size());
                    if (!rv)
                        std::cerr << "UDP expo client plugin - can't find required user-defined distributions 'virtual_users' and 'expo_means'." << std::endl;
                }
                return rv;
            }

        virtual void stats(std::ostream &os) 
            {
                HarpoonPluginPersonality hpp = getPersonality();
                if (hpp == plugin_personality_server)
                    server_stats(os);
                else if (hpp == plugin_personality_client)
                    client_stats(os);
            }

        virtual void server_session()
            {
                int recv_buf_size = getpagesize();
                char *recv_buf = new char[recv_buf_size];
                memset(recv_buf, 0x7f, recv_buf_size);
                int udp_fd = -1;

                std::vector<float> *pkt_sizes = getDistribution("file_sizes");
                assert(pkt_sizes && pkt_sizes->size());
                int next_ps = random() % pkt_sizes->size();

                try
                {
                    // udp socket for receiving dns-like chunks
                    if ((udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
                        throw -1;

                    struct sockaddr_in bind_addr;
                    memset(&bind_addr, 0, sizeof(struct sockaddr_in));
                    bind_addr.sin_family = AF_INET;
                    getAddress("server_pool", bind_addr.sin_addr, bind_addr.sin_port);
                    SOCKLEN_T addrlen = sizeof(struct sockaddr_in);
                    if (bind(udp_fd, (const struct sockaddr*)&bind_addr, addrlen) < 0)
                        throw -1;

                    while (!shouldExit())
                    {
                        struct pollfd pfd = { udp_fd, POLLIN, 0 };
                        int nready = poll(&pfd, 1, 1000);

                        if (shouldExit())
                            break;

                        if (nready < 0)
                        {
                            if (errno == EINTR)
                                continue;
                            else
                                throw errno;
                        }
                        else if (nready == 1 && pfd.revents & POLLIN)
                        {
                            struct sockaddr_in from;
                            SOCKLEN_T fromlen = sizeof(struct sockaddr_in);
                            int len = recvfrom(udp_fd, recv_buf, recv_buf_size, 0, (struct sockaddr*)&from, &fromlen);
                            if (len < 0)
                                throw -1;

                            m_bytes_recv += len;
                            m_bytes_recv_recent += len;

                            fromlen = sizeof(struct sockaddr_in);
                            len = sendto(udp_fd, recv_buf, std::min(recv_buf_size, int((*pkt_sizes)[next_ps++])), 0, (struct sockaddr*)&from, fromlen);
                            next_ps = next_ps % pkt_sizes->size();
                            if (len < 0)
                                throw -1;
                        }
                    }
                }
                catch ( ... )
                {
                    std::cerr << "udp expo server " << pthread_self() << 
                        " abrupt session end " << strerror(errno) <<
                        '/' << errno << std::endl;
                }

                close (udp_fd);
                delete [] recv_buf;
            }


        virtual void client_session()
            {
                int send_buf_size = getpagesize();
                char *send_buf = new char[send_buf_size];
                memset(send_buf, 0x7f, send_buf_size);

                // udp socket for sending dns-like chunks
                int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
                if (udp_fd < 0)
                    throw -1;

                struct sockaddr_in bind_addr;
                memset(&bind_addr, 0, sizeof(struct sockaddr_in));
                SOCKLEN_T addrlen = sizeof(struct sockaddr_in);
                if (bind(udp_fd, (const struct sockaddr*)&bind_addr, addrlen) < 0)
                    throw -1;

                std::vector<DnsSessionBlock> *session_list = new std::vector<DnsSessionBlock>();
                typedef std::vector<DnsSessionBlock>::iterator NSBI;

                std::vector<float> *virtual_users = getDistribution("virtual_users"); 
                std::vector<float> *expo_means = getDistribution("expo_means");

                std::vector<float> *pkt_sizes = getDistribution("file_sizes");
                assert(pkt_sizes && pkt_sizes->size());
                int next_ps = random() % pkt_sizes->size();


                struct timeval now;
                gettimeofday(&now, 0);
                for (int i = 0; i < virtual_users->front(); i++)
                {
                    DnsSessionBlock dsb;

                    int period = int((*expo_means)[(random() % expo_means->size())]);

                    struct timeval rand_offset;
                    timerclear(&rand_offset);
                    rand_offset.tv_usec = random() % (period * 1000);
                    rand_offset.tv_sec = rand_offset.tv_usec / 1000000;
                    rand_offset.tv_usec = rand_offset.tv_usec % 1000000;
                    timeradd(&now, &rand_offset, &dsb.m_nextsend);

                    dsb.m_sasrc.sin_family = AF_INET;
                    getAddress("client_source_pool", dsb.m_sasrc.sin_addr, dsb.m_sasrc.sin_port);

                    dsb.m_sadst.sin_family = AF_INET;
                    getAddress("client_destination_pool", dsb.m_sadst.sin_addr, dsb.m_sadst.sin_port);
                    dsb.m_period = period;

                    session_list->push_back(dsb);
                    m_num_sessions++;
                }
 
                try
                {
                    int lifetime = time(NULL) + getIntervalDuration();

                    // client thread lifetime
                    while (time(NULL) < lifetime)
                    {
                        if (shouldExit())
                            throw -1;

                        struct timeval nextsend = { INT_MAX, 0 };
                        for (NSBI iter = session_list->begin(); iter != session_list->end(); ++iter)
                        {
                            DnsSessionBlock &dsb = *iter;
                            if (timercmp(&dsb.m_nextsend, &nextsend, <))
                                nextsend = dsb.m_nextsend;
                        }
 
                        struct pollfd pfd = { udp_fd, POLLIN, 0 };

                        struct timeval now;
                        struct timeval zero = {0,0};
                        gettimeofday(&now, 0);
                        timerdiff(&nextsend, &now, &nextsend);

                        int nready = 0;
                        if (timercmp(&nextsend, &zero, >))
                            nready = poll(&pfd, 1, 1000);

                        if (shouldExit())
                            throw 0;

                        if (nready < 0)
                        {
                            if (errno == EINTR)
                                continue;
                            else
                                throw errno;
                        }
                        else if (nready == 1 && pfd.revents & POLLIN)
                        {
                            struct sockaddr_in from;
                            SOCKLEN_T fromlen = sizeof(struct sockaddr_in);
                            int len = recvfrom(udp_fd, send_buf, send_buf_size, 0, (struct sockaddr*)&from, &fromlen);
                            if (len < 0)
                                throw -1;

                            m_bytes_reply_recv += len;
                            m_bytes_reply_recv_recent += len;
                        }

                        // timeouts
                        gettimeofday(&now, 0);
                        if (timercmp(&nextsend, &now, >))
                            continue;

                        for (NSBI iter = session_list->begin(); iter != session_list->end(); ++iter)
                        {
                            DnsSessionBlock &dsb = *iter;

                            if (!timercmp(&dsb.m_nextsend, &now, >))
                            {
                                int next_send_ms = getExponential(dsb.m_period);
                                struct timeval next_send_offset;
                                next_send_offset.tv_sec = next_send_ms / 1000;
                                next_send_offset.tv_usec = (next_send_ms % 1000) * 1000;
                                timeradd(&next_send_offset, &dsb.m_nextsend, &dsb.m_nextsend);

                                int s = sendto(udp_fd, send_buf,
                                               std::min(send_buf_size, int((*pkt_sizes)[next_ps++])), 0,
                                               (struct sockaddr *)&dsb.m_sadst,
                                               sizeof(struct sockaddr_in));
                                if (s <= 0)
                                    std::cerr << "error in udp sendto? " << errno << std::endl;
                                next_ps = next_ps % pkt_sizes->size();
                                m_bytes_sent += s;
                                m_bytes_sent_recent += s;
                            }
                        }
                    }
                }
                catch ( ... )
                {
#ifdef DEBUG
                    std::cerr << "abrupt end in udp expo client " << pthread_self() << ": " << strerror(errno) << '/' << errno << std::endl;
#endif
                }

                close (udp_fd);
                delete (session_list);
                delete [] send_buf;
            }

        virtual void shutdown()
            {
#ifdef DEBUG
                std::cerr << "UDP expo plugin shutdown " << pthread_self() << std::endl;
#endif
                return;
            }

       int getExponential(int mean)
            {
                int r = random();
                double zero_one = double(r > 0 ? r : 1) / double(INT_MAX);
                double rv = (-1.0 * mean * log(zero_one));
                return int(rv);
            }

        static void server_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_recv_total", m_bytes_recv);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_recv_recent", m_bytes_recv_recent);

                time_t now = time (NULL);
                unsigned long interval = now - m_last_server_stats_retrieval;
                unsigned long up = now - m_server_started;
                double bw_total = (interval > 0 ? (m_bytes_recv * 8.0 / up) : 0.0);
                double bw_recent = (interval > 0 ? (m_bytes_recv_recent * 8.0 / interval) : 0.0);

                XmlRpcUtil::encode_struct_value(xmlrpc, "recv_bandwidth_total_bps", bw_total);
                XmlRpcUtil::encode_struct_value(xmlrpc, "recv_bandwidth_recent_bps", bw_recent);

                m_bytes_recv_recent = 0;
                m_last_server_stats_retrieval = now;
            }

        static void client_stats(std::ostream &xmlrpc)
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "num_sessions", m_num_sessions);
 
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_total", m_bytes_sent);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_sent_recent", m_bytes_sent_recent);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_reply_recv", m_bytes_reply_recv);
                XmlRpcUtil::encode_struct_value(xmlrpc, "bytes_reply_recv_recent", m_bytes_reply_recv_recent);


                time_t now = time (NULL);
                unsigned long interval = now - m_last_client_stats_retrieval;
                unsigned long up = now - m_client_started;
                double bw_total = (interval > 0 ? (m_bytes_sent * 8.0 / up) : 0.0);
                double bw_recent = (interval > 0 ? (m_bytes_sent_recent * 8.0 / interval) : 0.0);
                double bw_recv_total = (interval > 0 ? (m_bytes_reply_recv * 8.0 / up) : 0.0);
                double bw_recv_recent = (interval > 0 ? (m_bytes_reply_recv_recent * 8.0 / interval) : 0.0);

                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_total_bps", bw_total);
                XmlRpcUtil::encode_struct_value(xmlrpc, "send_bandwidth_recent_bps", bw_recent);
                XmlRpcUtil::encode_struct_value(xmlrpc, "reply_bandwidth_total_bps", bw_recv_total);
                XmlRpcUtil::encode_struct_value(xmlrpc, "reply_bandwidth_recent_bps", bw_recv_recent);
                m_bytes_sent_recent = 0;
                m_bytes_reply_recv_recent = 0;
                m_last_client_stats_retrieval = now;
            }

        static int m_num_sessions;
        static double m_bytes_sent;
        static double m_bytes_sent_recent;
        static double m_bytes_recv;
        static double m_bytes_recv_recent;
        static double m_bytes_reply_recv;
        static double m_bytes_reply_recv_recent;
        static time_t m_last_server_stats_retrieval;
        static time_t m_last_client_stats_retrieval;
        static time_t m_server_started;
        static time_t m_client_started;

    };

    int UDPExpoPlugin::m_num_sessions = 0;
    double UDPExpoPlugin::m_bytes_sent = 0.0;
    double UDPExpoPlugin::m_bytes_sent_recent = 0.0;
    double UDPExpoPlugin::m_bytes_reply_recv = 0.0;
    double UDPExpoPlugin::m_bytes_reply_recv_recent = 0.0;
    double UDPExpoPlugin::m_bytes_recv = 0.0;
    double UDPExpoPlugin::m_bytes_recv_recent = 0.0;
    time_t UDPExpoPlugin::m_last_client_stats_retrieval = time(NULL);
    time_t UDPExpoPlugin::m_last_server_stats_retrieval = time(NULL);
    time_t UDPExpoPlugin::m_client_started = time(NULL);
    time_t UDPExpoPlugin::m_server_started = time(NULL);
}


/*
 * factory function.  "factory_generator" is the symbol we look for
 * when loading harpoon plugins.
 */
extern "C"
{
#if STATIC_PLUGINS
    Harpoon::HarpoonPlugin *udpexpo_plugin_generator(void)
    {
        return dynamic_cast<Harpoon::HarpoonPlugin*>(new Harpoon::UDPExpoPlugin()); 
    }
#else
    Harpoon::UDPExpoPlugin *factory_generator(void)
    {
        return (new Harpoon::UDPExpoPlugin()); 
    }
#endif
}

