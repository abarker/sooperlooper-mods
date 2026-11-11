/*
** Copyright (C) 2026 Allen Barker
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
*/

//------ albBegin ----------------------------------
// BeatTracker: the musical position of a JACK transport, counted through
// tempo changes instead of recomputed from the frame number.
//
// Stock SooperLooper places JackSync pulses at framepos mod cycle_length,
// i.e. it recomputes the beat position every cycle as framepos / beat
// length.  That equals the true position only if the tempo has not
// changed since frame 0.  A timebase master that keeps its beat
// continuous through a tempo change (Hydrogen 1.2.x, a DAW playing
// through a tempo change) then disagrees by an amount that grows with
// elapsed time -- measured on a live rig as one eighth note.  See
// "JackSync Beat Tracking" in mod_docs/late_hit_patch_summary.rst.
//
// This class instead keeps a running beat count:
//
//   * each rolling cycle it advances by nframes * bpm / (60 * sr), using
//     the tempo JACK reports for that cycle, so a tempo change alters the
//     rate but never the position;
//   * while stopped it does not advance (the master is frozen too), and a
//     stop/start that resumes at the same frame simply continues;
//   * on a relocate -- a rolling cycle that does not start where the
//     previous rolling cycle ended -- it re-anchors to framepos * bpm /
//     (60 * sr), which is where a master that recomputes its position
//     from the frame on a locate (Hydrogen does) puts its beat.
//
// At constant tempo since the last relocate it reproduces the stock grid
// to within one sample.
//
// Grid boundaries are multiples of a step length in beats (1 for quarter
// notes, _eighth_cycle / 2 for a cycle).  The position is computed from
// the start of the current constant-tempo segment by multiplication, so
// no rounding drift accumulates, and each boundary is assigned to exactly
// one cycle (see boundaries()), so a pulse is never dropped or doubled
// at a buffer edge, whatever the tempo does between cycles.
//
// Measured against Hydrogen 1.2.4: Hydrogen's audio
// switches tempo one JACK cycle before the new tempo appears in the
// transport position, which leaves pulses about 1 ms late after a ramp.
// The error is bounded by one buffer and does not accumulate, so it is
// not compensated here.
//
// Following the master's own position (used
// only when audio_driver.hpp defines ALB_JACK_BBT_TRACKING): when the timebase
// master publishes a live bar/beat/tick position -- a DAW with a tempo
// map, Hydrogen in song mode -- follow_master() keeps the count on it.
// Small differences are nudged in a little each cycle, added to the end
// of the cycle so the count never runs backwards (no doubled or dropped
// pulse).  A relocate, or a large difference when following starts,
// jumps to the master's position.  A large jump in the master's position
// without a relocate -- Hydrogen's song looping back to bar 1 while the
// JACK frame keeps counting -- is kept as an offset instead, so the
// pulses stay evenly spaced even when the song is not a whole number of
// cycles long.
// A master that publishes constants (Hydrogen in pattern mode sends bar 1,
// beat 1, tick 0 throughout) never counts as live and is ignored.
//
// Cycle grid origin (ALB_CYCLE_CHANGE_REANCHOR, engine.hpp; off by
// default): boundaries() can count from an origin other than beat 0.
// When Eighths/Cycle changes while rolling, reanchor_cycle() moves the
// origin to the boundary of the old grid nearest the current position, so
// the new cycles start on the old grid (a bar line, if the old cycle was a
// bar) instead of at the next multiple of the new cycle from beat 0.  A
// relocate resets it to 0.
//
// Header-only and free of JACK/engine dependencies so the RT code and the
// unit tests (src/test_beat_tracker) run the same arithmetic.  RT-safe:
// scalar arithmetic only, no allocation, no locks, no syscalls.
//------ albEnd ----------------------------------

#ifndef __sooperlooper_beat_tracker__
#define __sooperlooper_beat_tracker__

#include <cmath>
#include <stdint.h>

