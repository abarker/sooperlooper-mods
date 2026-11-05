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

#include "test_looper.hpp"
#include <jack/jack.h>

#include <iostream>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>

#include "ladspa.h"

using namespace std;
using namespace SooperLooper;

extern	const LADSPA_Descriptor* ladspa_descriptor (unsigned long);


const LADSPA_Descriptor* TestLooper::descriptor = 0;


TestLooper::TestLooper (jack_client_t* j, unsigned int index, unsigned int chan_count)
	: _jack (j), _index(index), _chan_count(chan_count)
{
	char tmpstr[100];
	
	ok = false;
	requested_cmd = 0;
	last_requested_cmd = 0;
	request_pending = false;
	sync_inject_value   = 1.0f;
	sync_inject_enabled = false;
	sync_inject_single  = false;
	//------ albBegin ----------------------------------
	sync_inject_period  = 0;
	sync_clock          = 0;
	sync_pulse_at       = -1;
	//------ albEnd ----------------------------------
	process_count       = 0;
	//------ albBegin ----------------------------------
	/* -0.7 is the value run() used to hardcode; keep it as the default so
	 * every test written before injection existed is unaffected. */
	input_inject_value  = -0.7f;
	input_ramp          = false;
	request_at_clock    = -1;
	request_at_cmd      = -1;
	last_request_clock  = 0;
	input_clock         = 0;
	input_minus_sync    = 0;
	out_first           = 0.0f;
	out_first_frame     = 0;
	syncout_count       = 0;
	reset_output_stats();
	//------ albEnd ----------------------------------
	_input_ports = 0;
	_output_ports = 0;
	_instances = 0;
	_buffersize = 0;
	_use_sync_buf = 0;
	_our_syncin_buf = 0;
	_our_syncout_buf = 0;
    _dummy_buf = 0;
	
	if (!descriptor) {
		descriptor = ladspa_descriptor (0);
	}


	_instances = new LADSPA_Handle[_chan_count];
	_input_ports = new jack_port_t*[_chan_count];
	_output_ports = new jack_port_t*[_chan_count];

	set_buffer_size(jack_get_buffer_size(_jack));

	memset (_instances, 0, sizeof(LADSPA_Handle) * _chan_count);
	memset (_input_ports, 0, sizeof(jack_port_t*) * _chan_count);
	memset (_output_ports, 0, sizeof(jack_port_t*) * _chan_count);
    memset (ports, 0, sizeof(float) * 20);
	
	// set some rational defaults
	ports[DryLevel] = 1.0f;
	ports[WetLevel] = 1.0f;
	ports[Feedback] = 1.0f;
	ports[Rate] = 1.0f;
	ports[Multi] = -1.0f;
	ports[Sync] = 0.0f;
	ports[Quantize] = 0.0f;
	ports[UseRate] = 0.0f;
	
	_slave_sync_port = 1.0f;

	for (int i=0; i < _chan_count; ++i)
	{

		if ((_instances[i] = descriptor->instantiate (descriptor, jack_get_sample_rate (_jack))) == 0) {
			return;
		}

		snprintf(tmpstr, sizeof(tmpstr), "loop%d_in_%d", _index, i+1);
		
		if ((_input_ports[i] = jack_port_register (_jack, tmpstr, JACK_DEFAULT_AUDIO_TYPE,
						      JackPortIsInput, 0)) == 0) {
			
			cerr << "cannot register loop input port\n";
			return;
		}
		
		snprintf(tmpstr, sizeof(tmpstr), "loop%d_out_%d", _index, i+1);

		if ((_output_ports[i] = jack_port_register (_jack, tmpstr, JACK_DEFAULT_AUDIO_TYPE,
						       JackPortIsOutput, 0)) == 0) {
			cerr << "cannot register loop output port\n";
			return;
		}

		/* connect all scalar ports to data values */
		
		for (unsigned long n = 0; n < LASTPORT; ++n) {
			ports[n] = 0.0f;
			descriptor->connect_port (_instances[i], n, &ports[n]);
		}

		// connect dedicated Sync port to all other channels
		if (i > 0) {
			descriptor->connect_port (_instances[i], Sync, &_slave_sync_port);

			descriptor->connect_port (_instances[i], State, &_slave_dummy_port);
			descriptor->connect_port (_instances[i], LoopLength, &_slave_dummy_port);
			descriptor->connect_port (_instances[i], LoopPosition, &_slave_dummy_port);
			descriptor->connect_port (_instances[i], CycleLength, &_slave_dummy_port);
			descriptor->connect_port (_instances[i], LoopFreeMemory, &_slave_dummy_port);
			descriptor->connect_port (_instances[i], LoopMemory, &_slave_dummy_port);
		}
		
		descriptor->activate (_instances[i]);
	}

	// set some rational defaults
	ports[DryLevel] = 1.0;
	ports[WetLevel] = 1.0;
	ports[Feedback] = 1.0;
	ports[Multi] = -1.0;

	ok = true;
}

