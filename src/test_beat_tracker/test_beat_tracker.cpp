// Unit tests for src/beat_tracker.hpp (ROADMAP entry 15).  No JACK needed.
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
	test_stop_start ();
	test_unknown_tempo ();
	test_wrap ();
	test_step_change ();
	if (g_fail) { fprintf (stderr, "%d check(s) failed\n", g_fail); return 1; }
	printf ("all beat_tracker tests passed\n");
	return 0;
}
