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

/* -*- c-basic-offset: 4; indent-tabs-mode: nil; -*- */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ogg/ogg.h>
#include <errno.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#else
#include <winsock2.h>
#include <windows.h>
#define snprintf _snprintf
#endif

#include "thread/thread.h"
#include "avl/avl.h"
#include "httpp/httpp.h"
#include "net/sock.h"

#include "connection.h"
#include "global.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "logging.h"
#include "cfgfile.h"
#include "util.h"
#include "source.h"
#include "format.h"
#include "fserve.h"
#include "auth.h"

#undef CATMODULE
#define CATMODULE "source"

#define MAX_FALLBACK_DEPTH 10

mutex_t move_clients_mutex;

/* avl tree helper */
static int _compare_clients(void *compare_arg, void *a, void *b);
static void _parse_audio_info (source_t *source, const char *s);
static void source_shutdown (source_t *source);
static void process_listeners (source_t *source, int fast_clients_only, int deletion_expected);
#ifdef _WIN32
#define source_run_script(x,y)  WARN0("on [dis]connect scripts disabled");
#else
static void source_run_script (char *command, char *mountpoint);
#endif

/* Allocate a new source with the stated mountpoint, if one already
 * exists with that mountpoint in the global source tree then return
 * NULL.
 */
source_t *source_reserve (const char *mount)
{
    source_t *src = NULL;

    do
    {
        avl_tree_wlock (global.source_tree);
        src = source_find_mount_raw (mount);
        if (src)
        {
            src = NULL;
            break;
        }

        src = calloc (1, sizeof(source_t));
        if (src == NULL)
            break;

        /* make duplicates for strings or similar */
        src->mount = strdup (mount);
        src->max_listeners = -1;

        src->pending_clients_tail = &src->pending_clients;

        thread_mutex_create (src->mount, &src->lock);

        avl_insert (global.source_tree, src);

    } while (0);

    avl_tree_unlock (global.source_tree);
    return src;
}


/* Find a mount with this raw name - ignoring fallbacks. You should have the
 * global source tree locked to call this.
 */
source_t *source_find_mount_raw(const char *mount)
{
    source_t *source;
    avl_node *node;
    int cmp;

    if (!mount) {
        return NULL;
    }
    /* get the root node */
    node = global.source_tree->root->right;
    
    while (node) {
        source = (source_t *)node->key;
        cmp = strcmp(mount, source->mount);
        if (cmp < 0) 
            node = node->left;
        else if (cmp > 0)
            node = node->right;
        else
            return source;
    }
    
    /* didn't find it */
    return NULL;
}


/* Search for mount, if the mount is there but not currently running then
 * check the fallback, and so on.  Must have a global source lock to call
 * this function.
 */
source_t *source_find_mount (const char *mount)
{
    source_t *source = NULL;
    ice_config_t *config;
    mount_proxy *mountinfo;
    int depth = 0;

    config = config_get_config();
    while (mount != NULL)
    {
        /* limit the number of times through, maybe infinite */
        if (depth > MAX_FALLBACK_DEPTH)
        {
            source = NULL;
            break;
        }

        source = source_find_mount_raw(mount);
        if (source)
        {
            if (source->running)
                break;
            if (source->on_demand)
                break;
        }

        /* source is not running, meaning that the fallback is not configured
           within the source, we need to check the mount list */
        mountinfo = config->mounts;
        source = NULL;
        while (mountinfo)
        {
            if (strcmp (mountinfo->mountname, mount) == 0)
                break;
            mountinfo = mountinfo->next;
        }
        if (mountinfo)
            mount = mountinfo->fallback_mount;
        else
            mount = NULL;
        depth++;
    }

    config_release_config();
    return source;
}


int source_compare_sources(void *arg, void *a, void *b)
{
    source_t *srca = (source_t *)a;
    source_t *srcb = (source_t *)b;

    return strcmp(srca->mount, srcb->mount);
}


