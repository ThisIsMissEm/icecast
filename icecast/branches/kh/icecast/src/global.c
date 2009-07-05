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

#include <string.h>

#include "thread/thread.h"
#include "avl/avl.h"
#include "timing/timing.h"

#include "connection.h"
#include "refbuf.h"
#include "client.h"
#include "source.h"
#include "format.h"

#include "global.h"

ice_global_t global;

static mutex_t _global_mutex;

void global_initialize(void)
{
    global.server_sockets = 0;
    global.relays = NULL;
    global.master_relays = NULL;
    global.running = 0;
    global.clients = 0;
    global.sources = 0;
    global.source_tree = avl_tree_new(source_compare_sources, NULL);
    thread_mutex_create(&_global_mutex);
    thread_spin_create (&global.spinlock);
    thread_rwlock_create (&global.shutdown_lock);
    global.out_bitrate = rate_setup (151, 1000);
}

void global_shutdown(void)
{
    thread_mutex_destroy(&_global_mutex);
    thread_spin_destroy (&global.spinlock);
    thread_rwlock_destroy (&global.shutdown_lock);
    avl_tree_free(global.source_tree, NULL);
    rate_free (global.out_bitrate);
    global.out_bitrate = NULL;
}

void global_lock(void)
{
    thread_mutex_lock(&_global_mutex);
}

void global_unlock(void)
{
    thread_mutex_unlock(&_global_mutex);
}

void global_add_bitrates (struct rate_calc *rate, unsigned long value)
{
    thread_spin_lock (&global.spinlock);
    rate_add (rate, value, timing_get_time());
    thread_spin_unlock (&global.spinlock);
}

void global_reduce_bitrate_sampling (struct rate_calc *rate)
{
    thread_spin_lock (&global.spinlock);
    rate_reduce (rate, 0);
    thread_spin_unlock (&global.spinlock);
}

unsigned long global_getrate_avg (struct rate_calc *rate)
{
    unsigned long v;
    thread_spin_lock (&global.spinlock);
    v = rate_avg (rate);
    thread_spin_unlock (&global.spinlock);
    return v;
}

