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
// mod_docs/ROADMAP.rst entries 8 and 15.
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
// Measured against Hydrogen 1.2.4 (ROADMAP entry 15): Hydrogen's audio
// switches tempo one JACK cycle before the new tempo appears in the
// transport position, which leaves pulses about 1 ms late after a ramp.
// The error is bounded by one buffer and does not accumulate, so it is
// not compensated here.
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
		  _active (false), _relocated (false), _backwards (false) {}

	// Call exactly once per audio cycle.  framepos is the transport frame
	// at the start of the cycle, bpm the tempo reported for this cycle
	// (<= 0 means unknown), sr the sample rate.
	void update (uint32_t framepos, uint32_t nframes, double bpm,
	             bool rolling, double sr)
	{
		_active = false;
		_relocated = false;
		_backwards = false;

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
		_next_frame = framepos + nframes;
		_have_next = true;
		_active = true;
	}

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

	// Write into out[] the offsets (0 .. nframes-1) of every grid boundary
	// in this cycle for a grid of `step` beats; returns how many (at most
	// max_out).  Returns 0 when inactive or step <= 0.
	int boundaries (double step, uint32_t nframes,
	                uint32_t * out, int max_out) const
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
		double b = std::ceil ((_b0 - EPS * _rate) / step) * step;
		int n = 0;
		while (n < max_out) {
			double off = (b - _b0) / _rate;
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
	double frames_into_step (double step) const
	{
		if (!_active || !(step > 0.0)) {
			return 0.0;
		}
		return (_b0 - std::floor (_b0 / step) * step) / _rate;
	}

  private:
	bool     _have_pos;
	bool     _have_next;
	uint32_t _next_frame;
	uint32_t _seg_frame;      // start of the current constant-tempo segment
	double   _seg_beats;      // beat position at _seg_frame
	double   _b0;
	double   _b1;
	double   _rate;
	bool     _active;
	bool     _relocated;
	bool     _backwards;
};

} // namespace SooperLooper

#endif
