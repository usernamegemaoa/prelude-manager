/*****
*
* Copyright (C) 1999,2000 Yoann Vandoorselaere <yoann@mandrakesoft.com>
* All Rights Reserved
*
* This file is part of the Prelude program.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by 
* the Free Software Foundation; either version 2, or (at your option)
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; see the file COPYING.  If not, write to
* the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <sys/poll.h>
#include <assert.h>

#include <libprelude/common.h>
#include <libprelude/config-engine.h>
#include <libprelude/plugin-common.h>

#include "config.h"
#include "ssl.h"

#include <libprelude/prelude-io.h>
#include <libprelude/prelude-message.h>
#include <libprelude/prelude-auth.h>

#include "pconfig.h"
#include "server-logic.h"
#include "server-generic.h"


#define UNIX_SOCK "/var/lib/prelude/socket"



struct server_generic {
        int sock;
        int unix_srvr;
        socklen_t addrlen;
        struct sockaddr *addr;
        struct server_logic *logic;
        
        server_generic_accept_func_t *accept;
        server_generic_close_func_t *close;
};




static int server_close_connection_cb(void *sdata, prelude_io_t *pio, void *clientdata) 
{
        int ret;
        server_generic_t *server = sdata;

        server->close(clientdata);
        
        if ( server->unix_srvr )
                log(LOG_INFO, "closing connection on UNIX socket.\n");
        else {
                struct sockaddr_in *addr = (struct sockaddr_in *) server->addr;
                log(LOG_INFO, "closing connection with %s.\n", inet_ntoa(addr->sin_addr));
        }
        
        ret = prelude_io_close(pio);
        prelude_io_destroy(pio);
        
        return ret;
}




static int handle_normal_connection(prelude_io_t *fd, const char *addr) 
{
        int ret;

        ret = prelude_auth_recv(fd, addr);
        if ( ret < 0 ) {
                log(LOG_INFO, "Plaintext authentication failed with %s.\n", addr);
                return -1;
        }

        log(LOG_INFO, "Plaintext authentication succeed with %s.\n", addr);

        return 0;
}




static int handle_ssl_connection(prelude_io_t *fd, const char *addr) 
{
#ifdef HAVE_SSL
        SSL *ssl;
        
        ssl = ssl_auth_client(prelude_io_get_fd(fd));
        if ( ! ssl ) {
                log(LOG_INFO, "SSL authentication failed with %s.\n", addr);
                return -1;
        }
        
        log(LOG_INFO, "SSL authentication succeed with %s.\n", addr);
        
        prelude_io_set_ssl_io(fd, ssl);
        
        return 0;
#else
        log(LOG_INFO, "Client requested unavailable option : SSL.\n");
        return -1;
#endif
}






static int setup_connection(prelude_io_t *fd, const char *addr) 
{
        int ret, len;
        const char *ssl;
        char *ptr, buf[1024];
        
#ifdef HAVE_SSL
        ssl = "supported";
#else
        ssl = "unsupported";
#endif
        
        len = snprintf(buf, sizeof(buf), "ssl=%s;\n", ssl);

        ret = prelude_io_write_delimited(fd, buf, ++len);
        if ( ret < 0 ) {
                log(LOG_ERR, "error writing config to Prelude client.\n");
                return -1;
        }

        ret = prelude_io_read_delimited(fd, (void **)&ptr);
        if ( ret <= 0 ) {
                log(LOG_ERR, "error reading Prelude client config string.\n");
                return -1;
        }
        
        if ( strstr(ptr, "use_ssl=yes;") ) 
                ret = handle_ssl_connection(fd, addr);
        else 
                ret = handle_normal_connection(fd, addr);
        
        free(ptr);

        return ret;
}




#ifdef HAVE_TCPD_H

#include <tcpd.h>

int allow_severity = LOG_INFO, deny_severity = LOG_NOTICE;


/*
 *
 */
static int tcpd_auth(int clnt_sock) 
{
        int ret;
        struct request_info request;
        
        request_init(&request, RQ_DAEMON, "prelude-manager", RQ_FILE, clnt_sock, 0);
        
        fromhost(&request);

        ret = hosts_access(&request);
        if ( ! ret ) {
                syslog(deny_severity,
                       "prelude-manager: refused connect from %s", eval_client(&request));
                return -1;
        }

        syslog(allow_severity, "prelude-manager: connect from %s", eval_client(&request));
        
        return 0;
}
#endif