void source_clear_source (source_t *source)
{
    DEBUG1 ("clearing source \"%s\"", source->mount);
    client_destroy(source->client);
    source->client = NULL;
    source->parser = NULL;
    source->con = NULL;

    if (source->dumpfile)
    {
        INFO1 ("Closing dumpfile for %s", source->mount);
        fclose (source->dumpfile);
        source->dumpfile = NULL;
    }

    /* lets drop any clients still connected */
    while (source->active_clients)
    {
        client_t *client = source->active_clients;
        source->active_clients = client->next;
        source_free_client (source, client);
    }
    while (source->pending_clients)
    {
        client_t *client = source->pending_clients;
        source->pending_clients = client->next;
        source_free_client (source, client);
    }
    source->pending_clients_tail = &source->pending_clients;
    source->first_normal_client = NULL;

    /* flush out the stream data, we don't want any left over */
    while (source->stream_data)
    {
        refbuf_t *p = source->stream_data;
        source->stream_data = p->next;
        /* can be referenced by burst handler as well */
        while (p->_count > 1)
            refbuf_release (p);
        refbuf_release (p);
    }
    source->stream_data_tail = NULL;

    if (source->format && source->format->free_plugin)
    {
        source->format->free_plugin (source->format);
    }
    source->format = NULL;

    auth_clear (source->authenticator);

    source->burst_point = NULL;
    source->burst_size = 0;
    source->burst_offset = 0;
    source->queue_size = 0;
    source->queue_size_limit = 0;
    source->listeners = 0;
    source->no_mount = 0;
    source->shoutcast_compat = 0;
    source->max_listeners = -1;
    source->yp_public = 0;
    source->yp_prevent = 0;
    source->hidden = 0;
    util_dict_free (source->audio_info);
    source->audio_info = NULL;

    free(source->dumpfilename);
    source->dumpfilename = NULL;

    if (source->intro_file)
    {
        fclose (source->intro_file);
        source->intro_file = NULL;
    }

    free (source->on_connect);
    source->on_connect = NULL;

    free (source->on_disconnect);
    source->on_disconnect = NULL;

    source->on_demand_req = 0;
}


/* Remove the provided source from the global tree and free it */
void source_free_source (source_t *source)
{
    DEBUG1 ("freeing source \"%s\"", source->mount);
    avl_tree_wlock (global.source_tree);
    avl_delete (global.source_tree, source, NULL);
    avl_tree_unlock (global.source_tree);

    free(source->fallback_mount);
    source->fallback_mount = NULL;

    /* make sure all YP entries have gone */
    yp_remove (source->mount);

    /* There should be no listeners on this mount */
    if (source->active_clients)
        WARN1("active listeners on mountpoint %s", source->mount);

    if (source->pending_clients)
        WARN1("pending listeners on mountpoint %s", source->mount);

    thread_mutex_destroy (&source->lock);

    free (source->mount);
    free (source);

    return;
}


client_t *source_find_client(source_t *source, int id)
{
    client_t fakeclient, *client = NULL;
    connection_t fakecon;

    fakeclient.con = &fakecon;
    fakeclient.con->id = id;

    client = source->active_clients;
    while (client)
    {
        if (_compare_clients (NULL, client, &fakeclient) == 0)
            break;
        client = client->next;
    }

    return client;
}
    

/* Move clients from source to dest provided dest is running
 * and that the stream format is the same.
 * The only lock that should be held when this is called is the
 * source tree lock
 */
