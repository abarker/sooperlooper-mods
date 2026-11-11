// Unit tests for src/beat_tracker.hpp.  No JACK needed.
//
//   make -C src/test_beat_tracker && src/test_beat_tracker/test_beat_tracker
//
// Also a replay tool for checking the tracker against recordings:
//   test_beat_tracker --replay < cycles.txt
// reads lines "framepos nframes bpm rolling step" (one per audio cycle)
// and prints the absolute frame of every pulse.

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

#include "../beat_tracker.hpp"

using SooperLooper::BeatTracker;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_fail; \
	fprintf (stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf (stderr, __VA_ARGS__); fprintf (stderr, "\n"); } } while (0)

static const double SR = 48000.0;

// Run the tracker over `cycles` cycles of `nf` frames from frame `start`,
// with tempo bpm_at(cycle) and grid step `step`; return absolute pulse frames.
template <class F>
static std::vector<double> run (BeatTracker & bt, uint32_t start, uint32_t nf,
                                int cycles, F bpm_at, double step)
{
	std::vector<double> out;
	uint32_t pos = start;
	for (int c = 0; c < cycles; ++c) {
		bt.update (pos, nf, bpm_at (c), true, SR);
		uint32_t offs[8];
		int n = bt.boundaries (step, nf, offs, 8);
		for (int i = 0; i < n; ++i) out.push_back ((double) pos + offs[i]);
		pos += nf;
	}
	return out;
}

// Stock placement at constant tempo: pulses at multiples of T from frame 0.
static void test_constant_tempo_matches_stock ()
{
	const uint32_t sizes[] = {64, 256, 1000, 4096};
	const double tempos[] = {60.0, 104.0, 131.0, 175.0};
	const double steps[] = {0.5, 1.0, 2.0};
	for (uint32_t nf : sizes) for (double bpm : tempos) for (double step : steps) {
		BeatTracker bt;
		int cycles = (int) (600.0 * SR / nf);            // ten minutes
		std::vector<double> p = run (bt, 0, nf, cycles,
		                             [bpm](int) { return bpm; }, step);
		double T = step * 60.0 * SR / bpm;
		size_t expect = (size_t) std::floor ((double) cycles * nf / T - 1e-9) + 1;
		CHECK (p.size () == expect, "nf=%u bpm=%g step=%g: %zu pulses, expected %zu",
		       nf, bpm, step, p.size (), expect);
		// Within one sample of the exact boundary.  (Stock rounds to the
		// nearest sample; the tracker takes the first sample at or after
		// it, or the cycle's last sample when the boundary falls inside it.)
		for (size_t k = 0; k < p.size (); ++k) {
			double exact = k * T;
			CHECK (std::fabs (p[k] - exact) < 1.0 + 1e-6,
			       "nf=%u bpm=%g step=%g: pulse %zu at %.3f, exact %.3f",
			       nf, bpm, step, k, p[k], exact);
			if (std::fabs (p[k] - exact) >= 1.0 + 1e-6) break;
		}
	}
}

// A tempo change mid-roll keeps the beat continuous: the next boundary
// comes after the *remaining* part of the beat at the new tempo.
static void test_tempo_change_keeps_phase ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	// 120 BPM for 30.1 s worth of whole cycles, then 121 BPM.
	int c1 = (int) (30.1 * SR / nf);
	std::vector<double> p = run (bt, 0, nf, c1 + 2000,
	    [c1](int c) { return c < c1 ? 120.0 : 121.0; }, 1.0);
	double f_change = (double) c1 * nf;
	double beats_at_change = f_change * 120.0 / (60.0 * SR);
	double frac = beats_at_change - std::floor (beats_at_change);
	double first_after = f_change + (1.0 - frac) * 60.0 * SR / 121.0;
	double T121 = 60.0 * SR / 121.0;
	size_t i = 0;
	while (i < p.size () && p[i] < f_change) ++i;
	CHECK (i < p.size (), "no pulse after the change");
	for (int k = 0; k < 50 && i + k < p.size (); ++k) {
		double exact = first_after + k * T121;
		CHECK (std::fabs (p[i + k] - exact) <= 1.0,
		       "pulse %d after change at %.2f, expected %.2f", k, p[i + k], exact);
	}
	// And it is NOT where the stock grid would put it (frame 0 multiples
	// of the new beat), which is the whole point.
	double stock = std::ceil (f_change / T121) * T121;
	CHECK (std::fabs (first_after - stock) > 100.0,
	       "test setup: stock and tracker coincide (%.1f vs %.1f)", first_after, stock);
}

