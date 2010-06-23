/* -*- c-basic-offset: 4; -*- */
/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2009-2010,     Karl Heyes <karl@xiph.org>
 */

/* flv.c
 *
 * routines for processing an flv container
 *
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
#include "client.h"
#include "stats.h"

#include "logging.h"
#include "mpeg.h"
#include "format_mp3.h"

#define CATMODULE "flv"


struct flv
{
    int prev_tagsize;
    int block_pos;
    uint64_t prev_ms;
    int64_t samples;
    refbuf_t *seen_metadata;
    mpeg_sync mpeg_sync;
    unsigned char tag[30];
};

struct flvmeta
{
    int meta_pos;
    int arraylen;
};

#define FLVHEADER       11


static void flv_hdr (struct flv *flv, unsigned int len)
{
    unsigned long v = flv->prev_tagsize;

    v = flv->prev_tagsize;
    flv->tag [3] = v & 0xFF;
    v >>= 8;
    flv->tag [2] = v & 0xFF;
    v >>= 8;
    flv->tag [1] = v & 0xFF; // assume less than 2^24 

    v = len;
    flv->tag [7] = v & 0xFF;
    v >>= 8;
    flv->tag [6] = v & 0xFF;
    v >>= 8;
    flv->tag [5] = v & 0xFF;

    v = flv->prev_ms;
    flv->tag [10] = v & 0xFF;
    v >>= 8;
    flv->tag [9] = v & 0xFF;
    v >>= 8;
    flv->tag [8] = v & 0xFF;
    v >>= 8;
    flv->tag [11] = v & 0xFF;

}

/* Here we append to the scratch buffer each mp3 type frame. This frame includes the
 * header so with the flv header as well it becomes fairly wasteful but that is what
 * works.
 */
static int flv_mpX_hdr (struct mpeg_sync *mp, unsigned char *frame, unsigned int len)
{
    struct flv *flv = mp->callback_key;

    if (mp->raw_offset + len + 16 > mp->raw->len)
        return -1;
    flv_hdr (flv, len + 1);
    if (flv->tag[15] == 0x22)
    {
        /* it is unclear what the flv flags are for other samplerates */
        switch (mp->samplerate)
        {
            case 11025: flv->tag[15] |= (1<<2); break;
            case 22050: flv->tag[15] |= (2<<2); break;
            default:    flv->tag[15] |= (3<<2); break;
        }
        if (mp->channels == 2)
            flv->tag[15] |= 0x1;
    }
    memcpy (mp->raw->data + mp->raw_offset, &flv->tag[0], 16);
    flv->samples += mp->sample_count;
    flv->prev_ms = (flv->samples / (mp->samplerate/1000.0)) + 0.5;
    // The extra byte is for the flv audio id, usually 0x2F 
    flv->prev_tagsize = (len + FLVHEADER + 1);
    mp->raw_offset += 16;
    return 0;
}

/* Here we append to the scratch buffer each aac headerless frame. The flv tag data size
 * will be 2 bytes more than the frame for codes 0xAF and 0x1
 */
static int flv_aac_hdr (struct mpeg_sync *mp, unsigned char *frame, unsigned int len)
{
    struct flv *flv = mp->callback_key;

    if (mp->raw_offset + len + 17 > mp->raw->len)
        return -1;
    flv_hdr (flv, len + 2);
    // a single frame (headerless) follows this
    memcpy (mp->raw->data + mp->raw_offset, &flv->tag[0], 17);
    flv->samples += mp->sample_count;
    flv->prev_ms = (flv->samples / (mp->samplerate/1000.0)) + 0.5;
    // frame length + FLVHEADER + AVHEADER
    flv->prev_tagsize = (len + 11 + 2);
    mp->raw_offset += 17;
    return 0;
}


/* Simple function for generating codes for commonly used streams
 * players require this before they start playback. This is typically 2 bytes but can be more
 * 5 bits - codec details, we assume AAC LC here
 * 4 bits - samplerate table index.
 * 4 bits - channels table index.
 *
 */
