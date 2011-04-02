/* 
 * $Id: harpoon_plugin.hh,v 1.6 2005-11-07 03:40:15 jsommers Exp $
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


#ifndef __HARPOON_PLUGIN_HH__
#define __HARPOON_PLUGIN_HH__

#include <pthread.h>
#include "config.h"
#include "configurator.hh"
#include "harpoon_log.hh"


#define timerdiff(tvp, uvp, vvp)                                        \
	do {                                                            \
                (vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;          \
                (vvp)->tv_usec = (tvp)->tv_usec - (uvp)->tv_usec;       \
                if ((vvp)->tv_usec < 0) {                               \
                       	(vvp)->tv_sec--;                                \
               		(vvp)->tv_usec += 1000000;                      \
       		}                                                       \
	} while (0)


#ifndef timeradd
#define timeradd(tvp, uvp, vvp)                                         \
        do {                                                            \
                (vvp)->tv_sec = (tvp)->tv_sec + (uvp)->tv_sec;          \
                (vvp)->tv_usec = (tvp)->tv_usec + (uvp)->tv_usec;       \
                if ((vvp)->tv_usec >= 1000000) {                        \
                        (vvp)->tv_sec++;                                \
                        (vvp)->tv_usec -= 1000000;                      \
                }                                                       \
        } while (0)
#endif

namespace Harpoon
{
    inline void timeraddfloat(struct timeval &tv, float f)
    {
         int sec = int(f);
         int usec = int((f - float(sec)) * 1000000.0);
         tv.tv_sec += sec;
         tv.tv_usec += usec;
         if (tv.tv_usec >= 1000000)
         {
              tv.tv_sec++;
              tv.tv_usec -= 1000000;
         }
    }


    /*! @class HarpoonPlugin
     *
     *  HarpoonPlugin is the base class for all loadable Harpoon
     *  plugin modules.  you must implement the methods client_session() and
     *  server_session() in any derived class.  you may also implement init()
     *  and shutdown() methods.  see the sample DummyPlugin source code
     *  for a simple example, or TcpPlugin code for a more complicated
     *  example.
     */ 
    class HarpoonPlugin
    {
    public:
        // avoid doing anything fancy here since we can't
        // consistently pass information into the factory
        // function
        HarpoonPlugin() : m_config(0) {} 
        virtual ~HarpoonPlugin() {}


        /*! 
         *   init() is called when a plugin is first started.  it is
         *   important to note that it is called in a \b single \b threaded
         *   fashion.  that is, you need not worry about race conditions inside 
         *   this method so you can allocate any private plugin data for access 
         *   by plugin threads that will start later.
         *
         *  @param hpc the config object used by this plugin.
         */
        virtual bool init(HarpoonPluginConfig *hpc, HarpoonLog *hlog)
            {
                m_config = hpc;
                m_log = hlog;
                return true;
            }

        /*! 
         *   tick() is called every time the interval is increased in the
         *   main harpoon controller.  any periodic housekeeping can be
         *   done here.  note that this method is only called on *one* 
         *   worker thread object.
         */
        virtual void tick(int interval) { return; }


        //
        // gotta override these to implement the Harpoon "session"-level 
        // functionality of both client and server endpoints.  shutdown() 
        // can get called asynchronously by manager threads to allow plugin 
        // code to cleanup properly.
        //

        //! method implementing client side of Harpoon user abstraction
        virtual void client_session() = 0;
        
        //! method implementing server side of Harpoon user abstraction
        virtual void server_session() = 0;

        /*!
         * method called on death of the plugin thread.  this method is 
         * called for every thread.
         */
        virtual void shutdown() {}

        /*!
         * method called by controller to obtain stats from plugin.  note
         * carefully that this method is only called for \b one \b object
         * per plugin.  this may not be the best way to obtain statistics,
         * but a static stats() method would also not work because we
         * want to differentiate between clients and servers when generating
         * statistics.  one way to get around these problems is to have
         * static statistics counters and static methods for getting client
         * and server statistics, then from this method calling the appropriate
         * static method.  at least that's how the TCPPlugin does it.
         */
        virtual void stats(std::ostream &) = 0;


    protected:

        /*
         * a set of utility methods available to client and server 
         * implementors.  
         */
        //! get next (random) address from the named pool
        // @param address_type string tag to specify which address pool
        //  to get address from ('server_pool', 'client_source_pool',
        //  or 'client_destination_pool'
        // @ina address to fill in
        // @port port to fill in
        void getAddress(const std::string &address_type, 
                        in_addr &ina, unsigned short &port) const
            {
                m_config->getAddress(address_type, ina, port);
            }

        void getAddress(const char *pool_name,
                        in_addr &ina, unsigned short &port) const
            {
                std::string atype = pool_name;
                this->getAddress(atype, ina, port);
            }

        //! voluntarily check whether we should exit
        bool shouldExit() const
            { 
                assert (m_config);
                PluginRuntimeState *ps = m_config->getPluginRuntimeState();
                assert (ps);
                return (*ps->m_state != plugin_state_running && *ps->m_state != plugin_state_starting); 
            }

        //! set a plugin-specific piece of data for plugin-wide usage
        void setSharedData(SharedPluginState *sps) { m_config->setPluginPrivateData(sps); }
        //! get a plugin-specific piece of data for plugin-wide usage
        SharedPluginState *getSharedData() const { return (m_config->getPluginPrivateData()); }
        /*! get interval duration from plugin runtime state
         *  (NB: allows i-d to be plugin-specific, but currently isn't
         *   set that way from harpoon_main, etc.)
         */
        int getIntervalDuration() const 
            { 
                PluginRuntimeState *ps = m_config->getPluginRuntimeState();
                assert (ps);
                return (ps->m_interval_duration);
            }

        //! return plugin personality (server, client, or unspecified)
        HarpoonPluginPersonality getPersonality() const
            {
                assert (m_config);
                HarpoonPluginPersonality hpp;
                m_config->getPersonality(hpp);
                return (hpp);
            }

        //! get current plugin state (running, idle, stopped, starting)
        HarpoonPluginState getState() const 
            {
                assert (m_config);
                PluginRuntimeState *ps = m_config->getPluginRuntimeState();
                assert (ps);
                return (*ps->m_state);
            }

        //! get handle to named distribution data
        //  @param name name of distribution to get handle of
        std::vector<float> *getDistribution(std::string name) const
            {
                return (m_config->getNumericDistribution(name));
            }

        //! get handle to named property data.  property data is just
        //  a list of strings, not an associative mapping.
        //  @param name name of property list to get handle of
        std::vector<std::string> *getProperties(std::string name) const
            {
                return (m_config->getPropertyList(name));
            }

        ///! handle to the logger class.
        HarpoonLog *m_log;

    private:  
        // make these inaccessible to plugins -- they must call protected
        // accessor methods above.
        HarpoonPluginConfig *m_config;
    };
}

#endif // __HARPOON_PLUGIN_HH__