// Tempo changing every single cycle (a fast ramp): every boundary crossed
// exactly once, none dropped or doubled at buffer edges.
static void test_ramp_no_drop_no_double ()
{
	const uint32_t sizes[] = {64, 256, 4096};
	for (uint32_t nf : sizes) {
		BeatTracker bt;
		int cycles = (int) (120.0 * SR / nf);
		double step = 0.5;
		auto bpm_at = [](int c) { return 90.0 + 60.0 * std::fabs (std::sin (c * 0.013)); };
		std::vector<double> p = run (bt, 0, nf, cycles, bpm_at, step);
		// Total beats covered
		double beats = 0.0;
		for (int c = 0; c < cycles; ++c) beats += nf * bpm_at (c) / (60.0 * SR);
		size_t expect = (size_t) std::floor (beats / step - 1e-9) + 1;
		CHECK (p.size () == expect, "nf=%u: %zu pulses, expected %zu", nf, p.size (), expect);
		for (size_t k = 1; k < p.size (); ++k) {
			CHECK (p[k] > p[k - 1], "nf=%u: pulses %zu,%zu out of order or doubled", nf, k - 1, k);
		}
	}
}

// Relocates re-anchor from the frame; the backwards flag matches.
static void test_relocate ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	bt.update (0, nf, 120.0, true, SR);
	bt.update (nf, nf, 130.0, true, SR);           // tempo change, no relocate
	CHECK (!bt.relocated (), "tempo change flagged as relocate");
	bt.update (0, nf, 130.0, true, SR);            // back to 0
	CHECK (bt.relocated () && bt.backwards (), "locate to 0 not flagged backwards");
	CHECK (bt.start_beats () == 0.0, "not re-anchored at 0: %g", bt.start_beats ());
	uint32_t offs[4];
	CHECK (bt.boundaries (1.0, nf, offs, 4) == 1 && offs[0] == 0, "no pulse at frame 0 after locate");
	bt.update (480000, nf, 130.0, true, SR);       // forward jump
	CHECK (bt.relocated () && !bt.backwards (), "forward locate flags wrong");
	CHECK (std::fabs (bt.start_beats () - 480000 * 130.0 / (60.0 * SR)) < 1e-9,
	       "forward locate anchor %g", bt.start_beats ());
}

// set_position() after a relocation keeps the count where it was going
// (a realign the tempo raced): pulses continue one beat apart from it, at
// the new frames; a later relocation re-anchors to frame x tempo again.
static void test_set_position_keeps_count ()
{
	const uint32_t nf = 256;
	const double r = 120.0 / (60.0 * SR);
	BeatTracker bt;
	uint32_t pos = 0;
	for (int c = 0; c < 400; ++c, pos += nf) {
		bt.update (pos, nf, 120.0, true, SR);
	}
	const double kept = bt.end_beats ();            // where the count is going
	bt.update (1000000, nf, 120.0, true, SR);       // relocate elsewhere
	CHECK (bt.relocated (), "relocate not flagged");
	bt.set_position (kept);
	CHECK (bt.start_beats () == kept, "count not kept: %g vs %g", bt.start_beats (), kept);
	CHECK (std::fabs (bt.end_beats () - (kept + nf * r)) < 1e-9, "end of cycle %g", bt.end_beats ());
	// The next pulse: the first whole beat after `kept`, at the new frames.
	std::vector<double> p;
	uint32_t f = 1000000;
	for (int c = 0; c < 200; ++c) {
		if (c > 0) bt.update (f, nf, 120.0, true, SR);
		CHECK (c == 0 || !bt.relocated (), "continuing after set_position flagged as relocate");
		uint32_t offs[8];
		int n = bt.boundaries (1.0, nf, offs, 8);
		for (int i = 0; i < n; ++i) p.push_back ((double) f + offs[i]);
		f += nf;
	}
	CHECK (p.size () >= 2, "no pulses after set_position");
	if (p.size () >= 2) {
		const double first = 1000000 + (std::ceil (kept) - kept) / r;
		CHECK (std::fabs (p[0] - first) <= 1.0, "first pulse %g, expected %g", p[0], first);
		CHECK (std::fabs ((p[1] - p[0]) - 1.0 / r) <= 1.0, "pulse spacing %g", p[1] - p[0]);
	}
	bt.update (480000, nf, 120.0, true, SR);        // an ordinary relocate
	CHECK (std::fabs (bt.start_beats () - 480000 * r) < 1e-9,
	       "relocate after set_position not re-anchored: %g", bt.start_beats ());
}

