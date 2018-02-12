# 
# $Id: configure.ac,v 1.23 2006-08-07 12:18:31 jsommers Exp $
#

AC_PREREQ(2.57)
AC_INIT(Harpoon, 1.0, jsommers@cs.wisc.edu)
AC_CONFIG_HEADERS([src/config.h])
AC_CONFIG_SRCDIR([src/address_pool.cc])
AC_REVISION( $Id: configure.ac,v 1.23 2006-08-07 12:18:31 jsommers Exp $ )

AC_CONFIG_AUX_DIR([conf])

# we don't really do any installation at all of harpoon. 
AC_PREFIX_DEFAULT([/usr/local/harpoon])

AC_ARG_ENABLE([debug], AS_HELP_STRING([--enable-debug],[compile with debugging options]), AC_SUBST(USE_DEBUG, 1), AC_SUBST(USE_DEBUG, 0))

AC_ARG_ENABLE([static-plugins], AS_HELP_STRING([--enable-static-plugins],[compile plugins statically (into harpoon executable)]), AC_SUBST(STATIC_PLUGINS, 1), AC_SUBST(STATIC_PLUGINS, 0))

if test "$STATIC_PLUGINS" != "1"; 
then
    echo "building dynamically loadable plugins."
else
    echo "statically building plugins into harpoon executable."
    AC_DEFINE(STATIC_PLUGINS)
    AC_SUBST(STATIC_PLUGIN_OBJS, "plugins/dummy_plugin.o plugins/tcp_plugin.o plugins/infinite_tcp.o plugins/udpcbr_plugin.o plugins/udpblast_plugin.o plugins/udpexpo_plugin.o plugins/udpperiodic_plugin.o")
fi


AC_LANG(C++)

AC_PROG_CXX
AC_PROG_MAKE_SET
AC_PROG_INSTALL
AC_PATH_PROG([PYTHON],[python])
if test "$PYTHON" != ""; then
  verok=`$PYTHON -c 'import sys;print int((sys.version.split()[[0]]) >= 2.2)'`
  if test "$verok" != "1"; then
    AC_WARN("your python isn't version 2.2 or better - can't use self conf tools.")
    AC_SUBST(PYTHON, "")
  fi
fi


#
# need to do this early on, because we find the dlcompat libraries
# for macos-x in /sw and later checks depend on that...
#
AC_CANONICAL_SYSTEM
LIBDL="dl"
DARWIN_BROKEN_DLCOMPAT="0"

if test "$USE_DEBUG" = "1"; 
then
    CXXFLAGS="-g"
fi

CXXFLAGS="$CXXFLAGS -DBLOCKING_IO=0"

case "$target" in
    *solaris*)
	CXXFLAGS="$CXXFLAGS -DUSE_POLL=1 -D_REENTRANT" ;
	SHLIB_EXT="so" ;
	LIBS="-lpthread -lthread -lsocket -lnsl" ;
        SHLIB_FLAG="-shared -Xlinker -G" ;
	CPPFLAGS="$CPPFLAGS -D_POSIX_PTHREAD_SEMANTICS" ;
	if test "$CXX" = "CC"
	then
		TEMPLATEDIR="SunWS_cache" ;
		CXXFLAGS="$CXXFLAGS +w -mt" ;
		SHLIB_FLAG="-G" ;
                LIBS="-mt $LIBS" ;
	fi ;;
    *apple-darwin*)
	CXXFLAGS="$CXXFLAGS -Wall -pipe -DUSE_POLL=1";
	CPPFLAGS="$CPPFLAGS -I/usr/X11R6/include" ;
	LDFLAGS="$LDFLAGS -L/usr/X11R6/lib" ;
        SHLIB_FLAG="-bundle" ;
	SHLIB_EXT="dylib" ;
	LIBS="$LIBS -lpthread" ;;
    *linux*)
        CXXFLAGS="$CXXFLAGS -Wall -D_REENTRANT -DUSE_POLL=1 -fPIC" ;
        SHLIB_FLAG="-shared" ;
        SHLIB_EXT="so" ;
	LIBS="$LIBS -lpthread" ;;
    *freebsd5*)
	CXXFLAGS="$CXXFLAGS -Wall -pthread -DUSE_POLL=1" ;
	CPPFLAGS="$CPPFLAGS -I/usr/local/include" ;
        SHLIB_FLAG="-shared" ;
        SHLIB_EXT="so" ;
	LIBS="$LIBS -L/usr/local/lib -lc_r" ;
        LIBDL="c_r" ;;
    *dragonfly*)
	CXXFLAGS="$CXXFLAGS -Wall -pthread -DUSE_POLL=1" ;
	CPPFLAGS="$CPPFLAGS -I/usr/local/include" ;
        SHLIB_FLAG="-shared" ;
        SHLIB_EXT="so" ;
	LIBS="$LIBS -L/usr/local/lib -lc_r" ;
        LIBDL="c_r" ;;
    *)
	echo "unsupported platform - good luck!"
esac

AC_CHECK_HEADERS([float.h])
AC_CHECK_HEADERS([sys/socket.h])
AC_CHECK_HEADERS([ftconfig.h], ,[AC_DEFINE(HAVE_FLOWTOOLS,0)])
AC_CHECK_LIB([z], [gzopen], , 
 [AC_MSG_WARN("no zlib -> can't use flow tools library.")
  AC_DEFINE(HAVE_FLOWTOOLS,0)],
 [])
AC_CHECK_LIB([ft], 
 [ftio_read], 
 [AC_DEFINE(HAVE_FLOWTOOLS,1) 
  LIBFT="-lft"], 
 [AC_MSG_WARN("no flow-tools library.")
  AC_DEFINE(HAVE_FLOWTOOLS,0)
  LIBFT=""],
 [-lz])

# check for libraries
if test "$STATIC_PLUGINS" != "1"; 
then
    AC_CHECK_LIB([$LIBDL], [dlopen],,[AC_MSG_ERROR("need libdl for loading plugins")])
fi

AC_CHECK_LIB([expat], [XML_Parse],,[AC_MSG_ERROR("need libexpat for XML parsing")])
AC_CHECK_TYPE(socklen_t, AC_DEFINE(SOCKLEN_T,socklen_t), AC_DEFINE(SOCKLEN_T,int), 
[#if HAVE_SYS_SOCKET_H
 # include <sys/types.h>
 # include <sys/socket.h>
 # endif])

AC_CONFIG_FILES([Makefile src/Makefile src/plugins/Makefile docs/Makefile selfconf/Makefile])
AC_CONFIG_FILES([examples/tcp_client.xml examples/tcp_server.xml examples/tcp_client_ex2.xml examples/tcp_server_ex2.xml examples/udp_client.xml examples/udp_server.xml examples/dummy_plugin.xml examples/inftcp_client.xml examples/inftcp_server.xml])
AC_CONFIG_FILES([src/run_harpoon.sh])
AC_SUBST(SHLIB_EXT)
AC_SUBST(SHLIB_FLAG)
AC_SUBST(LIBFT)
AC_SUBST(TEMPLATEDIR)
AC_SUBST(SOCKLEN_T)
AC_SUBST(DARWIN_BROKEN_DLCOMPAT)
AC_SUBST(HAVE_FLOAT_H)
AC_SUBST(STATIC_PLUGINS)
AC_SUBST(STATIC_PLUGIN_OBJS)
AC_OUTPUT

