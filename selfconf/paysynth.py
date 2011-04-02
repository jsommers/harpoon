#!/usr/bin/env python

#
# $Id: paysynth.py,v 1.3 2005-03-14 17:46:30 jsommers Exp $
#

#
# NB: this is an undocumented python script for generating synthetic workloads
# from the payload plugin.
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


def write_config(fname, service, tport, flow_sizes, interconn, pkt_nums):

    pname = fname + 'plugin'
    pfile = 'client'+str(service)+'.out.gz'
    # write out client file
    outf = open(fname, 'w')
    print >>outf,"<harpoon_plugins>"
    print >>outf,"    <plugin name=\""+pname+"\" objfile=\"payload_plugin.so\" maxthreads=\"1\" personality=\"client\">"

    print >>outf,"        <active_sessions> 1 </active_sessions>"
    print >>outf,"        <state_machine_file type=\"property\"> autom.out.gz </state_machine_file>"
    print >>outf,"        <client_payloads type=\"property\">",pfile,"</client_payloads>"
    print >>outf,"        <payload_recycle> 20000 </payload_recycle>"
    print >>outf,"        <non_root> 0 </non_root>"
    print >>outf,"        <transport_type type=\"property\">",tport,"</transport_type>"
    print >>outf,"        <service type=\"property\">", service,"</service>"


    print >>outf,"        <interconnection_times>"
    for ic in interconn:
        print >>outf,ic,
    print >>outf
    print >>outf,"        </interconnection_times>"

    print >>outf,"        <flow_sizes>"
    for fl in flow_sizes:
        print >>outf,int(fl),
    print >>outf
    print >>outf,"        </flow_sizes>"

    print >>outf,"        <num_packets>"
    for pn in pkt_nums:
        pktout = int(pn)
        if pktout < 1:
            pktout = 1
        print >>outf,pktout,
    print >>outf
    print >>outf,"        </num_packets>"

    print >>outf,"        <control_dest_addr type=\"property\"> 10.2.23.119 </control_dest_addr>"
    print >>outf,"        <control_dest_port type=\"property\"> 10000 </control_dest_port>"

    print >>outf,"        <address_pool name=\"client_source_pool\">"
    print >>outf,"            <address ipv4=\"10.53.1.0/24\" port=\"0\"/>"
    print >>outf,"        </address_pool>"

    print >>outf,"        <address_pool name=\"client_destination_pool\">"
    print >>outf,"            <address ipv4=\"10.54.0.42/32\" port=\"0\"/>"
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

flow_sizes = makedist(pareto(10000, 1.15), 100000)
fast_interconn = makedist(exponential(1.0), 10000) # avg 1 sec
slow_interconn = makedist(exponential(0.25), 10000) # avg 4 sec
pkt_nums = makedist(exponential(0.2), 10000) # avg 5 pkts

write_config("client80.xml", 80, "tcp", flow_sizes, fast_interconn, pkt_nums)
write_config("client22.xml", 22, "tcp", flow_sizes, slow_interconn, pkt_nums)
write_config("client23.xml", 23, "tcp", flow_sizes, slow_interconn, pkt_nums)
write_config("client53.xml", 53, "udp", [ 1 ], slow_interconn, [ 1 ])
write_config("client25.xml", 25, "tcp", flow_sizes, slow_interconn, pkt_nums)

