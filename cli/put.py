#!/usr/bin/env python

#
# $Id: put.py,v 1.2 2005-01-27 04:24:40 jsommers Exp $
#

import httplib, socket, sys, getopt

def usage(proggie):
    print "usage: ", proggie, "[-h <host> -i <infile> -o <targetfile>]"
    sys.exit(0)

try:
    opts,args = getopt.getopt(sys.argv[1:], "h:i:o:", [])
except getopt.GetoptError,e:
    print "exception while processing options: ", e
    usage (sys.argv[0])

rhost = ''
infile = ''
tfile = ''

url_list = []
for o, a in opts:
    if o == "-h":
        rhost = a
    elif o == "-i":
        infile = a
    elif o == "-o":
        tfile = a

if len(rhost) == 0 or len(infile) == 0 or len(tfile) == 0:
    usage(sys.argv[0])


pfile = file(infile)
fstr = pfile.read(1000000)
pfile.close()

dstport = 8180
method = 'PUT'

print "sending file of length: ",len(fstr)

try:
    httpcon = httplib.HTTPConnection(rhost, 8180)
    httpcon.request(method, tfile, fstr)
    resp = httpcon.getresponse()
    httpcon.close()
    print "http returned: ",resp.status,resp.reason,resp.version,resp.msg
except:
    print >>sys.stderr,"error shipping file.  check hostname and whether server is running."