namespace SooperLooper {

class BeatTracker
{
  public:
	BeatTracker ()
		: _have_pos (false), _have_next (false), _next_frame (0),
		  _seg_frame (0), _seg_beats (0.0), _b0 (0.0), _b1 (0.0), _rate (0.0),
		  _cycle_rate (0.0), _nframes (0), _sr (0.0),
		  _active (false), _relocated (false), _backwards (false),
		  _m_have (false), _m_last (0.0), _m_changes (0), _m_quiet (0.0),
		  _followed (false), _snapped (false), _live (false),
		  _live_changed (false), _absorbed (false), _follow_err (0.0),
		  _m_offset (0.0), _err_avg (0.0), _tick (0.0), _settle_left (0.0),
		  _settle_sum (0.0), _settle_n (0), _settle_base (0.0) {}

	// Call exactly once per audio cycle.  framepos is the transport frame
	// at the start of the cycle, bpm the tempo reported for this cycle
	// (<= 0 means unknown), sr the sample rate.
	void update (uint32_t framepos, uint32_t nframes, double bpm,
	             bool rolling, double sr)
	{
		_active = false;
		_relocated = false;
		_backwards = false;
		_followed = false;
		_snapped = false;
		_live_changed = false;
		_absorbed = false;

		if (!rolling) {
			return;           // frozen; a resume at _next_frame continues
		}
		if (!(bpm > 0.0) || !(sr > 0.0)) {
			_have_pos = false;  // position meaningless; re-anchor later
			_have_next = false;
			return;
		}
		if (_have_next && framepos != _next_frame) {
			_relocated = true;
			// Compare as a signed distance so a uint32 frame counter
			// wrapping past 2^32 is not mistaken for a backwards jump.
			_backwards = (int32_t) (framepos - _next_frame) < 0;
		}
		const double rate = bpm / (60.0 * sr);      // beats per frame
		if (!_have_pos || _relocated) {
			// Anchor where a master that recomputes its position from the
			// frame (Hydrogen, on every locate) puts its beat.
			_seg_frame = framepos;
			_seg_beats = (double) framepos * rate;
			_have_pos = true;
			//------ albBegin ----------------------------------
			// ALB_CYCLE_CHANGE_REANCHOR: a relocate (or a fresh anchor)
			// counts cycles from beat 0 again, as before.
#ifdef ALB_CYCLE_CHANGE_REANCHOR
			_cycle_origin = 0.0;
#endif
			//------ albEnd ----------------------------------
		} else if (rate != _rate) {
			// Tempo change: start a new constant-tempo segment where the
			// previous cycle ended, so the position is continuous.
			_seg_frame = framepos;
			_seg_beats = _b1;
		}
		_rate = rate;
		// Position from the segment start by multiplication, not by
		// summing per-cycle increments, so no rounding drift builds up.
		_b0 = _seg_beats + (double) (uint32_t) (framepos - _seg_frame) * _rate;
		_b1 = _seg_beats + (double) (uint32_t) (framepos + nframes - _seg_frame) * _rate;
		// The rate boundaries() uses.  Equal to _rate unless
		// follow_master() nudges this cycle, so unless the master is
		// being followed (ALB_JACK_BBT_TRACKING) the pulses are exactly
		// as before.
		_cycle_rate = _rate;
		_nframes = nframes;
		_sr = sr;
		_next_frame = framepos + nframes;
		_have_next = true;
		_active = true;
	}