void source_move_clients (source_t *source, source_t *dest)
{
    unsigned int count = 0;
    /* we don't want the two write locks to deadlock in here */
    thread_mutex_lock (&move_clients_mutex);
    thread_mutex_lock (&dest->lock);

    /* if the destination is not running then we can't move clients */
    if (dest->running == 0 && dest->on_demand == 0)
    {
        WARN1 ("destination mount %s not running, unable to move clients ", dest->mount);
        thread_mutex_unlock (&dest->lock);
        thread_mutex_unlock (&move_clients_mutex);
        return;
    }

    do
    {
        thread_mutex_lock (&source->lock);

        if (source->on_demand == 0 && source->format == NULL)
        {
            INFO1 ("source mount %s is not available", source->mount);
            break;
        }
        if (source->format && dest->format)
        {
            if (source->format->type != dest->format->type)
            {
                WARN2 ("stream %s and %s are of different types, ignored", source->mount, dest->mount);
                break;
            }
        }

        /* we need to move the client and pending trees */
        while (source->active_clients)
        {
            client_t *client = source->active_clients;
            source->active_clients = client->next;

            /* when switching a client to a different queue, be wary of the 
             * refbuf it's referring to, if it's http headers then we need
             * to write them so don't release it.
             */
            if (client->write_to_client != format_http_write_to_client)
            {
                client_set_queue (client, NULL);
                client->write_to_client = NULL;
            }

            *dest->pending_clients_tail = client;
            dest->pending_clients_tail = &client->next;
            count++;
        }
        if (count != source->listeners)
            WARN2 ("count %u, listeners %u", count, source->listeners);
        count = 0;
        while (source->pending_clients)
        {
            client_t *client = source->pending_clients;
            source->pending_clients = client->next;
            /* clients on pending have a unique refbuf containing headers
             * so no need to change them here */
            *dest->pending_clients_tail = client;
            dest->pending_clients_tail = &client->next;
            count++;
        }
        source->pending_clients_tail = &source->pending_clients;
        if (count != source->new_listeners)
            WARN2 ("count %u, new listeners %u", count, source->new_listeners);
        
        count = source->listeners + source->new_listeners;
        INFO2 ("passing %d listeners to \"%s\"", count, dest->mount);

        dest->new_listeners += count;
        source->listeners = 0;
        source->new_listeners = 0;
        stats_event (source->mount, "listeners", "0");

    } while (0);

    thread_mutex_unlock (&source->lock);
    /* see if we need to wake up an on-demand relay */
    if (dest->running == 0 && dest->on_demand && count)
    {
        dest->on_demand_req = 1;
        slave_rescan();
    }
    thread_mutex_unlock (&dest->lock);
    thread_mutex_unlock (&move_clients_mutex);
}


/* clients need to be start from somewhere in the queue
 * so we will look for a refbuf which has been previous
 * marked as a sync point */
static void find_client_start (source_t *source, client_t *client)
{
    refbuf_t *refbuf = source->burst_point;

    while (refbuf)
    {
        if (refbuf->sync_point)
        {
            client_set_queue (client, refbuf);
            break;
        }
        refbuf = refbuf->next;
    }
}

/* get some data from the source. The stream data is placed in a refbuf
 * and sent back, however NULL is also valid as in the case of a short
 * timeout and there's no data pending.
 */
static void get_next_buffer (source_t *source)
{
    refbuf_t *refbuf = NULL;
    int no_delay_count = 0;

    while (global.running == ICE_RUNNING && source->running)
    {
        int fds = 0;
        time_t current = time(NULL);
        int delay = 200;

        /* service fast clients but jump out once in a while to check on
         * normal clients */
        if (no_delay_count < 10)
        {
            if (source->active_clients != source->first_normal_client)
            {
                delay = 0;
                no_delay_count++;
            }
        }
        else
            return;

        thread_mutex_unlock (&source->lock);

        if (source->con)
            fds = util_timed_wait_for_fd (source->con->sock, delay);
        else
        {
            thread_sleep (delay*1000);
            source->last_read = current;
        }

        /* take the lock */
        thread_mutex_lock (&source->lock);

        if (source->recheck_settings)
        {
            ice_config_t *config = config_get_config();
            source_update_settings (config, source);
            config_release_config ();
        }
        if (fds < 0)
        {
            if (! sock_recoverable (sock_error()))
            {
                WARN0 ("Error while waiting on socket, Disconnecting source");
                source->running = 0;
            }
            continue;
        }

        if (fds == 0)
        {
            if (source->last_read + (time_t)source->timeout < current)
            {
                WARN0 ("Disconnecting source due to socket timeout");
                source->running = 0;
                break;
            }
            if (delay == 0)
            {
                process_listeners (source, 1, 0);
                continue;
            }
            break;
        }
        source->last_read = current;
        refbuf = source->format->get_buffer (source);
        if (refbuf)
        {
            /* append buffer to the in-flight data queue,  */
            if (source->stream_data == NULL)
            {
                source->stream_data = refbuf;
                source->burst_point = refbuf;
                source->burst_offset = 0;
            }
            if (source->stream_data_tail)
                source->stream_data_tail->next = refbuf;
            source->stream_data_tail = refbuf;
            source->queue_size += refbuf->len;
            refbuf_addref (refbuf);

            /* move the starting point for new listeners */
            source->burst_offset += refbuf->len;
            if (source->burst_offset > source->burst_size)
            {
                if (source->burst_point->next)
                {
                    refbuf_t *to_go = source->burst_point;

                    source->burst_offset -= source->burst_point->len;
                    source->burst_point = source->burst_point->next;
                    refbuf_release (to_go);
                }
            }

            /* save stream to file */
            if (source->dumpfile && source->format->write_buf_to_file)
                source->format->write_buf_to_file (source, refbuf);
        }
        break;
    }
}


