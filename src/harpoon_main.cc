/*
 * $Id: harpoon_main.cc,v 1.5 2005-08-05 19:36:36 jsommers Exp $
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
#include <stdlib.h>
#include <unistd.h>
#include "harpoon_controller.hh"
#include "configurator.hh"


void usage(const char *proggie)
{
    std::cerr << "usage " << proggie << ": [-a] [-b<addr>] [-c] [-f<fname>] [-l<logfile>] [-p<port>] [-s<seed>] [-v] [-w<secs>] [-?]" << std::endl;
    std::cerr << "\t-a           set emulated time *not* to automatically increment.  must call XML-RPC to manually increment time if this option is set.  default is for emu-time to automatically tick." << std::endl;
    std::cerr << "\t-b<addr>     xmlrpc listener bind address.  default=* (0.0.0.0)." << std::endl;
    std::cerr << "\t-c           continually cycle over list of number of active sessions." << std::endl;
    std::cerr << "\t-f<fname>    load configuration file fname (can be" 
              << std::endl;
    std::cerr << "\t             specified multiple times.)" << std::endl;
    std::cerr << "\t-l<logfile>  write log messages to <logfile>.  default=stderr" << std::endl;
    std::cerr << "\t-p<port>     xmlrpc listener port.  default=8180." << std::endl;
    std::cerr << "\t-s<seed>     specify random seed" << std::endl;
    std::cerr << "\t-v<level>    specify verbosity level (0=min,10=max)" << std::endl;
    std::cerr << "\t-w<secs>     harpoon warp factor: set one epoch to equal"
              << std::endl;
    std::cerr << "\t             <secs> seconds.  default=60s" << std::endl;
    std::cerr << "\t-?           this message" << std::endl;
}


int main(int argc, char **argv)
{
    std::vector<std::string> config_files;
    int verbose = 0;
    int warp_factor = 60;
    bool autoincrtime = true;
    unsigned short xmlrpc_port = 8180;
    std::string xmlrpc_addr = "0.0.0.0";
    long int seed = time(NULL) % getpid();
    std::string log_file;
    bool continuousrun = false;
   
    int c;
    while ((c = getopt(argc, argv, "?abcf:l:p:s:v:w:")) != EOF) 
    {
        switch (c)
        {
        case 'a':
            autoincrtime = false;
            break;
        case 'b':
            xmlrpc_addr = optarg;
            break;
        case 'c':
            continuousrun = true;
            break;
        case 'f':
            config_files.push_back(std::string(optarg));
            break;
        case 'l':
            log_file = optarg;
            break;
        case 'p':
            xmlrpc_port = strtol(optarg, 0, 10);
            break;
        case 'v':
            verbose = strtol(optarg, 0, 10);
            break;
        case 'w':
            warp_factor = strtol(optarg, 0, 10);
            break;
        case 's':
            seed = strtol(optarg, 0, 10);
            break;
        default:
            usage(argv[0]);
            exit(0);
        }
    }


    Harpoon::HarpoonController *main_thread = new Harpoon::HarpoonController(log_file);

    std::map<std::string, Harpoon::HarpoonPluginConfig*> *cfg_map =
        new std::map<std::string, Harpoon::HarpoonPluginConfig*>();
    for (std::vector<std::string>::const_iterator iter = config_files.begin();
         iter != config_files.end();
         ++iter)
    {
        std::string fname = *iter;
        if (verbose)
            std::cerr << "loading " << fname << "... ";
        Harpoon::loadConfigFile(fname, cfg_map);
        if (verbose)
            std::cerr << " finished." << std::endl;
    }

    if (verbose)
    {
        for (Harpoon::HpcMapCI iter = cfg_map->begin(); iter != cfg_map->end(); ++iter)
        {
            std::cerr << "Checking load of " << iter->first << std::endl;
            Harpoon::HarpoonPluginConfig *hpc = iter->second;
            hpc->dumpConfig(std::cerr);
        }
    }

    struct in_addr xmlrpc_inaddr;
    memset(&xmlrpc_inaddr, 0, sizeof(xmlrpc_inaddr));
    if (inet_pton(AF_INET, xmlrpc_addr.c_str(), &xmlrpc_inaddr) != 1)
    {
        std::cerr << "Unable to parse XML/RPC bind address." << std::endl;
        exit (0);
    }

    main_thread->setVerbosity(verbose);
    main_thread->setConfig(cfg_map);
    main_thread->setContinuousRun(continuousrun);
    main_thread->setControlAddr(xmlrpc_inaddr);
    main_thread->setControlPort(xmlrpc_port);
    main_thread->setAutoincrTime(autoincrtime);
    main_thread->setWarpFactor(warp_factor);
    main_thread->setSeed(seed);

    main_thread->run();

    exit (0);
}