TestLooper::~TestLooper ()
{
	for (unsigned int i=0; i < _chan_count; ++i)
	{
		if (_instances[i]) {
			if (descriptor->deactivate) {
				descriptor->deactivate (_instances[i]);
			}
			if (descriptor->cleanup) {
				descriptor->cleanup (_instances[i]);
			}
			_instances[i] = 0;
		}
		
		if (_input_ports[i]) {
			jack_port_unregister (_jack, _input_ports[i]);
			_input_ports[i] = 0;
		}
		
		if (_output_ports[i]) {
			jack_port_unregister (_jack, _output_ports[i]);
			_output_ports[i] = 0;
		}
	}

	delete [] _instances;
	delete [] _input_ports;
	delete [] _output_ports;
}

float
TestLooper::get_control_value (int ctrl)
{
	int index = (int) ctrl;
	
	if (index >= 0 && index < LASTPORT) {
		return ports[index];
	}

	return 0.0f;
}

unsigned long long
TestLooper::get_global_samples_since_sync (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		return pLS->lGlobalSamplesSinceSync;
	}
	return 0;
}

//------ albBegin ----------------------------------
unsigned long
TestLooper::get_samples_since_sync (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		return pLS->lSamplesSinceSync;
	}
	return 0;
}

int
TestLooper::get_channel_state (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		return ((SooperLooperI *) _instances[index])->state;
	}
	return -1;
}

unsigned long
TestLooper::get_channel_loop_length (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		return pLS->headLoopChunk ? pLS->headLoopChunk->lLoopLength : 0;
	}
	return 0;
}

unsigned long
TestLooper::get_channel_history_size (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		return ((SooperLooperI *) _instances[index])->lInputBufSize;
	}
	return 0;
}

unsigned long
TestLooper::get_channel_history_short_count (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		return ((SooperLooperI *) _instances[index])->lHistoryShortCount;
	}
	return 0;
}

long
TestLooper::get_channel_sync_pos (unsigned int index) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		return pLS->headLoopChunk ? pLS->headLoopChunk->lSyncPos : 0;
	}
	return 0;
}

float
TestLooper::get_loop_sample (unsigned int index, unsigned long pos) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		if (pLS->headLoopChunk) {
			return pLS->pSampleBuf[(pLS->headLoopChunk->lLoopStart + pos)
			                       & pLS->lBufferSizeMask];
		}
	}
	return 0.0f;
}

float
TestLooper::get_prev_loop_sample (unsigned int index, unsigned long pos) const
{
	if (index < _chan_count && _instances[index]) {
		SooperLooperI * pLS = (SooperLooperI *) _instances[index];
		if (pLS->headLoopChunk && pLS->headLoopChunk->prev) {
			LoopChunk * prev = pLS->headLoopChunk->prev;
			return pLS->pSampleBuf[(prev->lLoopStart + pos)
			                       & pLS->lBufferSizeMask];
		}
	}
	return 0.0f;
}

void
TestLooper::set_replace_quantized (unsigned int index, bool value)
{
	if (index < _chan_count && _instances[index]) {
		((SooperLooperI *) _instances[index])->bReplaceQuantized = value;
	}
}
//------ albEnd ----------------------------------


void
TestLooper::request_cmd (int cmd)
{
	requested_cmd = cmd;
	request_pending = true;
}