/* general send routine per listener.  The deletion_expected tells us whether
 * the last in the queue is about to disappear, so if this client is still
 * referring to it after writing then drop the client as it's fallen too far
 * behind.
 *
 * return 1 for client should be specially handled, either removed or placed
 *          elsewhere
 *        0 for normal case.
 */
static int send_to_listener (source_t *source, client_t *client, int deletion_expected)
{
    int bytes;
    int loop = 20;   /* max number of iterations in one go */
    int total_written = 0;
    int ret = 1;

    /* new users need somewhere to start from */
    if (client->refbuf == NULL)
    {
        find_client_start (source, client);
        if (client->refbuf == NULL)
            return 0;
    }

    while (1)
    {
        /* jump out if client connection has died */
        if (client->con->error)
            break;

        /* lets not send too much to one client in one go, but don't
           sleep for too long if more data can be sent */
        if (total_written > 20000 || loop == 0)
            break;

        loop--;

        bytes = client->write_to_client (source, client);
        if (bytes <= 0)
        {
            ret = 0;
            break;  /* can't write any more */
        }

        total_written += bytes;
    }

    /* the refbuf referenced at head (last in queue) may be marked for deletion
     * if so, check to see if this client is still referring to it */
    if (deletion_expected && client->refbuf == source->stream_data)
    {
        DEBUG0("Client has fallen too far behind, removing");
        client->con->error = 1;
        ret = 1;
    }
    return ret;
}


static void process_listeners (source_t *source, int fast_clients_only, int deletion_expected)
{
    client_t *sentinel = NULL, *client, **client_p;
    unsigned int listeners = source->listeners;

    if (fast_clients_only)
    {
        sentinel = source->first_normal_client;
    }

    source->first_normal_client = source->active_clients;

    client = source->active_clients;
    client_p = &source->active_clients;
    while (client && client != sentinel)
    {
        int fast_client = send_to_listener (source, client, deletion_expected);

        if (fast_client)
        {
            client_t *to_go = client;

            *client_p = client->next;
            client = client->next;

            if (source->first_normal_client == to_go)
            {
                source->first_normal_client = to_go->next;
            }

            if (to_go->con->error)
            {
                source_free_client (source, to_go);
                source->listeners--;
                DEBUG0("Client removed");
            }
            else
            {
                /* move fast clients to beginning of list */
                if (client_p == &source->active_clients)
                    client_p = &to_go->next;

                to_go->next = source->active_clients;
                source->active_clients = to_go;
            }
        }
        else
        {
            client_p = &client->next;
            client = client->next;
        }
    }
    /* has the listener count changed */
    if (source->listeners != listeners)
    {
        INFO2("listener count on %s now %d", source->mount, source->listeners);
        stats_event_args (source->mount, "listeners", "%d", source->listeners);
        if (source->listeners == 0 && source->on_demand)
            source->running = 0;
    }
}


static void process_pending_clients (source_t *source)
{
    unsigned count = 0;
    client_t *client = source->pending_clients;

    while (client)
    {
        client_t *to_go = client;

        client = client->next;
        /*  trap from when clients have been moved */
        if (to_go->write_to_client == NULL)
        {
            /* trap for client moved to fallback file */
            if (source->file_only)
            {
                to_go->write_to_client = format_intro_write_to_client;
                client_set_queue (to_go, refbuf_new(4096));
                to_go->intro_offset = 0;
                to_go->pos = 4096;
            }
            else
            {
                to_go->write_to_client = source->format->write_buf_to_client;
                client_set_queue (to_go, source->stream_data_tail);
            }
        }

        to_go->next = source->active_clients;
        source->active_clients = to_go;

        count++;
        source->new_listeners--;
    }
    source->pending_clients = NULL;
    source->pending_clients_tail = &source->pending_clients;

    if (count)
    {
        DEBUG1("Adding %d client(s)", count);
        source->listeners += count;
        stats_event_args (source->mount, "listeners", "%d", source->listeners);
    }
}


