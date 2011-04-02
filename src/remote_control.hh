/*
 * $Id: remote_control.hh,v 1.4 2005-11-08 14:17:11 jsommers Exp $
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


#ifndef __REMOTE_CONTROL_HH__
#define __REMOTE_CONTROL_HH__

#define DEFAULT_PORT 8180

#include <stdio.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <pthread.h>

#ifndef TEST
#include "harpoon_controller.hh"
#endif

#include "config.h"
#include "expat.h"

namespace Harpoon
{
    class HarpoonRemoteControl
    {
    public:
        HarpoonRemoteControl(struct in_addr iaddr, unsigned short port = DEFAULT_PORT) :
            m_addr(iaddr), m_port(port), m_serving(true)
#ifndef TEST
                                                                                                  , m_controller(0)
#endif
            {
                /*
                 * register all XML-RPC methods callable by the world.
                 */
                m_method_map["system.listMethods"] = &HarpoonRemoteControl::xmlrpc_list_methods;
                m_method_map["system.null"] = &HarpoonRemoteControl::xmlrpc_null_method;
                m_method_map["queryPlugins"] = &HarpoonRemoteControl::xmlrpc_query_method;
                m_method_map["loadConfig"] = &HarpoonRemoteControl::xmlrpc_loadconfig_method;
                m_method_map["unloadConfig"] = &HarpoonRemoteControl::xmlrpc_unloadconfig_method;
                m_method_map["loadPlugin"] = &HarpoonRemoteControl::xmlrpc_loadplugin_method;
                m_method_map["unloadPlugin"] = &HarpoonRemoteControl::xmlrpc_unloadplugin_method;
                m_method_map["stopPlugin"] = &HarpoonRemoteControl::xmlrpc_stopplugin_method;
                m_method_map["startPlugin"] = &HarpoonRemoteControl::xmlrpc_startplugin_method;
                m_method_map["getStats"] = &HarpoonRemoteControl::xmlrpc_getstats_method;
                m_method_map["suicide"] = &HarpoonRemoteControl::xmlrpc_suicide_method;
                m_method_map["resetAll"] = &HarpoonRemoteControl::xmlrpc_resetall_method;
                m_method_map["incrTime"] = &HarpoonRemoteControl::xmlrpc_incrtime_method;
                pthread_mutex_init(&m_serving_mutex, NULL);
            }

        virtual ~HarpoonRemoteControl()
            {
                pthread_mutex_destroy(&m_serving_mutex);
            }
        
#ifndef TEST
        void setController(HarpoonController *hc) { m_controller = hc; }
#endif

        /*!
         * set the m_serving flag, telling main loop that its time has come and gone.
         */
        void shutdown()
            {
                pthread_mutex_lock(&m_serving_mutex);
                m_serving = false;
                pthread_mutex_unlock(&m_serving_mutex);
            }
        int handle_requests();

    private:
        struct in_addr m_addr;            //<! IPv4 address we bind to for requests
        unsigned short m_port;            //!< the port we listen on
        pthread_mutex_t m_serving_mutex;  //!< protection around m_serving flag for shutdown()
        bool m_serving;                   //!< are we still alive and serving requests?
        int m_fd;                         //!< our socket descriptor

#ifndef TEST
        HarpoonController *m_controller;  //!< handle to the HarpoonController object
#endif

        typedef void (HarpoonRemoteControl::* pMemMeth)(std::vector<std::string> &, std::ostringstream &);
        typedef std::map<std::string, pMemMeth>::const_iterator RpcMapCI;
        std::map<std::string, pMemMeth> m_method_map;

        void handle_request(int);
        int read_proto_line(int, char *, int);
        int read_proto_content(int, int, char *, int);
        void http_server_error(int, std::string &);
        void http_response_ok(std::ostringstream &);
        void write_to_client(int, std::ostringstream &);
        void http_post_method(int, std::istringstream *, int, std::string &);
        void http_put_method(int, int, std::string &);

        void xmlrpc_list_methods(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_null_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_query_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_unloadplugin_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_loadplugin_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_loadconfig_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_unloadconfig_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_stopplugin_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_startplugin_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_getstats_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_suicide_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_resetall_method(std::vector<std::string> &, std::ostringstream &);
        void xmlrpc_incrtime_method(std::vector<std::string> &, std::ostringstream &);
    };


    extern "C"
    {
        void *remote_control_entrypoint(void *); 
    }
}

#endif // __REMOTE_CONTROL_HH__
