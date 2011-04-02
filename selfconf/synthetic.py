#!/usr/bin/env python

#
# $Id: synthetic.py,v 1.3 2005-12-26 17:23:44 jsommers Exp $
#

#
# NB: this is an undocumented python script for generating synthetic workloads
# from known distributions using harpoon.  edit the lines that specify the 
# distributions for file sizes and inter-connection times (toward end of 
# script), and then run it.  
#

import random,sys,math

debug = 0

def makedist(f, samples):
    r = []
    sum = float(0.0)
    sqsum = float(0.0)
    for s in xrange(samples):
        v = f()
        r.append(v)
        if debug:
            sum += float(v)
            sqsum += float(v**2)

    if debug:
        avg = sum / float(samples)
        var = (sqsum * float(samples) - sum**2) / ( float(samples) * (float(samples - 1)))
        print avg, var

    return (r)

def pareto(offset,alpha):
    return lambda: offset*random.paretovariate(alpha)

def normal(mu,sigma):
    return lambda: random.gauss(mu,sigma)

def uniform(lo,hi):
    return lambda: random.uniform(lo,hi)

def exponential(lam):
    return lambda: random.expovariate(lam)

def weibull(alpha,beta):
    return lambda: random.weibullvariate(alpha,beta)

def lognormal(mu,sigma):
    return lambda: random.lognormvariate(mu,sigma)

def write_files(file_dist, ic_dist):

    # write out client file
    outf = open("synth_tcpclient.xml", 'w')
    print >>outf,"<harpoon_plugins>"
    print >>outf,"    <plugin name=\"TcpClient\" objfile=\"tcp_plugin.so\" maxthreads=\"1\" personality=\"client\">"

    print >>outf,"        <active_sessions> 1 </active_sessions>"

    print >>outf,"        <interconnection_times>"
    for ic in ic_dist:
        print >>outf,ic,
    print >>outf
    print >>outf,"        </interconnection_times>"

    print >>outf,"        <address_pool name=\"client_source_pool\">"
    print >>outf,"            <address ipv4=\"127.0.0.1\" port=\"0\"/>"
    print >>outf,"        </address_pool>"

    print >>outf,"        <address_pool name=\"client_destination_pool\">"
    print >>outf,"            <address ipv4=\"127.0.0.1\" port=\"10000\"/>"
    print >>outf,"        </address_pool>"

    print >>outf,"    </plugin>"
    print >>outf,"</harpoon_plugins>"
    outf.close()

    # write out server file
    outf = open("synth_tcpserver.xml", 'w')
    print >>outf,"<harpoon_plugins>"
    print >>outf,"    <plugin name=\"TcpServer\" objfile=\"tcp_plugin.so\" maxthreads=\"47\" personality=\"server\">"

    print >>outf,"        <active_sessions> 47 </active_sessions>"

    print >>outf,"        <file_sizes>"
    for fs in file_dist:
        print >>outf,int(fs),
    print >>outf
    print >>outf,"        </file_sizes>"

    print >>outf,"        <address_pool name=\"server_pool\">"
    print >>outf,"            <address ipv4=\"0.0.0.0\" port=\"10000\"/>"
    print >>outf,"        </address_pool>"

    print >>outf,"    </plugin>"
    print >>outf,"</harpoon_plugins>"
    outf.close()

    return


#############################################################

random.seed() # defaults to system time

#
# edit these two lines to specify the distribution family and parameters
# (first argument to makedist()), and number of values to generate (second
# parameter).
#

file_sizes = makedist(pareto(5000,1.2), 100000)
interconn_times = makedist(exponential(10.0), 100000)

write_files(file_sizes, interconn_times)

print "Done writing base config files (synth_tcpserver.xml and "
print "synth_tcpclient.xml)."
print ""
print "Now run harpoon_reconf.py to tune number of sessions, and edit"
print "address pools."

