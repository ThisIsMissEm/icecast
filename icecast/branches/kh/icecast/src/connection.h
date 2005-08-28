/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org, 
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include <sys/types.h>
#include <time.h>
#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#endif
#include "compat.h"
#include "httpp/httpp.h"
#include "thread/thread.h"
#include "net/sock.h"

struct _client_tag;
struct source_tag;

typedef struct connection_tag
{
    unsigned long id;

    time_t con_time;
    time_t discon_time;
    uint64_t sent_bytes;

    int sock;
    int serversock;
    int error;

#ifdef HAVE_OPENSSL
    /* SSL handler */
    SSL *ssl;
#endif
    int (*send)(struct connection_tag *handle, const char *buf, unsigned int len);
    int (*read)(struct connection_tag *handle, char *buf, unsigned int len);

    char *ip;
    char *host;

} connection_t;

void connection_initialize(void);
void connection_shutdown(void);
void connection_accept_loop(void);
void connection_close(connection_t *con);
connection_t *connection_create (sock_t sock, sock_t serversock, char *ip);
int connection_complete_source (struct source_tag *source,
        http_parser_t *parser, int response);

int connection_check_source_pass(http_parser_t *parser, const char *mount);
int connection_check_relay_pass(http_parser_t *parser);
int connection_check_admin_pass(http_parser_t *parser);

extern rwlock_t _source_shutdown_rwlock;

#endif  /* __CONNECTION_H__ */
