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

#ifndef __sooperlooper_engine__
#define __sooperlooper_engine__

#include <vector>
#include <string>
#include <cstdio>

#include <sigc++/sigc++.h>

#include "lockmonitor.hpp"
#include "ringbuffer.hpp"
#include "event.hpp"
#include "event_nonrt.hpp"
#include "audio_driver.hpp"
#include "midi_bind.hpp"
#include "command_map.hpp"

//------ albBegin ----------------------------------
// Forward-declare the JACK port handle type so engine.hpp need not
// pull in <jack/jack.h> directly.  Only pointer-to-jack_port_t is used
// as a member; the full definition is provided by engine.cpp which
// includes <jack/midiport.h>.
struct _jack_port;
typedef struct _jack_port jack_port_t;
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// ALB_SYNC_PULSE_LOG: compile-time toggle for a diagnostic log of JackSync
// cycle-boundary pulses (see Engine::generate_sync()'s JackSync branch in
// engine.cpp, inside the existing ALB_BBT_SYNC block).  Added to investigate
// occasional off-beat loop sync landings (mod_docs/ROADMAP.rst, the
// eighth-note off-beat sync investigation) -- specifically, to record which
// formula path fired and what the timebase master was reporting at the
// instant of every real pulse, since generate_sync() currently logs nothing
// at all and there is no way to reconstruct this after the fact.
//
// RT safety: the RT callback only fills a POD SyncPulseLogEvent (already-
// computed local values, no new work) and pushes it into a lock-free
// RingBuffer -- the same pattern already used for _sync_queue and
// _nonrt_update_event_queue below, just one more instantiation of it.  The
// actual fprintf()/fflush() to disk happens only in the non-RT mainloop()
// thread, never in the RT callback.  Pulses fire once per cycle (a few
// times a second at typical tempos), not once per buffer, so this is a
// small fraction of the audio callback's own call rate.
//
// Fully additive: no pulse-placement arithmetic is changed anywhere. Purely
// observes existing values and writes them out.
//
// WHEN DEFINED (default): pulses are logged to ALB_SYNC_PULSE_LOG_PATH.
// WHEN NOT DEFINED: none of this struct/queue/file exists -- comment out
// to remove entirely at compile time, no other change needed.
#define ALB_SYNC_PULSE_LOG

#ifdef ALB_SYNC_PULSE_LOG
// Fixed path is consistent with this rig's existing freeze_logs/ convention
// (see start.py's MIDI_REMAPPER_LOG_FILENAME); change here if a different
// destination is wanted.
#define ALB_SYNC_PULSE_LOG_PATH "/tmp/sl_sync_pulse.log"
#endif
//------ albEnd ----------------------------------