// Stop/start without a locate continues; with a locate to 0 re-anchors.
static void test_stop_start ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	bt.update (0, nf, 120.0, true, SR);
	bt.update (nf, nf, 125.0, true, SR);
	double before = bt.end_beats ();
	bt.update (2 * nf, nf, 125.0, false, SR);      // stopped: frozen
	bt.update (2 * nf, nf, 125.0, false, SR);
	CHECK (!bt.active (), "active while stopped");
	bt.update (2 * nf, nf, 125.0, true, SR);       // resume, same frame
	CHECK (!bt.relocated (), "resume flagged as relocate");
	CHECK (bt.start_beats () == before, "resume lost position: %g vs %g", bt.start_beats (), before);
	bt.update (3 * nf, nf, 125.0, false, SR);      // stop...
	bt.update (0, nf, 125.0, true, SR);            // ...locate 0, start
	CHECK (bt.relocated () && bt.start_beats () == 0.0, "pedal stop/start not re-anchored");
}

// Unknown tempo: no pulses, and re-anchor from the frame once it is known.
static void test_unknown_tempo ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	bt.update (0, nf, 120.0, true, SR);
	bt.update (nf, nf, 0.0, true, SR);
	uint32_t offs[4];
	CHECK (!bt.active () && bt.boundaries (1.0, nf, offs, 4) == 0, "pulses with unknown tempo");
	bt.update (2 * nf, nf, 100.0, true, SR);
	CHECK (bt.active () && std::fabs (bt.start_beats () - 2.0 * nf * 100.0 / (60.0 * SR)) < 1e-12,
	       "not re-anchored after unknown tempo");
}

// Frame counter wrapping past 2^32 is not a relocate.
static void test_wrap ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	uint32_t pos = 0xFFFFFFFFu - 3 * nf + 1;
	bt.update (pos, nf, 120.0, true, SR);
	for (int i = 0; i < 6; ++i) {
		pos += nf;
		bt.update (pos, nf, 120.0, true, SR);
		CHECK (!bt.relocated (), "wrap flagged as relocate at step %d", i);
	}
}

// Changing the step (Eighths/Cycle) mid-roll: boundaries are multiples
// of the new step from the anchor, still continuous in beats.
static void test_step_change ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	std::vector<double> beats_at_pulse;
	uint32_t pos = 0;
	for (int c = 0; c < 4000; ++c) {
		double step = c < 2000 ? 1.0 : 2.0;
		bt.update (pos, nf, 120.0, true, SR);
		uint32_t offs[4];
		int n = bt.boundaries (step, nf, offs, 4);
		for (int i = 0; i < n; ++i) {
			double b = ((double) pos + offs[i]) * 120.0 / (60.0 * SR);
			double r = std::fmod (b + 1e-6, step);
			CHECK (r < 2.0 / nf, "pulse at beat %.6f not on a step-%g boundary", b, step);
		}
		pos += nf;
	}
}

// ---- reanchor_cycle(): ALB_CYCLE_CHANGE_REANCHOR ----
#ifdef ALB_CYCLE_CHANGE_REANCHOR

// Run at 120 BPM with cycle step `s1` (beats) until the cycle starting at
// beat `change_at`, then switch to `s2` the way generate_sync() does
// (reanchor with the old step, then use the new one).  Optionally relocate
// to frame 0 at beat `relocate_at`.  Returns pulse positions in beats.
static std::vector<double> run_change (double s1, double s2, double change_at,
                                       double relocate_at = -1.0)
{
	const uint32_t nf = 256;
	const double rate = 120.0 / (60.0 * SR);
	BeatTracker bt;
	std::vector<double> beats;
	uint32_t pos = 0;
	double step = s1;
	bool changed = false, relocated = false;
	for (int c = 0; c < 6000; ++c) {
		bt.update (pos, nf, 120.0, true, SR);
		if (!changed && bt.start_beats () >= change_at) {
			bt.reanchor_cycle (step);
			step = s2;
			changed = true;
		}
		uint32_t offs[8];
		int n = bt.boundaries (step, nf, offs, 8, bt.cycle_origin ());
		for (int i = 0; i < n; ++i) beats.push_back (((double) pos + offs[i]) * rate);
		pos += nf;
		if (relocate_at >= 0.0 && !relocated && bt.end_beats () >= relocate_at) {
			pos = 0;
			relocated = true;
			beats.push_back (-1.0);              // marks the relocate
		}
	}
	return beats;
}