	// The master's position in beats, from JACK's BBT fields.  Uses
	// bar_start_tick (ticks from the song start to this bar) when the
	// master fills it in, which stays right when bars differ in length:
	// in Hydrogen's song mode a bar is a song column, as long as its
	// longest pattern.  Otherwise every bar is taken to be beats_per_bar
	// long.  The half tick centres the estimate, since JACK's tick is a
	// whole number rounded down.  Returns -1 if the fields are unusable.
	// tick_exact, if 0 or more, is the tick with its fraction (JACK's
	// tick_double, JackTickDouble), used instead of tick + 0.5.
	static double bbt_beats (int32_t bar, int32_t beat, int32_t tick,
	                         double bar_start_tick, double beats_per_bar,
	                         double ticks_per_beat, double tick_exact = -1.0)
	{
		if (!(ticks_per_beat > 0.0) || !(beats_per_bar > 0.0) || bar < 1 || beat < 1) {
			return -1.0;
		}
		//------ albBegin ----------------------------------
		const double t = (tick_exact >= 0.0) ? tick_exact : tick + 0.5;
		//------ albEnd ----------------------------------
		const double in_bar = (beat - 1) + t / ticks_per_beat;
		if (bar > 1 && bar_start_tick > 0.0) {
			return bar_start_tick / ticks_per_beat + in_bar;
		}
		return (bar - 1) * beats_per_bar + in_bar;
	}

	// Call after update(), in a cycle where the master publishes a
	// bar/beat/tick position: master_beats is that position, in beats, at
	// the start of the cycle, and ticks_per_beat the master's tick size (0
	// if unknown).  Does nothing unless this cycle is active and the
	// master's position is live -- seen changing at least twice, and
	// within the last half second.
	void follow_master (double master_beats, double ticks_per_beat = 0.0)
	{
		_followed = false;
		_snapped = false;
		_live_changed = false;
		_absorbed = false;
		if (!_active) {
			return;
		}
		if (_m_have && master_beats != _m_last) {
			if (_m_changes < 1000) ++_m_changes;
			_m_quiet = 0.0;
		} else if (_m_have) {
			_m_quiet += (double) _nframes;
		}
		_m_last = master_beats;
		_m_have = true;
		const bool live = !(_m_changes < 2 || _m_quiet > 0.5 * _sr);
		_live_changed = (live != _live);
		_live = live;
		if (!live) {
			return;               // constants (pattern mode) or gone quiet
		}
		const double SNAP  = 0.25;   // beats: a larger difference is a jump
		const double NUDGE = 0.05;   // fraction of a small error taken per cycle
		_tick = ticks_per_beat > 0.0 ? 1.0 / ticks_per_beat : 0.0;
		if (_relocated || _live_changed) {
			_m_offset = 0.0;
			_err_avg = 0.0;
			_settle_left = 0.0;
		}
		const double err = master_beats + _m_offset - _b0;
		_follow_err = err;
		if (_relocated || (_live_changed && std::fabs (err) > SNAP)) {
			// Jump to the master's position: a relocate (into a tempo map,
			// say), or following starting somewhere else.
			_seg_beats += err;
			_b0 += err;
			_b1 += err;
			_err_avg = 0.0;
			_settle_left = 0.0;
			_snapped = true;
		} else if (std::fabs (err) > SNAP) {
			// The master's position jumped with no relocate: a song
			// looping back to its start.  Keep the count, and measure the
			// master from here on relative to where it jumped to.  This
			// first estimate of the jump includes this one cycle's tick
			// rounding -- always the same way round just after a song
			// loops, so it would add up over many loops -- so it is
			// refined while settling (below).
			if (_settle_left > 0.0) {
				finish_settle ();
			}
			_m_offset -= err - _err_avg;
			_settle_left = 0.5 * _sr;
			_settle_sum = 0.0;
			_settle_n = 0;
			_settle_base = _err_avg;
			_absorbed = true;
			return;
		} else if (_settle_left > 0.0) {
			// Settling after a song loop: leave the count alone and average
			// the difference over many ticks, then correct the offset.
			_settle_sum += err;
			++_settle_n;
			_settle_left -= (double) _nframes;
			if (_settle_left <= 0.0) {
				finish_settle ();
			}
			return;
		} else {
			// Nudge: add a little of the error to the end of this cycle, so
			// the count stays continuous and never runs backwards.
			_err_avg += NUDGE * (err - _err_avg);
			double shift = NUDGE * err;
			const double span = _b1 - _b0;
			if (shift < -0.5 * span) {
				shift = -0.5 * span;
			}
			_seg_beats += shift;
			_b1 += shift;
			_cycle_rate = (_b1 - _b0) / (double) _nframes;
		}
		_followed = true;
	}