static void source_init (source_t *source)
{
    char *str = NULL;

    thread_mutex_lock (&source->lock);
    do
    {
        str = "0";
        if (source->yp_prevent)
            break;
        if ((str = httpp_getvar (source->parser, "ice-public")))
            break;
        if ((str = httpp_getvar (source->parser, "icy-pub")))
            break;
        /* handle header from icecast v2 release */
        if ((str = httpp_getvar (source->parser, "icy-public")))
            break;
        str = "0";
    } while (0);
    source->yp_public = atoi (str);
    stats_event (source->mount, "public", str);

    str = httpp_getvar(source->parser, "ice-genre");
    if (str == NULL)
        str = httpp_getvar(source->parser, "icy-genre");
    stats_event (source->mount, "genre", str);

    str = httpp_getvar(source->parser, "ice-description");
    if (str == NULL)
        str = httpp_getvar(source->parser, "icy-description");
    stats_event (source->mount, "server_description", str);

    str = httpp_getvar(source->parser, "ice-name");
    if (str == NULL)
        str = httpp_getvar(source->parser, "icy-name");
    stats_event (source->mount, "server_name", str);

    if (source->dumpfilename != NULL)
    {
        source->dumpfile = fopen (source->dumpfilename, "ab");
        if (source->dumpfile == NULL)
        {
            WARN2("Cannot open dump file \"%s\" for appending: %s, disabling.",
                    source->dumpfilename, strerror(errno));
        }
    }

    /* grab a read lock, to make sure we get a chance to cleanup */
    thread_rwlock_rlock (source->shutdown_rwlock);

    /* start off the statistics */
    source->listeners = 0;
    stats_event_inc (NULL, "sources");
    stats_event_inc (NULL, "source_total_connections");

    if (source->con)
        sock_set_blocking (source->con->sock, SOCK_NONBLOCK);

    DEBUG0("Source creation complete");
    source->last_read = time (NULL);
    source->running = 1;

    source->audio_info = util_dict_new();
    str = httpp_getvar(source->parser, "ice-audio-info");
    if (str)
    {
        _parse_audio_info (source, str);
        stats_event (source->mount, "audio_info", str);
    }

    thread_mutex_unlock (&source->lock);

    if (source->on_connect)
        source_run_script (source->on_connect, source->mount);

    /*
    ** Now, if we have a fallback source and override is on, we want
    ** to steal its clients, because it means we've come back online
    ** after a failure and they should be gotten back from the waiting
    ** loop or jingle track or whatever the fallback is used for
    */

    if (source->fallback_override && source->fallback_mount)
    {
        source_t *fallback_source;

        avl_tree_rlock(global.source_tree);
        fallback_source = source_find_mount(source->fallback_mount);

        if (fallback_source)
            source_move_clients (fallback_source, source);

        avl_tree_unlock(global.source_tree);
    }
    slave_rebuild ();
    thread_mutex_lock (&source->lock);
    if (source->yp_public)
        yp_add (source);
}



void source_main (source_t *source)
{
    source_init (source);

    while (global.running == ICE_RUNNING && source->running)
    {
        int remove_from_q;

        get_next_buffer (source);

        remove_from_q = 0;

        /* lets see if we have too much data in the queue, but do not
           remove it until later */
        if (source->queue_size > source->queue_size_limit)
            remove_from_q = 1;

        /* add pending clients */
        if (source->pending_clients)
            process_pending_clients (source);

        process_listeners (source, 0, remove_from_q);

        /* lets reduce the queue, any lagging clients should of been
         * terminated by now
         */
        if (source->stream_data)
        {
            /* normal unreferenced queue data will have a refcount 1, but
             * burst queue data will be at least 2, active clients will also
             * increase refcount */
            while (source->stream_data->_count == 1)
            {
                refbuf_t *to_go = source->stream_data;

                if (to_go->next == NULL || source->burst_point == to_go)
                {
                    /* this should not happen */
                    ERROR0 ("queue state is unexpected");
                    source->running = 0;
                    break;
                }
                source->stream_data = to_go->next;
                source->queue_size -= to_go->len;
                refbuf_release (to_go);
            }
        }
    }
    source->running = 0;

    source_shutdown (source);
}


