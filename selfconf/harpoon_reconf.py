#!/usr/bin/env python

#
# $Id: harpoon_reconf.py,v 1.7 2004-08-20 17:34:37 jsommers Exp $
#

import sys, getopt, string, random, math
from xml.sax import make_parser, handler

def usage(proggie):
    print >>sys.stderr,"usage: ",proggie," [-s <server_conf>] [-c <client_conf>] [-i <interval_duration (sec)>] [-b <target_byte_volume>] [-r <target_rate (bps)>]"
    print >>sys.stderr,"\tif both byte volume and rate are given, rate is preferred."
    print >>sys.stderr,"\toutput on stdout"
    sys.exit(0)
    
try:
    opts,args = getopt.getopt(sys.argv[1:], "ds:c:i:b:r:t:", [])
except getopt.GetoptError,e:
    print >>sys.stderr,"exception while processing options: ",e
    usage(sys.argv[0])


debug = 0
interval_duration = 60.0
client_fname = ''
server_fname = ''
target_volume = 0
target_rate = 0
ntrials = 100

for o,a in opts:
    if o == '-d':
        debug = 1
    if o == '-s':
        server_fname = a
    if o == '-c':
        client_fname = a
    if o == '-i':
        interval_duration = int(a)
    if o == '-b':
        target_volume = long(a)
    if o == '-r':
        target_rate = float(a)
    if o == '-t':
        ntrials = int(a)

if target_rate > 0.0:
    target_volume = target_rate / 8.0 * interval_duration
target_rate = target_volume / interval_duration * 8.0

if debug:
    print >>sys.stderr,"target volume: ",target_volume
    print >>sys.stderr,"interval duration: ",interval_duration
    print >>sys.stderr,"client conf file: ", client_fname
    print >>sys.stderr,"server conf file: ", server_fname

if target_volume <= 0 or interval_duration <= 0:
    print >>sys.stderr,"bad target volume, rate, or interval duration"
    usage(sys.argv[0])

if (not len(client_fname)):
    print >>sys.stderr,"missing client conf file"
    usage(sys.argv[0])

if (not len(server_fname)):
    print >>sys.stderr,"missing server conf file"
    usage(sys.argv[0])


def mimic_harpoon(interval_duration, interconnection_times, file_sizes, byte_targets, ntrials=100):
    icpos = random.randint(0,len(interconnection_times)-1)
    fpos = random.randint(0,len(file_sizes)-1)

    results = []

    for targetbytes in byte_targets:
        trial_results = []
        flows = long(0)
        for trials in xrange(ntrials):
            hp = 0
            imaginary_bytes = long(0)
            nullhps = 0

            while imaginary_bytes < targetbytes:
                t = 0
                hp += 1
                hpcontrib = 0
                while 1:
                    ic = interconnection_times[icpos]
                    icpos += 1
                    icpos = icpos % len(interconnection_times)
                    if (t + ic) > interval_duration:
                        break

                    t += ic
                    # also make a rudimentary account for headers
                    hpcontrib += (file_sizes[fpos] + 40 * (file_sizes[fpos] / 1500 + 2))
                    fpos += 1
                    fpos = fpos % len(file_sizes)
                    flows += 1

                if hpcontrib:
                    imaginary_bytes += hpcontrib
                else:
                    nullhps += 1

                if nullhps > (hp * 0.1):
                    print "warning: null sessions exceeded 10% total sessions."
                    print "warning: consider increasing interval."
                    sys.stdout.flush()

            trial_results.append(hp)

        tsum = 0.0
        tsqsum = 0.0
        max = 0
        trial_results.sort()
        median = trial_results[ntrials/2]
        for tresult in trial_results:
            if tresult > max:
                max = tresult
            tsum += float(tresult)
            tsqsum += float(tresult) ** 2.0
        mean = tsum / float(ntrials)
        var = (tsqsum * float(ntrials) - tsum ** 2.0) / ( float(ntrials) * (float(ntrials) - 1.0))
        if debug:
            print "targetbytes",targetbytes,"simbytes",imaginary_bytes,"median",median,"mean",int(mean),"stdev",math.sqrt(var),"max",max,"flows",(flows/ntrials)
            sys.stdout.flush()

        results.append(mean)

    return (results)


class MyFinder(handler.ContentHandler):
    def __init__(self):
        self._in_files = 0
        self._in_interconn = 0
        self.file_sizes = []
        self.interconn_times = []

    def startElement(self, name, attrs):
        if name == 'file_sizes':
            self._in_files = 1
        if name == 'interconnection_times':
            self._in_interconn = 1
        pass

    def endElement(self, name):
        self._in_interconn = 0
        self._in_files = 0

    def characters(self, content):
        if self._in_files:
            temp = content
            temp.strip()
            for f in temp.split():
                self.file_sizes.append(int(f))

        if self._in_interconn:
            temp = content
            temp.strip()
            for i in temp.split():
                self.interconn_times.append(float(i))

###################################################################
# main

parser = make_parser()
f = MyFinder()
parser.setContentHandler(f)
parser.parse(client_fname)
parser.parse(server_fname)

#if debug:
#    print "interconn times: ",f.interconn_times
#
#if debug:
#    print "file sizes",f.file_sizes

if not len(f.interconn_times):
    print >>sys.stderr,"unable to find interconnection times in client config file"
    sys.exit(-1)

if not len(f.file_sizes):
    print >>sys.stderr,"unable to find files sizes in server config file"
    sys.exit(-1)

target_volumes = [ target_volume ]
sessions = mimic_harpoon(interval_duration, f.interconn_times, f.file_sizes, target_volumes, ntrials)
print "number of sessions should be %d to achieve volume of %10.0f bytes (%8.1f bits/sec)" % (sessions[0],target_volume,target_rate)