	// End of settling after a song loop: take out of the offset the average
	// difference seen since, less the one before the loop.  A song is a
	// whole number of the master's ticks long, so the offset (a sum of
	// song lengths) is then rounded to a whole tick; that also removes any
	// small step (under half a tick) that happened at the loop itself, such
	// as a tempo change the master published a cycle late, which the
	// nudging then follows.
	void finish_settle ()
	{
		if (_settle_n > 0) {
			_m_offset -= _settle_sum / _settle_n - _settle_base;
		}
		if (_tick > 0.0) {
			_m_offset = std::floor (_m_offset / _tick + 0.5) * _tick;
		}
		_settle_left = 0.0;
	}

	// True if follow_master() adjusted the count this cycle; snapped() if
	// it jumped rather than nudged; absorbed() if the master jumped without
	// a relocate and the count was kept.  follow_error() is the master's
	// position (plus any offset) minus the count before the adjustment,
	// in beats.
	bool followed () const { return _followed; }
	bool snapped () const { return _snapped; }
	bool absorbed () const { return _absorbed; }
	double follow_error () const { return _follow_err; }
	// Whether the master's position is live (see follow_master()), and
	// whether that changed this cycle -- for logging.
	bool live () const { return _live; }
	bool live_changed () const { return _live_changed; }

	// True if this cycle advanced the position (rolling, tempo known).
	bool active () const { return _active; }
	// True if this cycle started somewhere other than where the previous
	// rolling cycle ended; backwards() if it started earlier.
	bool relocated () const { return _relocated; }
	bool backwards () const { return _backwards; }
	// Beat position at the start / end of this cycle.
	double start_beats () const { return _b0; }
	double end_beats () const { return _b1; }
	// Beats per frame during this cycle.
	double rate () const { return _rate; }

	//------ albBegin ----------------------------------
	// Set the count at the start of this cycle, keeping its tempo: for a
	// relocation that must not move the count (a realign the tempo raced;
	// see the engine).  Call right after update(), in an active cycle.
	void set_position (double beats)
	{
		if (!_active) {
			return;
		}
		_seg_frame = _next_frame - _nframes;
		_seg_beats = beats;
		_b0 = beats;
		_b1 = beats + (double) _nframes * _rate;
		_cycle_rate = _rate;
	}
	//------ albEnd ----------------------------------

	// Write into out[] the offsets (0 .. nframes-1) of every grid boundary
	// in this cycle for a grid of `step` beats; returns how many (at most
	// max_out).  Returns 0 when inactive or step <= 0.
	//------ albBegin ----------------------------------
	// ALB_CYCLE_CHANGE_REANCHOR: the grid is origin + k * step; callers
	// pass cycle_origin() for the cycle grid and nothing (0) otherwise.
#ifdef ALB_CYCLE_CHANGE_REANCHOR
	int boundaries (double step, uint32_t nframes,
	                uint32_t * out, int max_out, double origin = 0.0) const
#else
	int boundaries (double step, uint32_t nframes,
	                uint32_t * out, int max_out) const
#endif
	//------ albEnd ----------------------------------
	{
		if (!_active || !(step > 0.0) || max_out <= 0) {
			return 0;
		}
		// A boundary belongs to this cycle if its exact frame offset is in
		// [-EPS, nframes - EPS).  EPS absorbs rounding when a boundary falls
		// exactly on a cycle edge: it then goes to the later cycle at
		// offset 0, never to the earlier one at offset nframes.  The same
		// rule seen from the neighbouring cycle excludes it there, so each
		// boundary is counted once.
		const double EPS = 1e-6;                     // frames
		//------ albBegin ----------------------------------
#ifdef ALB_CYCLE_CHANGE_REANCHOR
		double b = origin + std::ceil ((_b0 - origin - EPS * _cycle_rate) / step) * step;
#else
		double b = std::ceil ((_b0 - EPS * _cycle_rate) / step) * step;
#endif
		//------ albEnd ----------------------------------
		int n = 0;
		while (n < max_out) {
			double off = (b - _b0) / _cycle_rate;
			if (off >= (double) nframes - EPS) {
				break;
			}
			if (off >= -EPS) {
				double s = std::ceil (off - EPS);    // first sample at/after it
				if (s < 0.0) s = 0.0;
				if (s > (double) (nframes - 1)) s = (double) (nframes - 1);
				out[n++] = (uint32_t) s;
			}
			b += step;
		}
		return n;
	}

