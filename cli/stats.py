#!/usr/bin/env python

#
# $Id: stats.py,v 1.3 2005-03-07 19:41:02 jsommers Exp $
#

import sys,xmlrpclib,pprint
from getopt import *

def usage(proggie):
        print "usage: ", proggie, "[-u <url>]*"
        exit

try:
        opts,args = getopt(sys.argv[1:], "u:", [])
except GetoptError,e:
        print "exception while processing options: ", e
        usage (sys.argv[0])


url_list = []
for o, a in opts:
        if o == "-u":
                url_list.append(a)

for server_url in url_list:
	server = xmlrpclib.ServerProxy(server_url)
	print "stats for ",server
	try:
		result = server.getStats()
		serverstat = result[0]
		print "server-wide information:"
		for field in serverstat:
			print "\t",field," ",serverstat[field]        
		del result[0]
	
		print "----"
		print "plugin-specific information:"
		print 
	
		for struct in result:
			print "\t",struct['plugin_name'],"is",struct['state'],
                        if not struct.has_key('uptime'):
                            print
                            continue
                        print "- up for ",struct['uptime']," seconds"
			print "\t\ttarget threads =",struct['target_threads']," active threads =",struct['active_threads']
			del struct['plugin_name']
			del struct['state']
			del struct['target_threads']
			del struct['active_threads']
			del struct['uptime']
	
			for key in struct:
				print "\t\t",key.strip(),"=",struct[key]
	
	except sys.exc_info, einfo:
		print "ERROR", einfo[exc_value]