void
TestLooper::run (jack_nframes_t offset, jack_nframes_t nframes)
{
	++process_count;

	//------ albBegin ----------------------------------
	/* As Looper::run(): the other channels' Sync port follows channel 0's
	 * (with Rel Sync, channel 0's port is already 2). */
	_slave_sync_port = ports[Sync];
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
	input_minus_sync = (long long) input_clock - (long long) sync_clock;
	if (request_at_clock >= 0 && !request_pending
	    && (long long) sync_clock >= request_at_clock) {
		requested_cmd = request_at_cmd;
		request_pending = true;
		request_at_clock = -1;
	}
	//------ albEnd ----------------------------------

	/* maybe change modes */

	if (request_pending) {
		
		if (ports[Multi] == requested_cmd) {
			/* defer till next call */
			ports[Multi] = -1;
		} else {
			ports[Multi] = requested_cmd;
			request_pending = false;
			//------ albBegin ----------------------------------
			last_request_clock = sync_clock;
			//------ albEnd ----------------------------------
			//cerr << "requested mode " << requested_cmd << endl;
		}

	} else if (ports[Multi] >= 0) {
		ports[Multi] = -1;
		//cerr << "reset to -1\n";
	}

    //------ albBegin ----------------------------------
    // Was: test_buf[i] = -0.7;  input_inject_value defaults to -0.7.
    LADSPA_Data test_buf[nframes];
    for (int i = 0; i < nframes; ++i)
    {
        test_buf[i] = input_ramp
            ? (float) ((input_clock + i) & 0xFFFFF) / 1048576.0f
            : input_inject_value;
    }
    //------ albEnd ----------------------------------
    //test_buf = (LADSPA_Data *) jack_port_get_buffer(_input_ports[0], nframes);

	//------ albBegin ----------------------------------
	/* As Looper::run(): the other channels get channel 0's tracked sync
	 * count (REL_SYNC_STALE_PHASE_FIX, plugin.cc). */
#if REL_SYNC_STALE_PHASE_FIX
	for (unsigned int i = 1; i < _chan_count; ++i) {
		((SooperLooperI *) _instances[i])->lTrackedSinceSync =
			((SooperLooperI *) _instances[0])->lTrackedSinceSync;
	}
#endif
	//------ albEnd ----------------------------------

	for (unsigned int i=0; i < _chan_count; ++i)
	{
		/* (re)connect audio ports */

		LADSPA_Data * out_buf =
			(LADSPA_Data*) jack_port_get_buffer (_output_ports[i], nframes) + offset;

		descriptor->connect_port (_instances[i], AudioInputPort, test_buf);
		descriptor->connect_port (_instances[i], AudioOutputPort, out_buf);


		if (i == 0) {
			/* Late-hit test support: fill sync input buffer before connecting. */
			//------ albBegin ----------------------------------
			// Was: the sync_inject_value fill only.
			// Read the period once: Python can set it to 0 between a check
			// and the modulo, which divides by zero.
			const unsigned long period = sync_inject_period;
			if (sync_inject_enabled && _our_syncin_buf && period) {
				const long long pulse_at = sync_pulse_at;
				for (nframes_t n = 0; n < _buffersize; ++n)
					_our_syncin_buf[n] = ((sync_clock + n) % period == 0
					                      || (long long) (sync_clock + n) == pulse_at)
						? 2.0f : 0.0f;
				if (pulse_at >= 0 && (long long) (sync_clock + _buffersize) > pulse_at)
					sync_pulse_at = -1;
				sync_clock += _buffersize;
			}
			else if (sync_inject_enabled && _our_syncin_buf) {
				for (nframes_t n = 0; n < _buffersize; ++n)
					_our_syncin_buf[n] = (sync_inject_single && n > 0)
						? 0.0f : sync_inject_value;
			}
			//------ albEnd ----------------------------------
			descriptor->connect_port (_instances[i], SyncInputPort, (LADSPA_Data*) _use_sync_buf + offset);
			descriptor->connect_port (_instances[i], SyncOutputPort, (LADSPA_Data*) _our_syncout_buf + offset);
		}
		else {
			// all others get the first channel's sync-out as input
			descriptor->connect_port (_instances[i], SyncInputPort, (LADSPA_Data*) _our_syncout_buf + offset);
			descriptor->connect_port (_instances[i], SyncOutputPort, (LADSPA_Data*) _dummy_buf + offset);
		}
		
		
		/* do it */
		descriptor->run (_instances[i], nframes);

		//------ albBegin ----------------------------------
		/* Capture channel 0's output so tests can assert on audio content.
		 * min/max accumulate across blocks because one block covers only a
		 * fraction of a loop period. */
		if (i == 0) {
			for (jack_nframes_t n = 0; n < nframes; ++n) {
				const float s = out_buf[n];
				if (s < output_min) output_min = s;
				if (s > output_max) output_max = s;
			}
			if (nframes > 0) output_last = out_buf[nframes - 1];
			if (nframes > 0) {
				out_first = out_buf[0];
				out_first_frame = input_clock;
			}
			for (jack_nframes_t n = 0; n < nframes; ++n) {
				if (_our_syncout_buf[offset + n] > 1.5f) {
					_syncout_frames[syncout_count % SYNCOUT_KEEP] = input_clock + n;
					++syncout_count;
				}
			}
		}
		//------ albEnd ----------------------------------
	}
	//------ albBegin ----------------------------------
	input_clock += nframes;
	//------ albEnd ----------------------------------
}

//------ albBegin ----------------------------------
void
TestLooper::reset_output_stats ()
{
	output_min  =  1.0e30f;
	output_max  = -1.0e30f;
	output_last =  0.0f;
}
//------ albEnd ----------------------------------

void
TestLooper::set_buffer_size (nframes_t bufsize)
{
	if (_buffersize != bufsize) {

		if (_use_sync_buf == _our_syncin_buf) {
			_use_sync_buf = 0;
		}

		if (_our_syncin_buf)
			delete [] _our_syncin_buf;
		
		if (_our_syncout_buf)
			delete [] _our_syncout_buf;

		if (_dummy_buf)
			delete [] _dummy_buf;
		
		_buffersize = bufsize;
		
		_our_syncin_buf = new LADSPA_Data[_buffersize];
		_our_syncout_buf = new LADSPA_Data[_buffersize];
		_dummy_buf = new LADSPA_Data[_buffersize];
		
		if (_use_sync_buf == 0) {
			_use_sync_buf = _our_syncin_buf;
		}
	}
}
