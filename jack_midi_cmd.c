/* JACK MIDI Commander
 *
 * Copyright (C) 2015 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define JACK_MIDI_QUEUE_SIZE (256)

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#ifndef WIN32
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#endif

static jack_port_t *midi_output_port = NULL;
static jack_client_t *j_client = NULL;

/* a simple state machine for this client */
static volatile enum {
	Run,
		Exit
} client_state = Run;

typedef struct my_midi_event {
	jack_nframes_t time;
	size_t size;
	jack_midi_data_t buffer[16];
} my_midi_event_t;

static my_midi_event_t event_queue[JACK_MIDI_QUEUE_SIZE];
static int queued_events_start = 0;
static int queued_events_end = 0;

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
	if (j_client) {
		jack_client_close (j_client);
		j_client=NULL;
	}
	fprintf(stderr, "bye.\n");
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
	void *out = jack_port_get_buffer(midi_output_port, nframes);
	jack_midi_clear_buffer(out);

	while (queued_events_end != queued_events_start) {
		jack_midi_event_write(out,
				event_queue[queued_events_end].time,
				event_queue[queued_events_end].buffer,
				event_queue[queued_events_end].size
				);
		queued_events_end = (queued_events_end + 1) % JACK_MIDI_QUEUE_SIZE;
	}
	return 0;
}

void jack_shutdown (void *arg) {
	fprintf(stderr,"recv. shutdown request from jackd.\n");
	client_state=Exit;
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
	jack_status_t status;
	j_client = jack_client_open (client_name, JackNullOption, &status);
	if (j_client == NULL) {
		fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return (-1);
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(j_client);
		fprintf (stderr, "jack-client name: `%s'\n", client_name);
	}
	jack_set_process_callback (j_client, process, 0);
	jack_on_shutdown (j_client, jack_shutdown, NULL);
	return (0);
}

static int jack_portsetup(void) {
	if ((midi_output_port = jack_port_register(j_client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
		fprintf (stderr, "cannot register midi ouput port !\n");
		return (-1);
	}
	return (0);
}

static void port_connect(char *midi_port) {
	if (midi_port && jack_connect(j_client, jack_port_name(midi_output_port), midi_port)) {
		fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(midi_output_port), midi_port);
	}
}

void catchsig (int sig) {
#ifndef _WIN32
	signal(SIGHUP, catchsig);
#endif
	fprintf(stderr,"caught signal - shutting down.\n");
	client_state=Exit;
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
	{"help", no_argument, 0, 'h'},
	{"version", no_argument, 0, 'V'},
	{NULL, 0, NULL, 0}
};

static void usage (int status) {
	printf ("jack_midi_command - JACK app to generate custom MIDI messages.\n\n");
	printf ("Usage: jack_midi_command [ OPTIONS ] [JACK-port]*\n\n");
	printf ("Options:\n\
			-h, --help                 display this help and exit\n\
			-V, --version              print version information and exit\n\
			\n");
	printf ("\n\
			This tool generates generates custom midi message from stdin\n\
			and sends them to a JACK-midi port.\n\
			\n");
	printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
			"Website and manual: <https://github.com/x42/jack-midi-cmd>\n"
			);
	exit (status);
}

static int decode_switches (int argc, char **argv) {
	int c;

	while ((c = getopt_long (argc, argv,
					"h"	/* help */
					"V",	/* version */
					long_options, (int *) 0)) != EOF)
	{
		switch (c)
		{

			case 'V':
				printf ("jack_midi_command version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2015 Robin Gareus <robin@gareus.org>\n");
				exit (0);

			case 'h':
				usage (0);

			default:
				usage (EXIT_FAILURE);
		}
	}

	return optind;
}

static void queue_event(my_midi_event_t *me) {
	if (((queued_events_start + 1) % JACK_MIDI_QUEUE_SIZE) == queued_events_end) {
		return;
	}
	memcpy(&event_queue[queued_events_start], me, sizeof(my_midi_event_t));
	queued_events_start = (queued_events_start + 1) % JACK_MIDI_QUEUE_SIZE;
}

static int parse_message(const char *msg) {
	int param[3];
	my_midi_event_t event;

	event.time = 0;

#define THREEBYTES(A, B, C)       \
	event.size = 3;               \
	event.buffer[0] = (A) & 0xff; \
	event.buffer[1] = (B) & 0x7f; \
	event.buffer[2] = (C) & 0x7f; \
	queue_event(&event);

	if (!strncmp(msg, "exit", 4)) {
		client_state = Exit;
	}
	else if (!strncmp(msg, "reconnect", 9)) {
		return 1;
	}
	else if (!strncmp(msg, "help", 4)) {
		printf(" -- Sorry, help yourself and read the source.\n");
	}
	// TODO add a more creative parser
	else if (3 == sscanf(msg, ". %x %x %x\n", &param[0], &param[1], &param[2])) {
		THREEBYTES(param[0], param[1], param[2])
	}
	else if (2 == sscanf(msg, "CC %i %i\n", &param[0], &param[1])) {
		THREEBYTES(0xb0, param[0], param[1])
	}
	else if (2 == sscanf(msg, "N %i %i\n", &param[0], &param[1])) {
		THREEBYTES(0x90, param[0], param[1])
	}
	else if (2 == sscanf(msg, "n %i %i\n", &param[0], &param[1])) {
		THREEBYTES(0x80, param[0], param[1])
	}
	else if (2 == sscanf(msg, "2 %i\n", &param[0])) {
		event.size = 2;
		event.buffer[0] =  param[0] & 0xff;
		event.buffer[1] =  param[1] & 0xff;
		queue_event(&event);
	}
	else if (1 == sscanf(msg, "1 %i\n", &param[0])) {
		event.size = 1;
		event.buffer[0] =  param[0] & 0xff;
		queue_event(&event);
	}
	else {
		printf(" -- Invalid Message, try 'help'\n");
	}
	return 0;
}

int main (int argc, char **argv) {
	fd_set rfds;
	struct timeval tv;
	int cns;
	char buf[1024] = "";

	decode_switches (argc, argv);

	// -=-=-= INITIALIZE =-=-=-

	if (init_jack("midicmd"))
		goto out;
	if (jack_portsetup())
		goto out;

	if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
		fprintf(stderr, "Warning: Can not lock memory.\n");
	}

	// -=-=-= RUN =-=-=-

	if (jack_activate (j_client)) {
		fprintf (stderr, "cannot activate client.\n");
		goto out;
	}

	cns = optind;
	while (cns < argc)
		port_connect(argv[cns++]);

#ifndef _WIN32
	signal (SIGHUP, catchsig);
	signal (SIGINT, catchsig);
#endif

	// -=-=-= JACK DOES ALL THE WORK =-=-=-


	printf("\n> "); fflush(stdout);
	while (client_state != Exit) {
		FD_ZERO(&rfds);
		FD_SET(STDIN_FILENO, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		// history would ne nice :) -> readline ? does it work x-platform?
		int rv = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
		if (rv < 0) { break; }
		if (rv == 0 || !FD_ISSET(STDIN_FILENO, &rfds)) { continue; }
		if (!fgets(buf, 1024, stdin)) {
			break;
		}
		switch (parse_message(buf)) {
			case 1:
				cns = optind;
				while (cns < argc)
					port_connect(argv[cns++]);
				break;
			default:
				break;
		}
		printf("> "); fflush(stdout);
	}
	printf("\n");

	// -=-=-= CLEANUP =-=-=-

out:
	cleanup(0);
	return(0);
}
/* vi:set ts=2 sts=2 sw=2: */
