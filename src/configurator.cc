/*
 * $Id: configurator.cc,v 1.4 2005-08-05 19:36:36 jsommers Exp $
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


#include <cstdlib>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "configurator.hh"


namespace Harpoon
{
    extern "C"
    {

        /*  
         * expat handlers.  this is messy stuff, and at present, undocumented.  looking
         * at the schema helps to follow the code and what it's trying to accomplish. the
         * basic thing is to keep track of what our depth is (how many start tags deep are
         * we?) and keep some state on what we should be expecting.  certain start tags
         * cause us to change our character data handler method, and end tags might cause
         * us to reset some handlers.
         */
        static void dataIgnorer(void *v, const XML_Char *xml_string, int len)
        {
            return; 
        }

        static bool parse_addr_attrs(const char **attrs, 
                                     std::string &addr, unsigned short &port)
        {
            const char **tmp = attrs;

            bool addrset = false;
            bool portset = false;

            addr = "0.0.0.0/0"; 
            port = 0;

            for (int i = 0; i < 2; ++i)
            {
                const char *attr = *tmp;
                const char *val = *(tmp+1);
                if (!strcmp(attr, "ipv4"))
                {
                    addr = val;
                    addrset = true;
                }
                else if (!strcmp(attr, "port"))
                {
                    port = htons(strtoul(val, 0, 10));
                    portset = true;
                }
                else
                    std::cerr << "unrecognized attribute in address parsing: " 
                              << attr << std::endl;
                tmp += 2;
            }
  
#ifdef DEBUG
            if (portset && addrset)
            {
                std::cerr << "parsed address: " << addr << ':' << port << std::endl;
            }
            else
                std::cerr << "can't parse address (starts: " << *attrs << std::endl;
#endif

            return (portset && addrset); 
        }

        
        static void endElement(void *v, const char *name)
        {
#ifdef DEBUG
            std::cerr << "endElement: " << name << std::endl;
#endif

            ConfigLoadContext *clc = static_cast<ConfigLoadContext *>(v);

            if (!clc->parse_state_good)
                return;

            if (!strcmp(name, "config_file"))
            {
                return;
            }
            else
            {
                clc->depth--;
                if (clc->depth == 1)
                    clc->hpc = 0;

                if (clc->handler_stack.size())
                    clc->handler_stack.pop_back();
                XML_CharacterDataHandler h = 0;
                if (clc->handler_stack.size())
                    h = clc->handler_stack.back();
                else
                    h = dataIgnorer;

                XML_SetCharacterDataHandler(clc->parser_stack.back(), h);
                clc->tag_stack.pop_back();
            }
        }


        static void distributionDataHandler(void *v, const XML_Char *xml_string, int len)
        {
            ConfigLoadContext *clc = static_cast<ConfigLoadContext *>(v);

            if (!clc->parse_state_good)
                return;

            clc->hpc->loadDistributionDataHelper(clc, xml_string, len);
        }


        // forward declaration
        static void cfgFileHandler(void *, const XML_Char *, int);


        static void startElement(void *v, const char *name, const char **attrs)
        {
            ConfigLoadContext *clc = static_cast<ConfigLoadContext *>(v);

            if (!clc->parse_state_good)
                return;

#ifdef DEBUG
            std::cerr << "startElement: " << name << " depth=" << clc->depth << std::endl;
#endif
            bool ok = false;
            if (clc->depth == 0)
            {
                if (!strcmp(name, "harpoon_plugins"))
                    ok = true;
            }
            else if (clc->depth == 1)
            {
                if (!strcmp(name, "config_file"))
                {
                    clc->handler_stack.push_back(cfgFileHandler);
                    XML_SetCharacterDataHandler(clc->parser_stack.back(), cfgFileHandler);
                    ok = true;
                }
                else if (!strcmp(name, "plugin"))
                {
                    // 
                    std::string plugin_name = "unknown";
                    std::string objfile = "unknown";
                    int maxthreads = -1;
                    Harpoon::HarpoonPluginPersonality personality = Harpoon::plugin_personality_unknown;
                    const char **tmp = attrs;
                    while (*tmp)
                    {
                        const char *val = *(tmp+1);
                        if (!val)
                        {
                            ok = false;
                            break;
                        }

                        if (!strcmp(*tmp, "name"))
                            plugin_name = val;
                        else if (!strcmp(*tmp, "objfile"))
                            objfile = val;
                        else if (!strcmp(*tmp, "maxthreads"))
                            maxthreads = strtol(val, 0, 10);
                        else if (!strcmp(*tmp, "personality"))
                        {
                            if (!strcmp(val, "client"))
                                personality = Harpoon::plugin_personality_client;
                            else if (!strcmp(val, "server"))
                                personality = Harpoon::plugin_personality_server;
                            else if (!strcmp(val, "unknown"))
                                personality = Harpoon::plugin_personality_unknown;
                            else 
                            {
                                ok = false;
                                break;
                            }
                        }
                        else
                        {
                            ok = false;
                            break;
                        }
                        tmp += 2;
                    }

                    ok = (plugin_name != "unknown" && objfile != "unknown" && maxthreads >= 0);

                    if (ok)
                    {
                        Harpoon::HpcMapCI iter =  clc->config_map->find(plugin_name);
                        Harpoon::HarpoonPluginConfig *hpc = 0;

                        if (iter == clc->config_map->end())
                        {
                            hpc = new Harpoon::HarpoonPluginConfig();
                            clc->config_map->insert(std::pair<std::string,Harpoon::HarpoonPluginConfig *>(plugin_name,hpc));
                        }
                        else
                            hpc = iter->second;
               

                        // weird, yes.  but these attribute tags are *required*
                        // according to the schema.
                        hpc->setName(plugin_name);
                        hpc->setObjFile(objfile);
                        hpc->setMaxThreads(maxthreads);
                        hpc->setPersonality(personality);
                        clc->hpc = hpc; 
#ifdef DEBUG
                        std::cerr << "parsed plugin attribs successfully: " << plugin_name << ',' << objfile << ',' << maxthreads << ',' << personality << std::endl;
#endif
                    }
                    else
                    {
                        std::cerr << "required attributes for <plugin> not present or invalid" << std::endl;
                    }
                }
                else
                {
                    ok = false;
                }
            }
            else
            {
#ifdef DEBUG
                std::cerr << "calling loadConfigDataStartTagHelper" << std::endl;
#endif
                ok = clc->hpc->loadConfigDataStartTagHelper(clc, name, attrs);
            }

            if (!ok)
            {
                clc->parse_state_good = false;

                // error message + tag stack trace
                std::cerr << "can't grok config file.  xml tag stack: " << std::endl;

                std::string tabs = "\t";
                for (std::deque<std::string>::const_iterator ci = clc->tag_stack.begin();
                     ci != clc->tag_stack.end(); ++ci)
                {
                    std::cerr << tabs << *ci << std::endl;
                    tabs += "\t";
                }

                return;
            }

            clc->tag_stack.push_back(std::string(name));
            clc->depth++; 
        }


        static void cfgFileHandler(void *v, const XML_Char *xml_string, int len)
        {
            std::string tmp_str = std::string(xml_string, len);
            std::string::size_type begin = tmp_str.find_first_not_of(" \t\n");
            std::string::size_type end = tmp_str.find_first_of(" \t\n", begin);
            std::string cfg_file_name = tmp_str.substr(begin, end-begin);


#ifdef DEBUG
            std::cerr << "doing inner parse of cfg file " << cfg_file_name 
                      << std::endl;
#endif

            ConfigLoadContext *clc = static_cast<ConfigLoadContext *>(v);

            if (!clc->parse_state_good)
                return;

            clc->handler_stack.pop_back();
            clc->handler_stack.push_back(dataIgnorer);
            XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);

            int depth_restore = clc->depth - 1;

            std::ifstream *ifs = new std::ifstream(cfg_file_name.c_str());
            if (*ifs && ifs->good())
            {
                clc->tag_stack.pop_back();
                clc->depth--;   // fake depth - make it look like cfg_file level isn't
                // here

                // create new parser for new data stream, set up handlers
                clc->parser_stack.push_back(XML_ParserCreate(NULL));
                XML_SetUserData(clc->parser_stack.back(), clc);
                XML_SetElementHandler(clc->parser_stack.back(), startElement, endElement);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);

                while (!ifs->eof())
                {   
                    char buf[BUFSIZ];
                    memset(buf, 0, BUFSIZ);
                    ifs->read(buf, BUFSIZ-1);
                    int read_len = ifs->gcount();

                    if (!XML_Parse(clc->parser_stack.back(), buf, read_len, (read_len == 0))) 
                        break;
                } 
            
                ifs->close();
                delete ifs;

                XML_ParserFree(clc->parser_stack.back());
                clc->parser_stack.pop_back();
            }

            clc->depth = depth_restore;
        }
    } // extern "C"


    /*!
     * a method internal to the HarpoonPluginConfig class for handling distribution
     * data.  at this point, we know that we're inside character data for some tag,
     * we also know that the tag is a recognized distribution tag.  we can just
     * rip through the character data and stuff the elements onto the proper distribution
     * vector.  note that we might wind up reading a partial value here, getting yanked
     * back to read in more data, then get called back here again by XML_Parse.  we hold
     * that partial read in the data_collect_state member of the ConfigLoadContext.  note
     * also that the character string is not a C-string - it has no termination character.
     * @param clc context/state of XML parsing
     * @param xml_string the character data string
     * @param len length of the character data string.
     */
    void HarpoonPluginConfig::loadDistributionDataHelper(ConfigLoadContext *clc, 
                                                         const XML_Char *xml_string, 
                                                         int len)
    {
        if (!clc->parse_state_good)
            return;

        std::string tmp_str = clc->data_collect_state + std::string(xml_string, len);
        clc->data_collect_state = "";


        std::string::size_type pos = tmp_str.find_first_not_of(" \t\n");
        std::string name = clc->tag_stack.back();

        while (pos < tmp_str.length() && pos != std::string::npos)
        {
            std::string::size_type next = tmp_str.find_first_of(" \t\n", pos);
            if (next == std::string::npos)
            {
                clc->data_collect_state = tmp_str.substr(pos);
                return;
            }
      
            std::string vstr = tmp_str.substr(pos, next-pos);
            if (clc->numeric_data)
            {
                float fval = atof(vstr.c_str());
                std::map<std::string,std::vector<float>*>::const_iterator uditer = m_distributions.find(name);
                if (uditer == m_distributions.end())
                {
                    std::vector<float> *newvec = new std::vector<float>();
                    m_distributions.insert(std::pair<std::string,std::vector<float>*>(name,newvec));
                    newvec->push_back(fval);       
                }
                else
                    uditer->second->push_back(fval);
            }
            else // not numeric data
            {
                std::map<std::string,std::vector<std::string>*>::const_iterator uditer = m_properties.find(name);
                if (uditer == m_properties.end())
                {
                    std::vector<std::string> *newvec = new std::vector<std::string>();
                    m_properties.insert(std::pair<std::string,std::vector<std::string>*>(name,newvec));
                    newvec->push_back(vstr);       
                }
                else
                    uditer->second->push_back(vstr);
            }

#if DEBUG
            std::cerr << "adding " << name << "->" << vstr << std::endl;
#endif

            pos = tmp_str.find_first_not_of(" \t\n", next);
        }
    }


    bool HarpoonPluginConfig::loadConfigDataStartTagHelper(ConfigLoadContext *clc,
                                                           const char *name, 
                                                           const char **attrs)
    {
        bool ok = true;
        assert (clc->depth >= 1);

        if (!strcmp(name, "config_file"))
        {
            clc->handler_stack.push_back(cfgFileHandler);
            XML_SetCharacterDataHandler(clc->parser_stack.back(), cfgFileHandler);
            return (ok);
        }

        if (clc->depth == 2)
        {
            std::string upper_tag = clc->tag_stack.back();
#ifdef DEBUG
            std::cerr << "at depth 2 in tag helper with upper tag as " << upper_tag << std::endl;
#endif 
            if (upper_tag != "plugin")
            {
                clc->handler_stack.push_back(dataIgnorer);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);   
                ok = false;
            }
            else if (!strcmp(name, "address_pool") && 
                     attrs[0] != 0 && attrs[1] != 0 && 
                     !strcmp(attrs[0],"name") && strlen(attrs[1]))
            {
                clc->handler_stack.push_back(dataIgnorer);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);   
                clc->pool_name = attrs[1];
                ok = true;
            }
            else if (attrs[0] != 0 && attrs[1] != 0 && 
                !strcmp(attrs[0],"type") && !strcmp(attrs[1],"numeric"))
            {
                clc->handler_stack.push_back(distributionDataHandler);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), distributionDataHandler);   
                clc->numeric_data = true;
                ok = true;
            }
            else if (attrs[0] != 0 && attrs[1] != 0 && 
                     !strcmp(attrs[0],"type") && !strcmp(attrs[1],"property"))
            {
                clc->handler_stack.push_back(distributionDataHandler);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), distributionDataHandler);   
                clc->numeric_data = false;
                ok = true;
            }
            else // default to numeric data handler
            {
                clc->handler_stack.push_back(distributionDataHandler);
                XML_SetCharacterDataHandler(clc->parser_stack.back(), distributionDataHandler);   
                clc->numeric_data = true;
                ok = true;
            }
        }
        else if (clc->depth == 3)
        {
            std::string upper_tag = clc->tag_stack.back();
            if (!strcmp(name, "address") && upper_tag == "address_pool")
            {
                std::string addrstr;
                unsigned short port;
                if (parse_addr_attrs(attrs, addrstr, port))
                {
                    bool add_ok = false;
                    if (clc->pool_name == "client_source_pool" ||
                        clc->pool_name == "client_src_pool")
                    {
                        add_ok = m_client_src_pool.add_addr(addrstr, port);
                    }
                    else if (clc->pool_name == "client_destination_pool" ||
                             clc->pool_name == "client_dest_pool" ||
                             clc->pool_name == "client_dst_pool")
                    {
                        add_ok = m_client_dst_pool.add_addr(addrstr, port);
                    }
                    else if (clc->pool_name == "server_pool")
                    {
                        add_ok = m_server_pool.add_addr(addrstr, port);
                    }
                    else
                        std::cerr << "unknown address pool " << clc->pool_name << std::endl;

                    if (!add_ok)
                    {
                        std::cerr << "address <" << addrstr << ':' << port 
                                  << "> couldn't be parsed - not added." 
                                  << std::endl;
                    }
                }
            }
        }
        else
        {
            ok = false;
            clc->handler_stack.push_back(dataIgnorer);
            XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);
        }

        return ok;
    }


    //! dump configuration to the given stream object
    void HarpoonPluginConfig::dumpConfig(std::ostream &os)
    {
        os << "name: " << m_name << std::endl;
        os << "objfile: " << m_objfile_name << std::endl;
        os << "maxthreads: " << m_max_threads << std::endl;

        os << "personality: " << getPluginPersonalityName(m_personality) 
                  << std::endl;

        if (m_personality == plugin_personality_server)
        {
            os << "server address pool:" << std::endl;
            m_server_pool.dump(os);
        }

        if (m_personality == plugin_personality_client)
        {
            os << "client source pool:" << std::endl;
            m_client_src_pool.dump(os);
            os << "client destination pool: " << std::endl;
            m_client_dst_pool.dump(os);
        }

        if (m_distributions.size())
            os << std::endl << "dumping distributions (first 10): " << std::endl;
        for (std::map<std::string,std::vector<float>*>::const_iterator iter = m_distributions.begin(); iter != m_distributions.end(); ++iter)
        {
            os << iter->first << ": ";
            int max = 10;
            std::vector<float> *distrdata = iter->second;
            for (std::vector<float>::const_iterator distiter = distrdata->begin(); distiter != distrdata->end() && max > 0; ++distiter)
            {
                os << *distiter << ' ';
                max--;
            }
            os << std::endl;
        }

        if (m_properties.size())
            os << std::endl << "dumping properties (first 10): " << std::endl;
        for (std::map<std::string,std::vector<std::string>*>::const_iterator iter = m_properties.begin(); iter != m_properties.end(); ++iter)
        {
            os << iter->first << ": ";
            int max = 10;
            std::vector<std::string> *distrdata = iter->second;
            for (std::vector<std::string>::const_iterator distiter = distrdata->begin(); distiter != distrdata->end() && max > 0; ++distiter)
            {
                os << *distiter << ' ';
                max--;
            }
            os << std::endl;
        }
    }


    //! string names of plugin personalities
    const char *HarpoonPluginConfig::m_personality_names[3] = {"unknown", "client", "server"};


    //! string names of plugin states
    const char *HarpoonPluginConfig::m_plugin_state_names[4] = {"idle", "running", "starting", "stopping" };



    /*!
     * load and parse an XML configuration file.  if the parse is successful, add
     * HarpoonPluginConfig objects to the map passed as a parameter.  the may key is the
     * plugin name.
     * @param fname file name to load
     * @param cfg_map the map to store parsed configurations into
     */
    int loadConfigFile(const std::string &fname, 
                       std::map<std::string, HarpoonPluginConfig*> *cfg_map)
                                     
    {
        ConfigLoadContext *clc = new ConfigLoadContext(cfg_map);

        char buf[BUFSIZ];
        clc->parser_stack.push_back(XML_ParserCreate(NULL));
        XML_SetUserData(clc->parser_stack.back(), clc);
        XML_SetElementHandler(clc->parser_stack.back(), startElement, endElement);
        XML_SetCharacterDataHandler(clc->parser_stack.back(), dataIgnorer);

        std::ifstream *ifs = new std::ifstream(fname.c_str());
        if (!ifs || !ifs->good())
        {
            XML_ParserFree(clc->parser_stack.back());
            clc->parser_stack.pop_back();
            delete (clc);
            return -1;
        }

        while (!ifs->eof())
        {   
            memset(buf, 0, BUFSIZ);
            ifs->read(buf, BUFSIZ-1);
            int read_len = ifs->gcount();
 
            if (!XML_Parse(clc->parser_stack.back(), buf, read_len, (read_len ==0)))
                break;

            if (!clc->parse_state_good)
                break;
        } 
            
        ifs->close();
        delete ifs;

        XML_ParserFree(clc->parser_stack.back());
        clc->parser_stack.pop_back();
        int rv = (!clc->parse_state_good);
        delete (clc);

        return rv;
    }
}



/*
 * unit testing code - also works as a config file sanity checker, sort of.
 */

#ifdef VALIDATOR 
int main(int argc, char **argv)
{
    srandom(time(NULL) % getpid());

    using namespace Harpoon;

    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <config file>" << std::endl;
        exit(0);
    }

    std::map<std::string, HarpoonPluginConfig*> *cfg_map = 
        new std::map<std::string, HarpoonPluginConfig*>();

    for (int i = 1; i < argc; ++i)
    {
        std::string fname = argv[i];
        std::cerr << "loading " << fname << std::endl;
        if (Harpoon::loadConfigFile(fname, cfg_map))
            std::cerr << "failed to load " << fname << ": " 
                      << strerror(errno) << '/' << errno << std::endl;
    }


    for (HpcMapCI iter = cfg_map->begin(); iter != cfg_map->end(); ++iter)
    {
        std::cerr << "Checking load of " << iter->first << std::endl;
        HarpoonPluginConfig *hpc = iter->second;
        hpc->dumpConfig(std::cerr);
    }
}

#endif // VALIDATOR

