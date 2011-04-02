/*
 * $Id: remote_control.cc,v 1.4 2005-08-05 19:36:36 jsommers Exp $
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


#include <stdio.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>

#if USE_POLL
#include <poll.h>
#endif

#include "config.h"
#include "remote_control.hh"
#include "xmlrpc_util.hh"

#define SERVER_BREED "ishmael"
#define SERVER_VERSION "0.1a"
#define BUFFER_SIZE 8192

namespace Harpoon
{

    //! structure holding expat state while parsing XML-RPC calls
    struct XmlRpcCallParseContext
    {
        XmlRpcCallParseContext()
            {
                m_method_name = "unknown";
                m_params.clear();
            }

        XML_Parser m_parser;                //<! pointer to the parser
        std::string m_method_name;          //<! string holding the method name
        std::vector<std::string> m_params;  //<! parameters are pushed onto this vector as they are encountered
    };


    /*
     * expat handler functions
     *
     */
    extern "C"
    {
        static void xmlrpcIgnoreHandler(void *v, const XML_Char *xml_string, int len)
        {
            return;
        }

        static void xmlrpcMethodParameterHandler(void *v, const XML_Char *xml_string, int len)
        {
            XmlRpcCallParseContext *xrpc = static_cast<XmlRpcCallParseContext*>(v);
            std::string param(xml_string, len);
        
            // trim the string - expat can give us some unwanted gunk
            std::string::size_type begin = 0;
            while (!isprint(param[begin]) && begin < param.length()) ++begin;

            std::string::size_type end = begin;
            while (isprint(param[end]) && end < param.length()) ++end;

            // check whether we have a good string - beware of single char
            // param case
            if (begin == end && !isprint(param[end]))
                return;

            if (param.substr(begin,end-begin).length())
                xrpc->m_params.push_back(param.substr(begin,end-begin));

#if DEBUG
            std::cerr << "method param: " << param << std::endl;
#endif
        }

        static void xmlrpcMethodNameHandler(void *v, const XML_Char *xml_string, int len)
        {
            XmlRpcCallParseContext *xrpc = static_cast<XmlRpcCallParseContext*>(v);
            std::string method_name(xml_string, len);
            xrpc->m_method_name = method_name;    
#if DEBUG
            std::cerr << "method name: " << method_name << std::endl;
#endif
        }

        static void xmlrpcEndHandler(void *v, const char *tag)
        {
            XmlRpcCallParseContext *xrpc = static_cast<XmlRpcCallParseContext*>(v);
#if DEBUG
            std::cerr << "end tag " << tag << std::endl;
#endif
            XML_SetCharacterDataHandler(xrpc->m_parser, xmlrpcIgnoreHandler);
        }

        static void xmlrpcStartHandler(void *v, const char *tag, const char **attrs)
        {
            XmlRpcCallParseContext *xrpc = static_cast<XmlRpcCallParseContext*>(v);
#if DEBUG
            std::cerr << "start tag " << tag << std::endl;
#endif

            if (!strcmp(tag, "methodName"))
                XML_SetCharacterDataHandler(xrpc->m_parser, xmlrpcMethodNameHandler);
            else if (!strcmp(tag, "param"))
                XML_SetCharacterDataHandler(xrpc->m_parser, xmlrpcMethodParameterHandler);
        }

        /*!
         * function called upon cancellation of the HTTP/XML-RPC listener thread.
         * used in pthread_cleanup_push() in remote_control_entrypoint(), below.  all
         * we do is call shutdown() on the HarpoonRemoteControl object.
         */
        void remote_control_cleanup(void *v)
        {
#if DEBUG
            std::cerr << "cleanup cancel handler firing" << std::endl;
#endif
            Harpoon::HarpoonRemoteControl *hrc = static_cast<Harpoon::HarpoonRemoteControl *>(v);
            hrc->shutdown();
        }

        /*!
         * thread entrypoint for HTTP/XML-RPC listener.  enable arbitrary cancellation
         * and push our cleanup method.  otherwise, we're stuck in handle_requests(),
         * serving requests until the end of the age.
         */
        void *remote_control_entrypoint(void *v)
        {
#if DEBUG
            std::cerr << "cleanup handler pushed" << std::endl;
#endif
            int old_val;
            pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_val);
            pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &old_val);

            pthread_cleanup_push(remote_control_cleanup, v);
            Harpoon::HarpoonRemoteControl *hrc = static_cast<Harpoon::HarpoonRemoteControl *>(v);
            hrc->handle_requests();
            pthread_cleanup_pop(1);
            return (0);
        }
    }


    /*!
     * method to list XML-RPC methods exported by this listener.
     */
    void HarpoonRemoteControl::xmlrpc_list_methods(std::vector<std::string> &params, 
                                                   std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

        for (RpcMapCI iter = m_method_map.begin(); iter != m_method_map.end();
             ++iter)
            XmlRpcUtil::encode_value(sstr, iter->first);

        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * null XML-RPC method.  can be used as a lightweight ping to see whether a
     * server is alive or not.
     */
    void HarpoonRemoteControl::xmlrpc_null_method(std::vector<std::string> &params,
                                                  std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::param_begin(sstr);
        XmlRpcUtil::encode_value(sstr, "null");
        XmlRpcUtil::param_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * implementation of the queryPlugins() XML-RPC method.  call the method exported
     * by HarpoonController that lists plugins and their statuses.
     */
    void HarpoonRemoteControl::xmlrpc_query_method(std::vector<std::string> &params,
                                                   std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);
#if TEST
        XmlRpcUtil::encode_value(sstr, "testing");
#else
        m_controller->listPlugins(sstr);
#endif
        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * implementation of unloadPlugin() XML-RPC method.  call HarpoonController to unload
     * the plugin named in the first parameter to the call.  we don't deal with multiple
     * unloadings by specifying multiple plugin names (yet?.)  note also that an easy way
     * to reinitialize statically allocated statistics for a plugin is to unload the object
     * code then reload it.
     */
    void HarpoonRemoteControl::xmlrpc_unloadplugin_method(std::vector<std::string> &params,
                                                          std::ostringstream &sstr)
    {
        bool rv = false;
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);
        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            rv = true;
#else
            rv = m_controller->unloadPlugin(params[i]);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }
        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * implementation of unloadConfig() XML-RPC method.  call HarpoonController to unload
     * the plugin *and* the configuration data named in the first parameter to the call.  we don't deal with multiple
     * unloadings by specifying multiple plugin names (yet?.)
     */
    void HarpoonRemoteControl::xmlrpc_unloadconfig_method(std::vector<std::string> &params,
                                                          std::ostringstream &sstr)
    {
        bool rv = false;
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);
        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            rv = true;
#else
            rv = m_controller->unloadConfig(params[i]);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }
        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }


    /*!
     * implementation of loadConfig() XML-RPC method.  loads the name XML config file.
     * note that the config file string can have subdirectories encoded in it, but if
     * that config file references other config files, the directories referenced must
     * be correct (i.e., relative to the working directory of this listener thread.)
     */
    void HarpoonRemoteControl::xmlrpc_loadconfig_method(std::vector<std::string> &params,
                                                        std::ostringstream &sstr)
    {
        bool rv = false;
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            rv = true;
#else
            rv = m_controller->loadConfig(params[0], sstr);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }
        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * implementation of loadPlugin() XML-RPC method.  tell the HarpoonController
     * to load the named plugin.
     */
    void HarpoonRemoteControl::xmlrpc_loadplugin_method(std::vector<std::string> &params,
                                                        std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            bool rv = true;
#else
            bool rv = m_controller->loadPlugin(params[i]);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }
        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    void HarpoonRemoteControl::xmlrpc_stopplugin_method(std::vector<std::string> &params,
                                                        std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            bool rv = true;
#else
            bool rv = m_controller->stopPlugin(params[0]);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }

        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * implementation of the startPlugin() XML-RPC method.  tell the HarpoonController
     * to start the named plugin.  if the object code for the plugin has not yet been
     * loaded through a loadPlugin() call, it will automatically get loaded as a side-effect
     * of this call.
     */
    void HarpoonRemoteControl::xmlrpc_startplugin_method(std::vector<std::string> &params,
                                                         std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

        for (unsigned int i = 0; i < params.size(); ++i)
        {
            XmlRpcUtil::struct_begin(sstr);
#if TEST
            bool rv = true;
#else
            bool rv = m_controller->startPlugin(params[i]);
#endif
            XmlRpcUtil::encode_struct_value(sstr, params[i], rv);
            XmlRpcUtil::struct_end(sstr);
        }

        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * get statistics from the HarpoonController and from all plugins.  though not
     * implemented yet, this listener thread should probably also add its own statistics.
     */
    void HarpoonRemoteControl::xmlrpc_getstats_method(std::vector<std::string> &params,
                                                      std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::array_begin(sstr);

#if TEST
        XmlRpcUtil::encode_value(sstr, "plugin stats testing");
#else
        m_controller->getPluginStats(sstr);
#endif

        XmlRpcUtil::array_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }

    /*!
     * tell the HarpoonController to kill itself and us - a big bloodbath.
     */
    void HarpoonRemoteControl::xmlrpc_suicide_method(std::vector<std::string> &params,
                                                     std::ostringstream &sstr)
    {
        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::param_begin(sstr);
        XmlRpcUtil::encode_value(sstr, "i have only one regret...");
        XmlRpcUtil::param_end(sstr);
        XmlRpcUtil::reply_end(sstr);

#if TEST
        kill(getpid(), SIGKILL);
#else
        m_controller->stop();
#endif
    }

    /*!
     * implementation of resetAll() XML-RPC method.  tell the HarpoonController to
     * stop all plugins and then restart all plugins.  the emulated interval is also reset
     * to 0 as a side-effect.  apart from resetting the emulated interval, this is just a
     * convenience method for other available XML-RPC calls.
     */
    void HarpoonRemoteControl::xmlrpc_resetall_method(std::vector<std::string> &params,
                                                      std::ostringstream &sstr)
    {
#if TEST
        bool rv = true;
#else
        bool rv = m_controller->resetPlugins();
#endif

        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::param_begin(sstr);
        XmlRpcUtil::encode_value(sstr, rv);
        XmlRpcUtil::param_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }


    /*!
     * implementation of incrTime() XML-RPC method.  tell the HarpoonController
     * to increment its emulated time by one.  returns current emulated interval
     * in xml return set (or -1 if failure.)
     */
    void HarpoonRemoteControl::xmlrpc_incrtime_method(std::vector<std::string> &params, std::ostringstream &sstr)
    {
        int rv = m_controller->incrTime();

        XmlRpcUtil::reply_begin(sstr);
        XmlRpcUtil::param_begin(sstr);
        XmlRpcUtil::encode_value(sstr, rv);
        XmlRpcUtil::param_end(sstr);
        XmlRpcUtil::reply_end(sstr);
    }


    /*!
     * send a character string to the client.
     * @param fd client socket descriptor
     * @param ostr reference to an ostringstream holding the string we send.
     */
    void HarpoonRemoteControl::write_to_client(int fd, std::ostringstream &ostr)
    {
        int remain = ostr.str().length();
        int begin = 0;
        while (remain)
        {
            int written = write(fd, ostr.str().substr(0, remain).c_str(), remain);
            if (written <= 0)
                break;

            remain -= written;
            begin += written;
        }
    }

    /*!
     * send a 500 server error to the client.  something bad happened...
     * @param fd client socket descriptor
     * @param reason a reason for all the badness.
     */
    void HarpoonRemoteControl::http_server_error(int fd, std::string &reason)
    {
        std::ostringstream ostr;
        ostr << "HTTP/1.0 500 Server Error: " << reason << "\r\n";
        ostr << "Server: " << SERVER_BREED << SERVER_VERSION << "\r\n";
        ostr << "Connection: close\r\n\r\n";
        write_to_client(fd, ostr);
    }

    /*!
     * encode a positive 200 response for a client.
     * @param ostr reference to an ostringstream to write the 
     *                            response on.
     */
    void HarpoonRemoteControl::http_response_ok(std::ostringstream &ostr)
    {
        ostr << "HTTP/1.0 200 OK\r\n";
        ostr << "Server: " << SERVER_BREED << SERVER_VERSION << "\r\n";
        ostr << "Connection: close\r\n";
    }

    /*!
     * main handler of HTTP POST methods.  for this server, the only reason we should
     * get POSTs is for XML-RPC calls.  we do the dirty work of parsing the XML in
     * this method, then calling the named method (if we recognize it) or sending an
     * error response if we've no idea what the client is asking for.
     * @param fd client socket descriptor
     * @param istr the content body of the POST request
     * @param content_length the content length that client sent us in the HTTP headers
     * @param content_type content type of the request.  it had better be text/xml.
     */
    void HarpoonRemoteControl::http_post_method(int fd, std::istringstream *istr, int content_length, std::string &content_type)
    {
        XmlRpcCallParseContext xcall;
        xcall.m_parser = 0;

        if (content_length > 0 && content_type == "text/xml")
        { 
            XML_Parser parser = XML_ParserCreate(NULL);
            xcall.m_parser = parser;
            XML_SetUserData(parser, &xcall);
            XML_SetElementHandler(xcall.m_parser, xmlrpcStartHandler, xmlrpcEndHandler);
            XML_SetCharacterDataHandler(xcall.m_parser, xmlrpcIgnoreHandler);
#if DEBUG
            std::cerr << "content length ok - content type ok - sucking in xml" << std::endl;
#endif
            // suck in xml, absorb initial blank lines (crlfs)

            while (content_length)
            {
                char buf[BUFFER_SIZE];
                istr->read(buf, std::min(BUFFER_SIZE, content_length));
                int len = istr->gcount();
                if (len <=0)
                    break;
                content_length -= len;

                if (!XML_Parse(xcall.m_parser, buf, len, !content_length))
                    break;
            }
            XML_ParserFree(xcall.m_parser);
            xcall.m_parser = 0;
        }
        else
        {
            std::string reason = "unimplemented method";
            http_server_error(fd, reason);
        }

        std::ostringstream response;
        std::ostringstream xmlstr;

#if DEBUG
        std::cerr << "method name: " << xcall.m_method_name << std::endl;
        for (std::vector<std::string>::const_iterator it = xcall.m_params.begin();
             it != xcall.m_params.end(); ++it)
            std::cerr << "\tparam: " << *it << std::endl;
#endif

        RpcMapCI iter = m_method_map.find(xcall.m_method_name);
        if (iter != m_method_map.end())
        {
            // this is fairly tricky, the whole pointer-to-member-method business,
            // that is.  the second component of the iterator basically contains
            // a vtbl offset that we have to first get (the innermost params)
            // then apply to a specific object (this) to get the actual member
            // method.  see the declaration in remote_control.hh.
            http_response_ok(response); 

#if DEBUG
            std::cerr << "XML-RPC method call " << xcall.m_method_name 
                      << "(" << xcall.m_params.size() << " params) " <<
                "( ";
            for (std::vector<std::string>::const_iterator strveciter = xcall.m_params.begin(); strveciter != xcall.m_params.end(); ++strveciter)
                std::cerr << *strveciter << ' ';
            std::cerr << ")" << std::endl;
#endif

            (this->*(iter->second))(xcall.m_params, xmlstr);

            response << "Content-type: text/xml\r\n";
            response << "Content-length: " << xmlstr.str().length() << "\r\n\r\n";
            response << xmlstr.str();
        }
        else
        {
            http_response_ok(response);

            XmlRpcUtil::reply_begin(xmlstr);
            XmlRpcUtil::encode_fault(xmlstr, 1, "Unimplemented Method");
            XmlRpcUtil::reply_end(xmlstr);

            response << "Content-type: text/xml\r\n";
            response << "Content-length: " << xmlstr.str().length() << "\r\n\r\n";
            response << xmlstr.str();
        }

#if DEBUG
        std::cerr << "writing to client: " << std::endl;
        std::cerr << response.str() << std::endl;
#endif
        write_to_client(fd, response);
    }

    /*!
     * handler for HTTP PUT methods.  unlike the POST handler, we do not get the content
     * given to us in a buffer - we suck that up ourselves and write it to disk.  the 
     * file written is named by the URI passed to us in HTTP.  before creating the file,
     * we strip off all leading '/'s, then try to create the file.  the obvious consequence
     * of this is that all files are written relative to the working directory of harpoon.
     * @param fd client socket descriptor
     * @param content_length length of the content given to us in the client HTTP headers
     * @param uri uri of the request.  we use this as the target file name.
     */
    void HarpoonRemoteControl::http_put_method(int fd, int content_length, std::string &uri)
    {
        // strip leading /'s
        std::string::size_type pos = uri.find_first_not_of('/');
        std::string uri_write = uri.substr(pos);

#if DEBUG
        std::cerr << "put method.  uri=" << uri << " uriwrite=" << uri_write << std::endl;
#endif

        int outfd = open(uri_write.c_str(), (O_WRONLY|O_CREAT|O_TRUNC), 0755);
        if (outfd < 0)
        {
            std::string reason = "couldn't create output file";
            http_server_error(fd, reason);
            return; 
            close(outfd);
        }

        char buffer[BUFFER_SIZE];
        while (content_length)
        {
            int len = read(fd, buffer, std::min(content_length, BUFFER_SIZE));
            if (len <= 0)
            {
                std::string reason = "PUT write failure";
                http_server_error(fd, reason);
                break;
            }

            content_length -= len;

            if (write(outfd, buffer, len) != len)
            {
                std::string reason = "PUT write failure";
                http_server_error(fd, reason);
                break;
            }
        }
        close(outfd);

        std::ostringstream ostr;
        http_response_ok(ostr);
        ostr << "\r\n\r\n"; // add final crlf
        write_to_client(fd, ostr);
    }


    /*!
     * read in content trailing the HTTP headers.  called only on POST requests.  we
     * know how much to expect, so we try to read exactly that, stuffing it in a character
     * buffer.
     * @param fd client socket descriptor
     * @param content_length length of the content from HTTP headers
     * @param buffer buffer to write content into
     * @param buflen length of the given buffer
     */
    int HarpoonRemoteControl::read_proto_content(int fd, 
                                                 int content_len,
                                                 char *buffer,
                                                 int buflen)
    {
        int remain = content_len;
        int len = 0;
        while (remain)
        {
            int red = read(fd, &buffer[len], (content_len - len));

            if (red == -1)
                return (len);
            else if (red == 0) 
                return (len);
            else
            {
                remain -= red;
                len += red;
                buffer[len] = '\0';
            } 
        }
#if DEBUG
        std::cerr << "read proto content: (" << len << ") <" << buffer << ">" << std::endl;
#endif

        return (len);
    }


    /*!
     * read one line of an HTTP request.  we do this character-by-character.  surprised?
     * this is pretty much what the Apache code does.  note that the file descriptor is
     * in non-blocking mode so that we don't get stuck reading a request from a slow or
     * semi-dead client.
     * @param fd client socket descriptor
     * @param buffer buffer to write protocol line into
     * @param buflen length of the given buffer
     */
    int HarpoonRemoteControl::read_proto_line(int fd, char *buffer, int buflen)
    {
        memset(buffer, 0, buflen);

        //
        // NB: required that fd is in non-blocking mode.
        //
        int pos = 0;

        // attempt to read one line, character-by-character
        while (pos < buflen)
        {
            int rlen = read(fd, &buffer[pos], 1);

            // for now, don't care about non-blocking.  the entire request
            // _should_ fit into a single packet anyway...
            if (rlen == -1)
                return -1;

            // NB: we don't check for both \r\n, just \n.  for
            // a dumb server like this, it's probably sufficient.  other
            // non-printable chars get filtered out by check above
            if (buffer[pos] == '\n')
                break;

            if (!isprint(buffer[pos]))
                continue;

            pos++;
        }

        // right trim
        buffer[pos+1] = '\0';
        while (isspace(buffer[pos]) && pos >= 0) { buffer[pos] = '\0'; --pos; }

        // left trim
        int end = pos;
        pos = 0;
        while (isspace(buffer[pos]) && pos < end) { buffer[pos] = '\0'; ++pos; }


        // ok if we return blank lines - our return value is the line
        // length.  (or -1 on failure)
#if DEBUG
        std::cerr << "read line: len(" << strlen(buffer) << ") <" << &buffer[0] << ">" << std::endl;
#endif
        return (strlen(buffer));
    }

    /*!
     * main routine for handling HTTP requests.  get HTTP protocol lines, pull out the
     * headers that we care about, and call into the proper PUT or POST method.  if the
     * request is any other method, return a 500 server error.  NB: this method is 
     * case-sensitive at present and should be fixed.  the easy way to do that is setting
     * an iostream manipulator on the istringstream that we use to pull request header
     * components into strings.  i probably could have done it in the time it took me
     * to write this...
     * @param fd client socket descriptor.
     */
    void HarpoonRemoteControl::handle_request(int fd)
    {
        char buffer[BUFFER_SIZE];
        std::string junk;
        int len = 0;

        // we may get a bunch of blank lines before method line.  this
        // stupidity should be tolerated (HTTP rfc says so...)
        for (len = read_proto_line(fd, buffer, BUFFER_SIZE); 
             len == 0;
             len = read_proto_line(fd, buffer, BUFFER_SIZE)) ;
            
        std::istringstream *istr = new std::istringstream(buffer);
        std::string method, uri, http_version;
        *istr >> method >> uri >> http_version;

        // quick bail-out
        if (!(method == "PUT" || method == "POST"))
        {
            std::string reason = "unrecognized method: " + method;
            http_server_error(fd, reason);
            delete (istr);
            return;
        }

        std::string agent, host, content_type;
        int content_length = 0;

        delete (istr);
        istr = 0;

        // now that we've received the method header, we can suck in
        // header lines until we hit a blank line, then decide whether
        // we should pull in any content.
        for (len = read_proto_line(fd, buffer, BUFFER_SIZE);
             len > 0;
             len = read_proto_line(fd, buffer, BUFFER_SIZE))
        {
            istr = new std::istringstream(buffer);
            std::string first;
            *istr >> first;

            for (std::string::size_type i = 0; i < first.length(); ++i)
                first[i] = toupper(first[i]);
 
            if (first == "USER-AGENT:")
            {
                *istr >> agent;
            }
            else if (first == "HOST:")
            {
                *istr >> host;
            }
            else if (first == "CONTENT-TYPE:")
            {
                *istr >> content_type;
            }
            else if (first == "CONTENT-LENGTH:")
            {
                std::string clen;
                *istr >> clen;
#if DEBUG
                std::cerr << "read content-length as " << clen << std::endl;
#endif
                content_length = strtol(clen.c_str(), 0, 10);
            }

            delete (istr);
        }

#if DEBUG
        std::cerr << "uri: " << uri << std::endl;
        std::cerr << "host: " << host << std::endl;
        std::cerr << "user agent: " << agent << std::endl;
        std::cerr << "content type: " << content_type << std::endl;
        std::cerr << "content length: " << content_length << std::endl;
#endif

        if (method == "POST")
        {
            if (read_proto_content(fd, content_length, buffer, BUFFER_SIZE) == content_length)
            {
                istr = new std::istringstream(buffer);
                http_post_method(fd, istr, content_length, content_type);
                delete (istr);
            }
            else
            {
                std::string reason = "invalid content length";
                http_server_error(fd, reason);
            }
        }
        else if (method == "PUT")
        { 
            http_put_method(fd, content_length, uri);
        }
    }


    /*!
     * main thread routine.  open up a socket, bind it to a local address, and accept
     * requests as long as we are alive.  note that all requests are handled in a
     * single-threaded manner.  the reasons for this are that the requests should be all
     * very simple ones, and we don't want to consume thread resources that can be used by
     * traffic generation threads on systems where this is a significant limitation.
     */
    int HarpoonRemoteControl::handle_requests()
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sockaddr_in));
        sin.sin_family = AF_INET;
        sin.sin_addr = m_addr;
        sin.sin_port = htons(m_port);
        bind(fd, (const struct sockaddr*)&sin, sizeof(struct sockaddr_in));
        listen(fd, 7); // arbitrary, small backlog of requests.  we handle requests from the main thread,
                       // so we don't want to block long, but the requests we are designed to deal with are
                       // few and relatively lightweight, and the request traffic *should* be low anyway.
        
        bool serving_requests = true;
        while (1)
        {
            pthread_mutex_lock(&m_serving_mutex);
            serving_requests = m_serving;
            pthread_mutex_unlock(&m_serving_mutex);
            if (!serving_requests)
            {
#if DEBUG
                std::cerr << "Remote control listener got shutdown request." << std::endl;
#endif
                break;
            }

            struct sockaddr_in cliaddr;
            SOCKLEN_T addr_len = sizeof(struct sockaddr_in);

#if USE_POLL
            struct pollfd pfd[1];
            pfd[0].fd = fd;
            pfd[0].events = POLLIN;
            int n = poll(&pfd[0], 1, 1000);
#else
            fd_set readfds, excfds;
            FD_ZERO(&readfds);
            FD_ZERO(&excfds);
            FD_SET(fd, &readfds);
            FD_SET(fd, &excfds);
            struct timeval timeout = { 1, 0};
            int n = select(fd+1, &readfds, 0, &excfds, &timeout);
#endif
            pthread_testcancel();

            if (n == 1 && 
#if USE_POLL
                (pfd[0].revents & POLLIN)
#else
                FD_ISSET(fd, &readfds)
#endif
                )
            {
                int clifd = accept(fd, (struct sockaddr *)&cliaddr, &addr_len);
                if (clifd > 0) /* && (fcntl(clifd, F_SETFL, O_NONBLOCK) != -1))*/
                    handle_request(clifd);
                close (clifd);
            }
            else if (
#if USE_POLL
                (pfd[0].revents & POLLERR ||
                 pfd[0].revents & POLLHUP ||
                 pfd[0].revents & POLLNVAL)
#else
                FD_ISSET(fd, &excfds) 
#endif
                 || (n < 0 && errno != EINTR))
            {
                std::cerr << "unrecoverable error in xmlrpc listener" << std::endl;
                break;
            }
        }
        close (fd);
        return (0);
    }
}



#if TEST
/*
 * for unit testing the XML-RPC/HTTP listener.
 */
int main(int argc, char **argv)
{
    Harpoon::HarpoonRemoteControl *rc = new Harpoon::HarpoonRemoteControl();
    pthread_t hrc_thread;

    sigset_t sset;
    sigfillset(&sset);
    sigprocmask(SIG_BLOCK, &sset, 0);
    pthread_create (&hrc_thread, NULL, Harpoon::remote_control_entrypoint, rc);

    while (1)
    {
        int sig;
        sigemptyset(&sset);
        sigaddset(&sset, SIGINT);
        sigaddset(&sset, SIGTERM);
        sigwait(&sset, &sig);
        if (sig == SIGINT || sig == SIGTERM)
        {
            std::cerr << "got signal " << sig << " - exiting" << std::endl;
            rc->shutdown();
            break;
        }
    }

    pthread_cancel (hrc_thread);
    std::cerr << "thread canceled" << std::endl;
    int *rv = new int;
    pthread_join(hrc_thread, (void**)&rv);
    std::cerr << "thread joined" << std::endl;
    delete rv;
    
    return 0;
}
#endif // TEST