static int  audio_specific_config (mpeg_sync *mp, unsigned char *p)
{
    int rates [] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0 };
    unsigned char count = 0;
    for (count = 0; rates[count] != mp->samplerate; count++)
    {
        if (rates [count] == 0)
            return 0;
    }
    p[0] = ((2 << 3) | (count >> 1));
    p[1] = ((unsigned char)(count << 7) | (mp->channels << 3));
    return 2;
}

static int flv_aac_firsthdr (struct mpeg_sync *mp, unsigned char *frame, unsigned int len)
{
    struct flv *flv = mp->callback_key;
    int c = audio_specific_config (&flv->mpeg_sync, &flv->tag[17]);

    flv_hdr (flv, 2+c);
    flv->tag[15] = 0xAF; // AAC audio, need these codes first
    flv->tag[16] = 0x0;
    memcpy (mp->raw->data, &flv->tag[0], 11+4+2+c);
    mp->raw_offset = 11+4+2+c;
    flv->prev_tagsize = 11 + 2 + c;
    flv->tag[16] = 0x01;   // as per spec. headerless frame follows this
    flv->mpeg_sync.frame_callback = flv_aac_hdr;
    // DEBUG2 ("codes for audiospecificconfig are %x, %x", flv->tag[17], flv->tag[18]);

    return flv_aac_hdr (mp, frame, len);
}


static int send_flv_buffer (client_t *client, struct flv *flv)
{
    int ret;
    char *buf = flv->mpeg_sync.raw->data + flv->block_pos;
    unsigned int len  = flv->mpeg_sync.raw_offset - flv->block_pos;

    if (len <= 0)
        return 0;
    ret = client_send_bytes (client, buf, len);
    if (ret < (int)len)
        client->schedule_ms += 300;
    if (ret > 0)
        flv->block_pos += ret;
    if (flv->block_pos == flv->mpeg_sync.raw_offset)
        flv->block_pos = flv->mpeg_sync.raw_offset = 0;
    return ret;
}


int write_flv_buf_to_client (client_t *client) 
{
    refbuf_t *ref = client->refbuf, *scmeta = ref->associated;
    mp3_client_data *client_mp3 = client->format_data;
    struct flv *flv = client_mp3->specific;
    int unprocessed, ret = -1;

    if (flv->block_pos)
        return send_flv_buffer (client, flv);

    /* check for metadata updates and insert if needed */
    if (flv->seen_metadata != scmeta)
    {
        /* the first assoc block is shoutcast meta, second is flv meta */
        refbuf_t *flvmeta = scmeta->associated;
        int len;
        char *src, *dst = flv->mpeg_sync.raw->data;

        if (flvmeta)
        {
            struct flvmeta *flvm = (struct flvmeta *)flvmeta->data;

            src = (char *)flvm + sizeof (*flvm);
            len  = flvm->meta_pos - sizeof (*flvm);
        }
        else
        {
            src="\002\000\012onMetaData"    // 13
                "\010\000\000\000\001"      //  5
                "\000\005title"             //  7
                "\002\000\001 "             //  4
                "\000\000\011";             //  3
            len = 32;
        }

        if (len + 15 < flv->mpeg_sync.raw->len)
        {
            unsigned char prev_type = flv->tag[4];
            flv->tag[4] = 18;    // metadata
            flv_hdr (flv, len);
            memcpy (dst, &flv->tag[0], 15);
            memcpy (dst+15, src, len);
            flv->mpeg_sync.raw_offset = len + 15;
            flv->prev_tagsize = len + 11;
            flv->tag[4] = prev_type;
            flv->seen_metadata = ref->associated;
            return send_flv_buffer (client, flv);
        }
        flv->seen_metadata = ref->associated;
    }

    unprocessed = mpeg_complete_frames (&flv->mpeg_sync, ref, client->pos);
    client->pos = ref->len;
    client->queue_pos += ref->len;
    if (unprocessed >= 0)
    {
        ret = send_flv_buffer (client, flv);
        if (unprocessed > 0)
            ref->len += unprocessed;   /* output was truncated, so revert changes */
    }
    return ret;
}


