/* om_shout.c
 *
 * - Output module for sending to shout streams
 *
 * Copyright (c) 2002 Karl Heyes <karl@xiph.org>
 *
 * This program is distributed under the terms of the GNU General
 * Public License, version 2. You may use, modify, and redistribute
 * it under the terms of this license. A copy should be included
 * with this source. */

#include <config.h>
#include <string.h>
#include <ogg/ogg.h>

#include <cfgparse.h>

#include <om_shout.h>

#define MODULE "om_shout/"
#include <logging.h>


#define SHOUT_TIMEOUT 10

#define DEFAULT_HOSTNAME "localhost"
#define DEFAULT_PORT 8000
#define DEFAULT_PASSWORD "password"
#define DEFAULT_USERNAME "source"
#define DEFAULT_MOUNT "/stream.ogg"
#define DEFAULT_RECONN_DELAY 30
#define DEFAULT_RECONN_ATTEMPTS -1


static void _output_connection_close (struct output_module *mod)
{
    struct output_shout_state *stream = mod->specific;
    LOG_DEBUG0("closed shout connection");
    shout_close (stream->shout);
    shout_free (stream->shout);
    stream->shout = NULL;
    if (stream->connected)
    {
        if (mod->in_use)
            ogg_stream_clear (&mod->os);

        stream->connected = 0;
    }
    stream->restart_time = time(NULL) + stream->reconnect_delay;
    stream->reconnect_count++;
}


void check_shout_connected (struct output_module *mod)
{
    struct output_shout_state *stream = mod->specific;
    if (shout_connection_ready (stream->shout))
        return;
    if (stream->reconnect_attempts == -1 ||
            stream->reconnect_attempts > stream->reconnect_count)
    {
        time_t now = time (NULL);

        if (stream->restart_time <= now)
        {
            int shouterr;

            if (stream->shout == NULL || shout_get_errno (stream->shout) != SHOUTERR_PENDING)
            {
                char audio_info[11];
                int failed = 1;
                struct output_state *state = mod->parent;

                LOG_DEBUG3 ("Time we started stream on %s:%d%s", stream->hostname, 
                        stream->port, stream->mount);
                /* allow for traping long icecast connects */
                stream->restart_time = now;

                do
                {
                    if ((stream->shout = shout_new ()) == NULL)
                        break;
                    if (shout_set_host (stream->shout, stream->hostname) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_password (stream->shout, stream->password) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_port (stream->shout, stream->port) != SHOUTERR_SUCCESS)
                        break;
                    if (stream->user && shout_set_user (stream->shout, stream->user) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_mount (stream->shout, stream->mount) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_agent (stream->shout, PACKAGE_STRING) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_name (stream->shout, state->name) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_genre (stream->shout, state->genre) != SHOUTERR_SUCCESS)
                        break;
                    if (state->url && shout_set_url (stream->shout, state->url) != SHOUTERR_SUCCESS)
                        break;
                    if (shout_set_description (stream->shout, state->description) != SHOUTERR_SUCCESS)
                        break;
                    if (stream->yp && shout_set_public (stream->shout, 1) != SHOUTERR_SUCCESS)
                        break;

                    shout_set_nonblocking (stream->shout, 1);
                    shout_set_format (stream->shout, SHOUT_FORMAT_VORBISPAGE);
                    shout_set_protocol (stream->shout, SHOUT_PROTOCOL_HTTP);

                    snprintf(audio_info, sizeof(audio_info), "%ld", state->vi.bitrate_nominal/1000);
                    shout_set_audio_info (stream->shout, SHOUT_AI_BITRATE, audio_info);

                    snprintf(audio_info, sizeof(audio_info), "%ld", state->vi.rate);
                    shout_set_audio_info (stream->shout, SHOUT_AI_SAMPLERATE, audio_info);

                    snprintf(audio_info, sizeof(audio_info), "%d", state->vi.channels);
                    shout_set_audio_info (stream->shout, SHOUT_AI_CHANNELS, audio_info);
                    failed = 0;
                }
                while (0);
                if (failed)
                {
                    _output_connection_close (mod);
                    return;
                }
            }
            if ((shouterr = shout_open(stream->shout)) == SHOUTERR_SUCCESS)
            {
                LOG_INFO3("Connected to server: %s:%d%s",
                        shout_get_host(stream->shout), shout_get_port(stream->shout), shout_get_mount(stream->shout));
                stream->connected = 1;
                stream->reconnect_count = 0;
                mod->need_headers = 1;
                return;
            }
            if (shouterr == SHOUTERR_PENDING)
            {
                if (now - stream->restart_time > SHOUT_TIMEOUT)
                {
                    LOG_ERROR3("Terminating connection to %s:%d%s",
                            shout_get_host(stream->shout), shout_get_port(stream->shout),
                            shout_get_mount(stream->shout));
                    LOG_ERROR1("no reply came in %d seconds", SHOUT_TIMEOUT);
                    _output_connection_close (mod);
                }
                /* ok, we just need to come back to this connection later */
            }
            else
            {
                LOG_ERROR4("Failed to connect to %s:%d%s (%s)",
                        shout_get_host(stream->shout), shout_get_port(stream->shout),
                        shout_get_mount(stream->shout), shout_get_error(stream->shout));
                _output_connection_close (mod);
            }
        }
        return;
    }
    LOG_INFO1 ("%d reconnect attempts, will keep re-trying", stream->reconnect_count);
}


