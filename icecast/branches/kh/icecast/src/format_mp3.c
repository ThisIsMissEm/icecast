/* -*- c-basic-offset: 4; -*- */
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

/* format_mp3.c
**
** format plugin for mp3
**
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
# include <strings.h>
#endif

#include "refbuf.h"
#include "source.h"
#include "client.h"

#include "stats.h"
#include "format.h"
#include "httpp/httpp.h"

#include "logging.h"

#include "format_mp3.h"
#include "mpeg.h"

#define CATMODULE "format-mp3"

/* Note that this seems to be 8192 in shoutcast - perhaps we want to be the
 * same for compability with crappy clients?
 */
#define ICY_METADATA_INTERVAL 16000

static void format_mp3_free_plugin(format_plugin_t *plugin, client_t *client);
static refbuf_t *mp3_get_filter_meta (source_t *source);
static refbuf_t *mp3_get_no_meta (source_t *source);

static int  format_mp3_create_client_data (format_plugin_t *plugin, client_t *client);
static void free_mp3_client_data (client_t *client);
static int  format_mp3_write_buf_to_client(client_t *client);
static void write_mp3_to_file (struct source_tag *source, refbuf_t *refbuf);
static void mp3_set_tag (format_plugin_t *plugin, const char *tag, const char *in_value, const char *charset);
static void format_mp3_apply_settings (format_plugin_t *format, mount_proxy *mount);


typedef struct {
   refbuf_t *associated;
   unsigned short interval;
   short metadata_offset;
   unsigned short since_meta_block;
} mp3_client_data;

/* client format flags */
#define CLIENT_IN_METADATA          CLIENT_FORMAT_BIT
#define CLIENT_USING_BLANK_META     (CLIENT_FORMAT_BIT<<1)

static refbuf_t blank_meta = { 17, 1, "\001StreamTitle='';", NULL, NULL, 0 };


int format_mp3_get_plugin (format_plugin_t *plugin, client_t *client)
{
    const char *metadata;
    mp3_state *state = calloc(1, sizeof(mp3_state));
    refbuf_t *meta;
    const char *s;

    plugin->get_buffer = mp3_get_no_meta;
    plugin->write_buf_to_client = format_mp3_write_buf_to_client;
    plugin->write_buf_to_file = write_mp3_to_file;
    plugin->create_client_data = format_mp3_create_client_data;
    plugin->free_plugin = format_mp3_free_plugin;
    plugin->set_tag = mp3_set_tag;
    plugin->apply_settings = format_mp3_apply_settings;

    s = httpp_getvar (plugin->parser, "content-type");
    if (s)
        plugin->contenttype = strdup (s);
    else
        /* We default to MP3 audio for old clients without content types */
        plugin->contenttype = strdup ("audio/mpeg");

    plugin->_state = state;

    /* initial metadata needs to be blank for sending to clients and for
       comparing with new metadata */
    meta = refbuf_new (17);
    memcpy (meta->data, "\001StreamTitle='';", 17);
    state->metadata = meta;
    state->interval = -1;

    metadata = httpp_getvar (plugin->parser, "icy-metaint");
    if (metadata)
    {
        state->inline_metadata_interval = atoi (metadata);
        if (state->inline_metadata_interval > 0)
        {
            state->offset = 0;
            plugin->get_buffer = mp3_get_filter_meta;
            state->interval = state->inline_metadata_interval;
        }
    }
    if (client)
    {
        if (plugin->type == FORMAT_TYPE_AAC || plugin->type == FORMAT_TYPE_MPEG)
        {
            client->format_data = malloc (sizeof (mpeg_sync));
            mpeg_setup (client->format_data);
        }
    }

    return 0;
}


