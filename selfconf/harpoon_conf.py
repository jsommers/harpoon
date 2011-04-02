#!/usr/bin/env python

#
# $Id: harpoon_conf.py,v 1.12 2004-08-20 17:34:37 jsommers Exp $
#

import sys, getopt, bz2, sre, string, random, socket, struct, math

def usage(proggie):
    print "usage: ",proggie," [-s <starttime>] [-e <endtime>] [-i <interval>] [-m <maxlines>] [-p<outfileprefix>] [-S <sourcetargetaddressprefix>] [-D <desttargetaddressprefix>] [-t <nmimictrials>] infile[.bz2]"
    sys.exit(0)
    
try:
    opts,args = getopt.getopt(sys.argv[1:], "ds:e:i:m:p:dS:D:t:", [])
except getopt.GetoptError,e:
    print "exception while processing options: ",e
    usage(sys.argv[0])

maxlines = 0
debug = 0
startsecs = long(0)
endsecs = long(sys.maxint)
interval = long(1)
outfile_pfx = 'harpoonconf'
source_prefixes = []
dest_prefixes = []
maxinterconn = 300.0
ntrials = 100


for o,a in opts:
    if o == '-d':
        debug = debug + 1
    if o == '-D':
        dest_prefixes.append(a)
    if o == '-S':
        source_prefixes.append(a)
    if o == '-s':
        startsecs = long(a)
    if o == '-e':
        endsecs = long(a)
    elif o == '-i':
        interval = long(a)
    elif o == '-m':
        maxlines = int(a)
    elif o == '-p':
        outfile_pfx = a
    elif o == '-t':
        ntrials = int(a)



# convert from seconds to interval_factor, it's 1-1 now (historical junk...)
interval_factor = long(1) 
interval *= interval_factor

# default to something reasonable
if not len(source_prefixes):
    source_prefixes.append('192.168.0.1/32')
if not len(dest_prefixes):
    dest_prefixes.append('192.168.0.1/32')

active_sessions = []
interval_bytes = []
interval_pkts = []
interval_connects = []
file_sizes  = []
interconn_times = []
source_freq = {}
dest_freq = {}
last_time = 0

pattern_hasconnects = sre.compile('<')
pattern_goodline = sre.compile('^([\d\.]+)\s+([\d\.]+)\s+<(.*)>$')
pattern_ppair = sre.compile('^\s*\S+\s+(\d+)\s+(\d+)\s+\d+\s+([\d\.]+)\s+([\d\.]+)\s+(\d+)\s+(\d+)\s+(\S*)$')


if not len(args):
    usage(sys.argv[0])


class AddressPool(object):
    def __init__(self, *cidrs):
        self.__maxaddrs__ = 0
        self.__parseCidrs__(*cidrs)
        self.__addrnum__ = 0

    def naddrs(self):
        return(self.__maxaddrs__)
  
    def __parseCidrs__(self, *cidrs):
        self.__addrlist__ = []
        for cidr in cidrs:
            dots = string.count(cidr, '.')
            slash = string.find(cidr, '/')
            prefix = cidr[0:slash]
            for d in xrange(dots,3):
                prefix = prefix + '.0'

            masklen = int(cidr[slash+1:len(cidr)])

            # conservatively sub 2 for host/broadcast addresses
            numaddrs = 2 ** (32 - masklen) - 2 
            if masklen == 32:
                numaddrs = 1
            self.__maxaddrs__ += numaddrs
            baseaddr = socket.inet_aton(prefix)
            thisaddrpair = dict(base=baseaddr, num=numaddrs)
            self.__addrlist__.append(thisaddrpair)

    def __addrok__(self, addr):
        addrl = struct.unpack('!I',addr)[0]
        lastbyte = addrl & 0x000000ff
        if lastbyte == 0x00 or lastbyte == 0xff:
            return False
        return True

    def next(self):
        while 1:
            if self.__addrnum__ == self.__maxaddrs__:
                return None
            addr = self.__makeAddr__()
            self.__addrnum__ += 1
            if self.__addrok__(addr):
                return (addr)

    def __makeAddr__(self):
        passed = 0
        addroffset = self.__addrnum__
        for addrpair in self.__addrlist__:
            base = struct.unpack('!I',addrpair['base'])[0]
            num = addrpair['num']
            passed += num
            if passed > addroffset:
                passed -= num
                offset = addroffset - passed
                addr = base + offset
                return struct.pack('!I',addr)