	// Frames from the most recent grid boundary at or before the start of
	// this cycle, for a grid of `step` beats (for logging).
	//------ albBegin ----------------------------------
	// ALB_CYCLE_CHANGE_REANCHOR: measured from the grid's origin.
#ifdef ALB_CYCLE_CHANGE_REANCHOR
	double frames_into_step (double step, double origin = 0.0) const
	{
		if (!_active || !(step > 0.0)) {
			return 0.0;
		}
		const double b = _b0 - origin;
		return (b - std::floor (b / step) * step) / _cycle_rate;
	}
#else
	double frames_into_step (double step) const
	{
		if (!_active || !(step > 0.0)) {
			return 0.0;
		}
		return (_b0 - std::floor (_b0 / step) * step) / _cycle_rate;
	}
#endif
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
	// ALB_CYCLE_CHANGE_REANCHOR: the cycle grid's origin, in beats, and
	// moving it when the cycle length changes.  Call reanchor_cycle() in
	// an active cycle, with the step of the grid being replaced; the new
	// grid then starts at that grid's boundary nearest this cycle's start
	// (the next one if it is closer, so a change sent slightly early still
	// lands on the boundary it was meant for).
#ifdef ALB_CYCLE_CHANGE_REANCHOR
	double cycle_origin () const { return _cycle_origin; }
	void reanchor_cycle (double old_step)
	{
		if (!_active || !(old_step > 0.0)) {
			return;
		}
		_cycle_origin += std::floor ((_b0 - _cycle_origin) / old_step + 0.5) * old_step;
	}
#endif
	//------ albEnd ----------------------------------

  private:
	bool     _have_pos;
	bool     _have_next;
	uint32_t _next_frame;
	uint32_t _seg_frame;      // start of the current constant-tempo segment
	double   _seg_beats;      // beat position at _seg_frame
	double   _b0;
	double   _b1;
	double   _rate;           // beats per frame at the reported tempo
	double   _cycle_rate;     // this cycle's rate, including any nudge
	uint32_t _nframes;
	double   _sr;
	bool     _active;
	bool     _relocated;
	bool     _backwards;
	// follow_master() state
	bool     _m_have;
	double   _m_last;         // the master's position last cycle
	int      _m_changes;      // times it has changed (capped)
	double   _m_quiet;        // frames since it last changed
	bool     _followed;
	bool     _snapped;
	bool     _live;
	bool     _live_changed;
	bool     _absorbed;
	double   _follow_err;
	double   _m_offset;       // added to the master's position (song loops)
	double   _err_avg;        // smoothed recent small differences
	double   _tick;           // the master's tick, in beats (0: unknown)
	double   _settle_left;    // frames of settling left after a song loop
	double   _settle_sum;     // sum of differences while settling
	int      _settle_n;
	double   _settle_base;    // _err_avg just before the song loop
	//------ albBegin ----------------------------------
#ifdef ALB_CYCLE_CHANGE_REANCHOR
	double   _cycle_origin = 0.0;   // cycle grid origin, in beats
#endif
	//------ albEnd ----------------------------------
};

} // namespace SooperLooper

#endif