static void mp3_set_tag (format_plugin_t *plugin, const char *tag, const char *in_value, const char *charset)
{
    mp3_state *source_mp3 = plugin->_state;
    char *value = NULL;

    if (tag==NULL)
    {
        source_mp3->update_metadata = 1;
        return;
    }

    if (in_value)
    {
        value = util_conv_string (in_value, charset, plugin->charset);
        if (value == NULL)
            value = strdup (in_value);
    }

    if (strcmp (tag, "title") == 0 || strcmp (tag, "song") == 0)
    {
        if (*tag == 's')
        {
            /* song typically includes artist */
            free (source_mp3->url_artist);
            source_mp3->url_artist = NULL;
            stats_event (plugin->mount, "artist", NULL);
        }
        free (source_mp3->url_title);
        source_mp3->url_title = value;
        stats_event (plugin->mount, "title", value);
        stats_event_time (plugin->mount, "metadata_updated");
    }
    else if (strcmp (tag, "artist") == 0)
    {
        free (source_mp3->url_artist);
        source_mp3->url_artist = value;
        stats_event (plugin->mount, "artist", value);
    }
    else if (strcmp (tag, "url") == 0)
    {
        free (source_mp3->url);
        source_mp3->url = value;
    }
    else
        free (value);
}


static char *filter_shoutcast_metadata (source_t *source, char *metadata, size_t meta_len)
{
    char *p = NULL;
    char *end;
    int len;

    do
    {
        if (metadata == NULL)
            break;
        metadata++;
        if (strncmp (metadata, "StreamTitle='", 13))
            break;
        if ((end = strstr (metadata+13, "\';")) == NULL)
            break;
        len = (end - metadata) - 13;
        p = calloc (1, len+1);
        if (p)
            memcpy (p, metadata+13, len);
    } while (0);
    return p;
}


static void format_mp3_apply_settings (format_plugin_t *format, mount_proxy *mount)
{
    mp3_state *source_mp3 = format->_state;

    source_mp3->interval = -1;
    free (format->charset);
    format->charset = NULL;
    source_mp3->queue_block_size = 1400;

    if (mount)
    {
        if (mount->mp3_meta_interval >= 0)
            source_mp3->interval = mount->mp3_meta_interval;
        if (mount->charset)
            format->charset = strdup (mount->charset);
        if (mount->queue_block_size)
            source_mp3->queue_block_size = mount->queue_block_size;
    }
    if (source_mp3->interval < 0)
    {
        const char *metadata = httpp_getvar (format->parser, "icy-metaint");
        source_mp3->interval = ICY_METADATA_INTERVAL;
        if (metadata)
        {
            int interval = atoi (metadata);
            if (interval > 0)
                source_mp3->interval = interval;
        }
    }
    if (format->charset == NULL)
        format->charset = strdup ("ISO8859-1");

    DEBUG1 ("sending metadata interval %d", source_mp3->interval);
    DEBUG1 ("charset %s", format->charset);
}


/* called from the source thread when the metadata has been updated.
 * The artist title are checked and made ready for clients to send
 */
static void mp3_set_title (source_t *source)
{
    const char streamtitle[] = "StreamTitle='";
    const char streamurl[] = "StreamUrl='";
    size_t size;
    unsigned char len_byte;
    refbuf_t *p;
    unsigned int len = sizeof(streamtitle) + 2; /* the StreamTitle, quotes, ; and null */
    mp3_state *source_mp3 = source->format->_state;

    /* work out message length */
    if (source_mp3->url_artist)
        len += strlen (source_mp3->url_artist);
    if (source_mp3->url_title)
        len += strlen (source_mp3->url_title);
    if (source_mp3->url_artist && source_mp3->url_title)
        len += 3;
    if (source_mp3->inline_url)
    {
        char *end = strstr (source_mp3->inline_url, "';");
        if (end)
            len += end - source_mp3->inline_url+2;
    }
    else if (source_mp3->url)
        len += strlen (source_mp3->url) + strlen (streamurl) + 2;
#define MAX_META_LEN 255*16
    if (len > MAX_META_LEN)
    {
        WARN1 ("Metadata too long at %d chars", len);
        return;
    }
    /* work out the metadata len byte */
    len_byte = (len-1) / 16 + 1;

    /* now we know how much space to allocate, +1 for the len byte */
    size = len_byte * 16 + 1;

    p = refbuf_new (size);
    if (p)
    {
        mp3_state *source_mp3 = source->format->_state;
        int r;
        char *title;

        memset (p->data, '\0', size);
        if (source_mp3->url_artist && source_mp3->url_title)
            r = snprintf (p->data, size, "%c%s%s - %s';", len_byte, streamtitle,
                    source_mp3->url_artist, source_mp3->url_title);
        else
            r = snprintf (p->data, size, "%c%s%s';", len_byte, streamtitle,
                    source_mp3->url_title);
        if (r > 0)
        {
            if (source_mp3->inline_url)
            {
                char *end = strstr (source_mp3->inline_url, "';");
                int urllen = size;
                if (end) urllen = end - source_mp3->inline_url + 2;
                if (size-r > urllen)
                    snprintf (p->data+r, size-r, "StreamUrl='%s';", source_mp3->inline_url+11);
            }
            else if (source_mp3->url)
                snprintf (p->data+r, size-r, "StreamUrl='%s';", source_mp3->url);
        }
        DEBUG1 ("shoutcast metadata block setup with %s", p->data+1);
        title = filter_shoutcast_metadata (source, p->data, size);
        logging_playlist (source->mount, title, source->listeners);
        yp_touch (source->mount);
        free (title);

        refbuf_release (source_mp3->metadata);
        source_mp3->metadata = p;
    }
}


