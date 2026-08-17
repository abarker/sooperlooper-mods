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

#ifndef __sooperlooper_audio_driver__
#define __sooperlooper_audio_driver__

#include <vector>
#include <string>

#include <sigc++/sigc++.h>

#include <inttypes.h>

//------ albBegin ----------------------------------
// Forward-declare the JACK client handle type so audio_driver.hpp need
// not include <jack/jack.h> directly.  Only a pointer is used in the
// default get_jack_client() return type; the full JACK definition is
// provided by files that include <jack/jack.h> (jack_audio_driver.hpp,
// engine.cpp).  Non-JACK backends return nullptr and are unaffected.
struct _jack_client;
typedef struct _jack_client jack_client_t;
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// ALB_BBT_SYNC: when defined, Engine::generate_sync() uses JACK BBT position
// fields (bar, beat, tick, bar_start_tick, ticks_per_beat) to place sync
// pulses on the timebase master's musical grid rather than anchoring to JACK
// frame 0.  This corrects pulse placement after transport locates to non-zero
// positions and after mid-transport Eighths/Cycle changes.
//
// WHEN DEFINED (default):
//   generate_sync() checks info.bbt_valid at runtime.  If the timebase master
//   has set JackPositionBBT (Hydrogen, Ardour, etc.) AND beat_type == 4 AND
//   bpm and ticks_per_beat are non-zero, the BBT path is taken.  Otherwise
//   the code falls back automatically to the original fmod(framepos) arithmetic.
//   PipeWire's JACK layer implements the full BBT API; no special handling needed.
//
// WHEN NOT DEFINED:
//   The BBT fields are not compiled into TransportInfo and generate_sync()
//   uses only the original fmod(framepos, _tempo_frames) arithmetic in all
//   cases — identical to stock SooperLooper 1.7.9 JackSync behaviour.
//   Comment out to force the fmod path (e.g. if a timebase master populates
//   BBT fields incorrectly, or for debugging grid-alignment issues).
//
// Note: ALB_BBT_SYNC is defined here in audio_driver.hpp (rather than in
// engine.cpp alongside ALB_RT_SAFE_COND_SIGNAL) because it also gates fields
// in TransportInfo, which is included by multiple translation units.  The
// jack_audio_driver.cpp population block and the engine.cpp pulse-placement
// block both check #ifdef ALB_BBT_SYNC.
#define ALB_BBT_SYNC
//------ albEnd ----------------------------------

namespace SooperLooper {

typedef float sample_t;
typedef uint32_t nframes_t;
typedef uint32_t port_id_t;

class Engine;

struct TransportInfo
{
	enum State {
		ROLLING=1,
		STOPPED
	};

	nframes_t framepos;
	nframes_t last_framepos;
	double bpm;
	State state;

	// future items (uncommented below when ALB_BBT_SYNC is defined)
	// int32_t bar;           /**< current bar */
	// int32_t beat;          /**< current beat-within-bar */
	// int32_t tick;          /**< current tick-within-beat */
	// double bar_start_tick;
	float beats_per_bar;  /**< time signature "numerator" */
	float beat_type;      /**< time signature "denominator" */
	// double ticks_per_beat;
	// nframes_t bbt_offset; /**< frame offset for the BBT fields */

	//------ albBegin ----------------------------------
	// BBT position fields from jack_position_t.  Only present when
	// ALB_BBT_SYNC is defined.  Populated by JackAudioDriver::get_transport_info()
	// from the jack_position_t returned by jack_transport_query() when the
	// timebase master has set the JackPositionBBT valid bit.
	// bbt_valid is false when no timebase master is active, when the master
	// does not set JackPositionBBT, or when ALB_BBT_SYNC is not defined.
	// Engine::generate_sync() reads bbt_valid before touching any other
	// BBT field, so an absent or unavailable timebase master falls back to
	// the stock fmod(framepos) path with no additional overhead.
#ifdef ALB_BBT_SYNC
	int32_t  bar;            /**< 1-indexed current bar */
	int32_t  beat;           /**< 1-indexed beat within bar */
	int32_t  tick;           /**< tick within beat (0 .. ticks_per_beat-1) */
	double   bar_start_tick; /**< cumulative ticks from transport origin to
	                              start of current bar */
	double   ticks_per_beat; /**< tick resolution, set by the timebase master;
	                              never assume a value.  Hydrogen 1.2.4 reports
	                              48.0 (observed on the rig 2026-08-17). */
	bool     bbt_valid;      /**< true iff the above fields were populated
	                              from JackPositionBBT this buffer */
#endif // ALB_BBT_SYNC
	//------ albEnd ----------------------------------
};


//------ albBegin ----------------------------------
// STOCK (sooperlooper 1.7.9):
//   class AudioDriver
//   {
// CHANGE: added sigc::trackable base class so that sigc++ slot
// connections to AudioDriver-derived objects are automatically
// cleaned up on destruction.  The stock class had no base; adding
// sigc::trackable is a safe, source-compatible change since it
// adds no data members and the existing ConnectionsChanged signal
// member already benefits from it.
class AudioDriver
	: public sigc::trackable
//------ albEnd ----------------------------------
{
  public:

	AudioDriver(std::string client_name="", std::string serv_name="");
	virtual ~AudioDriver();

	virtual bool initialize(std::string client_name="") = 0;
	virtual bool activate() = 0;
	virtual bool deactivate() = 0;

	virtual bool create_input_port  (std::string name, port_id_t & port_id) = 0;
	virtual bool create_output_port (std::string name, port_id_t & port_id) = 0;

	virtual bool destroy_input_port  (port_id_t portid) = 0;
	virtual bool destroy_output_port (port_id_t portid) = 0;

	virtual sample_t * get_input_port_buffer  (port_id_t port, nframes_t nframes) = 0;
	virtual sample_t * get_output_port_buffer (port_id_t port, nframes_t nframes) = 0;

	virtual unsigned int get_input_port_count  () = 0;
	virtual unsigned int get_output_port_count () = 0;

	virtual nframes_t get_input_port_latency  (port_id_t portid) = 0;
	virtual nframes_t get_output_port_latency (port_id_t portid) = 0;

	virtual std::string get_name() { return _client_name; }

	virtual void    set_engine (Engine * engine) { _engine = engine; }
	virtual Engine * get_engine () { return _engine; }

	virtual bool get_transport_info (TransportInfo &info) { return false; }
	virtual void set_transport_info (const TransportInfo &info) {}

	virtual bool set_timebase_master(bool flag) { return false; }
	virtual bool get_timebase_master() { return false; }

	virtual void reposition_transport(nframes_t framepos) {}

	virtual nframes_t get_samplerate() { return _samplerate; }
	virtual nframes_t get_buffersize() { return _buffersize; }

	//------ albBegin ----------------------------------
	// Return the underlying JACK client handle, or nullptr if this driver
	// is not a JACK implementation (e.g. CoreAudio AU build on macOS).
	// Used by Engine::initialize() to register a direct JACK MIDI input
	// port, giving JACK MIDI clients sample-accurate intra-block timestamps.
	// The default returns nullptr so that non-JACK backends compile without
	// change and the MIDI port registration is simply skipped.
	virtual jack_client_t * get_jack_client () { return nullptr; }
	//------ albEnd ----------------------------------

	sigc::signal0<void> ConnectionsChanged;

  protected:

	std::string _client_name;
	std::string _server_name;

	Engine * _engine;

	nframes_t _samplerate;
	nframes_t _buffersize;
};


};  // SooperLooper namespace

#endif