static bool has_pulse_near (const std::vector<double> & v, double b)
{
	for (double x : v) if (std::fabs (x - b) < 0.01) return true;
	return false;
}

static void test_reanchor_after_boundary ()
{
	// 4-beat cycles; change to 3 at beat ~9: nearest old boundary is 8.
	std::vector<double> p = run_change (4.0, 3.0, 9.0);
	CHECK (has_pulse_near (p, 11.0) && has_pulse_near (p, 14.0) && has_pulse_near (p, 17.0),
	       "after a change at beat 9, expected pulses at 11, 14, 17 (from the bar at 8)");
	CHECK (!has_pulse_near (p, 12.0) && !has_pulse_near (p, 15.0),
	       "pulses still on the grid counted from beat 0");
}

static void test_reanchor_just_before_boundary ()
{
	// Change at beat ~11.8, just before the bar at 12: anchors to 12.
	std::vector<double> p = run_change (4.0, 3.0, 11.8);
	CHECK (has_pulse_near (p, 12.0) && has_pulse_near (p, 15.0) && has_pulse_near (p, 18.0),
	       "a change just before the bar at 12 should anchor there");
	CHECK (!has_pulse_near (p, 11.0) && !has_pulse_near (p, 14.0),
	       "an early change anchored to the bar before");
}

static void test_reanchor_no_double_or_drop ()
{
	std::vector<double> p = run_change (4.0, 3.0, 9.0);
	for (size_t k = 1; k < p.size (); ++k) {
		double gap = p[k] - p[k - 1];
		CHECK (gap > 2.9, "pulses %zu,%zu only %.3f beats apart", k - 1, k, gap);
	}
}

static void test_reanchor_reset_by_relocate ()
{
	// Anchor at 8, then relocate to frame 0 at beat ~20: count from 0 again.
	std::vector<double> p = run_change (4.0, 3.0, 9.0, 20.0);
	size_t i = 0;
	while (i < p.size () && p[i] != -1.0) ++i;     // skip to the marker
	++i;
	CHECK (i < p.size () && std::fabs (p[i]) < 0.01, "no pulse at beat 0 after the relocate");
	CHECK (has_pulse_near (std::vector<double> (p.begin () + i, p.end ()), 3.0),
	       "after a relocate the grid should count from 0 (pulse at 3)");
}

static void test_reanchor_origin_only_for_cycle_grid ()
{
	// An odd cycle (1.5 beats) can put the origin on a half beat; the
	// quarter-note grid (called without an origin) must not move.
	const uint32_t nf = 256;
	BeatTracker bt;
	uint32_t pos = 0;
	for (int c = 0; c < 400; ++c) { bt.update (pos, nf, 120.0, true, SR); pos += nf; }
	bt.update (pos, nf, 120.0, true, SR);
	bt.reanchor_cycle (1.5);
	double org = bt.cycle_origin ();
	CHECK (std::fabs (std::fmod (org, 1.5)) < 1e-9 || std::fabs (std::fmod (org, 1.5) - 1.5) < 1e-9,
	       "origin %g not on the old 1.5-beat grid", org);
	std::vector<double> q;
	pos += nf;
	for (int c = 0; c < 2000; ++c) {
		bt.update (pos, nf, 120.0, true, SR);
		uint32_t offs[8];
		int n = bt.boundaries (1.0, nf, offs, 8);
		for (int i = 0; i < n; ++i) q.push_back (((double) pos + offs[i]) * 120.0 / (60.0 * SR));
		pos += nf;
	}
	for (double b : q) {
		CHECK (std::fabs (b - std::round (b)) < 0.01, "quarter pulse at %.3f moved off the beat", b);
	}
}

#endif // ALB_CYCLE_CHANGE_REANCHOR

// ---- follow_master(): following the master's position (ALB_JACK_BBT_TRACKING) ----

// What a master publishes for a true position of `beats`: whole ticks,
// plus the half tick the engine adds to centre the estimate.
static double published (double beats, double tpb)
{
	return (std::floor (beats * tpb) + 0.5) / tpb;
}