/* send the appropriate metadata, and return the number of bytes written
 * which is 0 or greater.  Check the client in_metadata value afterwards
 * to see if all metadata has been sent
 */
static int send_stream_metadata (client_t *client, refbuf_t *refbuf, unsigned int remaining)
{
    int ret = 0;
    char *metadata;
    int meta_len;
    refbuf_t *associated = refbuf->associated;
    mp3_client_data *client_mp3 = client->format_data;

    /* If there is a change in metadata then send it else
     * send a single zero value byte in its place
     */
    if (associated && associated != client_mp3->associated)
    {
        metadata = associated->data + client_mp3->metadata_offset;
        meta_len = associated->len - client_mp3->metadata_offset;
        client->flags &= ~CLIENT_USING_BLANK_META;
        refbuf_release (client_mp3->associated);
        refbuf_addref (associated);
        client_mp3->associated = associated;
    }
    else
    {
        if (associated || ((client->flags & CLIENT_USING_BLANK_META) &&
                    client_mp3->metadata_offset == 0))
        {
            metadata = "\0";
            meta_len = 1;
        }
        else
        {
            char *meta = blank_meta.data;
            metadata = meta + client_mp3->metadata_offset;
            meta_len = blank_meta.len - client_mp3->metadata_offset;
            client->flags |= CLIENT_USING_BLANK_META;
            refbuf_release (client_mp3->associated);
            client_mp3->associated = NULL;
        }
    }
    /* if there is normal stream data to send as well as metadata then try
     * to merge them into 1 write call */
    if (remaining)
    {
        char *merge = malloc (remaining + meta_len);
        memcpy (merge, refbuf->data + client->pos, remaining);
        memcpy (merge+remaining, metadata, meta_len);

        ret = client_send_bytes (client, merge, remaining+meta_len);
        free (merge);

        if (ret >= (int)remaining)
        {
            /* did we write all of it? */
            if (ret < (int)remaining + meta_len)
            {
                client_mp3->metadata_offset += (ret - remaining);
                client->flags |= CLIENT_IN_METADATA;
                client->schedule_ms += 300;
            }
            client_mp3->since_meta_block = 0;
            client->pos += remaining;
            client->queue_pos += remaining;
            return ret;
        }
        client->schedule_ms += 300;
        if (ret > 0)
        {
            client_mp3->since_meta_block += ret;
            client->pos += ret;
            client->queue_pos += ret;
        }
        return ret > 0 ? ret : 0;
    }
    ret = client_send_bytes (client, metadata, meta_len);

    if (ret == meta_len)
    {
        client_mp3->metadata_offset = 0;
        client->flags &= ~CLIENT_IN_METADATA;
        client_mp3->since_meta_block = 0;
        return ret;
    }
    if (ret > 0)
        client_mp3->metadata_offset += ret;
    client->schedule_ms += 300;
    client->flags |= CLIENT_IN_METADATA;

    return ret > 0 ? ret : 0;
}


/* Handler for writing mp3 data to a client, taking into account whether
 * client has requested shoutcast style metadata updates
 */