static int shout_audio_pageout (struct output_module *mod, ogg_page *page)
{
    struct output_shout_state *stream = mod->specific;
    int ret;
    uint64_t samples;

    if (mod->initial_packets)
    {
        mod->initial_packets--;
        if (mod->initial_packets == 0)
            ret = ogg_stream_flush (&mod->os, page);
        else
            ret = 0;
    }
    else if (stream->page_samples > (uint64_t)mod->parent->vi.rate)
        ret = ogg_stream_flush (&mod->os, page);
    else
        ret = ogg_stream_pageout (&mod->os, page);

    if (ret > 0)
    {
        samples = ogg_page_granulepos (page) - stream->prev_page_granulepos;
        stream->page_samples -= samples;
        stream->prev_page_granulepos = ogg_page_granulepos (page);
    }

    return ret;
}


int output_ogg_shout (struct output_module *mod, ogg_packet *op, unsigned samples)
{
    struct output_shout_state *stream = mod->specific;
    check_shout_connected (mod);
    if (stream->connected)
    {
        int send_it = 1;
        ogg_page page;
        long packetno = op->packetno;
        ogg_int64_t granule = op->granulepos;
        struct output_state *state = mod->parent;

        op -> packetno = mod->packetno++;

        if (state->new_headers || mod->need_headers)
        {
            int val = mod->serial;
            /* start of new logical stream */
            LOG_DEBUG0 ("initialising output stream");
            if (mod->in_use)
            {
                ogg_stream_clear (&mod->os);
            }
            while (val == mod->serial)
                val = rand();
            mod->serial = val;
            ogg_stream_init (&mod->os, val);
            mod->in_use = 1;
            ogg_stream_packetin (&mod->os, &state->packets[0]);
            ogg_stream_packetin (&mod->os, &state->packets[1]);
            ogg_stream_packetin (&mod->os, &state->packets[2]);
            mod->need_headers = 0;
            mod->start_pos = granule;
            stream->prev_page_granulepos = 0;
            stream->prev_packet_granulepos = 0;

            while (ogg_stream_flush (&mod->os, &page) > 0)
            {
                if (send_it && shout_send (stream->shout, &page, 1) != SHOUTERR_SUCCESS)
                {
                    send_it = 0;
                    LOG_ERROR4("Failed to write headers to %s:%d%s (%s)",
                            shout_get_host (stream->shout), shout_get_port (stream->shout),
                            shout_get_mount (stream->shout), shout_get_error (stream->shout));
                    _output_connection_close (mod);
                    return 0;
                }
            }
            mod->packetno = 3;
            mod->initial_packets = 2; /* flush 2 packets into a single page */
        }

        op->granulepos -= mod->start_pos;
        ogg_stream_packetin (&mod->os, op);

        stream->page_samples += (op->granulepos - stream->prev_packet_granulepos);
        stream->prev_packet_granulepos = op->granulepos;

        while (shout_audio_pageout (mod, &page) > 0)
        {
            if (send_it && shout_send (stream->shout, &page, 1) != SHOUTERR_SUCCESS)
            {
                send_it = 0;
                LOG_ERROR4("Failed to write to %s:%d%s (%s)",
                        shout_get_host (stream->shout), shout_get_port (stream->shout),
                        shout_get_mount (stream->shout), shout_get_error (stream->shout));
                _output_connection_close (mod);
                return 0;
            }
        }
        /* reset to what it was */
        op->packetno = packetno;
        op->granulepos = granule;
    }
    return 0;
}



static void output_shout_clear (struct output_module *mod)
{
    if (mod)
    {
        struct output_shout_state *stream = mod->specific;
        _output_connection_close (mod);
        shout_free (stream->shout);
        xmlFree (stream->hostname);
        xmlFree (stream->user);
        xmlFree (stream->password);
        xmlFree (stream->mount);
        free (stream);
    }
}



int parse_shout (xmlNodePtr node, void *arg)
{
    struct output_state *state = arg;
    char *hostname   = NULL,
         *user       = xmlStrdup (DEFAULT_USERNAME),
         *password   = xmlStrdup (DEFAULT_PASSWORD),
         *mount      = xmlStrdup (DEFAULT_MOUNT);
    int  yp          = 0,
         reconnect_delay = DEFAULT_RECONN_DELAY,
         reconnect_attempts = DEFAULT_RECONN_ATTEMPTS,
         port        = 8000;

    struct cfg_tag shout_tags[] =
    {
        { "hostname",           get_xml_string, &hostname },
        { "port",               get_xml_int,    &port },
        { "password",           get_xml_string, &password },
        { "username",           get_xml_string, &user },
        { "mount",              get_xml_string, &mount },
        { "yp",                 get_xml_bool,   &yp },
        { "reconnectdelay",     get_xml_int,    &reconnect_delay },
        { "reconnectattempts",  get_xml_int,    &reconnect_attempts },
        { NULL, NULL, NULL }
    };

    if (parse_xml_tags ("shout", node->xmlChildrenNode, shout_tags))
        return 1;

    if (hostname == NULL)
        return 0;

    while (1)
    {
        struct output_shout_state *stream = NULL;
        struct output_module *mod = calloc (1, sizeof (struct output_module));
        if (mod == NULL)
            break;
        mod->parent = state;
        mod->output_send = output_ogg_shout;
        mod->output_clear = output_shout_clear;
        stream = calloc (1, sizeof (struct output_shout_state));
        if (stream == NULL)
            break;
        mod->specific = stream;
        stream->hostname = hostname;
        stream->user = user;
        stream->password = password;
        stream->mount = mount;
        stream->port = port;
        stream->yp = yp;

        stream->reconnect_delay = reconnect_delay;
        stream->reconnect_attempts = reconnect_attempts;

        /* add populated struct to list of outputs */
        mod->next = state->head;
        state->head = mod;
        return 0;
    }
    fprintf (stderr, "shout output failed\n");
    if (hostname)   xmlFree (hostname);
    if (user)       xmlFree (user);
    if (password)   xmlFree (password);
    if (mount)      xmlFree (mount); 
    return -1;
}