// A master publishing constants (Hydrogen in pattern mode: bar 1, beat 1,
// tick 0) changes nothing, through tempo changes and a relocate.
static void test_follow_ignores_constant_master ()
{
	const uint32_t nf = 256;
	BeatTracker a, b;
	uint32_t pos = 0;
	std::vector<double> pa, pb;
	bool ever = false;
	for (int c = 0; c < 4000; ++c) {
		if (c == 2000) pos = 0;
		const double bpm = c < 1000 ? 120.0 : (c < 1500 ? 150.0 : 97.0);
		a.update (pos, nf, bpm, true, SR);
		b.update (pos, nf, bpm, true, SR);
		b.follow_master (published (0.0, 48.0));
		ever = ever || b.followed ();
		uint32_t oa[8], ob[8];
		int na = a.boundaries (1.0, nf, oa, 8), nb = b.boundaries (1.0, nf, ob, 8);
		for (int i = 0; i < na; ++i) pa.push_back ((double) pos + oa[i]);
		for (int i = 0; i < nb; ++i) pb.push_back ((double) pos + ob[i]);
		pos += nf;
	}
	CHECK (!ever, "a constant master was followed");
	CHECK (pa == pb, "a constant master changed the pulses");
}

// A DAW with a tempo map: after a relocate into the song the master's
// position is not frame * beat length.  The count jumps to the master's
// position on the relocate and stays with it.
static void test_follow_snaps_on_relocate ()
{
	const uint32_t nf = 256;
	const double bpm = 120.0, rate = bpm / (60.0 * SR), tpb = 1920.0, off = 0.1;  // under the snap threshold
	BeatTracker bt;
	uint32_t pos = 0;
	for (int c = 0; c < 400; ++c) {
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (published (pos * rate, tpb));
		pos += nf;
	}
	pos = 1000000;
	bt.update (pos, nf, bpm, true, SR);
	bt.follow_master (published (pos * rate + off, tpb));
	CHECK (bt.relocated () && bt.followed () && bt.snapped (), "relocate not followed by a jump");
	CHECK (std::fabs (bt.start_beats () - (pos * rate + off)) < 1.0 / tpb,
	       "not on the master after the relocate: %.6f vs %.6f",
	       bt.start_beats (), pos * rate + off);
	double worst = 0.0;
	for (int c = 0; c < 400; ++c) {
		pos += nf;
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (published (pos * rate + off, tpb));
		worst = std::max (worst, std::fabs (bt.start_beats () - (pos * rate + off)));
	}
	CHECK (worst < 1.0 / tpb, "drifted off the master after the relocate: %.6f beats", worst);
}

// A small difference is nudged away without doubling or dropping a pulse,
// even with coarse ticks (Hydrogen publishes 48 per beat).
static void test_follow_nudges_without_double ()
{
	const uint32_t nf = 256;
	const double bpm = 120.0, rate = bpm / (60.0 * SR), tpb = 48.0, off = 0.2;
	BeatTracker bt;
	uint32_t pos = 0;
	std::vector<double> p;
	for (int c = 0; c < 4000; ++c) {
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (published (pos * rate + off, tpb));
		uint32_t o[8];
		int n = bt.boundaries (1.0, nf, o, 8);
		for (int i = 0; i < n; ++i) p.push_back ((double) pos + o[i]);
		pos += nf;
	}
	const double err = bt.start_beats () - ((pos - nf) * rate + off);
	CHECK (std::fabs (err) < 0.01, "not converged: %.5f beats off", err);
	int bad = 0;
	for (size_t k = 1; k < p.size (); ++k)
		if (p[k] - p[k - 1] < 0.7 / rate) ++bad;
	CHECK (bad == 0, "%d pulse pair(s) closer than 0.7 beat", bad);
	const double beats = (pos - nf) * rate + off;
	CHECK (std::fabs ((double) p.size () - beats) <= 1.0,
	       "%zu pulses over %.2f beats", p.size (), beats);
}

// A master whose position stops changing for over half a second is no
// longer followed.
static void test_follow_quiet_master ()
{
	const uint32_t nf = 256;
	const double rate = 120.0 / (60.0 * SR);
	BeatTracker bt;
	uint32_t pos = 0;
	for (int c = 0; c < 50; ++c) {
		bt.update (pos, nf, 120.0, true, SR);
		bt.follow_master (published (pos * rate, 1920.0));
		pos += nf;
	}
	CHECK (bt.followed () && bt.live (), "a live master was not followed");
	const double frozen = published (pos * rate, 1920.0);
	int stops = 0;
	for (int c = 0; c < 200; ++c) {
		bt.update (pos, nf, 120.0, true, SR);
		bt.follow_master (frozen);
		if (bt.live_changed ()) ++stops;
		pos += nf;
	}
	CHECK (!bt.followed () && !bt.live (), "a frozen master was still followed");
	CHECK (stops == 1, "%d live->not-live transitions, expected 1", stops);
}