namespace SooperLooper {

#ifdef ALB_SYNC_PULSE_LOG
//------ albBegin ----------------------------------
// One record per actual JackSync cycle-boundary pulse.  POD by design: no
// strings, no pointers, safe to build on the stack in the RT callback and
// copy through a RingBuffer.  See ALB_SYNC_PULSE_LOG above.
struct SyncPulseLogEvent
{
	nframes_t framepos;      // info.framepos + offset: absolute transport
	                          // frame at which this pulse fired
	nframes_t thisval;       // computed position within the tempo-frame cycle
	nframes_t diff;          // computed offset into this buffer
	double    bpm;           // info.bpm this buffer
	double    tempo_frames;  // _tempo_frames at this instant
	float     eighth_cycle;  // _eighth_cycle at this instant
	int32_t   beat;          // info.beat (0 if the BBT path was not used)
	int32_t   tick;          // info.tick (0 if the BBT path was not used)
	bool      use_bbt;       // which formula path fired: BBT vs. frame-0 fallback
};
//------ albEnd ----------------------------------
#endif // ALB_SYNC_PULSE_LOG

class Looper;
class ControlOSC;
class MidiBridge;
	
class Engine
	: public sigc::trackable
{
  public:

	static const int TEMPO_WINDOW_SIZE = 4;
	static const int TEMPO_WINDOW_SIZE_MASK = 3;
	
	Engine();
	virtual ~Engine();

	bool initialize(AudioDriver * driver, int buschans=2, int port=9951, std::string pingurl="");
	void cleanup();

	AudioDriver * get_audio_driver () { return _driver; }
	ControlOSC  * get_control_osc () { return _osc; }

	void set_default_loop_secs (float secs) { _def_loop_secs = secs; }
	void set_default_channels (int chan) { _def_channel_cnt = chan; }
	
	void set_midi_bridge (MidiBridge * bridge);
	MidiBridge * get_midi_bridge() { return _midi_bridge; }
	
	bool is_ok() const { return _ok; }

	void set_ignore_quit(bool val) { _ignore_quit = val; }
	bool get_ignore_quit() { return _ignore_quit; }

	void quit(bool force=false);

	bool add_loop (unsigned int chans, float loopsecs=40.0f, bool discrete = true);
	bool add_loop (Looper * instance);
	bool remove_loop (Looper * loop);
	
	void set_force_discrete(bool flag) { _force_discrete = flag; }
	bool get_force_discrete() const { return _force_discrete; }
	
	size_t loop_count() { return _instances.size(); }

	size_t get_loop_channel_count(size_t instance, bool realtime);
	
	int process (nframes_t);

	void buffersize_changed (nframes_t);
	
	//RingBuffer<Event> & get_event_queue() { return *_event_queue; }

	bool get_common_input (unsigned int chan, port_id_t & port);
	bool get_common_output (unsigned int chan, port_id_t & port);
	sample_t * get_common_input_buffer (unsigned int chan);
	sample_t * get_common_output_buffer (unsigned int chan);

	size_t  get_common_output_count () { return _common_outputs.size(); }
	size_t  get_common_input_count () { return _common_outputs.size(); }
	
	EventGenerator & get_event_generator() { return *_event_generator;}

	bool push_command_event (Event::type_t type, Event::command_t cmd, int8_t instance);
	void push_control_event (Event::type_t type, Event::control_t ctrl, float val, int8_t instance, int src=0);

	void push_midi_command_event (Event::type_t type, Event::command_t cmd, int8_t instance, long framepos=-1);
	void push_midi_control_event (Event::type_t type, Event::control_t ctrl, float val, int8_t instance, long framepos=-1);
	
	void push_sync_event (Event::control_t ctrl, long framepos=-1, MIDI::timestamp_t timestamp=0);
	
	std::string get_osc_url (bool udp=true);
	int get_osc_port ();

	bool load_midi_bindings (std::string filename, bool append, CommandMap & cmdmap);
	bool load_midi_bindings (std::istream & instream, bool append, CommandMap & cmdmap);

	float get_control_value (Event::control_t, int8_t instance);
	std::string get_property_value (std::string prop, int8_t instance);
	void set_property_value (std::string prop, int8_t instance, std::string value);

	sigc::signal2<void, int, bool> LoopAdded;
	sigc::signal0<void> LoopRemoved;

	sigc::signal2<void, int , int> ParamChanged;

	// the main non-rt event processing loop
	void mainloop();
	
	bool push_nonrt_event (EventNonRT * event);
	
	void binding_learned(MidiBindInfo info);
	void next_midi_received(MidiBindInfo info);

	// session state
	bool load_session (std::string fname, std::string * readstr=0);
	bool save_session (std::string fname, bool write_audio = false, std::string * writestr=0);
	
	int get_id() const { return _unique_id; }

	bool get_transport_always_rolls() const { return _transport_always_rolls; }
	void set_transport_always_rolls();

  protected:	

	struct LoopManageEvent
	{
		enum EventType {
			AddLoop = 0,
			RemoveLoop,
			LoadSession
		};

		LoopManageEvent () {}
		LoopManageEvent (EventType et, Looper *loop) : etype(et), looper(loop) {}

		EventType etype;
		Looper * looper;
	};
	

	bool process_nonrt_event (EventNonRT * event);
	void process_rt_loop_manage_events ();
	
	// returns >= 0 offset position on tempo beats
	int generate_sync (nframes_t offset, nframes_t nframes);
	
	void update_sync_source ();
	void calculate_tempo_frames ();
	void calculate_midi_tick (bool rt=true);

	void do_global_rt_event (Event * ev, nframes_t offset, nframes_t nframes);

	bool do_push_command_event (RingBuffer<Event> * rb, Event::type_t type, Event::command_t cmd, int8_t instance, long framepos=-1);
	bool do_push_control_event (RingBuffer<Event> * rb, Event::type_t type, Event::control_t ctrl, float val, int8_t instance, long framepos=-1, int src=0);

	bool push_loop_manage_to_rt (LoopManageEvent & lme);
	bool push_loop_manage_to_main (LoopManageEvent & lme);
	
	void set_tempo (double tempo, bool rt=true);

	inline double avg_tempo(double tempo);
	inline void reset_avg_tempo(double tempo=0.0);

	void fill_common_outs(nframes_t nframes);
	void prepare_buffers(nframes_t nframes);

	void connections_changed();

	void handle_load_session_event();

	
	AudioDriver * _driver;
	
	ControlOSC * _osc;

	MidiBridge * _midi_bridge;
	
	typedef std::vector<Looper*> Instances;
	// the rt thread keeps this one
	Instances _rt_instances;

	// the non-rt keeps this copy
	Instances _instances;
	PBD::NonBlockingLock _instance_lock;

	// looper (de)allocation event
	RingBuffer<LoopManageEvent> * _loop_manage_to_rt_queue;
	RingBuffer<LoopManageEvent> * _loop_manage_to_main_queue;
	
	bool _ignore_quit;
	volatile bool _ok;

	volatile bool _learn_done;
	volatile bool _received_done;
	MidiBindInfo  _learninfo;
	MidiBindingEvent _learn_event;
	
	// RT event queue
	RingBuffer<Event> * _event_queue;
	RingBuffer<Event> * _midi_event_queue;
	RingBuffer<Event> * _sync_queue;
	RingBuffer<Event> * _nonrt_update_event_queue;

	EventGenerator * _event_generator;

	// non-rt event stuff

	RingBuffer<EventNonRT *> * _nonrt_event_queue;
	
	PBD::NonBlockingLock _event_loop_lock;
	pthread_cond_t  _event_cond;

	int _def_channel_cnt;
	float _def_loop_secs;
	nframes_t _buffersize;
	
	// global parameters
	enum SyncSourceType {
		FIRST_SYNC_SOURCE=-5, // must be first
		BrotherSync = -4,
		InternalTempoSync = -3,
		MidiClockSync = -2,
		JackSync = -1,
		NoSync = 0
		// anything > 0 is considered a loop number
	};

	float  *       _internal_sync_buf;
	int _sync_source;

	volatile double    _tempo;        // bpm
	volatile MIDI::timestamp_t _beatstamp; // timestamp at the beat of the last tempo change
	volatile MIDI::timestamp_t _prev_beatstamp; 
	bool _force_next_clock_start;
	volatile bool _send_midi_start_after_next_hit;
	bool _send_midi_start_on_trigger;

	float    _eighth_cycle; // eighth notes per loop cycle

	std::vector<port_id_t>  _common_inputs;
	std::vector<port_id_t>  _common_outputs;

	std::vector<sample_t *>    _common_input_buffers;
	std::vector<sample_t *>    _common_output_buffers;
	std::vector<sample_t *>    _temp_input_buffers;              
	std::vector<sample_t *>    _temp_output_buffers;              
	bool                    _use_temp_input;
	
	float              _curr_common_dry;
	float              _target_common_dry;

	float              _curr_common_wet;
	float              _target_common_wet;

	float              _curr_input_gain;
	float              _target_input_gain;
	
	float              _common_input_peak;
	float              _common_output_peak;
	float              _falloff_per_sample;
	
	bool               _auto_disable_latency;
	int                _selected_loop;
	bool               _jack_timebase_master;

	bool               _output_midi_clock;
	bool               _smart_eighths;
	bool               _force_discrete;
	
	int                _unique_id;
	bool               _transport_always_rolls;

   private:

	double _tempo_counter;
	double _tempo_frames;
	double _quarter_note_frames;
	double _quarter_counter;
	
	unsigned int _midi_ticks;     // counts ticks as they're coming in
	unsigned int _midi_loop_tick; // tick number to loop (sync) on

	nframes_t _running_frames;
	nframes_t _last_tempo_frame;
	volatile bool _tempo_changed;
	volatile bool _beat_occurred;
	volatile bool _conns_changed;
	volatile bool _sel_loop_changed;
	volatile bool _timebase_changed;

	double _tempo_averages[TEMPO_WINDOW_SIZE];
	double _running_tempo_sum;
	unsigned int    _avgindex;

	nframes_t       _longpress_frames;
	nframes_t       _solo_down_stamp;

	bool _use_sync_start;
	bool _use_sync_stop;

	bool _loading;

	SessionEvent* _load_sess_event;

	//------ albBegin ----------------------------------
	// Direct JACK MIDI input port.  Registered in initialize() so that
	// mididings (and other JACK MIDI clients) can connect without going
	// through an ALSA bridge.  Events are read in process() and injected
	// via MidiBridge::inject_midi() with their sample-accurate intra-block
	// timestamp, enabling sub-block buffer splitting in the event loop.
	// nullptr on non-JACK platforms or if registration fails.
	jack_port_t *	_jack_midi_in_port;
	// Direct JACK MIDI output port.  Registered alongside _jack_midi_in_port.
	// process() generates 24-ppqn MIDI clock ticks using a phase accumulator
	// that preserves sub-buffer accuracy across buffer boundaries.  Runs in
	// parallel with the ALSA clock thread; neither replaces the other.
	jack_port_t *	_jack_midi_out_port;
	// Phase accumulator for JACK MIDI clock generation: fractional frames
	// remaining until the next tick, carried across buffer boundaries.
	// Initialised to 0.0; reset to 0.0 when transport starts or tempo is zero.
	double          _jack_clock_tick_phase;
	// Transport rolling state captured in generate_sync() (JackSync branch)
	// each buffer so that process() can detect rolling transitions without a
	// second jack_transport_query() call.
	bool            _jack_transport_rolling;
	bool            _jack_last_rolling;
	//------ albEnd ----------------------------------

#ifdef ALB_SYNC_PULSE_LOG
	//------ albBegin ----------------------------------
	// See ALB_SYNC_PULSE_LOG above.  RT thread writes, mainloop() thread
	// reads and appends to _sync_pulse_log_file.  Both null-initialised in
	// the constructor, allocated/opened in initialize(), torn down in
	// cleanup() -- same lifecycle as _sync_queue.
	RingBuffer<SyncPulseLogEvent> * _sync_pulse_log_queue;
	std::FILE *                     _sync_pulse_log_file;
	//------ albEnd ----------------------------------
#endif // ALB_SYNC_PULSE_LOG
};



inline double Engine::avg_tempo(double tempo)
{
	//double alpha = 0.8;
	//tempo =  (_tempo*alpha + tempo*(1-alpha));
	_running_tempo_sum += tempo;
	_running_tempo_sum -= _tempo_averages[_avgindex];
	_tempo_averages[_avgindex] = tempo;
	_avgindex = (_avgindex + 1) & TEMPO_WINDOW_SIZE_MASK;
	return _running_tempo_sum / (double) TEMPO_WINDOW_SIZE;

}

inline void Engine::reset_avg_tempo(double tempo)
{
	for (int i=0; i < TEMPO_WINDOW_SIZE; ++i) {
		_tempo_averages[i] = tempo;
	}

	_running_tempo_sum = tempo * TEMPO_WINDOW_SIZE;
	_avgindex = TEMPO_WINDOW_SIZE_MASK;
}

	
};  // sooperlooper namespace

#endif
