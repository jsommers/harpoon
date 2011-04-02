#!/usr/bin/env python

#
# $Id: inventory.py,v 1.2 2004-05-31 18:30:15 jsommers Exp $
#

import sys,xmlrpclib,pprint
from getopt import *

def usage(proggie):
	print "usage: ",proggie,"[-u <url>]*"
	exit
try:
	opts,args = getopt(sys.argv[1:],"u:",[])
except GetoptError,e:
	print "exception while processing options: ",e
	usage(sys.argv[0])

url_list = []
for o,a in opts:
	if o == "-u":
		url_list.append(a)

for server_url in url_list:
	print "Plugin Inventory for ",server_url,":"
	server = xmlrpclib.ServerProxy(server_url)
	try:
		result = server.queryPlugins()
		for struct in result:
			if struct.has_key('uptime'):
				print '\tplugin<%(plugin_name)s> state<%(state)s> personality<%(personality)s> uptime<%(uptime)d>' % struct
			else:
				print '\tplugin<%(plugin_name)s> state<%(state)s> personality<%(personality)s>' % struct
	except sys.exc_info, einfo:
		print "ERROR", einfo[exc_value]