// Nothing is followed while stopped.
static void test_follow_not_when_stopped ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	bool ever = false;
	for (int c = 0; c < 100; ++c) {
		bt.update (0, nf, 120.0, false, SR);
		bt.follow_master (c * 0.01);
		ever = ever || bt.followed ();
	}
	CHECK (!ever, "followed while stopped");
}

// bbt_beats(): bar_start_tick when the master fills it in, otherwise
// bars of beats_per_bar.
static void test_bbt_beats_formula ()
{
	const double tpb = 48.0;
	// No bar_start_tick (0): bar 3, beat 2, tick 24 of 4/4 bars.
	double b = BeatTracker::bbt_beats (3, 2, 24, 0.0, 4.0, tpb);
	CHECK (std::fabs (b - (8.0 + 1.0 + 24.5 / tpb)) < 1e-12, "fallback formula gave %.6f", b);
	// Hydrogen song mode, columns of 4, 4 and 2 beats: bar 4 starts at
	// beat 10, and its own beats_per_bar (4) says nothing about the
	// earlier bars.
	b = BeatTracker::bbt_beats (4, 1, 0, 10.0 * tpb, 4.0, tpb);
	CHECK (std::fabs (b - (10.0 + 0.5 / tpb)) < 1e-12, "bar_start_tick formula gave %.6f", b);
	// Bar 1: bar_start_tick is 0 either way.
	b = BeatTracker::bbt_beats (1, 3, 0, 0.0, 4.0, tpb);
	CHECK (std::fabs (b - (2.0 + 0.5 / tpb)) < 1e-12, "bar 1 gave %.6f", b);
	CHECK (BeatTracker::bbt_beats (1, 1, 0, 0.0, 4.0, 0.0) < 0.0, "tpb 0 accepted");
	CHECK (BeatTracker::bbt_beats (0, 1, 0, 0.0, 4.0, tpb) < 0.0, "bar 0 accepted");
	CHECK (BeatTracker::bbt_beats (1, 1, 0, 0.0, 0.0, tpb) < 0.0, "0 beats per bar accepted");
	// The exact tick (JACK's tick_double) replaces tick + 0.5.
	b = BeatTracker::bbt_beats (4, 1, 0, 10.0 * tpb, 4.0, tpb, 0.25);
	CHECK (std::fabs (b - (10.0 + 0.25 / tpb)) < 1e-12, "tick_double gave %.6f", b);
	b = BeatTracker::bbt_beats (3, 2, 24, 0.0, 4.0, tpb, 24.0);
	CHECK (std::fabs (b - (8.0 + 1.0 + 24.0 / tpb)) < 1e-12, "tick_double fallback formula gave %.6f", b);
}

// Hydrogen in song mode with song columns of different lengths (4, 4, 2,
// 3, 4 beats, repeated): bar = column, beats_per_bar = that column's
// length, bar_start_tick = ticks before it.  Followed without a jump.
struct H2Song {
	std::vector<double> starts;   // beat at which each column starts
	std::vector<double> lens;
	H2Song () {
		const double L[] = { 4, 4, 2, 3, 4 };
		double at = 0.0;
		for (int r = 0; r < 20; ++r)
			for (int i = 0; i < 5; ++i) { starts.push_back (at); lens.push_back (L[i]); at += L[i]; }
	}
	// Published position for a true position of `beats`.
	double publish (double beats, double tpb) const {
		size_t c = 0;
		while (c + 1 < starts.size () && starts[c + 1] <= beats) ++c;
		const double t = std::floor ((beats - starts[c]) * tpb);   // ticks into the column
		const int beat = (int) std::floor (t / tpb) + 1;
		const int tick = (int) (t - (beat - 1) * tpb);
		return BeatTracker::bbt_beats ((int) c + 1, beat, tick, starts[c] * tpb, lens[c], tpb);
	}
};