static int format_mp3_write_buf_to_client (client_t *client) 
{
    int ret, written = 0;
    mp3_client_data *client_mp3 = client->format_data;
    refbuf_t *refbuf = client->refbuf;
    char *buf = refbuf->data + client->pos;
    size_t len = refbuf->len - client->pos;

    do
    {
        /* send any unwritten metadata to the client */
        if (client->flags & CLIENT_IN_METADATA)
        {
            ret = send_stream_metadata (client, refbuf, 0);

            if (client->flags & CLIENT_IN_METADATA)
                break;
            written += ret;
        }
        /* see if we need to send the current metadata to the client */
        if (client_mp3->interval)
        {
            size_t remaining = client_mp3->interval -
                client_mp3->since_meta_block;

            /* leading up to sending the metadata block */
            if (remaining <= len)
            {
                ret = send_stream_metadata (client, refbuf, remaining);
                if (client->flags & CLIENT_IN_METADATA)
                    break;
                written += ret;
                buf += remaining;
                len -= remaining;
                /* limit how much mp3 we send if using small intervals */
                if (len > client_mp3->interval)
                    len = client_mp3->interval;
            }
        }
        /* write any mp3, maybe after the metadata block */
        if (len)
        {
            ret = client_send_bytes (client, buf, len);

            if (ret > 0)
            {
                client_mp3->since_meta_block += ret;
                client->pos += ret;
                client->queue_pos += ret;
            }
            if (ret < (int)len)
                break;
            written += ret;
        }
        ret = 0;
    } while (0);

    if (ret < 0)
        client->schedule_ms += 250;
    if (ret > 0)
        written += ret;
    return written == 0 ? -1 : written;
}

static void format_mp3_free_plugin (format_plugin_t *plugin, client_t *client)
{
    /* free the plugin instance */
    mp3_state *format_mp3 = plugin->_state;

    if (client)
    {
        mpeg_cleanup (client->format_data);
        free (client->format_data);
        client->format_data = NULL;
    }
    free (format_mp3->url_artist);
    free (format_mp3->url_title);
    free (format_mp3->url);
    refbuf_release (format_mp3->metadata);
    refbuf_release (format_mp3->read_data);
    free (plugin->contenttype);
    free (format_mp3);
}


/* This does the actual reading, making sure the read data is packaged in
 * blocks of 1400 bytes (near the common MTU size). This is because many
 * incoming streams come in small packets which could waste a lot of 
 * bandwidth with many listeners due to headers and such like.
 */
static int complete_read (source_t *source)
{
    int bytes = 0;
    format_plugin_t *format = source->format;
    mp3_state *source_mp3 = format->_state;
    char *buf;
    refbuf_t *refbuf;
    client_t *client = source->client;

    if (source_mp3->read_data == NULL)
    {
        source_mp3->read_data = refbuf_new (source_mp3->queue_block_size);
        source_mp3->read_count = 0;
    }
    buf = source_mp3->read_data->data + source_mp3->read_count;
    if (source_mp3->read_count < source_mp3->queue_block_size)
    {
        int read_in = source_mp3->queue_block_size - source_mp3->read_count;
        bytes = client_read_bytes (client, buf, read_in);
        if (bytes < 0)
            return 0;
        rate_add (format->in_bitrate, bytes, client->worker->current_time.tv_sec);
    }
    source_mp3->read_count += bytes;
    refbuf = source_mp3->read_data;
    refbuf->len = source_mp3->read_count;
    format->read_bytes += bytes;

    if (source_mp3->read_count < source_mp3->queue_block_size)
        return 0;
    return 1;
}


static int validate_mpeg (mp3_state *source_mp3, mpeg_sync *mpeg_sync, refbuf_t *refbuf)
{
    int unprocessed = mpeg_complete_frames (mpeg_sync, refbuf, 0);
    if (unprocessed < 0)
        return -1;
    if (unprocessed > 0)
    {
        /* make sure the new block has a minimum of queue_block_size */
        size_t len = unprocessed > source_mp3->queue_block_size ? unprocessed : source_mp3->queue_block_size;
        refbuf_t *leftover = refbuf_new (len);
        memcpy (leftover->data, refbuf->data + refbuf->len, unprocessed);
        leftover->len = unprocessed;

        if (source_mp3->inline_metadata_interval > 0)
        {
            /* inline metadata may need reading before the rest of the mpeg data */
            if (source_mp3->build_metadata_len == 0 && source_mp3->offset > unprocessed)
            {
                source_mp3->offset -= unprocessed;
                source_mp3->read_data = leftover;
                source_mp3->read_count = unprocessed;
            }
            else
                mpeg_data_insert (mpeg_sync, leftover); /* will need to merge this after metadata */
        }
        else
        {
            source_mp3->read_data = leftover;
            source_mp3->read_count = unprocessed;
        }
    }
    return 0;
}


