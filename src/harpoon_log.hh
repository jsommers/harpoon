/*
 * $Id: harpoon_log.hh,v 1.3 2005-11-08 14:17:11 jsommers Exp $
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


#ifndef __HARPOON_LOG_HH__
#define __HARPOON_LOG_HH__

#include <iostream>
#include <iomanip>
#include <pthread.h>

#include "config.h"

namespace Harpoon
{
    class HarpoonLog 
    {
    public:
        HarpoonLog(std::ostream *os, int verbosity = 1) : m_os(os), m_verbosity(verbosity)
            {
                pthread_mutex_init(&m_mutex, NULL);
            }

        virtual ~HarpoonLog()
            {
                pthread_mutex_destroy(&m_mutex);
            }

        void log(int level, const char *cstr)
            {
                this->log(level, std::string(cstr));
            }

        void log(int level, std::string cppstr)
            {
                if (level > m_verbosity)
                    return;

                char timebuf[16];
                struct tm tms;
                time_t now = time(NULL);
                localtime_r(&now, &tms);

                pthread_mutex_lock(&m_mutex);
                strftime(timebuf, 15, "%T", &tms);

                *m_os << timebuf << " sev(" << std::setfill('0') 
                      << std::setw(2) << level << ") " 
                      << cppstr << std::endl;

                pthread_mutex_unlock(&m_mutex);
            }

        void setVerbosity(int v) { m_verbosity = v; }

    private:
        std::ostream *m_os;
        pthread_mutex_t m_mutex;
        int m_verbosity;
    };
}

#endif // __HARPOON_LOG_HH__