/*
 *
 */
static prelude_io_t *setup_inet_connection(int sock, struct sockaddr_in *addr) 
{
        int ret;
        const char *from;
        prelude_io_t *pio;
        
        from = inet_ntoa(((struct sockaddr_in *)addr)->sin_addr);
              
#ifdef HAVE_TCPD_H
        ret = tcpd_auth(sock);
        if ( ret < 0 )
                return NULL;
#endif
        
        log(LOG_INFO, "new connection from %s.\n", from);

        pio = prelude_io_new();
        if ( ! pio )
                return NULL;

        prelude_io_set_sys_io(pio, sock);
        
        ret = setup_connection(pio, from);
        if ( ret < 0  ) {
                log(LOG_INFO, "closing connection with %s.\n", from);
                prelude_io_close(pio);
                prelude_io_destroy(pio);
                return NULL;
        }
        
        return pio;
}




/*
 *
 */
static prelude_io_t *setup_unix_connection(int sock, struct sockaddr_un *addr)
{
        int ret;
        prelude_io_t *pio;
        
        log(LOG_INFO, "new UNIX connection.\n");

        pio = prelude_io_new();
        if ( ! pio )
                return NULL;

        prelude_io_set_sys_io(pio, sock);
        
        ret = handle_normal_connection(pio, "unix");
        if ( ret < 0 ) {
                log(LOG_INFO, "closing unix connection.\n");
                prelude_io_close(pio);
                prelude_io_destroy(pio);
                return NULL;
        }
        
        return pio;
}



/*
 *
 */
static void wait_connection(server_generic_t *server)
{
        int ret;
        int client;
        void *clientdata;
        prelude_io_t *pio;
        
        
        while ( 1 ) {
                
                client = accept(server->sock, server->addr, &server->addrlen);
                if ( client < 0 ) {
                        log(LOG_ERR, "couldn't accept connection.\n");
                        continue;
                }
                
                if ( server->unix_srvr )
                        pio = setup_unix_connection(client, (struct sockaddr_un *)server->addr);
                else
                        pio = setup_inet_connection(client, (struct sockaddr_in *)server->addr);
                
                if ( ! pio ) {
                        close(client);
                        continue;
                }

                ret = fcntl(client, F_SETFL, O_NONBLOCK);
                if ( ret < 0 ) {
                        log(LOG_ERR, "couldn't set non blocking mode for client.\n");
                        prelude_io_close(pio);
                        prelude_io_destroy(pio);
                        continue;
                }

                clientdata = NULL;
                
                ret = server->accept(pio, &clientdata);
                if ( ret < 0 )
                        continue;
                
                ret = server_logic_process_requests(server->logic, pio, clientdata);
                if ( ret < 0 ) {
                        log(LOG_ERR, "queueing client FD for server logic processing failed.\n");
                        prelude_io_close(pio);
                        prelude_io_destroy(pio);
                        continue;
                }
                
        }
}



/*
 *
 */
static int generic_server(int sock, struct sockaddr *addr, size_t alen) 
{
        int ret;
        
        ret = bind(sock, addr, alen);
        if ( ret < 0 ) {
                log(LOG_ERR, "couldn't bind to socket.\n");
                return -1;
        }
        
        ret = listen(sock, 10);
        if ( ret < 0 ) {
                log(LOG_ERR, "couldn't listen on socket.\n");
                return -1;
        }
        
        return 0;
}




/*
 * If the UNIX socket already exist, check if it is in use.
 * if it is not, delete it.
 *
 * FIXME: Using connect for this is dirty.
 *
 * return 1 if the socket is already in use.
 * return 0 if the socket is unused.
 * retuir -1 on error.
 */