def mimic_harpoon(interval_duration, interconnection_times, file_sizes, byte_targets, ntrials=100):
    if debug:
        print "mimicked interval:",interval_duration

    icpos = random.randint(0,len(interconnection_times)-1)
    fpos = random.randint(0,len(file_sizes)-1)

    results = []

    for targetbytes in byte_targets:
        trial_results = []
        flows = long(0)
        for trials in xrange(ntrials):
            hp = 0
            imaginary_bytes = 0
            nullhps = 0

            while imaginary_bytes < targetbytes:
                t = 0
                hp += 1
                hpcontrib = 0
                while 1:
                    ic = interconnection_times[icpos]
                    icpos += 1
                    icpos = icpos % len(interconnection_times)
                    if (t + ic) > interval:
                        break

                    t += ic

                    hpcontrib += file_sizes[fpos]
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

        tsum = 0
        tsqsum = 0
        max = 0
        trial_results.sort()
        median = trial_results[ntrials/2]
        for tresult in trial_results:
            if tresult > max:
                max = tresult
            tsum += tresult
            tsqsum += tresult ** 2
        mean = tsum / ntrials
        var = (tsqsum * ntrials - tsum ** 2) / ( ntrials * (ntrials - 1))
        if debug:
            print "targetbytes",targetbytes,"simbytes",imaginary_bytes,"median",median,"mean",mean,"stdev",math.sqrt(var),"max",max,"flows",(flows/ntrials)
            sys.stdout.flush()
        results.append(mean)

    return (results)


def ensurelength(maxpos, l1, l2, l3, l4):
    if maxpos >= len(l1):
        toapp = maxpos - len(l1) + 1
        for i in xrange(toapp):
            l1.append(0)
            l2.append(0)
            l3.append(0)
            l4.append(0)