static void source_shutdown (source_t *source)
{
    INFO1("Source \"%s\" exiting", source->mount);
    source->running = 0;

    if (source->on_disconnect)
        source_run_script (source->on_disconnect, source->mount);

    /* we have de-activated the source now, so no more clients will be
     * added, now move the listeners we have to the fallback (if any)
     */
    if (source->fallback_mount)
    {
        source_t *fallback_source;

        avl_tree_rlock(global.source_tree);
        fallback_source = source_find_mount (source->fallback_mount);

        if (fallback_source != NULL)
        {
            /* be careful wrt to deadlocking */
            thread_mutex_unlock (&source->lock);
            source_move_clients (source, fallback_source);
            thread_mutex_lock (&source->lock);
        }

        avl_tree_unlock (global.source_tree);
    }

    /* delete this sources stats */
    stats_event_dec (NULL, "sources");
    stats_event(source->mount, NULL, NULL);

    /* we don't remove the source from the tree here, it may be a relay and
       therefore reserved */
    source_clear_source (source);

    thread_mutex_unlock (&source->lock);

    global_lock();
    global.sources--;
    global_unlock();

    /* release our hold on the lock so the main thread can continue cleaning up */
    thread_rwlock_unlock(source->shutdown_rwlock);
}


static int _compare_clients(void *compare_arg, void *a, void *b)
{
    client_t *clienta = (client_t *)a;
    client_t *clientb = (client_t *)b;

    connection_t *cona = clienta->con;
    connection_t *conb = clientb->con;

    if (cona->id < conb->id) return -1;
    if (cona->id > conb->id) return 1;

    return 0;
}


int source_free_client (source_t *source, client_t *client)
{
    global_lock();
    global.clients--;
    global_unlock();
    stats_event_dec(NULL, "clients");

    if (source && source->authenticator && source->authenticator->release_client)
    {
        source->authenticator->release_client (source, client);
        return 0;
    }
    /* if no response has been sent then send a 404 */
    if (client->respcode == 0)
        client_send_404 (client, "Mount unavailable");
    else
        client_destroy (client);
    
    return 1;
}

static void _parse_audio_info (source_t *source, const char *s)
{
    const char *start = s;
    unsigned int len;

    while (start != NULL && *start != '\0')
    {
        if ((s = strchr (start, ';')) == NULL)
            len = strlen (start);
        else
        {
            len = (int)(s - start);
            s++; /* skip passed the ';' */
        }
        if (len)
        {
            char name[100], value[200];
            char *esc;

            sscanf (start, "%99[^=]=%199[^;\r\n]", name, value);
            esc = util_url_unescape (value);
            if (esc)
            {
                if (source->running)
                {
                    util_dict_set (source->audio_info, name, esc);
                    stats_event (source->mount, name, esc);
                }
                free (esc);
            }
        }
        start = s;
    }
}


static void source_apply_mount (source_t *source, mount_proxy *mountinfo)
{
    DEBUG1 ("Applying mount information for \"%s\"", source->mount);
    source->max_listeners = mountinfo->max_listeners;
    source->fallback_override = mountinfo->fallback_override;
    source->no_mount = mountinfo->no_mount;
    source->hidden = mountinfo->hidden;

    if (mountinfo->fallback_mount)
    {
        free (source->fallback_mount);
        source->fallback_mount = strdup (mountinfo->fallback_mount);
    }

    if (mountinfo->auth_type != NULL)
    {
        source->authenticator = auth_get_authenticator(
                mountinfo->auth_type, mountinfo->auth_options);
        stats_event(source->mount, "authenticator", mountinfo->auth_type);
    }
    if (mountinfo->dumpfile)
    {
        free (source->dumpfilename);
        source->dumpfilename = strdup (mountinfo->dumpfile);
    }
    if (source->intro_file)
        fclose (source->intro_file);
    if (mountinfo->intro_filename)
    {
        ice_config_t *config = config_get_config_unlocked ();
        unsigned int len  = strlen (config->webroot_dir) +
            strlen (mountinfo->intro_filename) + 1;
        char *path = malloc (len);
        if (path)
        {
            snprintf (path, len, "%s%s", config->webroot_dir,
                    mountinfo->intro_filename);

            source->intro_file = fopen (path, "rb");
            if (source->intro_file == NULL)
                WARN2 ("Cannot open intro file \"%s\": %s", path, strerror(errno));
            free (path);
        }
    }

    if (mountinfo->queue_size_limit)
        source->queue_size_limit = mountinfo->queue_size_limit;

    if (mountinfo->source_timeout)
        source->timeout = mountinfo->source_timeout;

    if (mountinfo->burst_size >= 0)
        source->burst_size = (unsigned int)mountinfo->burst_size;

    if (mountinfo->fallback_when_full)
        source->fallback_when_full = mountinfo->fallback_when_full;

    if (mountinfo->no_yp)
        source->yp_prevent = 1;

    if (mountinfo->on_connect)
    {
        free (source->on_connect);
        source->on_connect = strdup(mountinfo->on_connect);
    }

    if (mountinfo->on_disconnect)
    {
        free (source->on_disconnect);
        source->on_disconnect = strdup(mountinfo->on_disconnect);
    }
    if (source->format && source->format->apply_settings)
        source->format->apply_settings (source, mountinfo);
}