/* read an mp3 stream which does not have shoutcast style metadata */
static refbuf_t *mp3_get_no_meta (source_t *source)
{
    refbuf_t *refbuf;
    mp3_state *source_mp3 = source->format->_state;
    client_t *client = source->client;  // maybe move mp3_state into client instead of plugin?

    if (complete_read (source) == 0)
        return NULL;

    refbuf = source_mp3->read_data;
    source_mp3->read_data = NULL;

    if (client->format_data && validate_mpeg (source_mp3, client->format_data, refbuf) < 0)
    {
        refbuf_release (refbuf);
        return NULL;
    }
    source->client->queue_pos += refbuf->len;
    if (source_mp3->update_metadata)
    {
        mp3_set_title (source);
        source_mp3->update_metadata = 0;
    }
    refbuf->associated = source_mp3->metadata;
    refbuf_addref (source_mp3->metadata);
    refbuf->flags |= SOURCE_BLOCK_SYNC;
    return refbuf;
}


/* read mp3 data with inlined metadata from the source. Filter out the
 * metadata so that the mp3 data itself is store on the queue and the
 * metadata is is associated with it
 */
static refbuf_t *mp3_get_filter_meta (source_t *source)
{
    refbuf_t *refbuf;
    format_plugin_t *plugin = source->format;
    mp3_state *source_mp3 = plugin->_state;
    client_t *client = source->client;  // maybe move mp3_state into client instead of plugin?
    unsigned char *src;
    unsigned int bytes, mp3_block;

    if (complete_read (source) == 0)
        return NULL;

    refbuf = source_mp3->read_data;
    source_mp3->read_data = NULL;
    src = (unsigned char *)refbuf->data;

    if (source_mp3->update_metadata)
    {
        mp3_set_title (source);
        source_mp3->update_metadata = 0;
    }
    /* fill the buffer with the read data */
    bytes = source_mp3->read_count;
    refbuf->len = 0;
    while (bytes > 0)
    {
        unsigned int metadata_remaining;

        mp3_block = source_mp3->inline_metadata_interval - source_mp3->offset;

        /* is there only enough to account for mp3 data */
        if (bytes <= mp3_block)
        {
            refbuf->len += bytes;
            source_mp3->offset += bytes;
            break;
        }
        /* we have enough data to get to the metadata
         * block, but only transfer upto it */
        if (mp3_block)
        {
            src += mp3_block;
            bytes -= mp3_block;
            refbuf->len += mp3_block;
            source_mp3->offset += mp3_block;
            continue;
        }

        /* process the inline metadata, len == 0 indicates not seen any yet */
        if (source_mp3->build_metadata_len == 0)
        {
            memset (source_mp3->build_metadata, 0,
                    sizeof (source_mp3->build_metadata));
            source_mp3->build_metadata_offset = 0;
            source_mp3->build_metadata_len = 1 + (*src * 16);
        }

        /* do we have all of the metatdata block */
        metadata_remaining = source_mp3->build_metadata_len -
            source_mp3->build_metadata_offset;
        if (bytes < metadata_remaining)
        {
            memcpy (source_mp3->build_metadata +
                    source_mp3->build_metadata_offset, src, bytes);
            source_mp3->build_metadata_offset += bytes;
            break;
        }
        /* copy all bytes except the last one, that way we 
         * know a null byte terminates the message */
        memcpy (source_mp3->build_metadata + source_mp3->build_metadata_offset,
                src, metadata_remaining-1);

        /* overwrite metadata in the buffer */
        bytes -= metadata_remaining;
        memmove (src, src+metadata_remaining, bytes);

        /* assign metadata if it's greater than 1 byte, and the text has changed */
        if (source_mp3->build_metadata_len > 1 &&
                strcmp (source_mp3->build_metadata+1, source_mp3->metadata->data+1) != 0)
        {
            refbuf_t *meta = refbuf_new (source_mp3->build_metadata_len);
            memcpy (meta->data, source_mp3->build_metadata,
                    source_mp3->build_metadata_len);

            DEBUG1("shoutcast metadata %.4080s", meta->data+1);
            if (strncmp (meta->data+1, "StreamTitle=", 12) == 0)
            {
                char *title = filter_shoutcast_metadata (source,
                        source_mp3->build_metadata, source_mp3->build_metadata_len);
                logging_playlist (source->mount, title, source->listeners);
                stats_event_conv (source->mount, "title", title, source->format->charset);
                stats_event_time (source->mount, "metadata_updated");
                yp_touch (source->mount);
                free (title);
                refbuf_release (source_mp3->metadata);
                source_mp3->metadata = meta;
                source_mp3->inline_url = strstr (meta->data+1, "StreamUrl='");
            }
            else
            {
                ERROR0 ("Incorrect metadata format, ending stream");
                source->flags &= ~SOURCE_RUNNING;
                refbuf_release (refbuf);
                refbuf_release (meta);
                return NULL;
            }
        }
        source_mp3->offset = 0;
        source_mp3->build_metadata_len = 0;
    }
    /* the data we have just read may of just been metadata */
    if (refbuf->len == 0)
    {
        refbuf_release (refbuf);
        return NULL;
    }
    if (client->format_data && validate_mpeg (source_mp3, client->format_data, refbuf) < 0)
    {
        refbuf_release (refbuf);
        return NULL;
    }
    source->client->queue_pos += refbuf->len;
    refbuf->associated = source_mp3->metadata;
    refbuf_addref (source_mp3->metadata);
    refbuf->flags |= SOURCE_BLOCK_SYNC;

    return refbuf;
}