static void test_follow_song_mode_mixed_bars ()
{
	const uint32_t nf = 256;
	const double bpm = 120.0, rate = bpm / (60.0 * SR), tpb = 48.0;
	const H2Song song;
	BeatTracker bt;
	uint32_t pos = 0;
	std::vector<double> p;
	int snaps = 0;
	double worst = 0.0;
	for (int c = 0; c < 6000; ++c) {                 // ~64 beats, 20+ columns
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (song.publish (pos * rate, tpb));
		if (bt.snapped ()) ++snaps;
		if (bt.live ()) worst = std::max (worst, std::fabs (bt.start_beats () - pos * rate));
		uint32_t o[8];
		int n = bt.boundaries (1.0, nf, o, 8);
		for (int i = 0; i < n; ++i) p.push_back ((double) pos + o[i]);
		pos += nf;
	}
	CHECK (snaps == 0, "%d jump(s) following a song with mixed bar lengths", snaps);
	CHECK (worst < 0.02, "count up to %.4f beats off the song", worst);
	int bad = 0;
	for (size_t k = 1; k < p.size (); ++k)
		if (std::fabs ((p[k] - p[k - 1]) * rate - 1.0) > 0.02) ++bad;
	CHECK (bad == 0, "%d uneven beat pulse gap(s)", bad);
}

// Hydrogen's song looping (4/4 + 3/4 + 3/4 = 10 beats, as sampled live):
// the JACK frame keeps counting while the position returns to bar 1.  With
// a 4-beat cycle, which does not divide the song, the pulses must stay
// evenly spaced: each loop is absorbed as an offset, never a jump.
static double loop_song_publish (double beats)
{
	const double s = std::fmod (beats, 10.0);
	const int bar = s < 4 ? 1 : (s < 7 ? 2 : 3);
	const double st = bar == 1 ? 0 : (bar == 2 ? 4 : 7), bpb = bar == 1 ? 4 : 3;
	const double t = std::floor ((s - st) * 48.0);
	const int beat = (int) (t / 48.0) + 1, tick = (int) (t - (beat - 1) * 48.0);
	return BeatTracker::bbt_beats (bar, beat, tick, st * 48.0, bpb, 48.0);
}

static void test_follow_song_loop_absorbed ()
{
	const uint32_t nf = 256;
	const double bpm = 141.0, rate = bpm / (60.0 * SR), step = 4.0;
	BeatTracker bt;
	uint32_t pos = 0;
	std::vector<double> p;
	int snaps = 0, absorbs = 0;
	for (int c = 0; c < 40 * 48000 / 256; ++c) {     // 40 s, about 9 loops
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (loop_song_publish (pos * rate), 48.0);
		if (bt.snapped ()) ++snaps;
		if (bt.absorbed ()) ++absorbs;
		uint32_t o[8];
		int n = bt.boundaries (step, nf, o, 8);
		for (int i = 0; i < n; ++i) p.push_back ((double) pos + o[i]);
		pos += nf;
	}
	const int loops = (int) (pos * rate / 10.0);
	CHECK (snaps == 0, "%d jump(s) at song loops", snaps);
	CHECK (absorbs == loops, "%d loop(s) absorbed, expected %d", absorbs, loops);
	int bad = 0;
	for (size_t k = 1; k < p.size (); ++k)
		if (std::fabs ((p[k] - p[k - 1]) * rate - step) > 0.02) ++bad;
	CHECK (bad == 0, "%d uneven cycle gap(s) across song loops", bad);

	// Then a relocate to the start of bar 2 (song beat 4): the count must
	// land on the master's position itself, not on it plus the offset the
	// loops left behind.
	pos += 1000000;
	const double at = 4.0 - pos * rate;     // master = pos * rate + at
	for (int c = 0; c < 50; ++c) {
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (published (pos * rate + at, 48.0));
		if (c == 0) {
			CHECK (bt.relocated () && bt.snapped (), "relocate after song loops not a jump");
			CHECK (std::fabs (bt.start_beats () - 4.0) < 0.03,
			       "after song loops, relocate landed on %.4f, not 4", bt.start_beats ());
		}
		pos += nf;
	}
}

