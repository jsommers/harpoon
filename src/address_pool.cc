/*
 * $Id: address_pool.cc,v 1.4 2005-08-05 19:36:36 jsommers Exp $
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
#include <fstream>
#include <iomanip>
#include <sstream>

#include <vector>
#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "address_pool.hh"
#include "stdlib.h"

namespace Harpoon
{
    /*!
     * try adding a new address block to this pool.  
     * @param straddr a CIDR mask
     * @param port the port number
     */
    bool AddressPool::add_addr(const std::string &straddr, unsigned short port)
    {
        unsigned long addrbase = 0UL;
        unsigned int naddrs = 0;
        bool rv = parse_prefix(straddr, &addrbase, &naddrs);
        if (rv) 
        {
            AddrBlock *ab = new AddrBlock();

            ab->m_addrbase = addrbase;
            ab->m_n_addrs = naddrs;
            ab->m_port = port;
            m_addr_list.push_back(ab);
            m_n_addrs += naddrs;
        } 
        return (rv);
    }


    /*!
     * helper function for add_src() and add_dst() to do the dirty work
     * of parsing the CIDR mask and determining whether the mask is
     * good or not.  parsing is not resilient and should be fixed.
     */
    bool AddressPool::parse_prefix(const std::string &prefix,  
                                   unsigned long *addrbase, 
                                   unsigned int *naddrs)
    {
        std::string::size_type slash_pos = prefix.rfind("/");
        if (slash_pos == std::string::npos)
        { 
            std::cerr << "bad address - no prefix len?" << std::endl;
            
            // assume we've been given a single address
            *naddrs = 1;
        } 
        else
        {
            *naddrs = strtoul(prefix.substr(slash_pos+1).c_str(), 0, 10);
            if (*naddrs > 32)
            {
                std::cerr << "bad mask len " << prefix.substr(slash_pos+1) 
                          << std::endl;
                return false;
            }
            *naddrs = 1 << (32 - *naddrs);
        }

        *addrbase = inet_addr(prefix.substr(0,slash_pos).c_str());

        // if we've been given a prefix and we parsed the base address as
        // 0.0.0.0, something's wrong.
        if (*addrbase == INADDR_ANY && *naddrs > 1)
        {
            std::cerr << "bad base address " << prefix.substr(0,slash_pos) 
                      << std::endl;
            return false;
        }

        return true;
    }


    //! get a random address from the pool.
    void AddressPool::next(in_addr &addr, unsigned short &port) 
    {
        addr.s_addr = 0UL;
        port = 0;

        if (m_n_addrs)
        {    
            bool valid_lowbyte = false;
            while (!valid_lowbyte)
            {
                int addr_num = random() % m_n_addrs;
                int passed = 0;
  
                typedef std::vector<AddrBlock *>::const_iterator CI;
                for (CI iter = m_addr_list.begin(); iter != m_addr_list.end(); ++iter)
                {
                    passed += (*iter)->m_n_addrs;
                    if (addr_num < passed)
                    {
                        passed -= (*iter)->m_n_addrs;
                        addr_num = (addr_num - passed);
                        addr.s_addr = htonl(ntohl((*iter)->m_addrbase) + addr_num);

                        unsigned char low_byte = (ntohl(addr.s_addr) & 0x000000ff);
                        if (addr.s_addr != INADDR_ANY && (low_byte == 0 || low_byte == 255))
                            break;

                        valid_lowbyte = true;
                        port = (*iter)->m_port;
                        break;
                    }
                }
            }
        }
    }

    //! clear all pools
    void AddressPool::clear()
    {
        m_n_addrs = 0;
        for (std::vector<AddrBlock*>::iterator iter = m_addr_list.begin();
             iter != m_addr_list.end(); ++iter)
        {
            AddrBlock *ab = *iter;
            delete (ab);
        }
        m_addr_list.clear();
    }

    /*!
     * dump string representations of the address pool to the given stream.
     * @param os a stream to dump the pools to.
     */
    void AddressPool::dump(std::ostream &os)
    {
        os << "address list:" << std::endl;

        typedef std::vector<AddrBlock *>::const_iterator CI;
        for (CI iter = m_addr_list.begin(); iter != m_addr_list.end(); ++iter)
        {
            AddrBlock *ab = *iter;
            unsigned long first_addr = ab->m_addrbase;
            unsigned long last_addr = htonl(ntohl(ab->m_addrbase) + ab->m_n_addrs - 1);

            struct in_addr ina = { first_addr };
            char *tmpaddr = inet_ntoa(ina);
            std::string first = tmpaddr;
            struct in_addr inb = { last_addr };
            tmpaddr = inet_ntoa(inb);
            std::string last = tmpaddr;

            os << "\t" << first << " - " << last << " :" << ntohs(ab->m_port) << " (" << ab->m_n_addrs << ")" << std::endl;
        }

        if (!m_addr_list.size())
            std::cerr << "WARNING: no addresses configured" << std::endl;
    }
}