static int format_mp3_create_client_data (format_plugin_t *plugin, client_t *client)
{
    mp3_client_data *client_mp3 = calloc(1,sizeof(mp3_client_data));
    mp3_state *source_mp3 = plugin->_state;
    const char *metadata;
    size_t  remaining;
    char *ptr;
    int bytes;
    const char *useragent;

    if (client_mp3 == NULL)
        return -1;

    client->format_data = client_mp3;
    client->free_client_data = free_mp3_client_data;
    client->refbuf->len = 0;

    if (format_general_headers (plugin, client) < 0)
        return -1;

    client->refbuf->len -= 2;
    remaining = 4096 - client->refbuf->len;
    ptr = client->refbuf->data + client->refbuf->len;

    /* hack for flash player, it wants a length.  It has also been reported that the useragent
     * appears as MSIE if run in internet explorer */
    useragent = httpp_getvar (client->parser, "user-agent");
    if (httpp_getvar(client->parser, "x-flash-version") ||
            (useragent && strstr(useragent, "MSIE")))
    {
        bytes = snprintf (ptr, remaining, "Content-Length: 221183499\r\n");
        remaining -= bytes;
        ptr += bytes;
        /* avoid browser caching, reported via forum */
        bytes = snprintf (ptr, remaining, "Expires: Mon, 26 Jul 1997 05:00:00 GMT\r\n");
        remaining -= bytes;
        ptr += bytes; 
    }

    /* check for shoutcast style metadata inserts */
    metadata = httpp_getvar(client->parser, "icy-metadata");
    if (metadata && atoi(metadata))
    {
        if (source_mp3->interval >= 0)
            client_mp3->interval = source_mp3->interval;
        else
            client_mp3->interval = ICY_METADATA_INTERVAL;
        if (client_mp3->interval)
        {
            bytes = snprintf (ptr, remaining, "icy-metaint:%u\r\n",
                    client_mp3->interval);
            if (bytes > 0)
            {
                remaining -= bytes;
                ptr += bytes;
            }
        }
    }
    bytes = snprintf (ptr, remaining, "\r\n");
    remaining -= bytes;
    ptr += bytes;

    client->refbuf->len = 4096 - remaining;

    return 0;
}


static void free_mp3_client_data (client_t *client)
{
    mp3_client_data *client_mp3 = client->format_data;

    refbuf_release (client_mp3->associated);
    free (client->format_data);
    client->format_data = NULL;
}


static void write_mp3_to_file (struct source_tag *source, refbuf_t *refbuf)
{
    if (refbuf->len == 0)
        return;
    if (fwrite (refbuf->data, 1, refbuf->len, source->dumpfile) < refbuf->len)
    {
        WARN0 ("Write to dump file failed, disabling");
        fclose (source->dumpfile);
        source->dumpfile = NULL;
    }
}

