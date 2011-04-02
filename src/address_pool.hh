/*
 * $Id: address_pool.hh,v 1.5 2005-11-08 14:17:11 jsommers Exp $
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


#ifndef __ADDRESS_POOL_HH__
#define __ADDRESS_POOL_HH__

#include <iostream>
#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "config.h"

namespace Harpoon
{

    /*!
     * @class AddressPool
     * class used by HarpoonPluginConfig to hold address pools.  plugin code
     * never directly calls any methods of this class - it's called by
     * helper methods in HarpoonPluginConfig.
     */
    class AddressPool
    {
    public:
        AddressPool() : m_n_addrs(0)
            {
                m_addr_list.clear();
            }
        ~AddressPool() 
            {
                clear();
            }

        void next(in_addr &ina, unsigned short &); // next random source
        void clear();                             // wipe internal tables clean
        void dump(std::ostream &os = std::cerr);  // spill everything to a stream

        bool add_addr(const std::string &, unsigned short);

    private:
        /*!
         * @struct AddrBlock
         * nested structure used by AddressPool.  each block consists
         * of a base address (e.g., 192.168.0.0 for a 192.168/16 CIDR mask),
         * the number of addresses in the block (2^16 for the example - note
         * that we don't subtract host and broadcast addresses), and the
         * port number for this address block.  m_addrbase is stored in network
         * byte order, as is m_port.
         */
        struct AddrBlock
        {
            unsigned long m_addrbase;
            unsigned int m_n_addrs;
            unsigned short m_port;
        };

        int m_n_addrs;
        std::vector<AddrBlock*> m_addr_list;

        bool parse_prefix(const std::string &, unsigned long *, unsigned int *);
    };
}

#endif //  __ADDRESS_POOL_HH__