def interval_distribute(begin, end, bytes, pkts):
    global debug
    global startsecs
    global interval
    global active_sessions
    global interval_bytes
    global interval_pkts
    global interval_connects

    if debug > 1:
        print "ID: ",begin,end,bytes,pkts,interval

    # operate in interval_factor fractional seconds
    istart = long(begin - startsecs) * interval_factor
    iend = long(end - startsecs) * interval_factor

    Apos = long(istart // interval)
    Bpos = long(iend // interval)

    if debug > 1:
        print "start<",istart,">iend<",iend,">Apos<",Apos,">Bpos<",Bpos,">bytes<",bytes,">pkts<",pkts,">"

    ensurelength(Bpos, active_sessions, interval_bytes, interval_pkts, interval_connects)

    if Apos == Bpos:
        if debug > 1:
            print "->single at ",Apos,": ",(iend - istart),"bytes",bytes,"pkts",pkts
        active_sessions[Apos] += (iend - istart)
        interval_bytes[Apos] += bytes
        interval_pkts[Apos] += pkts
    else:
        frac_start = long(interval - (istart - (Apos * interval)))
        frac_end = long(iend - (Bpos * interval))

        active_sessions[Apos] += frac_start
        active_sessions[Bpos] += frac_end

        idivisor = float((iend - istart) - (frac_start + frac_end))
        ifrac = 0
        if idivisor > 0:
            ifrac = float(interval) / float(iend-istart)
        ibytes = int(ifrac * float(bytes))
        ipkts = int(ifrac * float(pkts))
        sbytes = (frac_start / float(iend-istart)) * bytes
        spkts = (frac_start / float(iend-istart)) * pkts
        ebytes = (frac_end / float(iend-istart)) * bytes
        epkts = (frac_end / float(iend-istart)) * pkts

        if debug > 1:
            print "ifrac<",ifrac,">ibytes<",ibytes,">ipkts<",ipkts,">sbytes<",sbytes,">spkts<",spkts,">ebytes<",ebytes,">epkts<",epkts,">"

        interval_bytes[Apos] += sbytes
        interval_bytes[Bpos] += ebytes
        interval_pkts[Apos] += spkts
        interval_pkts[Bpos] += epkts

        for i in xrange(Apos+1,Bpos):
            active_sessions[i] += interval
            interval_bytes[i] += ibytes
            interval_pkts[i] += ipkts

        if debug > 1:
            print "->span fracstart<",frac_start,">fracend<",frac_end,">"


infile = file(args[0])
mobj = sre.search(sre.compile("\.bz2$"), args[0])
if mobj:
    infile.close()
    infile = bz2.BZ2File(args[0])


if not infile:
    print "error opening ",args[0]
    sys.exit(0)

buffer = infile.readline()
if not buffer:
    print "error reading from ",args[0]
    usage(sys.argv[0])

mobj = sre.match(sre.compile('first-record ([\d\.]+)'), buffer)
if mobj:
    startsecs = float(mobj.group(1))
    print "got starting time from file header: ",startsecs
else:
    print "no starting time - bailing."
    sys.exit(0)
    
    
lines = long(0)
print >>sys.stderr,"progress (10k lines): ",
while 1:
    buffer = infile.readline()
    if not buffer:
        break

    # skip lines that don't have connections listed or
    # are obviously too short
    if len(buffer) < 10:
        continue
    
    if not sre.search(pattern_hasconnects, buffer):
        continue

    mobj = sre.match(pattern_goodline, buffer)
    if not mobj:
        print "didn't match good line? >",buffer,"<"
        continue

    lines += 1
    if lines == maxlines:
        print >>sys.stderr,"reached max lines..."
        break

    if lines % 10000 == 0:
        print >>sys.stderr,'.',
    if lines % 100000 == 0:
        print >>sys.stderr,' ',lines
        print >>sys.stderr,'.',

    ipsrc = mobj.group(1)
    ipdst = mobj.group(2)
    connects = mobj.group(3)
    ppairs = connects.split('><') 

    ctimehash = {}
    lastsrcp = 0
    lastdstp = 0
    lastconn = 0
    lastbytes = 0
    for pp in ppairs:
        mobj = sre.match(pattern_ppair, pp)
        if not mobj:
            continue

        srcport = int(mobj.group(1))
        dstport = int(mobj.group(2))

        ppstart = float(mobj.group(3))
        ppdur = float(mobj.group(4))

        pkts = int(mobj.group(5))
        bytes = int(mobj.group(6)) - (pkts * 40) # subtract headers

        # throw away ridiculously short flows - what is a reasonable thresh?
        if bytes < 100:
            continue

        if srcport == lastsrcp and dstport == lastdstp and lastconn == int(ppstart) and bytes == lastbytes:
            if debug > 1:
                print "tossed duplicate port pair?"
            continue

        lastconn = int(ppstart)
        lastsrcp = srcport
        lastdstp = dstport
        lastbytes = bytes

        if ppstart > endsecs:
            continue

        ctimehash[ppstart] = struct.pack('fii',ppdur,bytes,pkts)

        if debug > 1:
            print "connect: ",ppstart,ppdur

    ctimelist = ctimehash.keys()
    ctimelist.sort()
    if len(ctimelist) == 0:
        continue

    for ctime in ctimelist:
        pos = long(ctime - startsecs) * interval_factor
        pos = long(pos // interval)
        ensurelength(pos, active_sessions, interval_bytes, interval_pkts, interval_connects)
        interval_connects[pos] += 1


    start = ctimelist[0]
    end = ctimelist[0]
    d,b,p = struct.unpack('fii',ctimehash[ctimelist[0]])
    file_sizes.append(b)
    bytes = b
    pkts = p

    for cidx in xrange(1,len(ctimelist)):
        interconn = ctimelist[cidx] - ctimelist[cidx - 1]
        d,b,p = struct.unpack('fii',ctimehash[ctimelist[cidx]])
        file_sizes.append(b)

        if interconn >= maxinterconn:
            end = ctimelist[cidx - 1]

            # spread active hostpair, bytes, pkts across start, end
            interval_distribute(start, end, bytes, pkts)
            
            start = ctimelist[cidx]
            end = ctimelist[cidx]
            bytes = b
            pkts = p
        else:
            interconn_times.append(interconn)
            end = ctimelist[cidx]
            bytes += b
            pkts += p

    file_sizes.append(bytes)
    interval_distribute(start, end, bytes, pkts)

    source_freq[ipsrc] = source_freq.get(ipsrc, 0) + 1
    dest_freq[ipdst] = dest_freq.get(ipdst, 0) + 1

infile.close()
print >>sys.stderr,"done (",lines,"lines)"


random.shuffle(file_sizes)
random.shuffle(interconn_times)

if debug:
    outf = open(outfile_pfx + "_sessions.out", 'w')
    sum = long(0)
    maxactive = 0
    for i in xrange(len(active_sessions)):
        v = long(round(active_sessions[i] / interval))
        if v > maxactive:
            maxactive = v
        b = interval_bytes[i]
        p = interval_pkts[i]
        c = interval_connects[i]
        print >>outf,i,v,long(b),long(p),long(c)
        sum += v
    print >>outf
    print >>outf,"#sum:",sum
    outf.close()

    outf = open(outfile_pfx + "_files.out", 'w')
    for fs in file_sizes:
        print >>outf,fs,
    print >>outf
    outf.close()
    
    outf = open(outfile_pfx + "_conn.out", 'w')
    for ic in interconn_times:
        print >>outf,ic,
    print >>outf
    outf.close()

    outf = open(outfile_pfx + "_dstfreq.out", 'w')
    items = dest_freq.items()
    backitems = [ [v[1],v[0]] for v in items]
    backitems.sort()
    sorted = [ backitems[i][1] for i in range(0, len(backitems)) ]
    sorted.reverse()
    rank = 1
    for ip in sorted:
        print >>outf,rank,ip,dest_freq[ip]
        rank += 1
    outf.close()
    
    outf = open(outfile_pfx + "_srcfreq.out", 'w')
    items = source_freq.items()
    backitems = [ [v[1],v[0]] for v in items]
    backitems.sort()
    sorted = [ backitems[i][1] for i in range(0, len(backitems)) ]
    sorted.reverse()
    rank = 1
    for ip in sorted:
        print >>outf,rank,ip,source_freq[ip]
        rank += 1
    outf.close()


if debug:
    print >>sys.stderr,"mimicking harpoon to decide appropriate number of active sessions"

# calc number of active sessions by mimicking harpoon
client_sessions = mimic_harpoon(interval, interconn_times, file_sizes, interval_bytes, ntrials);


# produce xml config files for harpoon
outf = open(outfile_pfx + "_tcpclient.xml", 'w')
print >>outf,"<harpoon_plugins>"
print >>outf,"    <plugin name=\"TcpClient\" objfile=\"tcp_plugin.so\" maxthreads=\""+str(max(client_sessions))+"\" personality=\"client\">"

print >>outf,"        <active_sessions>"
for i in client_sessions:
    print >>outf,i,
print >>outf
print >>outf,"        </active_sessions>"

print >>outf,"        <interconnection_times>"
for ic in interconn_times:
    print >>outf,ic,
print >>outf
print >>outf,"        </interconnection_times>"

print >>outf,"        <address_pool name=\"client_source_pool\">"
spool = AddressPool(*source_prefixes)
vallist = source_freq.values()
vallist.sort()
vallist.reverse()
numaddrs = min(spool.naddrs(), len(vallist))
srctot = 0
for i in xrange(numaddrs):
    srctot += vallist[i]
for i in xrange(numaddrs):
    freq = int((float(vallist[i]) / float(srctot)) * float(spool.naddrs())) + 1
    adr = spool.next()
    if not adr:
        break
    if numaddrs == 1:
        freq = 1
    for f in xrange(freq):
        print >>outf,"            <address ipv4=\""+socket.inet_ntoa(adr)+"\" port=\"0\"/>"
print >>outf,"        </address_pool>"

print >>outf,"        <address_pool name=\"client_destination_pool\">"
dpool = AddressPool(*dest_prefixes)
vallist = dest_freq.values()
vallist.sort()
vallist.reverse()
numaddrs = min(dpool.naddrs(),len(vallist))
dsttot = 0
for i in xrange(numaddrs):
    dsttot += vallist[i]
for i in xrange(numaddrs):
    freq = int((float(vallist[i]) / float(dsttot)) * float(spool.naddrs())) + 1
    adr = dpool.next()
    if not adr:
        break
    if numaddrs == 1:
        freq = 1
    for f in xrange(freq):
        print >>outf,"            <address ipv4=\""+socket.inet_ntoa(adr)+"\" port=\"10000\"/>"
print >>outf,"        </address_pool>"

print >>outf,"    </plugin>"
print >>outf,"</harpoon_plugins>"
outf.close()

outf = open(outfile_pfx + "_tcpserver.xml", 'w')
print >>outf,"<harpoon_plugins>"
print >>outf,"    <plugin name=\"TcpServer\" objfile=\"tcp_plugin.so\" maxthreads=\"47\" personality=\"server\">"
print >>outf,"        <active_sessions> 47 </active_sessions>"
print >>outf,"        <file_sizes>"
for fs in file_sizes:
    print >>outf,fs,
print >>outf
print >>outf,"        </file_sizes>"
print >>outf,"        <address_pool name=\"server_pool\">"
print >>outf,"            <address ipv4=\"0.0.0.0\" port=\"10000\"/>"
print >>outf,"        </address_pool>"
print >>outf,"    </plugin>"
print >>outf,"</harpoon_plugins>"
outf.close()

