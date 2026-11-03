/*
 * Sends one MIDI note (note on, then note off one JACK cycle later) to a
 * JACK MIDI port, by default the engine's midi_in, then exits.  For
 * checking that SooperLooper's JACK MIDI input works without a MIDI
 * device.  test_jack_midi.bash builds and runs it; see windows/README.md.
 *
 * Build under Cygwin (after cygwin_compile.bash):
 *    gcc -o jack_midi_send windows/jack_midi_send.c \
 *        -I/usr/local/include -L/usr/local/lib -ljack64
 * Usage:
 *    ./jack_midi_send [note [port]]      (note 0-127, default 60)
 * Without a port it uses the first sooperlooper...:midi_in port it finds
 * (the engine's JACK client is sooperlooper, or sooperlooper_1 in a
 * build whose configure missed jack_client_open).
 *
 * Each step prints a line on stderr, so a failure shows where it
 * stopped.  It gives up after 2 s if JACK never runs its process
 * callback, and an alarm ends it after 10 s if a JACK call hangs.
 */

#include <jack/jack.h>
#include <jack/midiport.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static jack_port_t *out_port;
static int note = 60;
static volatile int stage = 0;  /* 1: send note on, 2: note off, 3: done */
static volatile int process_calls = 0;

static int process(jack_nframes_t nframes, void *arg)
{
	process_calls++;
	void *buf = jack_port_get_buffer(out_port, nframes);
	jack_midi_clear_buffer(buf);
	if (stage == 1 || stage == 2) {
		jack_midi_data_t *ev = jack_midi_event_reserve(buf, 0, 3);
		if (ev) {
			ev[0] = (stage == 1) ? 0x90 : 0x80;  /* channel 1 */
			ev[1] = note;
			ev[2] = (stage == 1) ? 100 : 0;
		}
		stage++;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *target = NULL;
	if (argc > 1) note = atoi(argv[1]) & 0x7f;
	if (argc > 2) target = argv[2];

	alarm(10);  /* default action: the program ends ("Alarm clock") */

	jack_client_t *client = jack_client_open("midi_send", JackNoStartServer, NULL);
	if (!client) {
		fprintf(stderr, "Cannot connect to the JACK server.\n");
		return 1;
	}
	fprintf(stderr, "Opened a JACK client.\n");
	out_port = jack_port_register(client, "out", JACK_DEFAULT_MIDI_TYPE,
	                              JackPortIsOutput, 0);
	if (!out_port) {
		fprintf(stderr, "Cannot register the output port.\n");
		return 1;
	}
	jack_set_process_callback(client, process, NULL);
	if (jack_activate(client)) {
		fprintf(stderr, "Cannot activate the client.\n");
		return 1;
	}
	fprintf(stderr, "Activated.\n");
	if (!target) {
		const char **ports = jack_get_ports(client, "^sooperlooper[^:]*:midi_in$",
		                                    JACK_DEFAULT_MIDI_TYPE, JackPortIsInput);
		if (!ports || !ports[0]) {
			fprintf(stderr, "No sooperlooper...:midi_in port (is SooperLooper running?).\n");
			jack_client_close(client);
			return 1;
		}
		static char found[256];
		snprintf(found, sizeof(found), "%s", ports[0]);
		jack_free(ports);
		target = found;
	}
	if (jack_connect(client, jack_port_name(out_port), target)) {
		fprintf(stderr, "Cannot connect to %s (is SooperLooper running?).\n", target);
		jack_client_close(client);
		return 1;
	}

	fprintf(stderr, "Connected to %s.\n", target);

	usleep(200000);  /* let the connection settle */
	fprintf(stderr, "Process callback calls so far: %d.\n", process_calls);
	stage = 1;
	for (int i = 0; i < 200 && stage < 3; i++) usleep(10000);
	if (stage < 3) {
		fprintf(stderr, "Gave up after 2 s: the note was not sent"
		        " (stage %d, %d process callback calls).\n", stage, process_calls);
		jack_client_close(client);
		return 1;
	}
	usleep(200000);  /* let the note off go out */
	printf("Sent note %d to %s.\n", note, target);
	fflush(stdout);

	jack_client_close(client);
	return 0;
}
