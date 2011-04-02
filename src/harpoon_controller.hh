/*
 * $Id: harpoon_controller.hh,v 1.11 2006-08-07 12:18:32 jsommers Exp $
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


#ifndef __HARPOON_CONTROLLER_HH__
#define __HARPOON_CONTROLLER_HH__

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <vector>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "configurator.hh"
#include "harpoon_plugin.hh"
#include "harpoon_log.hh"
#include "config.h"


#if STATIC_PLUGINS
typedef Harpoon::HarpoonPlugin*(*plugin_generator)(void);
extern "C"
{
    Harpoon::HarpoonPlugin *dummy_plugin_generator(void);
    Harpoon::HarpoonPlugin *tcp_plugin_generator(void);
    Harpoon::HarpoonPlugin *infinite_tcp_plugin_generator(void);
    Harpoon::HarpoonPlugin *udpcbr_plugin_generator(void);
    Harpoon::HarpoonPlugin *udpblast_plugin_generator(void);
    Harpoon::HarpoonPlugin *udpexpo_plugin_generator(void);
    Harpoon::HarpoonPlugin *udpperiodic_plugin_generator(void);
}
#endif

namespace Harpoon
{
    class HarpoonController;

    /*!
     * state kept for each plugin thread.  none of this should be accessible
     * to the plugin itself - only the thread control code that calls the
     * user code.  we are just referencing pointers/objects that are owned
     * elsewhere, so no explicit destructor is needed.
     */
    struct HarpoonWorkerThreadState
    {
        HarpoonWorkerThreadState() : m_plugin(0), m_controller(0), m_state(0),
                                     m_hpp(plugin_personality_unknown),
                                     m_user_mutex(0), m_user_cond(0), m_tid(0),
                                     m_current_active_sessions(0),
                                     m_target_active_sessions(0) {}
        HarpoonPlugin *m_plugin;         //!< pointer to plugin object
        HarpoonController *m_controller; //!< pointer to our controller
        HarpoonPluginState *m_state;     //!< state (running, stopping, etc.)
        HarpoonPluginPersonality m_hpp;  //!< client or server
        pthread_mutex_t *m_user_mutex;   //!< mutex used to control access to condition variable
        pthread_cond_t *m_user_cond;     //!< condition variable controlling number of threads active
        pthread_t *m_tid;                //!< our thread id
        int *m_current_active_sessions;  //!< number of current active threads
        int *m_target_active_sessions;   //!< number of threads that should be active
    };


    class HarpoonController
    {
    public:
        HarpoonController(std::string log_name = "") : 
                              m_warp_factor(60),
                              m_verbose(0),
                              m_interval(0),
                              m_continuous_run(true),
                              m_control_port(8180),
                              m_running(true),
                              m_autoincr_time(true),
                              m_cfg_map(0)
            {
                pthread_mutex_init(&m_control_mutex, NULL);
                m_control_addr.s_addr = INADDR_ANY;

                m_cfg_map = new std::map<std::string,HarpoonPluginConfig *>();

                if (log_name != "")
                {

#if __GNUG__ == 2 
                    m_logfile = new std::ofstream(log_name.c_str(), std::ios::app);
#else
                    m_logfile = new std::ofstream(log_name.c_str(), std::ios_base::app);
#endif

                    if (!m_logfile || !m_logfile->good())
                    {
                        m_logfile = &std::cerr;
                        std::cerr << "failed to open logfile " << log_name << ".  using stderr." << std::endl;
                    }
                }
                else
                {
                    m_logfile = &std::cerr;
                }
                m_log = new HarpoonLog(m_logfile, m_verbose);
            }
        virtual ~HarpoonController() 
	    { 
                delete (m_logfile);
	        delete (m_log);
                pthread_mutex_destroy(&m_control_mutex);
		for (CfgMapCI iter = m_cfg_map->begin(); 
		     iter != m_cfg_map->end();
		     iter++)
		{     
		    HarpoonPluginConfig *hpc = iter->second;
		    delete (hpc);
		}     
                delete (m_cfg_map);
	    }

        void setConfig(std::map<std::string, HarpoonPluginConfig*> *cfg)
            {
                m_cfg_map = cfg;
            }
        void setVerbosity(int v) { m_verbose = v; m_log->setVerbosity(v); }
        void setContinuousRun(bool b) { m_continuous_run = b; }
        void setControlAddr(struct in_addr &iaddr) { m_control_addr = iaddr; }
        void setControlPort(unsigned short p) { m_control_port = p; }
        void setAutoincrTime(bool b) { m_autoincr_time = b; }
        void setWarpFactor(int wf) { m_warp_factor = wf; }
        void setSeed(unsigned long s) { m_seed = s; }
    
        void run();
        void stop() { kill(getpid(), SIGINT); m_running = false; }

        /*!
         * parses and loads the named XML config file.
         * does *not* reload an existing configuration.  first stop
         * those plugins then unload prior to reloading a new configuration.
         *
         * @param fname the file name of the configuration to load.
         * @param os a stream to write names of plugins loaded an XML structure
         */
        bool loadConfig(const std::string &fname, std::ostream &os);

        /*!
         * unloads an existing configuration.  the plugin must already
         * be stopped for unloadConfig to succeed.
         *
         * @param name the plugin name of the configuration to dump.
         */
        bool unloadConfig(const std::string &name);

        /*!
         * loads a plugin.  the config must already be loaded.
         *
         * @param name the name of the plugin
         */
        bool loadPlugin(const std::string &name, bool have_lock = false);

        /*!
         * unloadPlugin() is different than unloadConfig() in that the
         * configuration remains in tact, but the object code representing the plugin
         * is dumped.  once the plugin is unloaded, it must subsequently be loaded
         * or destroyed.
         *
         * @param name the name of the plugin to unload.
         */
        bool unloadPlugin(const std::string &name, bool have_lock = false);
        
        /*!
         * writes an XML-structured list of plugins to the given output
         * stream.
         *
         * @param os an output stream to write plugin list.
         */
        void listPlugins(std::ostream &os);

        /*!
         * stops all worker threads for the named plugin.  sets
         * the state of the plugin to idle.
         *
         * @param name name of plugin
         */
        bool stopPlugin(const std::string &, bool have_lock = false);
        bool stopPlugins(void);


        /*!
         * starts worker threads for the named plugin.
         *
         * @param name name of plugin
         */
        bool startPlugin(const std::string &, bool have_lock = false);

        /*!
         * writes an XML-structured set of plugin stats to the given 
         * output stream.
         * 
         * @param os an output stream on which to write stats.
         */
        void getPluginStats(std::ostream &);

        /*!
         * performs a stopPlugin() to all loaded plugins, resets the
         * "interval" counter to 0 (i.e., resets the internal harpoon clock) and
         * then performs a startPlugin() to all plugins.  a convenience method
         * with the side-effect of resetting the harpoon clock.
         */
        bool resetPlugins();

        /*!
         * increment emulation time.  this XML-RPC interface should be called
         * in conjunction with the '-a' option to Harpoon, which indicates that
         * the user wishes to *not* have time automatically increment.  if
         * the '-a' option is not set from the command line, a call to this
         * function has no effect.
         * @return current emulation time
         */
        int incrTime(); 


    private:
        int m_warp_factor; /*!< number of seconds in an emulation epoch */
        int m_verbose;     /*!< verbosity level for diagnostics */
        int m_interval;        /*!< current emulation epoch */
        time_t m_last_reset;  /*!< time of most recent plugin reset */
        bool m_continuous_run; /*!< whether to stop when m_interval = max plugin epoch */
        struct in_addr m_control_addr; /*!< bind address used by XML-RPC */
        unsigned short m_control_port; /*!< port number used by XML-RPC */
        bool m_running;    /*!< flag to keep main thread running or not.
                            *   can be reset by an XML-RPC call or signal */
        bool m_autoincr_time; /*!< whether to automatically increment emulated time or do it manually via XML-RPC */
        unsigned long m_seed; /*!< rng seed; save for resets so that we induce same rng sequence with each reset */


        std::map<std::string, HarpoonPluginConfig *> *m_cfg_map;
        typedef std::map<std::string, HarpoonPluginConfig *>::const_iterator CfgMapCI;

        pthread_mutex_t m_control_mutex;
        HarpoonLog *m_log;
        std::ostream *m_logfile;

        int createWorkerThread(HarpoonWorkerThreadState *);
        bool installPlugin(HarpoonPluginConfig *);
        bool uninstallPlugin(HarpoonPluginConfig *);

        
        /*!
         * called when time automatically (from within main controller loop) 
         * or manually (from XML-RPC call) is incremented.  causes all worker
         * threads to get kicked with updated time.
         */
        void timeChangeNotify();
    };
}

#endif // __HARPOON_CONTROLLER_HH__
