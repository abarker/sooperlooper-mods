/*
** Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**  
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**  
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**  
*/

#ifndef __sooperlooper_looper__
#define __sooperlooper_looper__

#include <jack/jack.h>
#include "ladspa.h"
#include "../plugin.hpp"
typedef uint32_t nframes_t;

namespace SooperLooper {

class TestLooper 
{
  public:
	TestLooper (jack_client_t*, unsigned int index, unsigned int channel_count=1);
	~TestLooper ();
	
	void run (jack_nframes_t offset, jack_nframes_t nframes);

	float get_control_value (int ctrl);

	/* Test-support accessor: exposes the internal SooperLooperI
	 * lGlobalSamplesSinceSync counter directly, for tests that need to
	 * prove its value (e.g. that it freezes while fSyncMode == 0 and
	 * survives a 0->1 toggle) rather than only inferring it from
	 * button-press behaviour. Read-only, non-RT-path (test harness only). */
	unsigned long long get_global_samples_since_sync (unsigned int index = 0) const;
	//------ albBegin ----------------------------------
	/* Same, for the per-loop lSamplesSinceSync (Rel Sync's offset). */
	unsigned long get_samples_since_sync (unsigned int index = 0) const;
	/* Per-channel state and loop length (samples), for the channels whose
	 * State and LoopLength ports go to a dummy. */
	int get_channel_state (unsigned int index) const;
	unsigned long get_channel_loop_length (unsigned int index) const;
	/* The current take's sample at loop position pos (channel index). */
	float get_loop_sample (unsigned int index, unsigned long pos) const;
	//------ albEnd ----------------------------------

	void request_cmd (int cmd);
	
	void set_port (int n, float val) {
		ports[n] = val;
	}

	void set_buffer_size (nframes_t bufsize);
	
	int requested_cmd;
	int last_requested_cmd;

	bool ok;
	bool request_pending;

	/* Late-hit test support: when sync_inject_enabled is true, run() fills
	 * _our_syncin_buf with sync_inject_value before each callback.  Set 2.0
	 * to inject a sync pulse (> 1.5 threshold); set 0.0 for no-signal.
	 * Both fields are written from Python and read on the JACK thread —
	 * the same single-float race as ports[], acceptable for test code. */
	float sync_inject_value;
	bool  sync_inject_enabled;
	//------ albBegin ----------------------------------
	/* When true, the injected value goes on each buffer's first sample only
	 * and the rest are 0: one single-sample pulse per buffer at a known
	 * frame, as a real sync source gives, instead of a pulse on every
	 * sample.  Exact loop lengths can then be checked against it. */
	bool  sync_inject_single;

	/* When nonzero (and sync_inject_enabled), run() makes its own pulses
	 * instead of using sync_inject_value: a single-sample 2.0 every
	 * sync_inject_period samples of sync_clock, the number of samples run
	 * since sync_clock was last set.  Pulses then land on exact frames,
	 * with no race against Python, and a test reads the phase from
	 * sync_clock. */
	unsigned long      sync_inject_period;
	unsigned long long sync_clock;

	/* Exact press timing.  process_count goes up at the top of run(), so
	 * a test reading sync_clock after wait_buffers() races the callback
	 * and cannot know which block a request() lands in.  Instead:
	 * request_at_clock >= 0 makes run() request request_at_cmd in the
	 * first block whose sync_clock (its first sample) is >= it, then
	 * sets it back to -1; last_request_clock is the sync_clock of the
	 * block where the last requested command (either way) was applied. */
	long long          request_at_clock;
	int                request_at_cmd;
	unsigned long long last_request_clock;
	//------ albEnd ----------------------------------

	/* Incremented at the top of every run() call.  Python tests busy-wait
	 * on this to synchronise with exact JACK callback counts rather than
	 * using time.sleep(), making timing independent of JACK period size. */
	volatile int process_count;

	//------ albBegin ----------------------------------
	/* Audio-content test support.  Lets a test put a known signal in and
	 * read the resulting output back, so assertions can be made about what
	 * actually landed in the loop rather than only about state values.
	 *
	 * Input: run() has never read the real JACK input port -- it has always
	 * fed the plugin a constant buffer (see run()).  This only makes that
	 * constant settable; the default -0.7 is the historical hardcoded value,
	 * so every pre-existing test behaves identically.
	 *
	 * Output: run() samples channel 0's output buffer after the plugin has
	 * run.  output_min/output_max accumulate until reset_output_stats(), which
	 * is what a test wants when asking "did *any* part of the loop change?" --
	 * a single block only ever covers a fraction of a loop period.
	 *
	 * Written from Python, read on the JACK thread (and vice versa) -- the
	 * same single-float race as ports[] and sync_inject_*, acceptable here. */
	float input_inject_value;

	/* When true, the input is a ramp instead: the sample at frame f (f =
	 * input_clock, the samples run since the harness started) is
	 * (f mod 2^20) / 2^20, exact in a float, so a test can tell which
	 * input frame any recorded or played sample came from.
	 * out_first / out_first_frame: channel 0's output at the first sample
	 * of the most recent block, and that sample's frame. */
	bool  input_ramp;
	unsigned long long input_clock;
	/* input_clock - sync_clock, taken at the top of run() where both are
	 * the block's first sample (they advance at different points in
	 * run(), so reading the two from Python races). */
	long long input_minus_sync;
	float out_first;
	unsigned long long out_first_frame;

	float output_min;      /* running min since reset_output_stats() */
	float output_max;      /* running max since reset_output_stats() */
	float output_last;     /* last sample of the most recent block     */

	void reset_output_stats();
	//------ albEnd ----------------------------------

	int get_buffer_size() const { return (int)_buffersize; }

  protected:
	
	jack_client_t*     _jack;
	jack_port_t**       _input_ports;
	jack_port_t**       _output_ports;

	unsigned int _index;
	unsigned int _chan_count;
	LADSPA_Handle *      _instances;

	static const LADSPA_Descriptor* descriptor;

	LADSPA_Data        ports[LASTPORT];

	nframes_t          _buffersize;
	LADSPA_Data        * _our_syncin_buf;
	LADSPA_Data        * _our_syncout_buf;
	LADSPA_Data        * _use_sync_buf;
	LADSPA_Data        * _dummy_buf;

	LADSPA_Data         _slave_sync_port;
	LADSPA_Data         _slave_dummy_port;
	
};

};

#endif
