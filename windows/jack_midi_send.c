/*
 * Sends one MIDI note (note on, then note off one JACK cycle later) to a
 * JACK MIDI port, by default sooperlooper:midi_in, then exits.  For
 * checking that SooperLooper's JACK MIDI input works without a MIDI
 * device.  See windows/README.md.
 *
 * Build under Cygwin (after cygwin_compile.bash):
 *    gcc -o jack_midi_send windows/jack_midi_send.c \
 *        -I/usr/local/include -L/usr/local/lib -ljack64
 * Usage:
 *    ./jack_midi_send [note [port]]      (note 0-127, default 60)
 */

#include <jack/jack.h>
#include <jack/midiport.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static jack_port_t *out_port;
static int note = 60;
static volatile int stage = 0;  /* 1: send note on, 2: note off, 3: done */

static int process(jack_nframes_t nframes, void *arg)
{
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
	const char *target = "sooperlooper:midi_in";
	if (argc > 1) note = atoi(argv[1]) & 0x7f;
	if (argc > 2) target = argv[2];

	jack_client_t *client = jack_client_open("midi_send", JackNoStartServer, NULL);
	if (!client) {
		fprintf(stderr, "Cannot connect to the JACK server.\n");
		return 1;
	}
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
	if (jack_connect(client, jack_port_name(out_port), target)) {
		fprintf(stderr, "Cannot connect to %s (is SooperLooper running?).\n", target);
		jack_client_close(client);
		return 1;
	}

	usleep(200000);  /* let the connection settle */
	stage = 1;
	while (stage < 3) usleep(10000);
	usleep(200000);  /* let the note off go out */
	printf("Sent note %d to %s.\n", note, target);

	jack_client_close(client);
	return 0;
}
