/*
 * $Id: configurator.hh,v 1.8 2006-03-24 23:02:00 jsommers Exp $
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


#ifndef __CONFIGURATOR_HH__
#define __CONFIGURATOR_HH__

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <deque>
#include <map>
#include <pthread.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>

#include "address_pool.hh"
#include "config.h"
#include "expat.h"


#define DEFAULT_SERVER_PORT 10000


namespace Harpoon
{
    // forward declarations
    class HarpoonPluginConfig; 
    class HarpoonPlugin;
    struct HarpoonWorkerThreadState;




    /*
     * structure to maintain some state while mucking through the xml
     */
    struct ConfigLoadContext
    {   
        ConfigLoadContext(std::map<std::string, 
                          Harpoon::HarpoonPluginConfig *> *cm) :
            depth(0), parse_state_good(true), numeric_data(true),
            hpc(0), config_map(cm) 
        {
            pool_name = "";
        }

        int depth;
        bool parse_state_good;
        std::string data_collect_state;

        std::deque<std::string> tag_stack;
        std::deque<XML_CharacterDataHandler> handler_stack;
        std::deque<XML_Parser> parser_stack;

        bool numeric_data;
        std::string pool_name;

        Harpoon::HarpoonPluginConfig *hpc;
        std::map<std::string, Harpoon::HarpoonPluginConfig *> *config_map;
    };


    enum HarpoonPluginPersonality { plugin_personality_unknown = 0,
                                    plugin_personality_client = 1, 
                                    plugin_personality_server = 2 };


    enum HarpoonPluginState { plugin_state_idle = 0,
                              plugin_state_running = 1,
                              plugin_state_starting = 2,
                              plugin_state_stopping = 3 };


   class SharedPluginState
    {
    public:
        SharedPluginState() : m_ref_count(0)
            {
                pthread_mutex_init(&m_ref_mutex, NULL);
            }
        virtual ~SharedPluginState()
            {
                pthread_mutex_destroy(&m_ref_mutex);
            }
        void incrRefCount()
            {
                pthread_mutex_lock(&m_ref_mutex);
                m_ref_count++;
                pthread_mutex_unlock(&m_ref_mutex);
            }
       void decrRefCount()
            {
                pthread_mutex_lock(&m_ref_mutex);
                m_ref_count--;
                pthread_mutex_unlock(&m_ref_mutex);
            }
        int getRefCount()
            {
                pthread_mutex_lock(&m_ref_mutex);
                int rv = m_ref_count;
                pthread_mutex_unlock(&m_ref_mutex);
                return (rv);
            }
        bool isRefCountZero()
            {
                pthread_mutex_lock(&m_ref_mutex);
                bool rv = (m_ref_count == 0);
                pthread_mutex_unlock(&m_ref_mutex);
                return rv;
            }

        
    protected:
        int m_ref_count;
        pthread_mutex_t m_ref_mutex;
      
    };



    struct PluginRuntimeState
    {
        PluginRuntimeState() : m_objcode_handle(0),
                               m_factory_function(0),
                               m_user_mutex(0),
                               m_user_cond(0),
                               m_current_active_sessions(0),
                               m_target_active_sessions(0),
                               m_state(0),
                               m_started(0),
                               m_interval_duration(0),
                               m_plugin_private(0) {}
        ~PluginRuntimeState()
            {
#if !STATIC_PLUGINS
                if (m_objcode_handle)
                    dlclose(m_objcode_handle);
#endif
                if (m_user_mutex)
                {
                    pthread_mutex_destroy(m_user_mutex);
                    delete m_user_mutex;
                }
                if (m_user_cond)
                {
                    pthread_cond_destroy(m_user_cond);
                    delete m_user_cond;
                }
                if (m_current_active_sessions)
                    delete m_current_active_sessions;
                if (m_target_active_sessions)
                    delete m_target_active_sessions;
                if (m_state)
                    delete m_state;
                if (m_plugin_private)
                    delete m_plugin_private;
            }
             
        void *m_objcode_handle;
        HarpoonPlugin *(*m_factory_function)(void);
        pthread_mutex_t *m_user_mutex;
        pthread_cond_t *m_user_cond;
        int *m_current_active_sessions;
        int *m_target_active_sessions;
        std::vector<HarpoonWorkerThreadState *> m_threads;
        HarpoonPluginState *m_state;
        time_t m_started;
        int m_interval_duration;
        SharedPluginState *m_plugin_private;
    };


    class HarpoonPluginConfig
    {
    public:
        HarpoonPluginConfig() : m_max_threads(0), 
                                m_personality(plugin_personality_unknown),
                                m_plugin_runtime_state(0)
            {
                m_distributions.clear();
                m_properties.clear();
                m_name = "unknown";
                m_objfile_name = "unknown";
            }
        ~HarpoonPluginConfig() 
            { 
                for (std::map<std::string,std::vector<float>*>::iterator fmiter = m_distributions.begin();
                     fmiter != m_distributions.end(); fmiter++)
                {
                    std::vector<float> *vec = fmiter->second;
                    delete vec;
                }

                for (std::map<std::string,std::vector<std::string>*>::iterator smiter = m_properties.begin();
                     smiter != m_properties.end(); smiter++)
                {
                    std::vector<std::string> *vec = smiter->second;
                    delete vec;
                }

                if (m_plugin_runtime_state)
                    delete (m_plugin_runtime_state);
            }

        int get_max_epoch() const
            {
                std::vector<float> *active_sessions = getNumericDistribution("active_sessions");
                assert (active_sessions);
                return active_sessions->size();
            }

        int get_target_active_sessions(unsigned int interval) const
            {
                std::vector<float> *active_sessions = getNumericDistribution("active_sessions");
                assert (active_sessions);
                return int(((*active_sessions)[interval % active_sessions->size()]));
            }

        void getAddress(const std::string &address_type,
                        in_addr &ina, unsigned short &port)
            {
                if (address_type == "client_destination_pool")
                {
                    m_client_dst_pool.next(ina, port);
                }
                else if (address_type == "client_source_pool")
                {
                    m_client_src_pool.next(ina, port);
                }
                else if (address_type == "server_pool")
                {
                    m_server_pool.next(ina, port);
                }
                else
                {
                    assert(0);
                }
            }

        std::vector<float> *getNumericDistribution(std::string name) const
            {
                std::map<std::string,std::vector<float>*>::const_iterator uditer = m_distributions.find(name);
                if (uditer != m_distributions.end())
                    return (uditer->second);
                else
                    return 0;
            }

        std::vector<std::string> *getPropertyList(std::string name) const
            {
                std::map<std::string,std::vector<std::string>*>::const_iterator uditer = m_properties.find(name);
                if (uditer != m_properties.end())
                    return (uditer->second);
                else
                    return 0;
            }

        void setName(std::string &name) { m_name = name; }
        void getName(std::string &name) const { name = m_name; }
        void setObjFile(std::string &objfile) { m_objfile_name = objfile; }
        void getObjFile(std::string &objfile) const { objfile = m_objfile_name; }
        void setMaxThreads(int mt) { m_max_threads = mt; }
        void getMaxThreads(int &mt) const { mt = m_max_threads; }
        void setPersonality(HarpoonPluginPersonality &p) { m_personality = p; }
        void getPersonality(HarpoonPluginPersonality &p) const { p = m_personality; }

        void loadDistributionDataHelper(ConfigLoadContext *, 
                                        const XML_Char *,
                                        int);

        bool loadConfigDataStartTagHelper(ConfigLoadContext *, 
                                          const char *,
                                          const char **);

        void dumpConfig(std::ostream &);
        void setPluginPrivateData(SharedPluginState *sps) 
            { 
                m_plugin_runtime_state->m_plugin_private = sps;
            }
        SharedPluginState *getPluginPrivateData() const 
            { 
                return m_plugin_runtime_state->m_plugin_private; 
            }
        void setPluginRuntimeState(PluginRuntimeState *p)
            {
                m_plugin_runtime_state = p;
            }
        PluginRuntimeState *getPluginRuntimeState() const { return m_plugin_runtime_state; }
     
        static const char *getPluginPersonalityName(HarpoonPluginPersonality hpp)
            {
                if (hpp < 0 || hpp > 2)
                    return (m_personality_names[0]);
                else
                    return (m_personality_names[hpp]);
            }

        static const char *getPluginStateName(HarpoonPluginState hps)
            {
                if (hps < 0 || hps > 2)
                    return ("unknown");
                else
                    return (m_plugin_state_names[hps]);
            }

    private:
        std::map<std::string,std::vector<float>*> m_distributions;
        std::map<std::string,std::vector<std::string>*> m_properties;

        AddressPool m_client_src_pool;
        AddressPool m_client_dst_pool;
        AddressPool m_server_pool;
   
        std::string m_name;
        std::string m_objfile_name;
        unsigned int m_max_threads;
        HarpoonPluginPersonality m_personality;

        PluginRuntimeState *m_plugin_runtime_state;

        static const char *m_personality_names[3];
        static const char *m_plugin_state_names[4];
    };


    typedef std::map<std::string, HarpoonPluginConfig *>::const_iterator HpcMapCI;
    int loadConfigFile(const std::string &,  
                       std::map<std::string, HarpoonPluginConfig*> *);

}


#endif // __PROTO_DISTRIBUTION_HH__
