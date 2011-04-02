/*
 * $Id: xmlrpc_util.hh,v 1.3 2005-11-08 14:17:11 jsommers Exp $
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


#ifndef __XMLRPC_UTIL_HH__
#define __XMLRPC_UTIL_HH__

#include <iostream>
#include "config.h"

namespace Harpoon
{

    /*!
     * @class XmlRpcUtil
     * contains a set of static utility methods for generating
     * legal XML-RPC method results.  
     */
    class XmlRpcUtil
    {
    public:
        static void reply_begin(std::ostream &os)
            {
                os << "<?xml version='1.0'?>" << std::endl;
                os << "<methodResponse><params>" << std::endl;
            }

        static void reply_end(std::ostream &os)
            {
                os << "</params></methodResponse>" << std::endl;
            }

        static void array_begin(std::ostream &os)
            {
                os << "<param><value><array><data>" << std::endl;

            }

        static void array_end(std::ostream &os)
            {
                os << "</data></array></value></param>" << std::endl;
            }

        static void param_begin(std::ostream &os)
            {
                os << "<param>" << std::endl;
            }

        static void param_end(std::ostream &os)
            {
                os << "</param>" << std::endl;
            }

        static void encode_value(std::ostream &os, const std::string &str)
            {
                os << "<value><string>" << str << "</string></value>" << std::endl;
            }

        static void encode_value(std::ostream &os, const char *cstr)
            {
                os << "<value><string>" << cstr << "</string></value>" << std::endl;
            }

        static void encode_value(std::ostream &os, int i)
            {
                os << "<value><i4>" << i << "</i4></value>" << std::endl;
            }

        static void struct_begin(std::ostream &os)
            {
                os << "<value><struct>" << std::endl;
            }

        static void struct_end(std::ostream &os)
            {
                os << "</struct></value>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, const std::string &name, const std::string &value)
            {
                encode_struct_value(os, name.c_str(), value.c_str());
            }

        static void encode_struct_value(std::ostream &os, const char *name, const char *value)
            {
                os << "<member><name>" << name << "</name>" << std::endl;
                os << "<value><string>" << value << "</string></value></member>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, const char *name, bool value)
            {
                os << "<member><name>" << name << "</name>" << std::endl;
                os << "<value><boolean>" << value << "</boolean></value></member>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, std::string &str, bool value)
            {
                encode_struct_value(os, str.c_str(), value);  
            }

        static void encode_struct_value(std::ostream &os, const char *name, long value)
            {
                os << "<member><name>" << name << "</name>" << std::endl;

                // oops for 64-bit platforms. XML-RPC only supports 4 byte ints
                os << "<value><i4>" << int(value) << "</i4></value></member>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, std::string &str, long value)
            {
                encode_struct_value(os, str.c_str(), value);
            }

        static void encode_struct_value(std::ostream &os, const char *name, int value)
            {
                os << "<member><name>" << name << "</name>" << std::endl;
                os << "<value><i4>" << value << "</i4></value></member>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, std::string str, int value)
            {
                encode_struct_value(os, str.c_str(), value);
            }

        static void encode_struct_value(std::ostream &os, const char *name, double value)
            {
                os << "<member><name>" << name << "</name>" << std::endl;
                os << "<value><double>" << value << "</double></value></member>" << std::endl;
            }

        static void encode_struct_value(std::ostream &os, std::string &str, double value)
            {
                encode_struct_value(os, str.c_str(), value);
            }

        static void encode_fault(std::ostream &os, int code, const char *faultcstr)
            {
                os << "<fault>" << std::endl;
                os << "  <value>" << std::endl;
                os << "     <struct>" << std::endl;
                os << "       <member>" << std::endl;
                os << "         <name>faultCode</name>" << std::endl;
                os << "         <value><int>" << code << "</int></value>" << std::endl;
                os << "       </member>" << std::endl;
                os << "       <member>" << std::endl;
                os << "         <name>faultString</name>" << std::endl;
                os << "         <value><string>" << faultcstr << "</string></value>" << std::endl;
                os << "       </member>" << std::endl;
                os << "     </struct>" << std::endl;
                os << "  </value>" << std::endl;
                os << "</fault>" << std::endl;
            }

        static void encode_fault(std::ostream &os, int code, const std::string &faultstr)
            {
                encode_fault(os, code, faultstr.c_str());
            }

    private:
        // disallow construction
        XmlRpcUtil() {}
        ~XmlRpcUtil() {}
    };

}

#endif // __XMLRPC_UTIL_HH__