void source_update_settings (ice_config_t *config, source_t *source)
{
    mount_proxy *mountproxy = config->mounts;

    /* set global settings first */
    source->queue_size_limit = config->queue_size_limit;
    source->timeout = config->source_timeout;
    source->burst_size = config->burst_size;

    source->dumpfilename = NULL;
    auth_clear (source->authenticator);
    source->authenticator = NULL;

    while (mountproxy)
    {
        if (strcmp (mountproxy->mountname, source->mount) == 0)
        {
            if (source->file_only)
                WARN1 ("skipping fallback to file \"%s\"", source->mount);
            else
                source_apply_mount (source, mountproxy);
            break;
        }
        mountproxy = mountproxy->next;
    }
    if (source->fallback_mount)
        DEBUG1 ("fallback %s", source->fallback_mount);
    if (source->dumpfilename)
        DEBUG1 ("Dumping stream to %s", source->dumpfilename);
    if (source->yp_prevent)
        DEBUG0 ("preventing YP listings");
    if (source->on_connect)
        DEBUG1 ("connect script \"%s\"", source->on_connect);
    if (source->on_disconnect)
        DEBUG1 ("disconnect script \"%s\"", source->on_disconnect);
    if (source->on_demand)
        DEBUG0 ("on-demand set");
    if (source->hidden)
    {
        stats_event_hidden (source->mount, NULL, 1);
        DEBUG0 ("hidden from xsl");
    }
    else
        stats_event_hidden (source->mount, NULL, 0);

    if (source->file_only)
        stats_event (source->mount, "file_only", "1");
    if (source->max_listeners == -1)
        stats_event (source->mount, "max_listeners", "unlimited");
    else
    {
        char buf [10];
        snprintf (buf, sizeof (buf), "%lu", source->max_listeners);
        stats_event (source->mount, "max_listeners", buf);
    }
    if (source->on_demand)
        stats_event (source->mount, "on-demand", "1");
    else
        stats_event (source->mount, "on-demand", NULL);

    DEBUG1 ("max listeners to %d", source->max_listeners);
    DEBUG1 ("queue size to %u", source->queue_size_limit);
    DEBUG1 ("burst size to %u", source->burst_size);
    DEBUG1 ("source timeout to %u", source->timeout);
    DEBUG1 ("fallback_when_full to %u", source->fallback_when_full);
    source->recheck_settings = 0;
}


void source_update (ice_config_t *config)
{
    avl_node *node;
    char limit [20];

    snprintf (limit, sizeof (limit), "%d", config->client_limit);
    stats_event (NULL, "client_limit", limit);
    snprintf (limit, sizeof (limit), "%d", config->source_limit);
    stats_event (NULL, "source_limit", limit);

    avl_tree_rlock (global.source_tree);
    node = avl_get_first (global.source_tree);
    while (node)
    {
        source_t *source = node->key;

        /* we can't lock the source as we have config locked, so flag the
         * source for updating */
        source->recheck_settings = 1;

        node = avl_get_next (node);
    }

    avl_tree_unlock (global.source_tree);
}


void *source_client_thread (void *arg)
{
    source_t *source = arg;
    const char ok_msg[] = "HTTP/1.0 200 OK\r\n\r\n";
    int bytes = sizeof (ok_msg)-1;

    if (source->client)
    {
        source->client->respcode = 200;
        bytes = sock_write_bytes (source->client->con->sock, ok_msg, sizeof (ok_msg)-1);
    }
    if (bytes < (int)sizeof (ok_msg)-1)
    {
        global_lock();
        global.sources--;
        global_unlock();
        WARN0 ("Error writing 200 OK message to source client");
        source_free_source (source);
    }
    else
    {
        if (source->client)
            source->client->con->sent_bytes += bytes;

        stats_event_inc(NULL, "source_client_connections");
        stats_event (source->mount, "listeners", "0");
        source_main (source);
        source_free_source (source);

        slave_rebuild ();
    }
    return NULL;
}


