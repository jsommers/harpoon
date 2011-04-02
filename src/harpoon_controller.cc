/*
 * $Id: harpoon_controller.cc,v 1.17 2006-08-07 12:18:32 jsommers Exp $
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


#include <cstring>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"
#include "harpoon_controller.hh"
#include "harpoon_plugin.hh"
#include "configurator.hh"
#include "remote_control.hh"
#include "harpoon_log.hh"
#include "xmlrpc_util.hh"


namespace Harpoon
{
    extern "C"
    {
        void *signal_trapper(void *v)
        {
            HarpoonController *hc = static_cast<HarpoonController *>(v);

            while (1)
            {
                sigset_t sset;
                sigemptyset(&sset);
                sigaddset(&sset, SIGINT);
                sigaddset(&sset, SIGTERM);
                sigaddset(&sset, SIGUSR1);
                sigaddset(&sset, SIGUSR2);
                sigaddset(&sset, SIGHUP);
                sigaddset(&sset, SIGHUP);
                sigaddset(&sset, SIGCHLD);
                sigaddset(&sset, SIGPIPE);
                sigaddset(&sset, SIGQUIT);

                int sig;
                int err = sigwait(&sset, &sig);
                if (!err)
                {
                    switch (sig)
                    {
                    case SIGINT:
                    case SIGTERM:
                        std::cerr << "going down in a ball of flames..." << std::endl;
                        hc->stop();
                        pthread_exit(0);
                        break;

                    case SIGUSR1:
                        std::cerr << "got SIGUSR1 - stopping all plugins" << std::endl;
                        hc->stopPlugins();
                        break;

                    case SIGUSR2:
                        std::cerr << "got SIGUSR2 - resetting all plugins" << std::endl;
                        hc->resetPlugins();
                    break;

                    default:
                        // ignore it
                        break; 
                    }
                }
                else
                {
                    std::cerr << "Fatal error in sigwait(): " << strerror(errno) << '/' << err << std::endl;
                    hc->stop();
                    pthread_exit(0);
                }
            }
            return (0);
        }


        void worker_thread_cleanup(void *v)
        {
            HarpoonWorkerThreadState *ws = static_cast<HarpoonWorkerThreadState *>(v);
            ws->m_plugin->shutdown();
            HarpoonPlugin *hp = ws->m_plugin;
            ws->m_plugin = 0;
            delete (hp);
        }


        void *worker_thread_loop(void *v)
        {
            HarpoonWorkerThreadState *ws = static_cast<HarpoonWorkerThreadState* >(v);

            int old_val = 0;

            // bad things could happen (lost state, lost resources)
            // if we allow arbitrary cancellations.  since we don't
            // really have control over what goes on inside the plugin
            // code, the best we can do is ask that sessions periodically
            // check whether the plugin has been shutdown (state != running)
            // and voluntarily exit.  setting up cancellation handlers
            // in a general way would be otherwise *really* hard.  the
            // cancellation(3THR) man page on solaris has helpful information.
            // also see jackie farrell's book (published by o'reilly - also
            // a former coworker of mine at DEC) for general pthreads
            // reference.
            pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &old_val);

            pthread_cleanup_push(worker_thread_cleanup, ws);

            assert (ws->m_hpp != plugin_personality_unknown);

            while (*ws->m_state == plugin_state_running ||
                   *ws->m_state == plugin_state_starting)
            {
                pthread_mutex_lock(ws->m_user_mutex);

                // block if we're running and enough user threads are
                // already running, or block if we are still starting up.
                while ((*ws->m_state == plugin_state_running &&
                        *(ws->m_current_active_sessions) >= 
                        *(ws->m_target_active_sessions)) ||
                       *ws->m_state == plugin_state_starting)
                {

                    // NB: timespec to pthread_cond_timedwait is *absolute*
                    // time.  set our timer to wake up in 5 seconds.
                    struct timespec ts = { time(NULL) + 5, 0 };
                    pthread_cond_timedwait(ws->m_user_cond, ws->m_user_mutex, &ts);
                }

                // could have exited above loop by state going to 
                // stopping or idle - check
                bool bail_out = (*ws->m_state != plugin_state_running);

                // increment active sessions only if we aren't bailing out
                if (!bail_out)
                    *(ws->m_current_active_sessions) = *(ws->m_current_active_sessions) + 1;
                pthread_mutex_unlock(ws->m_user_mutex);

                if (bail_out)
                    break;

                // check whether we've been canceled
                pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, &old_val);
                pthread_testcancel();
                pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &old_val);

                try
                {
                    if (ws->m_hpp == plugin_personality_client)
                        ws->m_plugin->client_session();    
                    else if (ws->m_hpp == plugin_personality_server)
                        ws->m_plugin->server_session();
                }
                catch (...)
                {
                    std::cerr << "thread " << pthread_self() 
                              << " died in a bad way" << std::endl;
                    abort();
                }

                // check whether we've been canceled
                pthread_setcancelstate (PTHREAD_CANCEL_ENABLE, &old_val);
                pthread_testcancel();
                pthread_setcancelstate (PTHREAD_CANCEL_DISABLE, &old_val);

                pthread_mutex_lock(ws->m_user_mutex);
                *(ws->m_current_active_sessions) = *(ws->m_current_active_sessions) - 1;
                pthread_cond_broadcast(ws->m_user_cond);
                pthread_mutex_unlock(ws->m_user_mutex);
            }

            pthread_cleanup_pop(1);
            return (0);
        }
    }


    int HarpoonController::createWorkerThread(HarpoonWorkerThreadState *wts)
    {
        return (pthread_create(wts->m_tid, NULL, worker_thread_loop, wts));
    }


    //
    // private method - no need to explicitly acquire m_control_mutex
    //
    bool HarpoonController::uninstallPlugin(HarpoonPluginConfig *hpc)
    {
        std::ostringstream ostr;
        std::string name;
        hpc->getName(name);

	std::string tmp;

        ostr << "uninstalling plugin " << name;
        m_log->log(7, ostr.str());
	ostr.str(tmp);

        PluginRuntimeState *ps = hpc->getPluginRuntimeState();
        if (ps && *ps->m_state == plugin_state_idle)
        {
#if !STATIC_PLUGINS
            if (dlclose(ps->m_objcode_handle))
            {
                ostr << "error unloading plugin: " << dlerror();
                m_log->log(1, ostr.str());
            }
#endif
            ps->m_objcode_handle = 0;
            ps->m_factory_function = 0;
            return true;
        }
        else
            m_log->log(1, "can't uninstall a non-idle plugin");
        return false;
    }



    //
    // private method - no need to explicitly acquire m_control_mutex
    //
    bool HarpoonController::installPlugin(HarpoonPluginConfig *hpc)
    {
        PluginRuntimeState *ps = hpc->getPluginRuntimeState();
        assert (ps);
        std::string mod_name;
        hpc->getObjFile(mod_name);

#if STATIC_PLUGINS
        ps->m_objcode_handle = 0; // no objcode_handle for static build
        plugin_generator fn_handle = 0;
 
        // find last period to strip extension
        std::string::size_type p = mod_name.find_last_of("."); 
        std::string plugin_name = mod_name.substr(0, p);

        if (plugin_name == "dummy_plugin")
            fn_handle = dummy_plugin_generator;
        else if (plugin_name == "infinite_tcp_plugin")
            fn_handle = infinite_tcp_plugin_generator;
        else if (plugin_name == "tcp_plugin")
            fn_handle = tcp_plugin_generator;
        else if (plugin_name == "udpcbr_plugin")
            fn_handle = udpcbr_plugin_generator;
        else if (plugin_name == "udpblast_plugin")
            fn_handle = udpblast_plugin_generator;
        else if (plugin_name == "udpexpo_plugin")
            fn_handle = udpexpo_plugin_generator;
        else if (plugin_name == "udpperiodic_plugin")
            fn_handle = udpperiodic_plugin_generator;
#else
        void *mod_handle = dlopen(mod_name.c_str(), RTLD_LAZY);
        if (!mod_handle) 
        {
            m_log->log(0, dlerror());
            ps->m_objcode_handle = 0;
            return false;
        }
        ps->m_objcode_handle = mod_handle;

#ifdef DARWIN_BROKEN_DLCOMPAT
        void *fn_handle = dlsym(mod_handle, "_factory_generator");
#else
        void *fn_handle = dlsym(mod_handle, "factory_generator");
#endif
#endif

        if (!fn_handle)
        {
#if STATIC_PLUGINS
            std::ostringstream ostr; 
            ostr << "Unable to find factory function for " << plugin_name << " (static build)";
            m_log->log(1, ostr.str());
#else
            m_log->log(1, dlerror());
#endif
            return false;
        }
        ps->m_factory_function = (HarpoonPlugin *(*)(void))fn_handle;
        return true;
    }



    bool HarpoonController::loadConfig(const std::string &config_file, 
                                       std::ostream &xmlrpc)
    {
        bool rv = false;
        std::map<std::string,HarpoonPluginConfig*> tmp_map;
        std::ostringstream ostr;
	std::string tmp;
        ostr << "loading config file " << config_file;
        m_log->log(7, ostr.str());
        ostr.str(tmp);
 
        pthread_mutex_lock(&m_control_mutex);

        if (!loadConfigFile(config_file, &tmp_map))
        {
            for (HpcMapCI iter = tmp_map.begin(); iter != tmp_map.end();
                 iter++)
            {
                HpcMapCI existing = m_cfg_map->find(iter->first);
                if (existing == m_cfg_map->end())
                {
                    m_cfg_map->insert(std::pair<std::string,HarpoonPluginConfig*>(iter->first, iter->second));
                    XmlRpcUtil::encode_struct_value(xmlrpc, "plugin", iter->first);
                }
            }
            ostr << "successfully loaded config file " << config_file << std::ends;
            m_log->log(1, ostr.str());
            rv = true;
        }
        else
        {
            ostr << "error loading config file " << config_file << std::ends;
            m_log->log(1, ostr.str());
            rv = false;
        }

        pthread_mutex_unlock(&m_control_mutex);
        return (rv);
    }



    bool HarpoonController::unloadConfig(const std::string &name)
    {
        std::ostringstream ostr;
	std::string tmp;
        ostr << "unloading plugin config " << name;
        m_log->log(7, ostr.str());
        ostr.str(tmp);

        CfgMapCI cfgiter = m_cfg_map->find(name);
        if (cfgiter == m_cfg_map->end())
            return (false);

        PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();
        if (!ps)
            return (false);

        pthread_mutex_lock(&m_control_mutex);

        bool rv = unloadPlugin(name, true);

        if (!rv)
        {  
            ostr << name << ": couldn't unload plugin.";
            m_log->log(1, ostr.str());
        }
        else
        {
            CfgMapCI inneriter = m_cfg_map->find(name);
            if (inneriter != m_cfg_map->end())
            {
                delete (inneriter->second);
                m_cfg_map->erase(name);

                ostr << name << ": successfully unloaded plugin config";
                m_log->log(1, ostr.str()); 
            }
            else
            {
                rv = false;
                ostr << name << ": couldn't find plugin to unload";
                m_log->log(1, ostr.str());
            }
        }
        pthread_mutex_unlock(&m_control_mutex);
        return rv;
    }



    bool HarpoonController::loadPlugin(const std::string &name, bool have_lock)
    {
        CfgMapCI cfgiter = m_cfg_map->find(name);

        bool rv = true;
	std::string tmp;
        std::ostringstream ostr;
        ostr << "loading plugin " << name;
        m_log->log(7, ostr.str());
        ostr.str(tmp);

        if (!have_lock)
            pthread_mutex_lock(&m_control_mutex);

        if (cfgiter != m_cfg_map->end())
        {
            PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();
            if (ps && *ps->m_state == plugin_state_idle)
            {
                if (uninstallPlugin(cfgiter->second) &&
                    installPlugin(cfgiter->second))
                {
                    ostr << name << ": successfully reloaded plugin";
                    m_log->log(1, ostr.str());
                    rv = true;
                }
                else
                {
                    ostr << name << ": failed to reload plugin";
                    m_log->log(1, ostr.str());
                    rv = false;
                }
            }
            else if (!ps)
            {
                // cfg exists, but no plugin state object.  create the plugin
                // state
                PluginRuntimeState *new_plugin = new PluginRuntimeState();
                cfgiter->second->setPluginRuntimeState(new_plugin);
                installPlugin(cfgiter->second);

                new_plugin->m_user_mutex = new pthread_mutex_t; 
                pthread_mutex_init(new_plugin->m_user_mutex, NULL);

                new_plugin->m_user_cond = new pthread_cond_t; 
                pthread_cond_init(new_plugin->m_user_cond, NULL);

                new_plugin->m_current_active_sessions = new int;
                *new_plugin->m_current_active_sessions = 0;
                new_plugin->m_target_active_sessions = new int;
                *new_plugin->m_target_active_sessions = 0;

                new_plugin->m_started = time(NULL);

                new_plugin->m_state = new HarpoonPluginState;
                *new_plugin->m_state = plugin_state_idle;

                new_plugin->m_interval_duration = m_warp_factor;

                ostr << name << ": successfully loaded plugin";
                m_log->log(1, ostr.str());

                rv = true;
            }
            else // plugin exists and is non-idle
            {
                ostr << name << ": plugin already loaded and non-idle";
                m_log->log(1, ostr.str());

                rv = false;
            }
        }
        else
        {
            rv = false;
            ostr << name << ": no such plugin exists to load";
            m_log->log(1, ostr.str());
        }

        if (!have_lock)
            pthread_mutex_unlock(&m_control_mutex);

        return rv;
    }



    bool HarpoonController::unloadPlugin(const std::string &name, bool have_lock)
    {
        std::ostringstream ostr;
	std::string tmp;
        ostr << "unloading plugin " << name;
        m_log->log(7, ostr.str());
        ostr.str(tmp);

        CfgMapCI cfgiter = m_cfg_map->find(name);
        if (cfgiter == m_cfg_map->end())
            return false;

        PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();

        bool rv = true;
        if (!have_lock)
            pthread_mutex_lock(&m_control_mutex);

        if (ps && *ps->m_state != plugin_state_idle)
        {
            m_log->log(1, "a plugin must be idle prior to unloading");
            rv = false;
        }

        if (uninstallPlugin(cfgiter->second))
        {
            delete (ps);
            cfgiter->second->setPluginRuntimeState(0);
            rv =  true;
        }
        else
            rv =  false;

        ostr << "unload plugin image status: " << (rv ? "ok" : "not ok");
        m_log->log(1, ostr.str());

        if (!have_lock)
            pthread_mutex_unlock(&m_control_mutex);
        return (rv);
    }



    void HarpoonController::listPlugins(std::ostream &xmlrpc)
    {
        m_log->log(7, "listPlugins called");

        pthread_mutex_lock(&m_control_mutex);

        for (CfgMapCI iter = m_cfg_map->begin(); iter != m_cfg_map->end(); ++iter)
        {
            PluginRuntimeState *ps = iter->second->getPluginRuntimeState();

            XmlRpcUtil::struct_begin(xmlrpc);
            XmlRpcUtil::encode_struct_value(xmlrpc, "plugin_name", iter->first);

            HarpoonPluginPersonality hpp;
            iter->second->getPersonality(hpp);
            XmlRpcUtil::encode_struct_value(xmlrpc, "personality",        
                                            HarpoonPluginConfig::getPluginPersonalityName(hpp));

            if (ps) 
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "state", HarpoonPluginConfig::getPluginStateName(*ps->m_state));
                XmlRpcUtil::encode_struct_value(xmlrpc, "uptime", (time(NULL) - ps->m_started));
            }
            else
            {
                XmlRpcUtil::encode_struct_value(xmlrpc, "state", "not loaded");
            }
            XmlRpcUtil::struct_end(xmlrpc);
        }

        pthread_mutex_unlock(&m_control_mutex);
    }



    bool HarpoonController::stopPlugins()
    {
        std::ostringstream ostr;

        pthread_mutex_lock(&m_control_mutex);

        ostr << "stopping all plugins: ";
        for (CfgMapCI cfgiter = m_cfg_map->begin();
             cfgiter != m_cfg_map->end(); 
             ++cfgiter)
        {
            bool stoprv = stopPlugin(cfgiter->first, true);
            ostr << cfgiter->first << ':' << (stoprv ? "success" : "failure")
                 << ' ';
        }
        pthread_mutex_unlock(&m_control_mutex);

        m_log->log(1, ostr.str());
        return (true);
    }



    bool HarpoonController::stopPlugin(const std::string &name, bool have_lock)
    {
        std::ostringstream ostr;
	std::string tmp;
        ostr << "stopping plugin " << name;
        m_log->log(7, ostr.str());
        ostr.str(tmp);

        CfgMapCI cfgiter = m_cfg_map->find(name);
       
        bool rv = true;
        if (!have_lock)
            pthread_mutex_lock(&m_control_mutex);
        if (cfgiter != m_cfg_map->end())
        {
            PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();
            if (ps && 
                (*ps->m_state == plugin_state_running ||
                 *ps->m_state == plugin_state_starting))
            {
                *ps->m_state = plugin_state_stopping;
                                
                // send cond broadcast
                pthread_mutex_lock(ps->m_user_mutex);
                pthread_cond_broadcast (ps->m_user_cond);
                pthread_mutex_unlock(ps->m_user_mutex);

                // cancel threads
                typedef std::vector<HarpoonWorkerThreadState *>::const_iterator WTSCI;
                for (WTSCI wtsiter = ps->m_threads.begin();
                     wtsiter != ps->m_threads.end(); ++wtsiter)
                {
                    HarpoonWorkerThreadState *wts = *wtsiter;
                    pthread_cancel(*wts->m_tid);
                }

                // join all the dead threads, clean/destroy all the
                // worker thread state blocks
                for (WTSCI wtsiter = ps->m_threads.begin();
                     wtsiter != ps->m_threads.end(); ++wtsiter)
                {
                    HarpoonWorkerThreadState *wts = *wtsiter;
                    void *thr_rv;
                    pthread_join(*wts->m_tid, &thr_rv);
                    if (wts->m_plugin)
                        delete (wts->m_plugin);
                    delete (wts);
                }
                ps->m_threads.clear();

                // set current sessions/target sessions to 0
                *ps->m_current_active_sessions = 0;
                *ps->m_target_active_sessions = 0;

                // set state to idle 
                *ps->m_state = plugin_state_idle;
            }
        }
        else
        {
            // stopPlugin attempted on a non-existent plugin -- that's ok.
            // silently fail
            rv = true;
        }
        if (!have_lock)
            pthread_mutex_unlock(&m_control_mutex);

        ostr << name << ": plugin stopped - threads killed and reaped";
        m_log->log(0, ostr.str());

        return rv;
    }


   
    bool HarpoonController::startPlugin(const std::string &name, bool have_lock)
    {
        CfgMapCI cfgiter = m_cfg_map->find(name);
        PluginRuntimeState *ps = 0;
        bool rv = true;
        int n_threads = 0;

        bool newly_created = false;

        std::ostringstream ostr;
	std::string tmp;
        ostr << "starting plugin " << name;
        m_log->log(7, ostr.str());
        ostr.str(tmp);

        if (!have_lock)
            pthread_mutex_lock(&m_control_mutex);

        if (cfgiter != m_cfg_map->end())
        {
            ps = cfgiter->second->getPluginRuntimeState();

            if (ps && *ps->m_state != plugin_state_idle)
            {
                ostr << name << ": can't start a non-idle plugin" << std::ends;
                m_log->log(0, ostr.str());
                ostr.str(tmp);
                rv = false;
            }
        }
        else
        {
            ostr << name << ": can't start non-existent plugin" << std::ends;
            m_log->log(0, ostr.str());
            ostr.str(tmp);
            rv = false;
        }


        if (rv && !ps)
        {
            ps = new PluginRuntimeState();
            cfgiter->second->setPluginRuntimeState(ps);
            newly_created = true;

            ostr << name << ": no plugin state existed on start - created" << std::ends;
            m_log->log(2, ostr.str());
            ostr.str(tmp);
        }


        if (rv && ps && ps->m_factory_function == 0)
        {
            // now we have a pointer to a plugin that's idle and ready
            // to start
            if (!installPlugin(cfgiter->second))
            {
                ostr << name << ": failure installing plugin on start" 
                     << std::ends;
                m_log->log(0, ostr.str());
                ostr.str(tmp);

                delete (ps);
                cfgiter->second->setPluginRuntimeState(0);
                rv = false;
            }
        }
        

        if (rv && ps)
        {
            ps->m_state = new HarpoonPluginState;
            *ps->m_state = plugin_state_starting;
            ps->m_started = time(NULL);
            ps->m_user_mutex = new pthread_mutex_t;
            pthread_mutex_init(ps->m_user_mutex, NULL);
            ps->m_user_cond = new pthread_cond_t;
            pthread_cond_init(ps->m_user_cond, NULL);

            ps->m_current_active_sessions = new int;
            ps->m_target_active_sessions = new int;

            *ps->m_current_active_sessions = 0;
            *ps->m_target_active_sessions = cfgiter->second->get_target_active_sessions(m_interval);

            ps->m_interval_duration = m_warp_factor;

            cfgiter->second->getMaxThreads(n_threads);

            for (int i = 0; i < n_threads; ++i)
            {            
                HarpoonWorkerThreadState *wts = new HarpoonWorkerThreadState();

                wts->m_plugin = ps->m_factory_function();

                wts->m_controller = this;
                wts->m_state = ps->m_state;
                cfgiter->second->getPersonality(wts->m_hpp);
                wts->m_user_mutex = ps->m_user_mutex;
                wts->m_user_cond = ps->m_user_cond;
                wts->m_current_active_sessions = ps->m_current_active_sessions;
                wts->m_target_active_sessions = ps->m_target_active_sessions;
                wts->m_tid = new pthread_t;

                wts->m_plugin->init(cfgiter->second, m_log);

                if (!createWorkerThread(wts))
                    ps->m_threads.push_back(wts); 
                else
                {
                    ostr << "failure creating worker thread" << std::ends;
                    m_log->log(0, ostr.str());
                    ostr.str(tmp);

                    delete (wts); 
                    rv = false;
                }
            }

            // release the hounds..
            pthread_mutex_lock(ps->m_user_mutex);
            *ps->m_state = plugin_state_running;
            pthread_cond_broadcast(ps->m_user_cond);
            pthread_mutex_unlock(ps->m_user_mutex);
        }

        if (!have_lock)
            pthread_mutex_unlock(&m_control_mutex);

        if (rv)
        {
            ostr << name << ": started plugin with " << n_threads << " threads."
                 << std::ends;
            m_log->log(1, ostr.str());
        }

        return rv;
    }



    void HarpoonController::getPluginStats(std::ostream &xmlrpc)
    {
        m_log->log(7, "sending plugin stats");

        pthread_mutex_lock(&m_control_mutex);

        // system-wide statistics 
        XmlRpcUtil::struct_begin(xmlrpc);
        XmlRpcUtil::encode_struct_value(xmlrpc, "emulation_interval", m_interval);
        XmlRpcUtil::struct_end(xmlrpc);
       
        for (CfgMapCI cfgiter = m_cfg_map->begin();
             cfgiter != m_cfg_map->end();
             ++cfgiter)
        {
            PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();
            XmlRpcUtil::struct_begin(xmlrpc);
            XmlRpcUtil::encode_struct_value(xmlrpc, "plugin_name", cfgiter->first);
            HarpoonPluginPersonality hpp;
            cfgiter->second->getPersonality(hpp);
            XmlRpcUtil::encode_struct_value(xmlrpc, "personality",        
                                            HarpoonPluginConfig::getPluginPersonalityName(hpp));

            if (!ps)
	    {
                XmlRpcUtil::encode_struct_value(xmlrpc, "state", "not loaded");
                XmlRpcUtil::struct_end(xmlrpc);
    	        continue;
            }

            XmlRpcUtil::encode_struct_value(xmlrpc, "target_threads", *ps->m_target_active_sessions);
            XmlRpcUtil::encode_struct_value(xmlrpc, "active_threads", *ps->m_current_active_sessions);
            XmlRpcUtil::encode_struct_value(xmlrpc, "uptime", (time(NULL) - ps->m_started));
            XmlRpcUtil::encode_struct_value(xmlrpc, "state", HarpoonPluginConfig::getPluginStateName(*ps->m_state));
            
            if (ps->m_threads.size())
            {
                // since stats is a static method and we don't really know
                // the name of any derived class of HarpoonPlugin, we need 
                // to get just one reference to an object 
                HarpoonPlugin *obj_handle = (ps->m_threads[0])->m_plugin;
                obj_handle->stats(xmlrpc);
            }

            XmlRpcUtil::struct_end(xmlrpc);
        }

        pthread_mutex_unlock(&m_control_mutex);
    }



    bool HarpoonController::resetPlugins()
    {
        std::ostringstream ostr;
        ostr << "<stopping plugins: ";

        pthread_mutex_lock(&m_control_mutex);

        for (CfgMapCI cfgiter = m_cfg_map->begin();
             cfgiter != m_cfg_map->end(); 
             ++cfgiter)
        {
            bool rv = stopPlugin(cfgiter->first, true);
            ostr << cfgiter->first << ":" << (rv ? "ok" : "!ok") << ' ';
        }

        // reset the emulated interval
        m_interval = 0;
        m_last_reset = time(NULL); 
        srandom(m_seed);

        ostr << "><starting plugins: ";
        for (CfgMapCI cfgiter = m_cfg_map->begin();
             cfgiter != m_cfg_map->end(); 
             ++cfgiter)
        {
            bool rv = startPlugin(cfgiter->first, true);
            ostr << cfgiter->first << ":" << (rv ? "ok" : "!ok") << ' ';
        }

        pthread_mutex_unlock(&m_control_mutex);

        ostr << ">";
        m_log->log(1, ostr.str()); 
        return true;
    }


    int HarpoonController::incrTime()
    {
        if (m_autoincr_time)
            return -1;

        pthread_mutex_lock(&m_control_mutex);
        m_interval++;
        pthread_mutex_unlock(&m_control_mutex);
        timeChangeNotify();
        return (m_interval);
    }

    void HarpoonController::timeChangeNotify()
    {
        // when interval changes, reset target thread counts for each plugin
        // send cond broadcast

        for (CfgMapCI cfgiter = m_cfg_map->begin();
             cfgiter != m_cfg_map->end();
             ++cfgiter)
        {
            PluginRuntimeState *ps = cfgiter->second->getPluginRuntimeState();
            if (!ps)
                continue;

            if (*ps->m_state != plugin_state_running)
                continue;

            int target_threads = cfgiter->second->get_target_active_sessions(m_interval);
            int old_active = *ps->m_target_active_sessions;

            if (!m_continuous_run)
            {
               if (m_interval >= cfgiter->second->get_max_epoch())
                   target_threads = 0;
            }

            pthread_mutex_lock(ps->m_user_mutex); 
            *ps->m_target_active_sessions = target_threads;
            pthread_cond_broadcast(ps->m_user_cond);
            pthread_mutex_unlock(ps->m_user_mutex); 

            HarpoonPlugin *obj_handle = (ps->m_threads[0])->m_plugin;
            obj_handle->tick(m_interval);

            std::ostringstream ostr;
            ostr << "Changing target threads for " << cfgiter->first
                 << " at emulation time " << std::setfill('0') 
                 << std::setw(3) << m_interval << ".00" 
                 << " to " << target_threads << ". (Previously "
                 << old_active << ".)" << std::endl;
            m_log->log(0, ostr.str());
        }
    }

    void HarpoonController::run()
    {
        m_running = true; 
        m_interval = 0;

        sigset_t sset;
        sigemptyset(&sset);
        sigaddset(&sset, SIGINT);
        sigaddset(&sset, SIGTERM);
        sigaddset(&sset, SIGUSR1);
        sigaddset(&sset, SIGUSR2);
        sigaddset(&sset, SIGHUP);
        sigaddset(&sset, SIGHUP);
        sigaddset(&sset, SIGCHLD);
        sigaddset(&sset, SIGPIPE);
        sigaddset(&sset, SIGQUIT);
        if (sigprocmask(SIG_BLOCK, &sset, 0))  
        {
            std::cerr << "Error in sigprocmask(): " << strerror(errno) << '/' << errno << std::endl;
            exit(-1);
        }


        pthread_t signal_thread;
        if (pthread_create(&signal_thread, NULL, signal_trapper, this))
        {
            std::cerr << "Error in creating signal handler thread: " << strerror(errno)
                      << '/' << errno << std::endl;
            exit (-1);
        }


        pthread_t hrc_thread;
        HarpoonRemoteControl *rc = new HarpoonRemoteControl(m_control_addr, m_control_port);
        rc->setController(this);
        if (pthread_create(&hrc_thread, NULL, Harpoon::remote_control_entrypoint, rc))
        {
            std::cerr << "Error in creating HTTP/XML-RPC listener thread: " << strerror(errno)
                      << '/' << errno << std::endl;
            exit (-1);
        }

        resetPlugins(); // load, start any plugins registered on command line

        std::ostringstream ostr;
        ostr << "harpoon started.  verbosity<" << m_verbose << ">warp_factor<" << m_warp_factor << ">autoincr?<" << m_autoincr_time << ">continuousrun?<" << m_continuous_run << ">";
        m_log->log(3, ostr.str());

        while (m_running)
        {
            time_t last_status = m_last_reset;
            time_t curr_time = time(NULL);
            time_t next_tick = curr_time + ((curr_time - m_last_reset) % m_warp_factor);
            
            if (m_autoincr_time && 
                curr_time > m_last_reset &&
                curr_time >= next_tick)
            {
                pthread_mutex_lock(&m_control_mutex);
                m_interval++;
                pthread_mutex_unlock(&m_control_mutex);

                timeChangeNotify();
            }


            // write status to log every 10 seconds
            if (((last_status - curr_time) % 10) == 0)
            {
                int frac = 0;
                if (m_autoincr_time)
                    frac = int(((curr_time-m_last_reset) % m_warp_factor) / double(m_warp_factor) * 100.0);

                std::string junk;
                ostr.str(junk);
                ostr << std::setfill('0') << std::setw(3) << m_interval
                     << '.' << std::setfill('0') << std::setw(2) << frac
                     << " - emulation time tick";
                m_log->log(5, ostr.str());
                last_status = curr_time;
            }

            sleep(1);
        }
        rc->shutdown();

        stopPlugins();
        void *rv = 0;
        pthread_cancel(signal_thread);
        pthread_join(signal_thread, &rv);
        pthread_cancel(hrc_thread);
        pthread_join(hrc_thread, &rv);
    }
}