void flv_write_UI16 (unsigned char *p, unsigned val)
{
    p[1] = val & 0xFF;
    val >>= 8;
    p[0] = val & 0xFF;
}


refbuf_t *flv_meta_allocate (size_t len)
{
    char *ptr;
    refbuf_t *buffer = refbuf_new (sizeof (struct flvmeta) + len + 30);
    struct flvmeta *flvm = (struct flvmeta *)buffer->data;
    memset (flvm, 0, sizeof (struct flvmeta));
    ptr = buffer->data + sizeof (struct flvmeta);
    memcpy (ptr, "\002\000\012onMetaData", 13);
    memcpy (ptr+13, "\010\000\000\000\000", 5);
    flvm->meta_pos = sizeof (struct flvmeta) + 18;
    return buffer;
}


void flv_meta_append (refbuf_t *buffer, const char *tag, const char *value)
{
    struct flvmeta *flvm = (struct flvmeta *)buffer->data;
    int taglen = 0, valuelen = 0;
    unsigned char *ptr, *array_sizing;

    if (tag)   taglen = strlen (tag);
    if (value) valuelen = strlen (value);

    if (taglen + valuelen + 5 + flvm->meta_pos > buffer->len - 3)
        tag = NULL; // force end of the metadata
    if (tag == NULL)
    {
        memcpy (buffer->data+flvm->meta_pos, "\000\000\011", 3);
        flvm->meta_pos += 3;
        return;
    }
    array_sizing = (unsigned char *)buffer->data + sizeof (*flvm) +16; // 64k elements enough?
    ptr = (unsigned char *)buffer->data + flvm->meta_pos;

    flv_write_UI16 (ptr, taglen);
    memcpy (ptr+2, tag, taglen);
    ptr += (taglen + 2);
    *ptr = 0x02; // a text string
    ptr++;
    flv_write_UI16 (ptr, valuelen);
    memcpy (ptr+2, value, valuelen);
    ptr += (valuelen + 2);
    flvm->arraylen++;
    flv_write_UI16 (array_sizing, flvm->arraylen); // over 64k tags not handled
    flvm->meta_pos = (char*)ptr - buffer->data;
}


void flv_create_client_data (format_plugin_t *plugin, client_t *client)
{
    mp3_client_data *client_mp3 = client->format_data;
    struct flv *flv = calloc (1, sizeof (struct flv));
    int bytes;
    char *ptr = client->refbuf->data;

    mpeg_setup (&flv->mpeg_sync, plugin->mount);
    client_mp3->specific = flv;

    bytes = snprintf (ptr, 200, "HTTP/1.0 200 OK\r\n"
            "content-type: video/x-flv\r\n"
            "Cache-Control: no-cache\r\n"
            "Pragma: no-cache\r\n"
            "\r\n"
            "FLV\x1\x4%c%c%c\x9", 0,0,0);

    flv->mpeg_sync.raw = refbuf_new (4096);
    flv->tag[4] = 8;    // Audio details only
    if (plugin->type == FORMAT_TYPE_AAC)
    {
        flv->mpeg_sync.frame_callback = flv_aac_firsthdr;
    }
    if (plugin->type == FORMAT_TYPE_MPEG)
    {
        flv->tag[15] = 0x22; // MP3, specific settings not available yet
        flv->mpeg_sync.frame_callback = flv_mpX_hdr;
    }
    flv->mpeg_sync.callback_key = flv;

    client->respcode = 200;
    client->refbuf->len = bytes;
}


void free_flv_client_data (struct flv *flv)
{
    mpeg_cleanup (&flv->mpeg_sync);
}