#ifndef _WIN32
static void source_run_script (char *command, char *mountpoint)
{
    pid_t pid, external_pid;

    /* do a fork twice so that the command has init as parent */
    external_pid = fork();
    switch (external_pid)
    {
        case 0:
            switch (pid = fork ())
            {
                case -1:
                    ERROR2 ("Unable to fork %s (%s)", command, strerror (errno));
                    break;
                case 0:  /* child */
                    DEBUG1 ("Starting command %s", command);
                    execl (command, command, mountpoint, NULL);
                    ERROR2 ("Unable to run command %s (%s)", command, strerror (errno));
                    exit(0);
                default: /* parent */
                    break;
            }
            exit (0);
        case -1:
            ERROR1 ("Unable to fork %s", strerror (errno));
            break;
        default: /* parent */
            waitpid (external_pid, NULL, 0);
            break;
    }
}
#endif


static void *source_fallback_file (void *arg)
{
    char *mount = arg;
    const char *type;
    char *path;
    unsigned int len;
    FILE *file = NULL;
    source_t *source = NULL;
    ice_config_t *config;

    do
    {
        if (mount == NULL || mount[0] != '/')
            break;
        config = config_get_config();
        len  = strlen (config->webroot_dir) + strlen (mount) + 1;
        path = malloc (len);
        if (path)
            snprintf (path, len, "%s%s", config->webroot_dir, mount);
        
        config_release_config ();
        if (path == NULL)
            break;

        file = fopen (path, "rb");
        if (file == NULL)
        {
            DEBUG1 ("unable to open file \"%s\"", path);
            free (path);
            break;
        }
        free (path);
        source = source_reserve (mount);
        if (source == NULL)
        {
            DEBUG1 ("mountpoint \"%s\" already reserved", mount);
            break;
        }
        type = fserve_content_type (mount);
        source->parser = httpp_create_parser();
        httpp_initialize (source->parser, NULL);
        httpp_setvar (source->parser, "content-type", type);
        source->hidden = 1;
        source->file_only = 1;
        source->yp_prevent = 1;
        source->intro_file = file;
        file = NULL;

        if (connection_complete_source (source) < 0)
            break;
        source_client_thread (source);
    } while (0);
    if (file)
        fclose (file);
    free (mount);
    return NULL;
}


/* rescan the mount list, so that xsl files are updated to show
 * unconnected but active fallback mountpoints
 */
void source_recheck_mounts (void)
{
    ice_config_t *config = config_get_config();
    mount_proxy *mount = config->mounts;

    avl_tree_rlock (global.source_tree);

    while (mount)
    {
        int update_stats = 0;
        int hidden;
        source_t *source = source_find_mount (mount->mountname);

        hidden = mount->hidden;
        if (source)
        {
            /* something is active, maybe a fallback */
            if (strcmp (source->mount, mount->mountname) == 0)
            {
                /* normally the source thread would deal with this there
                 * isn't one for inactive on-demand relays */
                if (source->on_demand && source->running == 0)
                    update_stats = 1;
            }
            else
                update_stats = 1;
        }
        else
            stats_event (mount->mountname, NULL, NULL);
        if (update_stats)
        {
            source = source_find_mount_raw (mount->mountname);
            if (source)
                source_update_settings (config, source);
            else
            {
                stats_event_hidden (mount->mountname, NULL, hidden);
                stats_event (mount->mountname, "listeners", "0");
                if (mount->max_listeners < 0)
                    stats_event (mount->mountname, "max_listeners", "unlimited");
                else
                    stats_event_args (mount->mountname, "max_listeners", "%d", mount->max_listeners);
            }
        }
        /* check for fallback to file */
        if (global.running == ICE_RUNNING && mount->fallback_mount)
        {
            source_t *fallback = source_find_mount (mount->fallback_mount);
            if (fallback == NULL)
            {
                thread_create ("Fallback file thread", source_fallback_file,
                        strdup (mount->fallback_mount), THREAD_DETACHED);
            }
        }

        mount = mount->next;
    }
    avl_tree_unlock (global.source_tree);
    config_release_config();
}