static int is_unix_socket_already_used(int sock, struct sockaddr *addr, int addrlen) 
{
        int ret;
        
        ret = access(UNIX_SOCK, F_OK);
        if ( ret < 0 )
                return 0;
        
        ret = connect(sock, addr, addrlen);
        if ( ret == 0 ) {
                log(LOG_INFO, "Prelude Manager UNIX socket is already used. Exiting.\n");
                return 1;
        }

        /*
         * The unix socket exist on the file system,
         * but no one use it... Delete it.
         */
        ret = unlink(UNIX_SOCK);
        if ( ret < 0 ) {
                log(LOG_ERR, "couldn't delete UNIX socket.\n");
                return -1;
        }
        
        return 0;
}



/*
 *
 */
static int unix_server_start(server_generic_t *server) 
{
        int ret;
        struct sockaddr_un *addr;
        
        addr = malloc(sizeof(struct sockaddr_un));
        if ( ! addr ) {
                log(LOG_ERR, "memory exhausted.\n");
                return -1;
        }
        
        server->sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if ( server->sock < 0 ) {
                log(LOG_ERR, "couldn't create socket.\n");
		return -1;
	}
        
        addr->sun_family = AF_UNIX;
        strncpy(addr->sun_path, UNIX_SOCK, sizeof(addr->sun_path));
        
        ret = is_unix_socket_already_used(server->sock, (struct sockaddr *) addr, sizeof(*addr));
        if ( ret == 1 || ret == -1 ) {
                close(server->sock);
                free(addr);
                return -1;
        }
        
        ret = generic_server(server->sock, (struct sockaddr *) addr, sizeof(*addr));
        if ( ret < 0 ) {
                close(server->sock);
                free(addr);
                return -1;
        }

        server->addr = (struct sockaddr *) addr;
        
        return 0;
}




/*
 *
 */
static int inet_server_start(server_generic_t *server, const char *saddr, uint16_t port) 
{
        int ret, on = 1;
        struct sockaddr_in *addr;

        addr = malloc(sizeof(struct sockaddr_in));
        if ( ! addr ) {
                log(LOG_ERR, "memory exhausted.\n");
                return -1;
        }
        
        server->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if ( server->sock < 0 ) {
                log(LOG_ERR, "couldn't create socket.\n");
                return -1;
        }
        
        addr->sin_family = AF_INET;
        addr->sin_port = htons(port);
        addr->sin_addr.s_addr = inet_addr(saddr);

        ret = setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
        if ( ret < 0 ) {
                log(LOG_ERR, "couldn't set SO_REUSEADDR socket option.\n");
                goto err;
        }

        ret = setsockopt(server->sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(int));
        if ( ret < 0 ) {
                log(LOG_ERR, "couldn't set SO_KEEPALIVE socket option.\n");
                goto err;
        }
        
        ret = generic_server(server->sock, (struct sockaddr *) addr, sizeof(*addr));
        if ( ret < 0 )
                goto err;

#ifdef HAVE_SSL
        ret = ssl_init_server();
	if ( ret < 0 )
                goto err;
#endif

        server->addr = (struct sockaddr *) addr;

        return 0;

 err:
        close(server->sock);
        return -1;
}




/*
 *
 */
server_generic_t *server_generic_new(const char *addr, uint16_t port, server_generic_accept_func_t *acceptf,
                                     server_read_func_t *readf, server_generic_close_func_t *closef)
{
        int ret;
        server_generic_t *server;

        server = malloc(sizeof(*server));
        if ( ! server ) {
                log(LOG_ERR, "memory exhausted.\n");
                return NULL;
        }

        server->accept = acceptf;
        server->close = closef;
        
        server->logic = server_logic_new(server, readf, server_close_connection_cb);
        if ( ! server->logic ) {
                log(LOG_ERR, "couldn't initialize server pool.\n");
                free(server);
                return NULL;
        }
        
        ret = strcmp(addr, "unix");
        if ( ret == 0 ) {
                server->unix_srvr = 1;
                ret = unix_server_start(server);
        } else {
                server->unix_srvr = 0;
                ret = inet_server_start(server, addr, port);
        }

        if ( ret < 0 ) {
                server_logic_stop(server->logic);
                free(server);
                return NULL;
        }
        
        return server;
}




void server_generic_start(server_generic_t *server) 
{
        wait_connection(server);
}




void server_generic_close(server_generic_t *server) 
{
        server_logic_stop(server->logic);

        if ( server->unix_srvr )
                unlink(UNIX_SOCK);
}