// Following that starts with the master somewhere else (the transport
// started in bar 3 of a song with a tempo change before it) jumps to the
// master once, even without a relocate.
static void test_follow_start_mid_song ()
{
	const uint32_t nf = 256;
	const double bpm = 120.0, rate = bpm / (60.0 * SR), off = 2.03;
	BeatTracker bt;
	uint32_t pos = 480000;
	int snaps = 0;
	for (int c = 0; c < 200; ++c) {
		bt.update (pos, nf, bpm, true, SR);
		bt.follow_master (published (pos * rate + off, 1920.0));
		if (bt.snapped ()) ++snaps;
		pos += nf;
	}
	CHECK (snaps == 1, "%d jump(s) when following started off the master", snaps);
	const double err = bt.start_beats () - ((pos - nf) * rate + off);
	CHECK (std::fabs (err) < 0.01, "%.4f beats off the master after starting", err);
}

// The song sampled live: 4/4 + 3/4 at 141 BPM, then bar 3 at 100 BPM,
// looping for ten minutes.  Modelled as Hydrogen does it: the position
// follows the tempo that applies one cycle before JACK reports it.  The
// count must stay on the master's position (unwrapped) for the whole run,
// with every loop absorbed and no jumps -- one-cycle rounding errors and
// the tempo step at each loop must not add up.
static void test_follow_song_loop_tempo_change ()
{
	const uint32_t nf = 256;
	BeatTracker bt;
	uint32_t pos = 0;
	double U = 0.0;                   // the master's position, unwrapped
	double shown_bpm = 141.0;         // tempo JACK reports (a cycle late)
	int snaps = 0, absorbs = 0;
	double worst = 0.0;
	const int cycles = 600 * 48000 / 256;
	for (int c = 0; c < cycles; ++c) {
		const double song = std::fmod (U, 10.0);
		const double bpm_now = song >= 7.0 ? 100.0 : 141.0;
		bt.update (pos, nf, shown_bpm, true, SR);
		bt.follow_master (loop_song_publish (U), 48.0);
		if (bt.snapped ()) ++snaps;
		if (bt.absorbed ()) ++absorbs;
		if (c > 1000) worst = std::max (worst, std::fabs (bt.start_beats () - U));
		U += nf * bpm_now / (60.0 * SR);
		shown_bpm = bpm_now;
		pos += nf;
	}
	const int loops = (int) (U / 10.0);
	CHECK (snaps == 0, "%d jump(s) in a looping song with a tempo change", snaps);
	CHECK (absorbs == loops, "%d loop(s) absorbed, expected %d", absorbs, loops);
	CHECK (worst < 0.02, "count up to %.4f beats off the master over %d loops", worst, loops);
	printf ("  song-loop model: %d loops, count within %.4f beats of the master\n", loops, worst);
}

static int replay ()
{
	BeatTracker bt;
	unsigned long fp, nf; double bpm, step; int rolling;
	while (scanf ("%lu %lu %lf %d %lf", &fp, &nf, &bpm, &rolling, &step) == 5) {
		bt.update ((uint32_t) fp, (uint32_t) nf, bpm, rolling != 0, SR);
		uint32_t offs[8];
		int n = bt.boundaries (step, (uint32_t) nf, offs, 8);
		for (int i = 0; i < n; ++i) printf ("%lu\n", fp + offs[i]);
	}
	return 0;
}

int main (int argc, char ** argv)
{
	if (argc > 1 && std::string (argv[1]) == "--replay") return replay ();
	test_constant_tempo_matches_stock ();
	test_tempo_change_keeps_phase ();
	test_ramp_no_drop_no_double ();
	test_relocate ();
	test_set_position_keeps_count ();
	test_stop_start ();
	test_unknown_tempo ();
	test_wrap ();
	test_step_change ();
	test_follow_ignores_constant_master ();
	test_follow_snaps_on_relocate ();
	test_follow_nudges_without_double ();
	test_follow_quiet_master ();
	test_follow_not_when_stopped ();
	test_bbt_beats_formula ();
	test_follow_song_mode_mixed_bars ();
	test_follow_song_loop_absorbed ();
	test_follow_start_mid_song ();
	test_follow_song_loop_tempo_change ();
#ifdef ALB_CYCLE_CHANGE_REANCHOR
	test_reanchor_after_boundary ();
	test_reanchor_just_before_boundary ();
	test_reanchor_no_double_or_drop ();
	test_reanchor_reset_by_relocate ();
	test_reanchor_origin_only_for_cycle_grid ();
	printf ("reanchor tests ran\n");
#endif
	if (g_fail) { fprintf (stderr, "%d check(s) failed\n", g_fail); return 1; }
	printf ("all beat_tracker tests passed\n");
	return 0;
}
