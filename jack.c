/*
 * Copyright (C) 2008 Mark Hills <mark@pogo.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <jack/jack.h>

#include "device.h"
#include "jack.h"
#include "player.h"
#include "timecoder.h"

#define MAX_DECKS 4
#define SCALE 32768


struct jack_t {
    jack_port_t *input_port[DEVICE_CHANNELS],
        *output_port[DEVICE_CHANNELS];
};

static jack_client_t *client = NULL;
static int rate, decks = 0;
struct device_t *device[MAX_DECKS];


/* Interleave samples from a set of JACK buffers into a local buffer */

static void interleave(signed short *buf, jack_default_audio_sample_t *jbuf[],
                       jack_nframes_t nframes)
{
    int n;
    while(nframes--) {
        for(n = 0; n < DEVICE_CHANNELS; n++) {
            *buf = (signed short)(*jbuf[n] * SCALE);
            buf++;
            jbuf[n]++;
        }
    }
}


/* Uninterleave samples from a local buffer into a set of JACK buffers */

static void uninterleave(jack_default_audio_sample_t *jbuf[],
                         signed short *buf, jack_nframes_t nframes)
{
    int n;
    while(nframes--) {
        for(n = 0; n < DEVICE_CHANNELS; n++) {
            *jbuf[n] = (jack_default_audio_sample_t)*buf / SCALE;
            buf++;
            jbuf[n]++;
        }
    }
}


/* Process the given number of frames of audio on input and output
 * of the given JACK device */

static void process_deck(struct device_t *dv, jack_nframes_t nframes)
{
    int n;
    signed short buf[1024 * 2];
    jack_default_audio_sample_t *jbuf[DEVICE_CHANNELS];
    struct jack_t *jack = (struct jack_t*)dv->local;

    /* Timecode input */

    for(n = 0; n < DEVICE_CHANNELS; n++) {
        jbuf[n] = jack_port_get_buffer(jack->input_port[n], nframes);
        assert(jbuf[n] != NULL);
    }

    interleave(buf, jbuf, nframes);
    if(dv->timecoder)
        timecoder_submit(dv->timecoder, buf, nframes, rate);

    /* Audio output */

    for(n = 0; n < DEVICE_CHANNELS; n++) {
        jbuf[n] = jack_port_get_buffer(jack->output_port[n], nframes);
        assert(jbuf[n] != NULL);
    }

    player_collect(dv->player, buf, nframes, rate);
    uninterleave(jbuf, buf, nframes);
}


/* Process callback, which triggers the processing of audio on all
 * decks controlled by this file */

static int process_callback(jack_nframes_t nframes, void *arg)
{
    int n;

    for(n = 0; n < decks; n++)
        process_deck(device[n], nframes);

    return 0;
}


/* Shutdown callback */

static void shutdown_callback(void *arg)
{
}


/* Initialise ourselves as a JACK client, called once per xwax
 * session, not per deck */

static int start_jack_client(void)
{
    const char *server_name;
    jack_status_t status;

    client = jack_client_open("xwax", JackNullOption, &status, &server_name);
    if(client == NULL) {
        if(status & JackServerFailed)
            fprintf(stderr, "JACK: Failed to connect\n");
        else
            fprintf(stderr, "jack_client_open: Failed (0x%x)\n", status);
        return -1;
    }

    if(jack_set_process_callback(client, process_callback, 0) != 0) {
        fprintf(stderr, "JACK: Failed to set process callback\n");
        return -1;
    }

    jack_on_shutdown(client, shutdown_callback, 0);

    rate = jack_get_sample_rate(client);
    fprintf(stderr, "JACK: %dHz\n", rate);

    return 0;
}


/* Register the JACK ports needed for a single deck */

static int register_ports(struct jack_t *jack, const char *name)
{
    int n;
    const char channel[] = { 'L', 'R' };
    char port_name[32];

    assert(DEVICE_CHANNELS == 2);
    for(n = 0; n < DEVICE_CHANNELS; n++) {
	sprintf(port_name, "%s_timecode_%c", name, channel[n]);
        jack->input_port[n] = jack_port_register(client, port_name,
                                                 JACK_DEFAULT_AUDIO_TYPE,
                                                 JackPortIsInput, 0);
	if(jack->input_port[n] == NULL) {
	    fprintf(stderr, "JACK: Failed to register timecode input port\n");
	    return -1;
	}
	sprintf(port_name, "%s_playback_%c", name, channel[n]);
	jack->output_port[n] = jack_port_register(client, port_name,
                                                  JACK_DEFAULT_AUDIO_TYPE,
                                                  JackPortIsOutput, 0);
	if(jack->output_port[n] == NULL) {
	    fprintf(stderr, "JACK: Failed to register audio playback port\n");
	    return -1;
	}
    }
    return 0;
}


/* Start audio rolling on this deck */

static int start(struct device_t *dv)
{
    /* On the first call to start, start audio rolling for all decks */

    if(jack_activate(client) != 0) {
        fprintf(stderr, "jack_activate: Failed\n");
        return -1;
    }
    return 0;
}


/* Stop audio rolling on this deck */

static int stop(struct device_t *dv)
{
    return 0;
}


/* Close JACK deck and any allocations */

static int clear(struct device_t *dv)
{
    free(dv->local);
    return 0;
}


/* Initialise a new JACK deck, creating a new JACK client if required,
 * and the approporiate input and output ports */

int jack_init(struct device_t *dv, const char *name)
{
    struct jack_t *jack;

    /* If this is the first JACK deck, initialise the global JACK services */

    if(client == NULL) {
        if(start_jack_client() == -1)
            return -1;
    }

    jack = malloc(sizeof(struct jack_t));
    if(!jack) {
        perror("malloc");
        return -1;
    }

    if(register_ports(jack, name) == -1)
	return -1;

    dv->local = jack;

    dv->pollfds = NULL;
    dv->handle = NULL;
    dv->start = start;
    dv->stop = stop;
    dv->clear = clear;

    assert(decks < MAX_DECKS);
    device[decks] = dv;
    decks++;

    return 0;

 fail:
    free(jack);
    return -1;
}