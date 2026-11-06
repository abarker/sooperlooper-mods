/* SooperLooper.c  :  
   Copyright (C) 2002-2005 Jesse Chappell <jesse@essej.net>

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
// ========================================================================


   This is still based on the original LADSPA plugin, yeah it's a mess.

*/
   
/*****************************************************************************/

#include <climits>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <iostream>

using namespace std;

/*****************************************************************************/

#include "ladspa.h"

#include "plugin.hpp"
#include "utils.hpp"

#include "event.hpp"

using namespace SooperLooper;

//------ albBegin ----------------------------------
// Late-hit record window: constants, load probe, and debug logging
// infrastructure added to support the late-hit correction feature.
//
// LATE_HIT_WINDOW_MS: if the user presses Record or Stop within this
// many ms *after* a sync boundary, the hit is treated as belonging to that
// boundary rather than waiting for the next one.
//
// SL_LoadProbe: C++ static initializer that fires when the binary loads
// (before main) and writes a marker file to confirm the experimental build
// is running.  Useful when slgui swallows stderr.
//
// sl_dbg_file / sl_dbg_open / SL_LOG / SL_LOG_RT: log-file infrastructure so
// debug output goes to /tmp/sl_debug.log (or ~/sl_debug.log) regardless of
// how the process was launched.  ALL of this infrastructure is gated on
// #define SYNCWDEBUG below.  When SYNCWDEBUG is not defined: no log file is
// created, no sentinel file is written to /tmp, and all SL_LOG / SL_LOG_RT
// calls expand to nothing.  The only unconditional output is the
// fprintf(stderr, ...) banner in engine.cpp after session load.
//
// SYNCWDEBUG / SYNCWDBG: per-press timing diagnostics.  Define SYNCWDEBUG to
// enable all debug output.  Leave it commented out for production/performance
// builds.  WARNING: when enabled, SL_LOG_RT calls fflush() on every button
// press, which can cause RT-thread blocking and xruns.

// Window size in milliseconds for late-hit record correction.
// LATE_HIT_WINDOW_MS is the compile-time default and must remain defined
// so it can be used as the initialiser for g_late_hit_window_ms below.
#define LATE_HIT_WINDOW_MS 100.0f

// Runtime value of the late-hit window.  Starts at the compile-time default
// and may be overwritten by looper.cpp after reading the session file.
//
// IMPORTANT — non-standard config mechanism:
// Every other engine parameter in this codebase is delivered through the
// LADSPA port system: looper.cpp allocates a float, calls connect_port(),
// and plugin.cc reads it via a pointer stored in SooperLooperI.  That
// mechanism is the right choice for values that need OSC/GUI exposure or that
// may change while the engine is running.
//
// g_late_hit_window_ms is intentionally NOT a LADSPA port.  It is a plain
// file-scope global written by looper.cpp (via an extern declaration there)
// during session load, before instantiate() is called.  This is acceptable
// because:
//   1. The value is a setup-time preference, not a real-time control.
//   2. It is read at buffer-processing time but never changes mid-session.
//   3. Keeping it out of the port system avoids touching looper.cpp's OSC
//      dispatch, the LADSPA descriptor port table, and slgui.
//
// Consequence: this mechanism only works in the standalone build where
// looper.cpp and plugin.cc share an address space.  When plugin.cc is loaded
// as a true LADSPA plugin by a DAW host, looper.cpp is absent and the global
// retains its LATE_HIT_WINDOW_MS default for the entire session.  That
// is acceptable behaviour for the plugin case.
//
// Future: if the value ever needs GUI exposure, the correct path is to add a
// LADSPA port (matching all the other parameters) and remove this global.
float g_late_hit_window_ms = LATE_HIT_WINDOW_MS;

// Maximum fraction of one Quantize unit that the late-hit window may span.
// effectiveLateHitWindow() always evaluates both limits and returns the
// smaller:
//
//     min( g_late_hit_window_ms * 0.001 * sampleRate,
//          min(g_late_hit_max_quant_unit_fraction, 0.5) * lLastSyncPeriodSamples )
//
// lLastSyncPeriodSamples is the Quantize unit in samples (eighthSamples *
// eighthPerCycle for QUANT_CYCLE, eighthSamples for QUANT_8TH), so the
// fraction term is relative to the spacing of the boundaries the Quantize
// setting gives.  A value of 0.25 means "at most 25 % of one unit".  Values
// above 0.5 act as 0.5: the nearer boundary always wins.
//
// Setting either parameter to a sufficiently large value renders that term
// larger than any practical T, so the other term wins exclusively:
//
//   Large g_late_hit_max_quant_unit_fraction  →  only the ms cap matters.
//   Large g_late_hit_window_ms                →  only the fraction cap matters.
//
// Setting EITHER parameter to zero makes the corresponding term zero, so the
// minimum is zero and the late-hit window is disabled entirely, reverting to
// stock SooperLooper behaviour.  Setting BOTH to zero has the same effect.
//
// The default (100.0) acts as 0.5, so by default only the ms cap and the
// half-unit cap apply.
//
// lLastSyncPeriodSamples is initialised to ULONG_MAX in activate() so that
// the fraction term is effectively infinite before the first complete sync
// period is measured; the ms term therefore wins until real tempo data
// arrives, regardless of the configured fraction.
//
// Both globals are saved and loaded from the session file as XML properties
// "late_hit_window_ms" and "late_hit_max_quant_unit_fraction" in the
// <Globals> node (written/read by engine.cpp save_session/load_session).
// The compile-time constants below are the fallback when loading a session
// that pre-dates this patch.
#define LATE_HIT_MAX_QUANT_UNIT_FRACTION 100.0f
float g_late_hit_max_quant_unit_fraction = LATE_HIT_MAX_QUANT_UNIT_FRACTION;

//------ albBegin ----------------------------------
// RECORD_FAILSAFE_SECONDS: if a record is left running in STATE_RECORD for
// longer than this many seconds -- e.g. the performer got distracted and
// never pressed anything to stop it -- treat it exactly as if the
// long-press CANCEL_RECORD_MUTE command had just arrived: discard the take
// and mute, same as cancelling by hand.  See the guard added at the
// CANCEL_RECORD_MUTE remap in run(), and "New Feature: record
// failsafe" in mod_docs/late_hit_patch_summary.rst.
//
// Deliberately NOT gated on STATE_TRIG_STOP too: that state is a brief,
// self-resolving wait for the next quantize boundary (at most one loop
// cycle) with no way to get stuck in it, unlike STATE_RECORD, which has no
// automatic exit at all.
//
// Same non-LADSPA-port mechanism as g_late_hit_window_ms above, and for the
// same reasons: a setup-time preference, not a real-time control, saved and
// loaded as the XML property "record_failsafe_seconds" in the <Globals>
// node by engine.cpp save_session/load_session.  The compile-time constant
// below is the fallback for a session that predates this feature, and the
// default when running standalone.
//
// <= 0.0f disables the failsafe entirely, matching the late-hit window's
// "0 disables" convention.
//
// Default is OFF (0.0f), unlike g_late_hit_window_ms's "on" default above.
// Reason: this feature can discard a take outright with no press at all,
// so it ships opt-in -- set record_failsafe_seconds (> 0) explicitly in
// the session file, or via OSC /set once that's wired up, to enable it.
#define RECORD_FAILSAFE_SECONDS_DEFAULT 0.0f
float g_record_failsafe_seconds = RECORD_FAILSAFE_SECONDS_DEFAULT;
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// MUTE_LATE_HIT_MS: window (in milliseconds) within which a MUTE press --
// either entering mute from an actively playing loop, or leaving mute
// (unmute) -- arriving just after a sync boundary is treated as if it had
// arrived exactly at that boundary, instead of waiting for the NEXT
// boundary (up to a full quantize cycle later). See "New Feature:
// mute/unmute late-hit correction" in mod_docs/late_hit_patch_summary.rst.
//
// A plain ms value, with no fraction setting of its own (unlike
// g_late_hit_window_ms, see g_late_hit_max_quant_unit_fraction above).
// Mute/unmute never reconstructs content -- it only chooses immediate vs
// wait.  It does share Record's half-unit cap: a window past the middle of
// the Quantize unit would act at once on a press meant for the next
// boundary, muting or unmuting early.  With the cap the nearer boundary
// wins here too.
//
// Same non-LADSPA-port mechanism as the other late-hit/failsafe globals
// above, and for the same reasons: a setup-time preference, not a
// real-time control, saved/loaded as the XML property "mute_late_hit_ms"
// in the <Globals> node by engine.cpp save_session/load_session.
//
// 0.0f disables the feature entirely -- same "0 disables" convention as
// g_late_hit_window_ms / g_record_failsafe_seconds.
//
// Default is ON at 100.0f, matching g_late_hit_window_ms's own default
// exactly -- unlike record_failsafe (which can silently discard an
// entire take), this feature can only ever turn a "wait for the next
// boundary" into "act now," never lose content, so it carries the same
// default-on rationale as Record's own late-hit correction rather than
// record_failsafe's opt-in one. Confirmed working live on the rig
// 2026-09-04 ("tested live, I didn't notice any anomalies in muting")
// before this default was flipped from the original 0.0f (off) it
// shipped with.
#define MUTE_LATE_HIT_MS_DEFAULT 100.0f
float g_mute_late_hit_ms = MUTE_LATE_HIT_MS_DEFAULT;
//------ albEnd ----------------------------------

// Compile-time safety switch for the Multiply-end late-hit correction.
// Set to 0 to disable only the Multiply-end correction while leaving
// the Record/Stop correction (which has more test history) unaffected.
// The Insert-end correction (not yet implemented) will use the same flag.
#define LATE_HIT_MULTIPLY_CORRECTION 1

//------ albBegin ----------------------------------
// PAUSE_FROM_MUTE_FIX: compile-time safety switch for a stock
// SooperLooper 1.7.9 bug fix (not fork-introduced -- verified present
// verbatim in original_plugin.cc). Written up in
// mod_docs/late_hit_patch_summary.rst, "Quantized Pause from Muted Now
// Resolves to Paused".
//
// The bug: with mute-quantize on and sync active, pressing Pause while
// Muted has to wait for the next sync boundary. The wait-for-sync
// resolution logic (MULTI_MUTE/MULTI_PAUSE case) only special-cases the
// reverse direction (Paused+Mute -> Mute); it has no case for
// Mute+Pause -> Paused, so it falls through to the generic "resolve to
// Playing" branch instead. The *immediate*-action path a few lines
// above already handles this exact transition correctly and
// symmetrically in both directions, so this is confidently a stock
// omission, not intended behaviour.
//
// The fix has two parts, both gated by this one flag:
//   1. Add the missing case to the wait-for-sync branch (the
//      MULTI_MUTE/MULTI_PAUSE case's STATE_MUTE/STATE_ONESHOT/
//      STATE_PAUSED sub-case).
//   2. transitionToNext()'s STATE_PAUSED case currently sets
//      wasMuted = false unconditionally. Once part 1 can reach it from
//      STATE_MUTE, that would leave wasMuted wrong (read by several
//      undo/cancel resolution points to decide whether an Undo lands
//      back in Mute or Play), so wasMuted is instead derived from
//      pLS->state (still the pre-transition value at that point) --
//      true only when actually coming from Mute, matching the
//      immediate path's own semantics. Verified this branch has zero
//      existing callers before this fix (nothing else in the file ever
//      sets nextState to STATE_PAUSED), so this is provably
//      zero-regression: there is no prior behaviour to break.
//
// Set to 0 to revert both parts to exact stock (buggy) behaviour --
// same "just in case" escape hatch LATE_HIT_MULTIPLY_CORRECTION already
// provides for a different correction.
#define PAUSE_FROM_MUTE_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// SYNC_MODE_LATE_HIT_RESET_FIX: compile-time safety switch for a
// fork-introduced bug (NOT stock -- the vulnerable field and its
// gating pattern were both added by this fork's own late-hit
// correction work).
//
// The bug: lGlobalSamplesSinceSync (added in the original Record
// late-hit correction; also read by the later mute/unmute extension)
// is incremented and reset entirely inside the `if (fSyncMode >= 1.0f)`
// block further down in this function (alongside bSyncPulseReceived).
// While fSyncMode == 0 that whole block is skipped, so the counter
// FREEZES at whatever value it last held -- it neither increments nor
// resets (there is nothing rolling to produce a pulse to reset it on).
// When fSyncMode returns to >= 1.0, the counter resumes incrementing
// from that stale frozen value; nothing resets it at the transition
// itself, only the next real sync pulse does. Any button press landing
// in that gap -- Record start/stop, Insert, Mute, Unmute -- captures a
// lGlobalSyncAtButtonPress (T) reflecting stale pre-disable timing
// rather than real elapsed time, and can misfire the late-hit
// correction on a number with no relationship to any real sync
// boundary. Reproduced by testdef_stale_sync_counter_after_toggle.py
// through both the Record and the Mute/Unmute call sites.
//
// This is exactly the class of problem Looper::reset_late_hit_state()
// (looper.cpp, "C6 fix") already guards against -- but that function is
// only ever called from Engine::update_sync_source(), which fires on a
// sync-SOURCE change (Event::SyncTo / "sync_source") or add_loop(). It
// was never wired to the per-loop sync PORT itself (Event::SyncMode,
// the plain "sync" on/off control -- a per-loop checkbox in slgui, and
// also what this rig's big_tempo tool toggles via OSC on every JACK
// transport start/stop) transitioning off->on, which is the trigger
// this investigation actually found live.
//
// The fix lives here rather than in Looper::set_port() (looper.cpp,
// where update_sync_source()'s equivalent reset is triggered from):
// this file's test harness (src/test_plugin) drives plugin.cc directly
// through a minimal TestLooper wrapper that never links looper.cpp at
// all, so a fix placed in Looper::set_port() would be unverifiable by
// the very regression tests written for it. Detecting the transition
// here in run() -- using fSyncModeLastBuffer, a new per-instance field
// tracking the port's own previous-buffer value -- is exercised
// identically by the real engine and by the offline test harness,
// since both ultimately just write into the same shared Sync port and
// let run() read it each buffer.
//
// Safety: this runs in the same place, and at the same rate, as the
// pre-existing lGlobalSamplesSinceSync increment/reset logic it
// protects -- a few scalar reads/writes on a per-instance struct field,
// no lock, no allocation, no syscall.
//
// Set to 0 to revert to the pre-fix (buggy) behaviour -- same "just in
// case" escape hatch PAUSE_FROM_MUTE_FIX and LATE_HIT_MULTIPLY_CORRECTION
// already provide for other corrections.
#define SYNC_MODE_LATE_HIT_RESET_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// REL_SYNC_STALE_PHASE_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 counts the same way).
//
// The bug: lSamplesSinceSync, the samples since the last cycle pulse,
// counts only while the loop's Sync is on, and not at all in
// STATE_TRIG_START.  After Sync is turned off and on again (the slgui
// checkbox, a sync settings change, or this rig's big_tempo at every JACK
// transport stop and start) it holds a stale value until the next pulse.
// A Rel Sync record pressed in that gap takes the stale value as its
// offset from the pulse.  The loop comes out the wrong length (stock's
// "first Rel Sync recording comes out wrong"); if the value is over a
// cycle, the stop, which waits for the counter to equal the offset, never
// comes.
//
// The fix: the pulses reach channel 0 of every loop whatever its Sync
// setting, so channel 0 tracks them all the time (lTrackedSinceSync, a
// pass over each buffer's sync input in every state), and Looper::run()
// copies that count to the other channels, whose sync input is channel
// 0's output.  When a loop enters Rel Sync (fSyncMode becomes 2 from
// anything else, including the first run() after instantiation) or its
// Quantize changes under Rel Sync, the loop's counter takes the tracked
// count, so a record pressed then starts at the press with the right
// offset.  The tracked count is REL_SYNC_PHASE_UNKNOWN when it really is
// not known: no pulse seen yet, a Quantize change (the pulse spacing
// changes), a sync source change (Looper::reset_late_hit_state()), or no
// pulse for over twice the longest pulse spacing since the count became
// known (the transport stopped, so a count from before the stop is never
// used).  Under Rel Sync an unknown tracked
// count makes the counter unknown too.  A Rel Sync record pressed while
// the counter is unknown waits in STATE_TRIG_START, which counts under
// Rel Sync like the other states, so the next pulse resets the counter to
// 0 and the recording starts there, offset 0, as plain Sync would.  Plain
// Sync and Sync off are unchanged.
//
// Regression tests: testdef_rel_sync_stale_phase.py,
// testdef_rel_sync_known_phase.py.
//
// The switch itself, and REL_SYNC_PHASE_UNKNOWN, are defined in
// plugin.hpp, since looper.cpp and the test harness use them too.  Set it
// to 0 to revert to the stock (buggy) behaviour.
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// REL_SYNC_LATE_HIT: compile-time switch for a NEW behaviour (defined in
// plugin.hpp; needs REL_SYNC_STALE_PHASE_FIX).
//
// The problem: a Rel Sync record starts when the command arrives, which
// is trigger_latency samples after the pedal (stock never reads
// trigger_latency for a record start; the "this needs to be adjusted for
// latency" note at the Rel Sync start below), so the first bit of what
// was played on the press is missing.  And its stop waits until the
// counter equals the start's offset, so a stop arriving even one sample
// past that point records another whole Quantize unit (a cycle, or an
// eighth under 8th).  The late-hit corrections never run under Rel Sync
// (fSyncMode == 1 only): they trim to the pulse, not to the offset.
//
// The change:
//
// * Early start.  A Rel Sync record that starts at the press (the count
//   is known) starts E = trigger_latency + g_rel_sync_preroll_ms samples
//   earlier: [0, E) is filled from the input history, as the late-hit
//   start backfill does, and recording carries on at E.  The offset
//   stays the press's, so the stop still comes at whole Quantize units
//   after the press; there the last E samples are dropped (the loop is
//   whole units long, starting E before the press), faded into the
//   loop's start as the late-hit stop's seam fade does, and playback
//   carries on E samples into the loop.  A record that waits for a
//   pulse starts at the pulse with E = 0, as now.
//
// * Late stop.  STATE_RECORD notes the loop position each time the take
//   passes its end point (the check STATE_TRIG_STOP makes).  Every
//   command that ends a Rel Sync take goes through STATE_TRIG_STOP
//   (Record, Overdub, a quantized Mute or mute_trigger, Multiply, ...);
//   if it arrived at most the late-hit window (effectiveLateHitWindow())
//   past that point, at least one Quantize unit after the start,
//   STATE_TRIG_STOP ends the take on its first sample, at that point less
//   E: the same trim, seam fade and playback position as above with the
//   overrun added, then on to the command's state as usual.  Otherwise
//   it waits for the end point as now.  (The plain Sync late stop is
//   still Record presses only.)
//
// g_rel_sync_preroll_ms: the extra early start, for a press made on the
// first note.  A setup-time preference saved/loaded as the XML property
// "rel_sync_preroll_ms" in the <Globals> node, like g_mute_late_hit_ms.
// Default 0: only trigger_latency, which is the measured pedal delay.
//
// Regression tests: testdef_rel_sync_late_hit.py.
#define REL_SYNC_PREROLL_MS_DEFAULT 0.0f
float g_rel_sync_preroll_ms = REL_SYNC_PREROLL_MS_DEFAULT;
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// SYNC_PREROLL: compile-time switch for a NEW behaviour (defined in
// plugin.hpp).
//
// The problem: a plain Sync recording starts exactly on the boundary, so
// a strum attack or pickup played just before the one is not in the loop
// (the 2026-10-25 rig takes: attacks 7-85 ms before the record start).
//
// The change: a plain Sync recording that starts on a boundary pulse
// starts P = g_sync_preroll_ms earlier: [0, P) is filled from the input
// history, as the Rel Sync early start does (startTakeEarly()), and the
// sync position moves back with it (lSyncPos = -P), so a restart or a
// trigger still puts the boundary at loop position P.  Both start paths:
// the wait for the pulse (STATE_TRIG_START), and the late-hit start, whose
// backfill goes back P further.  Every end -- at the pulse
// (STATE_TRIG_STOP), the Record late stop, the late stop for other
// commands -- drops P more samples (lStartEarly), so the loop is still
// whole cycles, P before the boundaries, and playback carries on P into
// the loop.  The fork's Multiply late end expects a cycle to start on the
// pulse; it allows for the pre-roll through lSyncPos.  A start that is not
// on a pulse (Quantize Off or Loop with Sync on) has no pre-roll.
//
// g_sync_preroll_ms: saved/loaded as "sync_preroll_ms" in the <Globals>
// node, settable over OSC, like g_rel_sync_preroll_ms.  Default 0: no
// pre-roll.  Negative values are refused, as for the other settings.
//
// Regression tests: testdef_sync_preroll.py.
#define SYNC_PREROLL_MS_DEFAULT 0.0f
float g_sync_preroll_ms = SYNC_PREROLL_MS_DEFAULT;

// g_free_preroll_ms: FREE_PREROLL's pre-roll (plugin.hpp), for unsynced
// takes.  Saved/loaded as "free_preroll_ms" in the <Globals> node,
// settable over OSC, like g_sync_preroll_ms.  Default 0: no pre-roll.
// Defined whatever the switch, for the engine; read only with it on.
// Regression tests: testdef_trigger_latency_all_edges.py (the Preroll
// classes).
#define FREE_PREROLL_MS_DEFAULT 0.0f
float g_free_preroll_ms = FREE_PREROLL_MS_DEFAULT;

// The input history: stock's input-latency ring, which late starts, the
// sync pre-roll and the Rel Sync early start also copy from, so its length
// limits them.  instantiate() makes it at least this long, rounded up to a
// power of two (the ring wraps with a mask), and never shorter than
// stock's 32768 samples (683 ms at 48 kHz).  Default 4 s: half of a 4-beat
// cycle at 60 BPM plus a pre-roll; 262144 samples, 1 MB per channel, at
// 48 kHz.  Set from the command line (--input-history, sooperlooper.cpp)
// before any loop is made; it cannot change for a loop that exists, since
// the ring is allocated with it.
#define INPUT_HISTORY_SECONDS_DEFAULT 4.0f
float g_input_history_seconds = INPUT_HISTORY_SECONDS_DEFAULT;

// The input history's length in samples at this sample rate (also used by
// engine.cpp to warn about settings it cannot hold).
unsigned long sl_input_history_samples(unsigned long sample_rate)
{
	unsigned long size = 32768;
	const double want = (double) g_input_history_seconds * (double) sample_rate;
	while (size < want && size < (1UL << 26))
		size <<= 1;
	return size;
}
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// TRIG_STOP_MEMORY_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 has the same unchecked call; probed in the plugin harness).
//
// The bug: a recording waiting for its stop (STATE_TRIG_STOP) keeps
// recording, and asks ensureLoopSpace() for room each buffer without
// checking the result.  When the loop memory is full the call returns
// NULL and the next sample dereferences it: the engine crashes.  Any stop
// that waits past the end of memory gets there -- one that never comes
// (Sync on with no pulses; a stale Rel Sync offset over a cycle), or a
// long take whose next boundary falls after the memory runs out.
//
// The fix: check it as STATE_RECORD does and end the same way -- keep what
// was recorded and play it -- also dropping the pending stop and any
// command queued to follow it.
//
// Regression tests: testdef_trig_stop_memory.py.
//
// Set to 0 to revert to the stock (crashing) behaviour.
#define TRIG_STOP_MEMORY_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// LATE_STOP_ANY_COMMAND: compile-time switch for a NEW behaviour (on for
// the rig trial; 0 for the previous behaviour).
//
// The problem: the plain Sync late stop (the MULTI_RECORD / STATE_RECORD
// case) runs only for a Record press.  A take ended by any other command
// -- Overdub, a quantized Mute or mute_trigger, Multiply, ... -- waits in
// STATE_TRIG_STOP for the next pulse, so one pressed just after a pulse
// records another whole cycle.
//
// The change: STATE_TRIG_STOP makes the same check on the first sample of
// the buffer where the command arrived: the last pulse at most the late-hit
// window (effectiveLateHitWindow()) before the buffer
// (lGlobalSyncAtButtonPress, T), with the same guards as the Record check.
// Then the take ends now, trimmed by T + 1 to end just before that pulse,
// with the seam fade and playback position of the Record late stop, and
// goes on to the command's state as usual (transitionToNext()).  A take
// still waiting in a later buffer has T past the window (a pulse in
// between would have ended it), so the check cannot fire late.  The
// Record press keeps its own path in the dispatch, unchanged; it never
// reaches STATE_TRIG_STOP inside the window.
//
// Regression tests: testdef_late_stop_any_command.py.
#define LATE_STOP_ANY_COMMAND 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// RESTART_POSITION_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves identically; checked on a dummy server).
//
// The bug: a trigger into play (trigger, or mute_trigger from Mute),
// or a oneshot, starts the loop at the top of its buffer, index 0.  In normal play
// the loop runs input + output latency ahead in the buffer -- playback
// after a record starts at index IL + OL (MULTI_TRIGGER and the
// record-end paths add it) -- so that what is heard lines up with what
// was played.  A restart adds nothing, so the loop plays IL + OL behind
// (1486 samples, 31 ms, at 743/743) until the next restart, including
// with sync on, where it then sits that far behind the drums.
//
// The fix adds the same offset in the STATE_TRIGGER_PLAY case of
// transitionToNext(), which every trigger into play goes through:
// immediate ones from MULTI_TRIGGER and synced ones from the
// waitingForSync sites in run().  Record and TrigStop are excluded,
// because MULTI_TRIGGER already adds the offset after the transition
// for them.  Oneshot gets the same in its two places: MULTI_ONESHOT's
// immediate branch (which sets the position itself and already adds
// the offset from Record and TrigStop) and the STATE_ONESHOT case for
// synced ones.  Not covered (same bug, unused on the rig):
// playback-sync re-alignment.  See ROADMAP entry 26.
// Regression tests: testdef_restart_position.py.
//
// Not related to the trigger_latency control (how late commands reach
// the engine, used at record and overdub start); this fix doesn't read it.
//
// Set to 0 to revert to the stock (buggy) behaviour.
#define RESTART_POSITION_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// TRIGGER_FEED_FADE_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves identically; checked on a dummy server).
//
// The bug: Replace and Substitute fade the loop's feedback to 0, so the
// new layer does not mix with the old one.  Play, Mute, Pause, and
// MULTI_ONESHOT's immediate branch fade it back in; Trigger, and a
// Oneshot that waited for the pulse, did not (transitionToNext()'s
// STATE_TRIGGER_PLAY and STATE_ONESHOT cases).  Play writes
// loop * feedback back at the write position, so the loop was erased
// one pass after the restart: heard once, then silence.
//
// The fix fades the feedback in, in both cases, as STATE_PLAY does.  It
// changes nothing where the feedback is already up.  See ROADMAP entry 43.
// Set to 0 to revert to the stock (buggy) behaviour.
#define TRIGGER_FEED_FADE_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// OVERDUB_WRAP_FILL_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves the same; measured on a dummy server, ROADMAP
// entry 44).
//
// The bug: an Overdub, Replace or Substitute writes OL + IL behind the
// play position, and copies the previous layer into its new chunk in
// front of that stretch (front fill, [0, start - OL - IL)) and behind it
// (back fill, from the play position to the loop's end) as the play
// position passes.  Started less than OL + IL into the loop, the layer's
// first samples are written near the loop's end, inside the back fill,
// and the back fill later copies the old layer over them: the layer lost
// its first OL + IL - start samples (1287 of them 200 samples in, at
// 743/743).  The front fill's mark, clamped to 0, also copied over
// sample 0.
//
// The fix: when that stretch wraps (or starts at 0), no front fill, and
// the back fill stops where the stretch begins; the stretch is filled as
// it is written, as it always was.  Forward play only; a loop shorter
// than the stretch keeps the stock marks.  With TL_OVER_IL_LAYER_FIX the
// stretch is longer by lLayerLag, and this allows for it.
// Set to 0 to revert to the stock (buggy) behaviour.
#define OVERDUB_WRAP_FILL_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// MULTIPLY_END_FIX: compile-time safety switch for a STOCK bug, and for
// this fork's late multiply end, which had its own form of it.
//
// In STATE_MULTIPLY, dCurrPos is the play position; the record position
// (rCurrPos) is IL + OL behind it.  With Round on, a multiply end finished
// when the play position reached the loop end, set dCurrPos to 0 and went
// on (to Play, usually).  So:
//
//   - stock: the record position was still IL + OL short of the end, so
//     the last IL + OL samples of the new loop were never written (silence
//     there, or only what Play's fade-out of the input put there), on every
//     pass of the multiplied loop;
//   - fork, late multiply end (LATE_HIT_MULTIPLY_CORRECTION): the length
//     is cut back to the boundary just passed, the play position is already
//     IL + OL + the lateness past it, and the reset to 0 left the loop
//     playing that much late.
//
// The fix: a rounded multiply end finishes when the record position
// reaches the end, and the play position carries its overshoot on
// instead of starting again at 0.  A late multiply end finishes at once
// (its record position is already past the end), and the input that
// followed the boundary is faded into the loop's start, as after an
// on-time end (as for a late record stop).
//
// Also stock: the finish came after the sample's position was advanced,
// and the break left that sample to the next state, which played it
// again at the reset position; the multiplied loop (and the take, after
// an Undo, which carries the position over) played one sample early.
// The finish now moves on to the next sample.
//
// Set to 0 to revert to the stock (buggy) behaviour.
#define MULTIPLY_END_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// MULTIPLY_FRONTFILL_FIX: compile-time safety switch for a STOCK bug
// (ROADMAP entry 45; harness, stock 1.7.9 not run).
//
// The bug: beginMultiply() ended its front fill (the old loop copied into
// the new chunk before the multiply's first write) at
// dCurrPos - (IL - OL) - (OL + IL), while the multiply's first write is at
// dCurrPos - (OL + IL).  With input_latency > output_latency the IL - OL
// samples between were never filled: silence in the old loop just before
// the multiply.  With output_latency > input_latency the fill copied the
// old loop over the first OL - IL samples the multiply wrote.  No effect
// when the two are equal.
//
// The fix drops the IL - OL (as beginOverdub() and beginInsert() have it,
// commented out).  Set to 0 to revert to the stock (buggy) behaviour.
#define MULTIPLY_FRONTFILL_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// ROUNDED_MULTIPLY_FILL_FIX: compile-time safety switch for a STOCK bug
// (ROADMAP entry 46; harness, stock 1.7.9 not run).
//
// The bug: endMultiply() starts an OL + IL "leftover" fill at every end.
// With Round on the multiply runs on to the cycle's end, and the
// STATE_MULTIPLY loop does that fill at rpCurrPos, the record position
// modulo the SOURCE loop's length, used as a position in the new loop.
// Past the multiply's first cycle the two differ by whole cycles, so the
// fill copied the old loop over OL + IL already-recorded samples one or
// more cycles back (31 ms at 743/743).  Within the first cycle it only
// touched samples the multiply then wrote again.
//
// The fix: no leftover fill at a rounded end.  A rounded multiply records
// on to the end, so it writes every position the fill would.  An
// unrounded end keeps it.  Set to 0 to revert to the stock (buggy)
// behaviour.
#define ROUNDED_MULTIPLY_FILL_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// SYNCWDEBUG: master switch for ALL debug output in this file.
// When defined:
//   - a sentinel file is written at binary-load time.
//   - a log file is opened on first SL_LOG call.
//   - SL_LOG startup banners (instantiate/activate) go to the log file.
//   - SYNCWDBG(SL_LOG_RT(...)) per-press messages go to the log file with
//     an fflush() after each write (immediate, crash-safe, but RT-blocking).
// When NOT defined (default):
//   - No files are created.
//   - SL_LOG and SL_LOG_RT expand to nothing.
//   - SYNCWDBG(...) expands to nothing.
//   - The only output is the session-load fprintf(stderr,...) in engine.cpp.
//
// Both files land in $HOME/.sooperlooper/logs/ (created if it does not
// already exist yet) -- SooperLooper's own per-user directory, per the
// "look for ... in ~/.sooperlooper/bindings/" TODO already present
// upstream (predating this fork) in midi_bind.cpp and engine.cpp. Uses
// the same directory as engine.cpp's ALB_SYNC_PULSE_LOG, deliberately:
// one place to remember for any SooperLooper-generated diagnostic
// output, rather than two.
// Falls back to /tmp if $HOME is unset or the directory
// could not be created, same fallback engine.cpp's
// alb_sync_pulse_log_open() uses.
//
// Default is DISABLED (commented out) to avoid xruns in performance builds.
// Uncomment to enable all diagnostics for debugging.
//#define SYNCWDEBUG
#ifdef SYNCWDEBUG
#define SYNCWDBG(x) x
#else
#define SYNCWDBG(x)
#endif
//------ albEnd ----------------------------------

/*****************************************************************************/
// Resolves (creating if necessary) $HOME/.sooperlooper/logs/<name>,
// writing the full path into out (of size outsz). Returns false (out
// left untouched) if $HOME is unset or the directory could not be
// created/verified, in which case callers fall back to /tmp themselves.
//
// Deliberately NOT shared with engine.cpp's own version of this logic
// (alb_sync_pulse_log_open()): plugin.cc is written to remain usable
// standalone, as a bare LADSPA plugin loaded by a host with no
// engine.cpp/looper.cpp in the same address space (see the
// g_late_hit_window_ms comment above), so it takes on no new
// cross-translation-unit dependency just for this.
//
// Only ever called from non-RT contexts (the load-time static
// initializer below, and the first SL_LOG call, which always happens
// before instantiate()/activate() and thus before the JACK RT callback
// starts), so the mkdir()/stat() calls here carry no RT-safety concern.
#ifdef SYNCWDEBUG
#include <sys/stat.h>
static bool
sl_sooperlooper_log_path (char* out, size_t outsz, const char* name)
{
    const char* home = getenv ("HOME");
    if (!home) {
        return false;
    }
    char dir[512];
    struct stat st;
    snprintf (dir, sizeof(dir), "%s/.sooperlooper", home);
    mkdir (dir, 0755);  // ignore failure (e.g. EEXIST) -- checked below
    snprintf (dir, sizeof(dir), "%s/.sooperlooper/logs", home);
    mkdir (dir, 0755);
    if (stat (dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    snprintf (out, outsz, "%s/%s", dir, name);
    return true;
}
#endif // SYNCWDEBUG

/*****************************************************************************/
// Static initializer: runs when the binary/library is loaded, before any
// plugin functions are called.  Writes to multiple locations so at least
// one will be visible regardless of how the process is launched.
// Gated on SYNCWDEBUG: in production builds no files are written.
static struct SL_LoadProbe {
    SL_LoadProbe() {
#ifdef SYNCWDEBUG
        // Write a sentinel file confirming the experimental build is loaded.
        // The late-hit window value is NOT printed here: g_late_hit_window_ms
        // has not yet been overwritten by the session file at binary-load time.
        // The actual runtime value is logged by looper.cpp after session load.
        char path[768];
        FILE* f = 0;
        if (sl_sooperlooper_log_path (path, sizeof(path), "sl_loaded.txt")) {
            f = fopen (path, "w");
        }
        if (!f) {
            f = fopen ("/tmp/sl_loaded.txt", "w");
        }
        if (f) {
            fprintf(f, "SooperLooper experimental binary (late-hit patch) loaded\n");
            fclose(f);
        }
#endif // SYNCWDEBUG
    }
} sl_load_probe;

/*****************************************************************************/
// Debug log file for the experimental late-hit window patch.
// The entire infrastructure below is compiled only when SYNCWDEBUG is defined.
// When SYNCWDEBUG is not defined, SL_LOG and SL_LOG_RT expand to nothing and
// no log file is ever created.
//
// WARNING (when enabled): SL_LOG_RT calls fflush() on every button press.
// fflush() acquires the C stdio mutex and issues a kernel write() syscall,
// both of which can block the RT thread.  This is intentional for accurate
// post-crash log recovery but WILL increase xrun risk.  Only enable
// SYNCWDEBUG in debug builds; never in a live-performance build.
#ifdef SYNCWDEBUG
// Tries $HOME/.sooperlooper/logs/sl_debug.log first, then /tmp/sl_debug.log,
// then falls back to stderr.
static FILE* sl_dbg_file = NULL;

static void sl_dbg_open() {
    if (!sl_dbg_file) {
        char path[768];
        if (sl_sooperlooper_log_path (path, sizeof(path), "sl_debug.log")) {
            sl_dbg_file = fopen(path, "w");  // "w" not "a": fresh file on each startup
        }
        if (!sl_dbg_file) {
            sl_dbg_file = fopen("/tmp/sl_debug.log", "w");
        }
        if (!sl_dbg_file) {
            sl_dbg_file = stderr;
            fprintf(stderr, "SooperLooper: could not open log file, using stderr\n");
        } else {
            fprintf(sl_dbg_file, "SooperLooper: log file opened\n");
            fflush(sl_dbg_file);
        }
    }
}

// Use SL_LOG(...) for non-RT messages (startup banners, session load).
// Calls sl_dbg_open() if needed, then writes and fflushes.
#define SL_LOG(...) do { sl_dbg_open(); fprintf(sl_dbg_file, __VA_ARGS__); fflush(sl_dbg_file); } while(0)
// SL_LOG_RT: RT-callable version.
// Writes to sl_dbg_file only if already open (set during instantiate/activate,
// which always run before the JACK RT callback starts).
// NOTE: fflush() is called after every write so that log data survives a
// crash without buffering delay.  This WILL block the RT thread on the
// stdio mutex and kernel write() — xruns are the known cost when enabled.
// Use SYNCWDBG(SL_LOG_RT(...)) to wrap all RT-path log calls so they
// compile away completely when SYNCWDEBUG is not defined.
#define SL_LOG_RT(...) do { if (sl_dbg_file) { fprintf(sl_dbg_file, __VA_ARGS__); fflush(sl_dbg_file); } } while(0)
#else  // SYNCWDEBUG not defined — no logging, no files created
#define SL_LOG(...)    do { } while(0)
#define SL_LOG_RT(...) do { } while(0)
#endif // SYNCWDEBUG
//------ albEnd ----------------------------------


/*****************************************************************************/
//#define LOOPDEBUG

#ifdef LOOPDEBUG
#define DBG(x) x
#else
#define DBG(x)
#endif

//------ albBegin ----------------------------------
// effectiveLateHitWindow(): compute the late-hit window in samples.
//
// Always evaluates both limits and returns the smaller:
//
//     min( g_late_hit_window_ms * 0.001 * sampleRate,
//          min(g_late_hit_max_quant_unit_fraction, 0.5) * lLastSyncPeriodSamples )
//
// lLastSyncPeriodSamples is the Quantize unit, the spacing of the boundaries
// the Quantize setting gives (one cycle = eighthPerCycle eighth-notes for
// QUANT_CYCLE; one eighth-note for QUANT_8TH).  The fraction is of that
// unit: 0.25 → 25 % of it.  It is capped at 0.5 whatever the setting, so
// the nearer boundary always wins: a press past the middle of the unit is
// early for the next boundary, never late for the one before.  (A wider
// window took such a press as late for the previous boundary and gave a
// take one unit too long or too short.)
//
// The intermediate product is kept in double to handle the ULONG_MAX sentinel
// that activate() stores in lLastSyncPeriodSamples before the first measured
// sync period: the resulting fractionLimit is then much larger than any
// practical msLimit, so the ms term wins until real tempo data is available.
//
// The eighthPerCycle parameter is retained in the signature for call-site
// compatibility but is no longer used in the formula.
#if TRIGGER_LATENCY_ALL_EDGES
// commandWindowExtra() (increment (e), TRIGGER_LATENCY_ALL_EDGES): how
// much later than its press a command arrives, trigger_latency.  Whether a
// press is on time, late within a window, or waits for the next boundary
// is decided from the press: every window, counted from the boundary to
// the command's arrival, is this much longer.  Placement still counts to
// the arrival: that is where the loop must be now.
static inline unsigned long commandWindowExtra(const SooperLooperI *pLS)
{
	const float tl = *pLS->pfTriggerLatency;
	return tl > 0.0f ? (unsigned long) tl : 0;
}

// inSlackWindow() (increment (e)): the slack window of a Trigger or
// Oneshot with Sync on (less than an eighth after a pulse, it acts at
// once), from the press.  At least 1, as run()'s eighthSamples is when no
// tempo is set (transitionToNext()'s is 0), so the two agree.
static inline bool inSlackWindow(const SooperLooperI *pLS, unsigned long eighthSamples)
{
	return (unsigned long) pLS->lSamplesSinceSync
		< (eighthSamples > 0 ? eighthSamples : 1) + commandWindowExtra(pLS);
}
#endif

// lateHitWindowWanted(): the window the settings ask for, before the
// input-history limit below; effectiveLateHitWindow() is what applies.  A
// press between the two is one a longer history would have corrected
// (noteHistoryShort()).
static inline unsigned long lateHitWindowWanted(const SooperLooperI *pLS) {
    const unsigned long msLimit =
        (unsigned long)(g_late_hit_window_ms * 0.001f * pLS->fSampleRate);
    // At most half the Quantize unit: the nearer boundary wins.  A press
    // past the middle of the unit is early for the next boundary, never
    // late for the one before it.
    const double fraction = std::min ((double) g_late_hit_max_quant_unit_fraction, 0.5);
    double fractionLimit = fraction * (double) pLS->lLastSyncPeriodSamples;
#if TRIGGER_LATENCY_ALL_EDGES
    // Increment (e): the window, and the half-unit cap, are the press's;
    // the command arrives trigger_latency later.
    const unsigned long extra = commandWindowExtra (pLS);
#else
    const unsigned long extra = 0;
#endif
    // If fractionLimit >= msLimit the ms term wins; cast is safe because
    // fractionLimit < msLimit guarantees it fits in unsigned long.
    if (fractionLimit >= (double)msLimit) return msLimit + extra;
    return (unsigned long)fractionLimit + extra;
}

static inline unsigned long effectiveLateHitWindow(const SooperLooperI *pLS,
                                                    unsigned int eighthPerCycle) {
    (void)eighthPerCycle; // formerly used for / eighthPerCycle division; now unused
    // Hard cap: backfill reads from pfInputLatencyBuf which is lInputBufSize
    // samples deep.  A window wider than the ring buffer would read garbage.
    const unsigned long wanted = lateHitWindowWanted (pLS);
    return (wanted >= pLS->lInputBufSize) ? pLS->lInputBufSize - 1 : wanted;
}
//------ albEnd ----------------------------------



/* The maximum sample memory  (in seconds). */

#ifndef SAMPLE_MEMORY
#define SAMPLE_MEMORY 80.0
#endif

#define XFADE_SAMPLES 128

// settle time for tap trigger (trigger if two changes
// happen within at least X samples)
//#define TRIG_SETTLE  4410
#define TRIG_SETTLE  2205  

// another thing that shouldn't be hardcoded
#define MAX_LOOPS 512


#define SAFETY_FEEDBACK 0.96f

/*****************************************************************************/


/* States of the sampler */

#define STATE_OFF        0
#define STATE_TRIG_START 1
#define STATE_RECORD     2
#define STATE_TRIG_STOP  3
#define STATE_PLAY       4
#define STATE_OVERDUB    5
#define STATE_MULTIPLY   6
#define STATE_INSERT     7
#define STATE_REPLACE    8
#define STATE_DELAY      9
#define STATE_MUTE       10
#define STATE_SCRATCH    11
#define STATE_ONESHOT    12
#define STATE_SUBSTITUTE  13
#define STATE_PAUSED     14
#define STATE_UNDO_ALL	15
#define STATE_TRIGGER_PLAY 16
#define STATE_UNDO		17
#define STATE_REDO		18
#define STATE_REDO_ALL	19
#define STATE_OFF_MUTE	20

/* 1s digit of
 * Multicontroller parameter functions */

#define MULTI_UNDO       0

#define MULTI_REDO       1   // posibbly MULTI_DELAY if toggled, or REDO_TOG if muted

#define MULTI_REPLACE    2   // QUANT_TOG in mute mode

#define MULTI_REVERSE    3   // ROUND_TOG in mute mode

#define MULTI_SCRATCH    4
#define MULTI_RECORD     5
#define MULTI_OVERDUB    6
#define MULTI_MULTIPLY   7
#define MULTI_INSERT     8
#define MULTI_MUTE       9

// extra features, cannot be picked directly
#define MULTI_DELAY      10   // only 
#define MULTI_REDO_TOG   11   // only when in mute mode
#define MULTI_QUANT_TOG  12   // only when in mute mode
#define MULTI_ROUND_TOG  13   // only when in mute mode

#define MULTI_ONESHOT    14
#define MULTI_TRIGGER    15
#define MULTI_SUBSTITUTE 16
#define MULTI_UNDO_ALL   17
#define MULTI_REDO_ALL   18

#define MULTI_MUTE_ON    19
#define MULTI_MUTE_OFF   20
#define MULTI_PAUSE      21
#define MULTI_PAUSE_ON      22
#define MULTI_PAUSE_OFF      23

#define MULTI_SET_SYNC_POS   Event::SET_SYNC_POS
#define MULTI_RESET_SYNC_POS Event::RESET_SYNC_POS
#define MULTI_MUTE_TRIGGER   Event::MUTE_TRIGGER
#define MULTI_RECORD_OR_OVERDUB Event::RECORD_OR_OVERDUB
#define MULTI_RECORD_OVERDUB_END Event::RECORD_OVERDUB_END_SOLO
#define MULTI_UNDO_TWICE     Event::UNDO_TWICE

//------ albBegin ----------------------------------
// Symbolic, never a literal: the numeric block above (0-23) mirrors the
// first 24 Event::command_t values by hand, and CANCEL_RECORD_MUTE is
// appended at the end of that enum where its ordinal is not fixed.
#define MULTI_CANCEL_RECORD_MUTE Event::CANCEL_RECORD_MUTE
#define MULTI_CANCEL_OVERDUB_UNDO Event::CANCEL_OVERDUB_UNDO

// Guard the hand-maintained correspondence above.  The MULTI_UNDO..
// MULTI_PAUSE_OFF literals (0-23) must stay aligned with the first 24
// values of Event::command_t.  Insert an enum anywhere in that range and
// the enum shifts while the literals do not, so commands silently begin
// doing each other's jobs -- with no compiler diagnostic of any kind.
//
// PAUSE_OFF alone catches any insertion at or before ordinal 23; the
// others localise which sub-range drifted.  Python-side counterpart:
// commandOrdinalContract in test_plugin/testdef_cancel_record_mute.py.
static_assert((int) Event::UNDO      == MULTI_UNDO,      "Event::command_t drifted from the MULTI_* literals (at/before UNDO)");
static_assert((int) Event::RECORD    == MULTI_RECORD,    "Event::command_t drifted from the MULTI_* literals (at/before RECORD)");
static_assert((int) Event::MUTE      == MULTI_MUTE,      "Event::command_t drifted from the MULTI_* literals (at/before MUTE)");
static_assert((int) Event::UNDO_ALL  == MULTI_UNDO_ALL,  "Event::command_t drifted from the MULTI_* literals (at/before UNDO_ALL)");
static_assert((int) Event::MUTE_ON   == MULTI_MUTE_ON,   "Event::command_t drifted from the MULTI_* literals (at/before MUTE_ON)");
static_assert((int) Event::PAUSE_OFF == MULTI_PAUSE_OFF, "Event::command_t drifted from the MULTI_* literals (at/before PAUSE_OFF)");
//------ albEnd ----------------------------------


/*****************************************************************************/

#define LIMIT_BETWEEN_0_AND_1(x)          \
	f_clamp (x, 0.0f, 1.0f)

#define LIMIT_BETWEEN_NEG1_AND_1(x)          \
	f_clamp (x, -1.0f, 1.0f)


#define MAX(x,y) \
	f_max (x, y)


//------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT || LATE_STOP_ANY_COMMAND || SYNC_PREROLL
// endTakeTrimmed(): end a take lateSamples before the current position
// (REL_SYNC_LATE_HIT, LATE_STOP_ANY_COMMAND, SYNC_PREROLL; see their
// comment blocks).
// Sets the length, fades the dropped samples into the loop's start (the late-hit stop's
// seam fade: after a normal stop STATE_PLAY writes the input following
// the end into the start, fading it out, index i at 1 - (i + 1) / xfade;
// here that input is already in the buffer past the new end), and
// returns the fade level STATE_PLAY carries on from at index lateSamples.
// The caller sets the state and the position.
static inline float endTakeTrimmed(SooperLooperI *pLS, LoopChunk *loop,
                                   unsigned long lateSamples, int xfadeSamples)
{
	const unsigned long length = (unsigned long) loop->dCurrPos - lateSamples;
	loop->lLoopLength  = length;
	loop->lCycleLength = length;
	loop->lCycles = 1;
	for (unsigned long i = 0; i < lateSamples && i + 1 < (unsigned long) xfadeSamples; ++i) {
		const float fGain = 1.0f - (float) (i + 1) / xfadeSamples;
		pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask] +=
			fGain * pLS->pSampleBuf[(loop->lLoopStart + length + i) & pLS->lBufferSizeMask];
	}
	return LIMIT_BETWEEN_0_AND_1 (1.0f - (float) lateSamples / xfadeSamples);
}
#endif

// noteHistoryShort(): a late start, pre-roll or Rel Sync early start was
// refused because the input history is too short; `needed` samples would
// have done.  The engine's main loop prints a warning (RT-safe: two stores).
static inline void noteHistoryShort(SooperLooperI *pLS, unsigned long needed)
{
	if (needed > pLS->lHistoryShortNeeded)
		pLS->lHistoryShortNeeded = needed;
	pLS->lHistoryShortCount = pLS->lHistoryShortCount + 1;
}

#if REL_SYNC_LATE_HIT || SYNC_PREROLL
// startTakeEarly(): start a new take `early` samples before the sample
// at ring position histPos of the input history (REL_SYNC_LATE_HIT,
// SYNC_PREROLL).  Fills [0, early) with the input before it, with the
// fade-in a normal start gives index i, so recording carries on at
// `early`; moves the sync position back with the start; returns the
// fade level STATE_RECORD carries on from.
static inline float startTakeEarly(SooperLooperI *pLS, LoopChunk *loop,
                                   const LADSPA_Data *pfHistory, unsigned long histPos,
                                   unsigned long early, int xfadeSamples)
{
	const unsigned long readPos = (histPos + pLS->lInputBufSize - early) & pLS->lInputBufMask;
	for (unsigned long i = 0; i < early; ++i) {
		const float fGain = (i + 1 < (unsigned long) xfadeSamples)
			? (float) (i + 1) / xfadeSamples : 1.0f;
		pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask] =
			fGain * pfHistory[(readPos + i) & pLS->lInputBufMask];
	}
	loop->dCurrPos = (double) early;
	loop->lStartEarly = early;
	loop->lSyncPos -= (long) early;
	loop->lOrigSyncPos = loop->lSyncPos;
	return LIMIT_BETWEEN_0_AND_1 ((float) early / xfadeSamples);
}
#endif

//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
// edgeDelaySamples() (TRIGGER_LATENCY_ALL_EDGES, plugin.hpp): how long after
// a command its edge falls so that the edge lands on the press.  The sound
// played at the press reaches the input input_latency after it, the command
// trigger_latency (the command latency) after it.  Negative
// (trigger_latency > input_latency) is 0 here: see edgeAdvanceSamples().
static inline long edgeDelaySamples(const SooperLooperI *pLS)
{
	const long s = (long) *pLS->pfInputLatency - (long) *pLS->pfTriggerLatency;
	return s > 0 ? s : 0;
}

// edgeAdvanceSamples() (increment (c)): the other way, trigger_latency -
// input_latency when positive: the sound played at the press reached the
// input that long before the command, so a take starts that much early,
// from the input history, and ends that much early, trimmed.  0 otherwise.
static inline long edgeAdvanceSamples(const SooperLooperI *pLS)
{
	const long k = (long) *pLS->pfTriggerLatency - (long) *pLS->pfInputLatency;
	return k > 0 ? k : 0;
}

// A take's edges (FREE_PREROLL, plugin.hpp): its free pre-roll counts as
// command latency at both.  takeEdgeDelaySamples() and
// takeEdgeAdvanceSamples() are edgeDelaySamples() and edgeAdvanceSamples()
// with `preroll` added to trigger_latency; without one (always, with the
// switch off) they are the same.  takePreroll(): a take's own.
static inline unsigned long takePreroll(const LoopChunk *loop)
{
#if FREE_PREROLL
	return loop ? loop->lFreePreroll : 0;
#else
	(void) loop;
	return 0;
#endif
}

static inline long takeEdgeDelaySamples(const SooperLooperI *pLS, unsigned long preroll)
{
	const long e = (long) *pLS->pfTriggerLatency - (long) *pLS->pfInputLatency + (long) preroll;
	return e < 0 ? -e : 0;
}

static inline long takeEdgeAdvanceSamples(const SooperLooperI *pLS, unsigned long preroll)
{
	const long e = (long) *pLS->pfTriggerLatency - (long) *pLS->pfInputLatency + (long) preroll;
	return e > 0 ? e : 0;
}

#if FREE_PREROLL
// freePrerollSamples(): g_free_preroll_ms in samples.
static inline unsigned long freePrerollSamples(const SooperLooperI *pLS)
{
	return g_free_preroll_ms > 0.0f
		? (unsigned long) (g_free_preroll_ms * 0.001f * pLS->fSampleRate) : 0;
}
#endif
#if !(REL_SYNC_LATE_HIT || SYNC_PREROLL)
#error "TRIGGER_LATENCY_ALL_EDGES needs startTakeEarly() and endTakeTrimmed() (REL_SYNC_LATE_HIT or SYNC_PREROLL)"
#endif

// takeTrimWanted() (increment (c)): a take ended now should end
// edgeAdvanceSamples() back (plus its free pre-roll, FREE_PREROLL), and
// it is longer than that.
static inline bool takeTrimWanted(const SooperLooperI *pLS, const LoopChunk *loop)
{
	return loop && takeEdgeAdvanceSamples(pLS, takePreroll(loop)) > 0
		&& loop->dCurrPos > (double) takeEdgeAdvanceSamples(pLS, takePreroll(loop));
}

// A take ended by another command, Sync off: end it edgeDelaySamples()
// later through STATE_TRIG_STOP into `next`, sending `pulse` when it
// fires.  False (do as now) when not deferred.
static inline bool deferTakeEnd(SooperLooperI *pLS, LoopChunk *loop,
				float fSyncMode, bool bRoundIntegerTempo,
				int next, float pulse)
{
	// FREE_PREROLL: a take with a pre-roll always, for the restart's
	// placement (STATE_TRIG_STOP), even with no wait and no trim.
	if (pLS->state != STATE_RECORD || fSyncMode != 0.0f || bRoundIntegerTempo
	    || !loop || (takeEdgeDelaySamples(pLS, takePreroll(loop)) <= 0
			 && !takeTrimWanted(pLS, loop) && takePreroll(loop) == 0))
		return false;
	pLS->state = STATE_TRIG_STOP;
	pLS->nextState = next;
	pLS->lEdgeDelay = takeEdgeDelaySamples(pLS, takePreroll(loop));
	// The other way (increment (c)): at once, trimmed back to the press.
	pLS->lEdgeTrim = (pLS->lEdgeDelay > 0 || !takeTrimWanted(pLS, loop)) ? 0
		: (unsigned long) takeEdgeAdvanceSamples(pLS, takePreroll(loop));
	pLS->bEdgeStopDeferred = true;
	pLS->fEdgeStopPulse = pulse;
	return true;
}

// A layer ended by another command (2b-ii, 2b-iii): keep the layer going
// edgeDelaySamples() more, then go to `next` (the layer states' sample
// loop), sending `pulse` (what the immediate branch sends; 0 for none).
// False (do as now) when not deferred.
static inline bool deferLayerEnd(SooperLooperI *pLS, LoopChunk *loop, int next,
				 float pulse)
{
	if ((pLS->state != STATE_OVERDUB && pLS->state != STATE_REPLACE
	     && pLS->state != STATE_SUBSTITUTE)
	    || !loop || edgeDelaySamples(pLS) <= 0)
		return false;
	pLS->lEdgeDelay = edgeDelaySamples(pLS);
	pLS->bEdgeEndDeferred = true;
	pLS->lEdgeEndNext = next;
	pLS->fEdgeEndPulse = pulse;
	return true;
}

// A take ended at once whatever the Sync (2b-iii): deferTakeEnd() without
// its Sync and round-tempo conditions; the caller passes its branch's own.
// STATE_TRIG_STOP fires it when the countdown runs out, and only then.
static inline bool deferTakeEndNow(SooperLooperI *pLS, LoopChunk *loop,
				   int next, float pulse)
{
	if (pLS->state != STATE_RECORD || !loop
	    || (takeEdgeDelaySamples(pLS, takePreroll(loop)) <= 0
		&& !takeTrimWanted(pLS, loop) && takePreroll(loop) == 0))
		return false;
	pLS->state = STATE_TRIG_STOP;
	pLS->nextState = next;
	pLS->lEdgeDelay = takeEdgeDelaySamples(pLS, takePreroll(loop));
	// The other way (increment (c)): at once, trimmed back to the press.
	pLS->lEdgeTrim = (pLS->lEdgeDelay > 0 || !takeTrimWanted(pLS, loop)) ? 0
		: (unsigned long) takeEdgeAdvanceSamples(pLS, takePreroll(loop));
	pLS->bEdgeStopDeferred = true;
	pLS->fEdgeStopPulse = pulse;
	return true;
}

// Where a deferred Trigger, Oneshot or Scratch starts the loop, before
// IL + OL: as the immediate branch places it, when it fires.  The top of
// the loop (Oneshot: plus the sync offset; reversed: less one); `slack`
// (Sync on, the command acted at once in the slack window): count + 1
// more, read now, which is where the immediate restart would be s later.
static double restartPosition(const SooperLooperI *pLS, const LoopChunk *loop,
			      int next, bool slack, float fSyncOffsetSamples)
{
	const bool fwd = pLS->fCurrRate > 0;
	double pos = (double) loop->lLoopLength - loop->lSyncPos;
	if (next == STATE_ONESHOT)
		pos += fwd ? fSyncOffsetSamples : -1.0 - fSyncOffsetSamples;
	else if (!fwd)
		pos -= 1.0;
	if (slack)
		pos += (fwd ? 1.0 : -1.0) * ((double) pLS->lSamplesSinceSync + 1.0);
	if (next == STATE_ONESHOT)
		pos = fmod(pos, loop->lLoopLength);
	return pos;
}
#endif
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
// startOneshotPass(): a oneshot starting now plays one loop length less
// `skipped`, the samples of the pass that a slack-window start skips
// (see ONESHOT_ONE_PASS_FIX in plugin.hpp).
static inline void startOneshotPass(SooperLooperI *pLS, const LoopChunk *loop,
				    double skipped)
{
	pLS->dOneshotLeft = (double) loop->lLoopLength - skipped;
}
#endif
//------ albEnd ----------------------------------

#if SYNC_PREROLL
// syncPreRollSamples(): the plain Sync pre-roll, if the input history
// holds it beyond `already` samples back and this block.
static inline unsigned long syncPreRollSamples(SooperLooperI *pLS,
                                               unsigned long already,
                                               unsigned long SampleCount)
{
	const unsigned long p = (unsigned long) (g_sync_preroll_ms * 0.001f * pLS->fSampleRate);
	if (p > 0 && already + p + SampleCount > pLS->lInputBufSize) {
		noteHistoryShort (pLS, already + p + SampleCount);
		return 0;
	}
	return p;
}
#endif
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// SIGNED_SYNC_POS_FIX: compile-time safety switch for a STOCK bug.
//
// The problem: the loop's own boundaries (its sync output, used with Sync
// off by quantized commands and by loops synced to it) are where
// (pos + lSyncPos) % unit == 0.  lSyncPos can be negative (stock Rel Sync,
// the fork's Rel Sync early start, SYNC_PREROLL), and with a Cycle or Loop unit
// (unsigned long) the negative sum is converted to unsigned, so one
// position in [0, -lSyncPos) can test as a boundary too: where
// -lSyncPos - pos == 2^64 mod unit.  Shown in the harness
// (testdef_signed_sync_pos.py: an extra boundary every pass).  The 8th
// unit (unsigned int) is promoted to long and was already right.
//
// The change: the test is done in signed arithmetic, which is 0 exactly
// on the grid whatever the sign; for a sum of 0 or more nothing changes
// (plain Sync's lSyncPos is 0).  Set to 0 for stock.
#define SIGNED_SYNC_POS_FIX 1
#if SIGNED_SYNC_POS_FIX
static inline bool onSyncGrid(long pos, long syncPos, unsigned long unit)
{
	return ((pos + syncPos) % (long) unit) == 0;
}
#endif
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// REL_SYNC_POS_FIX: compile-time safety switch for a STOCK bug.
//
// The problem: a Rel Sync recording sets lSyncPos (where the sync grid
// falls in the loop; a synced Trigger or mute_trigger restarts the loop at
// -lSyncPos, and with Sync off the loop's own boundaries are there) from
// the count since the last boundary at its start.  Stock 1.7.9:
// "since < syncSamples/2 ? since : since - syncSamples".  With a tempo
// set, syncSamples is HALF the Quantize unit (0 without a tempo, where the
// result is always `since`, which is right), and both are unsigned int.
// A start in the unit's second half came out half a unit off, and one in
// its second quarter wrapped to about 2^32: a restart played about half a
// cycle out of phase (probed in the harness, testdef_rel_sync_pos.py).
//
// The change: from the nearer boundary, in signed arithmetic: `since` in
// the first half of the unit (2 * syncSamples), `since - unit` in the
// second, so a restart puts the loop's first boundary after its start on
// the beat.  The unit comes from the tempo, so it can differ from the real
// pulse spacing by the tempo's rounding to whole samples (about a sample
// per eighth).  Set to 0 for stock.
#define REL_SYNC_POS_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// SYNC_COUNT_OFF_BY_ONE_FIX: compile-time safety switch for a STOCK
// off-by-one.
//
// The problem: lSamplesSinceSync is updated once per sample, so read
// before a block's first sample (a command acting at once) or in
// STATE_TRIG_START (which counts at the bottom) it is one less than the
// distance from the pulse (the record late-hit already trims T + 1 for
// this).  Two stock uses took it as the distance: a synced Trigger or
// mute_trigger in the slack window (with a tempo set, pressed less than
// an eighth after a pulse it acts at once, starting the loop
// lSamplesSinceSync in) came back one sample late; and a Rel Sync
// recording's sync position put its grid one sample late, so a restart
// that waited for the pulse came back one sample early.  Measured in the
// harness (testdef_sync_count_off_by_one.py).
//
// The change: add one at both: after the slack-window Trigger's
// transitionToNext, and in the REL_SYNC_POS_FIX block (so this needs that
// switch).  A restart that waits for the pulse is unchanged: it acts
// inside the play loop after that sample's count update, where the count
// is the distance (0 on the pulse).  Set to 0 for stock.
#define SYNC_COUNT_OFF_BY_ONE_FIX 1
#if SYNC_COUNT_OFF_BY_ONE_FIX && !REL_SYNC_POS_FIX
#error "SYNC_COUNT_OFF_BY_ONE_FIX needs REL_SYNC_POS_FIX (its Rel Sync half is there)"
#endif
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// ONESHOT_SLACK_FIX: compile-time safety switch for a STOCK bug.
//
// The problem: with Sync on and a tempo set, a Once (one-shot) pressed
// less than an eighth after a pulse acts at once (the slack window), but
// unlike Trigger it started the loop from the top without allowing for
// the time since the pulse, so the whole pass played that much behind the
// beat (up to an eighth; measured in the harness,
// testdef_oneshot_slack.py).
//
// The change: start as far in as the press is past the pulse, as Trigger
// does (with SYNC_COUNT_OFF_BY_ONE_FIX's count + 1): a late press sounds
// like an on-time one with the first moment skipped.  A Once that waits
// for the pulse is unchanged.  Set to 0 for stock.
#define ONESHOT_SLACK_FIX 1
//------ albEnd ----------------------------------
//------ albBegin ----------------------------------
// restartPosition() (TRIGGER_LATENCY_ALL_EDGES) mirrors the immediate
// Trigger and Oneshot branches with these two fixes on.
#if TRIGGER_LATENCY_ALL_EDGES && (!SYNC_COUNT_OFF_BY_ONE_FIX || !ONESHOT_SLACK_FIX)
#error "restartPosition() mirrors the immediate branches with these fixes on"
#endif
//------ albEnd ----------------------------------


// reads loop audio into buffer, up to frames length, starting from loop_offset.  if fewer frames are
// available returns amount read.  if 0 is returned loop is done.
unsigned long
sl_read_current_loop_audio (LADSPA_Handle instance, float * buf, unsigned long frames, unsigned long loop_offset)
{
	SooperLooperI * pLS = (SooperLooperI *)instance;

	if (!pLS || !buf) return 0;

	LoopChunk * loop = pLS->headLoopChunk;
	if (!loop) return 0;
	if (loop_offset > loop->lLoopLength) return 0;
	
	// adjust for sync pos, so that a loop_offset of 0 actually means start from the syncpos
	unsigned long adj_offset  = (loop_offset + (loop->lLoopLength - loop->lSyncPos)) % loop->lLoopLength;
	unsigned long frames_left = loop->lLoopLength - loop_offset;
	unsigned long startpos = (loop->lLoopStart + adj_offset) & pLS->lBufferSizeMask;
	unsigned long first_chunk;
	unsigned long second_chunk=0;

	if (adj_offset > (loop->lLoopLength - loop->lSyncPos)) {
		// between sync and end of loop mem, clamp frames
		frames = std::min(loop->lLoopLength-adj_offset, frames);
	}

	if (frames > frames_left) {
		frames = frames_left;
	}
	
	if ( startpos > ((startpos + frames) & pLS->lBufferSizeMask)) {
		// crosses buffer boundary, 2 chunks needed
		first_chunk = pLS->lBufferSize - startpos;
		second_chunk = frames - first_chunk;
	}
	else {
		first_chunk = frames;
		second_chunk = 0;
	}


	// read first chunk
	memcpy ((char *)buf, (char *) &pLS->pSampleBuf[startpos], first_chunk * sizeof(LADSPA_Data));

	if (second_chunk) {
		memcpy ((char *) (buf + first_chunk), (char *) pLS->pSampleBuf, second_chunk * sizeof(LADSPA_Data));
	}

	return frames;
}

void
sl_set_samples_since_sync (LADSPA_Handle instance, unsigned long frames)
{
	SooperLooperI * pLS = (SooperLooperI *)instance;

	if (!pLS) return;

	pLS->lSamplesSinceSync = frames;
}

void
sl_set_replace_quantized (LADSPA_Handle instance, bool value)
{
	SooperLooperI * pLS = (SooperLooperI *)instance;

	if (!pLS) return;
	pLS->bReplaceQuantized = value;
}

void
sl_set_loop_index (LADSPA_Handle instance, unsigned int value, unsigned int chan)
{
	SooperLooperI * pLS = (SooperLooperI *)instance;
	
	if (!pLS) return;
	pLS->lLoopIndex = value;
	pLS->lChannelIndex = chan;
	DBG(fprintf(stderr, "set loop: %u  chan: %u\n", pLS->lLoopIndex, pLS->lChannelIndex));
}

bool
sl_get_replace_quantized (LADSPA_Handle instance)
{
	SooperLooperI * pLS = (SooperLooperI *)instance;
	if (!pLS) return false;
	return pLS->bReplaceQuantized;
}


bool sl_has_loop (const LADSPA_Handle instance)
{
	const SooperLooperI * pLS = (const SooperLooperI *)instance;
	if (!pLS) return false;
        return pLS->headLoopChunk != 0;
}

static bool invalidateTails (SooperLooperI * pLS, unsigned long bufstart, unsigned long buflen, LoopChunk * currloop)
{
	LoopChunk * tailLoop = pLS->tailLoopChunk;
	LoopChunk * tmploop;
	
	if (!tailLoop) {
		return false;
	}
	
	while (tailLoop  && tailLoop != currloop && 
	       (((bufstart + buflen) < pLS->lBufferSize &&
		 (tailLoop->lLoopStart >= bufstart && tailLoop->lLoopStart < (bufstart + buflen)))
		||
		((bufstart + buflen) >= pLS->lBufferSize &&
		 (tailLoop->lLoopStart < ((bufstart + buflen) & pLS->lBufferSizeMask)
		  || tailLoop->lLoopStart >= bufstart))))
	{
		
		// this invalidates a loop
		if (tailLoop->valid) {
			DBG(fprintf(stderr, "%u:%u  invalidating %08x\n", pLS->lLoopIndex, pLS->lChannelIndex, (unsigned) tailLoop));
			tailLoop->valid = 0;
			if (tailLoop->next) {
				tailLoop->next->prev = 0;
			}
			tmploop = tailLoop->next;
			//tailLoop->next = 0;
			tailLoop = tmploop;
		}
		else {
			// this really shouldn't happen
			DBG(fprintf(stderr, "%u:%u  we've hit the last tail this shoudlnt happen\n",pLS->lLoopIndex, pLS->lChannelIndex));
			tailLoop = 0;
		}

		// tailLoop = (tailLoop == pLS->lastLoopChunk) ? pLS->pLoopChunks: tailLoop + 1;
		pLS->tailLoopChunk = tailLoop;
	}

// 	if (!tailLoop || tailLoop == currloop) {
// 		DBG(fprintf(stderr, "we've hit nothing or ourselves, returning false\n"));
// 		return false;
// 	}
	if (pLS->tailLoopChunk == NULL) {
		// we invalidated the last loop
		pLS->tailLoopChunk = currloop;
	}
	
	return true;
}

static LoopChunk * ensureLoopSpace(SooperLooperI* pLS, LoopChunk *loop, unsigned long morelength, LoopChunk * pendsrc)
{
	// TODO: check to see if we'll require more space than the buffer allows
	
	if (!loop) {
		loop = (pLS->headLoopChunk == pLS->lastLoopChunk) ? pLS->pLoopChunks: pLS->headLoopChunk + 1;
		loop->lLoopStart = (pLS->headLoopChunk->lLoopStart + pLS->headLoopChunk->lLoopLength) & pLS->lBufferSizeMask;
		loop->lLoopLength = 0;
		loop->lCycleLength = 0;
		loop->lCycles = 0;
		loop->dCurrPos = 0;
		loop->frontfill = 0;
		loop->backfill = 0;
		loop->valid = 1;
		loop->mult_out = 0;
		loop->lSyncOffset = 0;
		loop->lSyncPos = 0;
		loop->lOrigSyncPos = 0;
		
		loop->next = NULL;
		loop->prev = pLS->headLoopChunk;
		if (loop->prev) {
			loop->prev->next = loop;
			// do this to help out the overdub fail case
			loop->lCycles = loop->prev->lCycles;
			loop->lCycleLength = loop->prev->lCycleLength;
			loop->lLoopLength = loop->prev->lLoopLength;
			loop->dCurrPos = loop->prev->dCurrPos;
		}
		loop->srcloop = pendsrc;

		pLS->headLoopChunk = loop;
	}
	else {

		// check to see if we'll require more space than the buffer allows
		if (loop->lLoopLength + morelength > pLS->lBufferSize) {
			DBG(fprintf(stderr, "%u:%u  requesting more space than exists in buffer!\n",pLS->lLoopIndex, pLS->lChannelIndex));
			return NULL;
		}
	}
	
	// invalidate any tails in this area
	invalidateTails (pLS, loop->lLoopStart, loop->lLoopLength + morelength, loop);

	return loop;
}


// creates a new loop chunk and puts it on the head of the list
// returns the new chunk
static LoopChunk * pushNewLoopChunk(SooperLooperI* pLS, unsigned long initLength, LoopChunk * pendsrc)
{
   LoopChunk * loop;   

   if (pLS->headLoopChunk) {
      // use the next spot in memory

      loop = ensureLoopSpace (pLS, NULL, initLength, pendsrc);

      if (!loop) {
	      return NULL;
      }
      

      // we are the new head
      pLS->headLoopChunk = loop;
      
   }
   else {
      // first loop on the list!
      loop = pLS->pLoopChunks;
      loop->next = loop->prev = NULL;
      pLS->headLoopChunk = pLS->tailLoopChunk = loop;
      loop->lLoopStart = 0;
      loop->valid = 1;
   }
   

   DBG(fprintf(stderr, "%u:%u  New head is %08x   start: %lu\n",pLS->lLoopIndex, pLS->lChannelIndex, (unsigned)loop, loop->lLoopStart);)

   //------ albBegin ----------------------------------
   // Chunks are reused: no early start or end point from a previous take.
#if REL_SYNC_LATE_HIT || SYNC_PREROLL
   loop->lStartEarly = 0;
#endif
#if FREE_PREROLL
   loop->lFreePreroll = 0;
#endif
#if REL_SYNC_LATE_HIT
   loop->lRelSyncLastPoint = 0;
#endif
   //------ albEnd ----------------------------------

   return loop;
}


// pop the head off and free it
static void popHeadLoop(SooperLooperI *pLS, bool forceClear)
{
   LoopChunk *dead;
   dead = pLS->headLoopChunk;

   if (dead && dead->prev) {
      // leave the next where is is for redo
      //dead->prev->next = NULL;
	   pLS->headLoopChunk = dead->prev;
	   if (!pLS->headLoopChunk->prev) {
		   pLS->tailLoopChunk = pLS->headLoopChunk; 
	   }
   }
   else if (dead && (!dead->next || forceClear)) {
	   // only clear the loop if this was the only loop in history
	   pLS->headLoopChunk = NULL;
	   // pLS->tailLoopChunk is still valid to support redo
	   // from nothing
   }
}

// clear all LoopChunks (undoAll , can still redo them back)
static void clearLoopChunks(SooperLooperI *pLS)
{
   
   pLS->headLoopChunk = NULL;
}

static void undoLoop(SooperLooperI *pLS, bool forceClear)
{
   LoopChunk *loop = pLS->headLoopChunk;
   LoopChunk *prevloop;
   
   if (!loop) return;
	
   prevloop = loop->prev;

   //if (prevloop && prevloop == loop->srcloop) {
      // if the previous was the source of the one we're undoing
      // pass the dCurrPos along, otherwise leave it be.
   // nevermind, ALWAYS pass it along, what the hell
   if (prevloop) {
	   prevloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, prevloop->lLoopLength);
   }
   
   popHeadLoop(pLS, forceClear);
   DBG(fprintf(stderr, "%u:%u  Undoing last loop %08x: new head is %08x\n" , pLS->lLoopIndex, pLS->lChannelIndex, (unsigned)loop,
	       (unsigned)pLS->headLoopChunk);)
}


//------ albBegin ----------------------------------
// CANCEL_OVERDUB_UNDO: throw away, at once, the overdub layer an eager
// overdub press has just started, making the layer under it current at the
// same position.  Input writing stops immediately rather than fading: the
// discarded layer does not matter, and the layer under it -- which becomes
// current, and may later be undone and redone -- must not take any input.
// The switch is not heard in practice: the overdub writes behind the play
// position (by the latency), so at the play position both layers hold the
// same audio.  The layer is unlinked, so a later redo cannot restore it.
// The caller sets the resulting state and play fade.
static void discardEagerOverdubLayer(SooperLooperI *pLS)
{
	pLS->fLoopFadeAtten    = 0.0f;  pLS->fLoopFadeDelta    = 0.0f;
	pLS->fFeedFadeAtten    = 1.0f;  pLS->fFeedFadeDelta    = 0.0f;
	pLS->fLoopSrcFadeAtten = 0.0f;  pLS->fLoopSrcFadeDelta = 0.0f;
	pLS->fFeedSrcFadeAtten = 1.0f;  pLS->fFeedSrcFadeDelta = 0.0f;
	undoLoop(pLS, false);
	if (pLS->headLoopChunk) {
		pLS->headLoopChunk->next = NULL;
	}
}
//------ albEnd ----------------------------------

static void redoLoop(SooperLooperI *pLS)
{
   LoopChunk *loop = NULL;
   LoopChunk *nextloop = NULL;

   if (pLS->headLoopChunk) {
      loop = pLS->headLoopChunk;
      nextloop = loop->next;
   }
   else if (pLS->tailLoopChunk) {
      // we've undone everything, use the tail
      loop = NULL;
      nextloop = pLS->tailLoopChunk;
   }

   if (nextloop) {
      
	   //if (loop && loop == nextloop->srcloop) {
	   // if the next is using us as a source
	   // pass the dCurrPos along, otherwise leave it be.
	   // nevermind, ALWAYS pass it along
	   if (loop) {
		   nextloop->dCurrPos = fmod(loop->dCurrPos+loop->lStartAdj, nextloop->lLoopLength);
	   }
	   else {
		   // start at loop beginning
		   nextloop->dCurrPos = nextloop->lLoopLength - nextloop->lSyncPos;
	   }
	   
	   pLS->headLoopChunk = nextloop;
	   
	   DBG(fprintf(stderr, "%u:%u  Redoing last loop %08x: new head is %08x\n", pLS->lLoopIndex, pLS->lChannelIndex, (unsigned)loop,
		       (unsigned)pLS->headLoopChunk);)

   }
}



/*****************************************************************************/

/* Construct a new plugin instance. */
LADSPA_Handle 
instantiateSooperLooper(const LADSPA_Descriptor * Descriptor,
			unsigned long             SampleRate)
{

   SooperLooperI * pLS;
   char * sampmem;
   
   // important note: using calloc to zero all data
   pLS = (SooperLooperI *) calloc(1, sizeof(SooperLooperI));
   
   if (pLS == NULL) 
      return NULL;

   pLS->pSampleBuf = NULL;
   pLS->pLoopChunks = NULL;
   pLS->pInputBuf = NULL;
   
   pLS->fSampleRate = (LADSPA_Data)SampleRate;

   pLS->fTotalSecs = SAMPLE_MEMORY;
   
   // HACK for the moment!
   sampmem = getenv("SL_SAMPLE_TIME");
   if (sampmem != NULL) {
	   if (sscanf(sampmem, "%f", &pLS->fTotalSecs) != 1) {
		   pLS->fTotalSecs = SAMPLE_MEMORY;
	   }
	   // printf ("Got sample mem: %f\n", pLS->fTotalSecs);
   }
   
   // we do include the LoopChunk structures in the Buf, so we really
   // get a little less the SAMPLE_MEMORY seconds
   //pLS->lBufferSize = (unsigned long)((LADSPA_Data)SampleRate * pLS->fTotalSecs * sizeof(LADSPA_Data));
   pLS->lBufferSize = (unsigned long) pow (2.0, ceil (log2 ((LADSPA_Data)SampleRate * pLS->fTotalSecs)));
   pLS->fTotalSecs = pLS->lBufferSize / (float) SampleRate;
   pLS->lBufferSizeMask = pLS->lBufferSize - 1;
   
   // not using calloc to force touching all memory ahead of time 
   // this could be bad if you try to allocate too much for your system
   // well, we are using calloc again... so sad
   pLS->pSampleBuf = (LADSPA_Data *) calloc(pLS->lBufferSize,  sizeof(LADSPA_Data));
   if (pLS->pSampleBuf == NULL) {
	   goto cleanup;
   }
   // we'll warm up up to 50 secs worth of the loop mem as a tradeoff to the low-mem mac people
	// Removed because this causes heavy cpu load on first record!!!
   //memset (pLS->pSampleBuf, 0, min(pLS->lBufferSize, (unsigned long) (SampleRate * 50) ) * sizeof(LADSPA_Data));

   pLS->lLoopChunkCount = MAX_LOOPS;

   // not using calloc to force touching all memory ahead of time -- PSYCHE!
   pLS->pLoopChunks = (LoopChunk *) calloc(pLS->lLoopChunkCount,  sizeof(LoopChunk));
   if (pLS->pLoopChunks == NULL) {
	   goto cleanup;
   }
   memset (pLS->pLoopChunks, 0, pLS->lLoopChunkCount * sizeof(LoopChunk));

   pLS->lastLoopChunk = pLS->pLoopChunks + pLS->lLoopChunkCount - 1;

   // this is the input buffer to handle input latency.  32k max samples of input latency
   //------ albBegin ----------------------------------
   // STOCK (sooperlooper 1.7.9): pLS->lInputBufSize = 32768;
   // CHANGE: at least g_input_history_seconds (see its comment), as a power
   // of two, never shorter than stock's.
   pLS->lInputBufSize = sl_input_history_samples (SampleRate);
   pLS->lHistoryShortCount = 0;
   pLS->lHistoryShortNeeded = 0;
   //------ albEnd ----------------------------------
   pLS->lInputBufMask = pLS->lInputBufSize - 1;
   pLS->pInputBuf = (LADSPA_Data *) calloc(pLS->lInputBufSize, sizeof(LADSPA_Data));
   pLS->lInputBufWritePos = 0;
   pLS->lInputBufReadPos = 0;
   
   /* just one for now */
   //pLS->lLoopStart = 0;
   //pLS->lLoopStop = 0;   
   //pLS->lCurrPos = 0;

   pLS->state = STATE_OFF;
   pLS->wasMuted = false;
   pLS->recSyncEnded = false;

   //------ albBegin ----------------------------------
   // Startup banner: always logged to confirm this is the experimental build.
   // Appears once per loop instance during instantiation.
   SL_LOG("SooperLooper [EXPERIMENTAL] instantiated -- late-hit patch active\n");
   // lLastSyncPeriodSamples starts as ULONG_MAX (calloc gives 0, which would
   // make the fraction term evaluate to 0 before the first sync period is
   // measured).  activate() also resets it to ULONG_MAX on each reset.
   pLS->lLastSyncPeriodSamples = ULONG_MAX;
   //------ albEnd ----------------------------------
   
   DBG(fprintf(stderr,"%u:%u  instantiated with buffersize: %lu\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lBufferSize));

   
   pLS->pfQuantMode = &pLS->fQuantizeMode;
   pLS->pfRoundMode = &pLS->fRoundMode;
   pLS->pfRedoTapMode = &pLS->fRedoTapMode;
   
   //cerr << "INSTANTIATE: " << pLS << endl;
   
   return pLS;

cleanup:

   if (pLS->pSampleBuf) {
	   free (pLS->pSampleBuf);
   }
   if (pLS->pLoopChunks) {
	   free (pLS->pLoopChunks);
   }
   return NULL;
   
}

/*****************************************************************************/

/* Throw away a simple delay line. */
void 
cleanupSooperLooper(LADSPA_Handle Instance) {
	
	SooperLooperI * pLS;
	
	pLS = (SooperLooperI *)Instance;
	
	if (pLS->pLoopChunks) {
		free (pLS->pLoopChunks);
	}
	
	if (pLS->pSampleBuf) {
		free (pLS->pSampleBuf);
	}
	
	//cerr << "******* cleanup SL instance" << endl;
	
	
	free(pLS);
}


/*****************************************************************************/

/* Initialise and activate a plugin instance. */
void
activateSooperLooper(LADSPA_Handle Instance) {

  SooperLooperI * pLS;
  pLS = (SooperLooperI *)Instance;

  //cerr << "Activate: " << pLS << endl;
	 
  pLS->lLastMultiCtrl = -1;
  //------ albBegin ----------------------------------
  pLS->lLastOverdubFrom = -1;   // cancel_overdub_undo bookkeeping
  //------ albEnd ----------------------------------

  //------ albBegin ----------------------------------
  // Activation banner: always logged to confirm experimental build is active.
  // Appears once per loop instance during activation.
  SL_LOG("SooperLooper [EXPERIMENTAL] activating loop %u:%u -- late-hit patch active\n",
      pLS->lLoopIndex, pLS->lChannelIndex);
  //------ albEnd ----------------------------------

  pLS->lScratchSamples = 0;
  pLS->lTapTrigSamples = 0;
  pLS->lRampSamples = 0;
  pLS->bPreTap = 1; // first tap init
  pLS->fLastScratchVal = 0.0f;
  pLS->fLastTapCtrl = -1;
  pLS->fCurrRate = 1.0f;
  pLS->fNextCurrRate = 0.0f;
  pLS->fQuantizeMode = 0;
  pLS->fRoundMode = 0;  
  pLS->bHoldMode = 0;
  pLS->fSyncMode = 0;
  pLS->fMuteQuantized = 0;
  pLS->fOverdubQuantized = 0;
  pLS->bReplaceQuantized = true;
  pLS->fRedoTapMode = 1;
  pLS->bRateCtrlActive = (int) *pLS->pfRateCtrlActive;

  pLS->waitingForSync = 0;
  pLS->donePlaySync = false;
  pLS->rounding = false;
  pLS->safetyFeedback = true;
  
  pLS->fWetCurr = pLS->fWetTarget = *pLS->pfWet;
  pLS->fDryCurr = pLS->fDryTarget = *pLS->pfDry;
  pLS->fFeedbackCurr = pLS->fFeedbackTarget = *pLS->pfFeedback;
  pLS->fRateCurr = pLS->fRateTarget = *pLS->pfRate;
  pLS->fScratchPosCurr = pLS->fScratchPosTarget = *pLS->pfScratchPos;

  pLS->fLoopFadeDelta = 0.0f;
  pLS->fLoopFadeAtten = 0.0f;
  pLS->fLoopSrcFadeDelta = 0.0f;
  pLS->fLoopSrcFadeAtten = 0.0f;
  pLS->fFeedSrcFadeDelta = 0.0f;
  pLS->fFeedSrcFadeAtten = 1.0f;
  pLS->fPlayFadeDelta = 0.0f;
  pLS->fPlayFadeAtten = 0.0f;
  pLS->fFeedFadeDelta = 0.0f;
  pLS->fFeedFadeAtten = 1.0f;

  // todo make this a port, for now 2ms
  //pLS->fLoopXfadeSamples = 0.002 * pLS->fSampleRate;
  
  pLS->state = STATE_OFF;
  pLS->nextState = -1;
  pLS->wasMuted = false;
  pLS->recSyncEnded = false;
  //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT
  pLS->lRelSyncPressEarly = 0;
#endif
  //------ albEnd ----------------------------------

  pLS->lSamplesSinceSync = 0;
  //------ albBegin ----------------------------------
#if REL_SYNC_STALE_PHASE_FIX
  pLS->lTrackedSinceSync    = REL_SYNC_PHASE_UNKNOWN;   // no pulse seen yet
  pLS->lTrackedSyncPeriod   = 0;
  pLS->fQuantModeLastBuffer = -1.0f;                    // any Quantize differs
#endif
  //------ albEnd ----------------------------------

  pLS->lInputBufWritePos = 0;
  pLS->lFramesUntilInput = 0;
  pLS->lFramesUntilFilled = 0;
  //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
  pLS->lEdgeDelay = 0;
  pLS->lEdgeEarly = 0;
  pLS->lEdgeTrim = 0;
#if FREE_PREROLL
  pLS->lFreePrerollNext = 0;
#endif
  pLS->bEdgeStopDeferred = false;
  pLS->fEdgeStopPulse = 0.0f;
  pLS->bEdgeEndDeferred = false;
  pLS->lEdgeEndNext = STATE_PLAY;
  pLS->bEdgeStopReverse = false;
  pLS->fEdgeEndPulse = 0.0f;
  pLS->bEdgeStopScratch = false;
#endif
  //------ albEnd ----------------------------------
  //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
  pLS->dOneshotLeft = 0.0;
#endif
#if TL_OVER_IL_LAYER_FIX
  pLS->lLayerLag = 0;
#endif
  //------ albEnd ----------------------------------
  memset (pLS->pInputBuf, 0, pLS->lInputBufSize * sizeof(LADSPA_Data));
  //------ albBegin ----------------------------------
  pLS->lGlobalSamplesSinceSync = 0;
  pLS->bSyncPulseReceived = false;
  pLS->lLastSyncPeriodSamples = ULONG_MAX; // sentinel: no period measured yet
  pLS->bLateHitBypassed = false;           // C2 fix: set each buffer by Looper::run()
  // Initialise as "already on" so the very first run() call after
  // instantiation never sees a spurious off->on transition (harmless
  // either way here, since the three fields above are already fresh,
  // but this keeps SYNC_MODE_LATE_HIT_RESET_FIX's transition check
  // meaningful only for a real, later toggle). See its comment block
  // near the top of run() for the full writeup.
  pLS->fSyncModeLastBuffer = 1.0f;
  //------ albEnd ----------------------------------
  
  clearLoopChunks(pLS);


  if (pLS->pfSecsTotal) {
     *pLS->pfSecsTotal = (LADSPA_Data) pLS->fTotalSecs;
  }

  if (pLS->pfSecsFree) {
	  *pLS->pfSecsFree = pLS->lBufferSize / pLS->fSampleRate;
  }
  
  //fprintf(stderr,"activated\n");  
}

/*****************************************************************************/

/* Connect a port to a data location. */
void 
connectPortToSooperLooper(LADSPA_Handle Instance,
			     unsigned long Port,
			     LADSPA_Data * DataLocation)
{
   
   SooperLooperI * pLS;

   //fprintf(stderr,"connectPortTo %d  %08x\n", Port, DataLocation);  

   
   pLS = (SooperLooperI *)Instance;
   switch (Port) {
      case DryLevel:
	 pLS->pfDry = DataLocation;
	 break;
      case WetLevel:
	 pLS->pfWet = DataLocation;
	 break;

      case Feedback:
	 pLS->pfFeedback = DataLocation;
	 break;
      case TriggerThreshold:
	 pLS->pfTrigThresh = DataLocation;
	 break;
      case Rate:
	 pLS->pfRate = DataLocation;
	 break;
      case ScratchPosition:
	 pLS->pfScratchPos = DataLocation;
	 break;
      case Multi:
	 pLS->pfMultiCtrl = DataLocation;
	 break;
      case TapDelayTrigger:
	 pLS->pfTapCtrl = DataLocation;
	 break;
      case UseFeedbackPlay:
	 pLS->pfUseFeedbackPlay = DataLocation;
	 break;
      case Quantize:
	 pLS->pfQuantMode = DataLocation;
	 break;
      case Sync:
	 pLS->pfSyncMode = DataLocation;
	 break;
      case PlaybackSync:
	 pLS->pfPlaybackSyncMode = DataLocation;
	 break;
      case UseRate:
	 pLS->pfRateCtrlActive = DataLocation;
	 break;
      case FadeSamples:
	 pLS->pfXfadeSamples = DataLocation;
	 break;
      case TempoInput:
	 pLS->pfTempo = DataLocation;
	 break;
      case EighthPerCycleLoop:
	 pLS->pfEighthPerCycle = DataLocation;
	 break;
      case Round:
	 pLS->pfRoundMode = DataLocation;
	 break;
      case RedoTap:
	 pLS->pfRedoTapMode = DataLocation;
	 break;
      case UseSafetyFeedback:
	 pLS->pfUseSafetyFeedback = DataLocation;
	 break;
      case OutputLatency:
	 pLS->pfOutputLatency = DataLocation;
	 break;
      case InputLatency:
	 pLS->pfInputLatency = DataLocation;
	 break;
      case TriggerLatency:
	 pLS->pfTriggerLatency = DataLocation;
	 break;
      case MuteQuantized:
	 pLS->pfMuteQuantized = DataLocation;
	 break;
      case OverdubQuantized:
	 pLS->pfOverdubQuantized = DataLocation;
	 break;
      case SyncOffsetSamples:
	 pLS->pfSyncOffsetSamples = DataLocation;
	 break;
      case RoundIntegerTempo:
	 pLS->pfRoundIntegerTempo = DataLocation;
	 break;
	 
      case AudioInputPort:
	 pLS->pfInput = DataLocation;
	 break;
      case AudioOutputPort:
	 pLS->pfOutput = DataLocation;
	 break;
      case SyncInputPort:
	 pLS->pfSyncInput = DataLocation;
	 break;
      case SyncOutputPort:
	 pLS->pfSyncOutput = DataLocation;
	 break;

      case State:
	 pLS->pfStateOut = DataLocation;
	 break;
      case LoopLength:
	 pLS->pfLoopLength = DataLocation;
	 break;
      case LoopPosition:
	 pLS->pfLoopPos = DataLocation;
	 break;
      case CycleLength:
	 pLS->pfCycleLength = DataLocation;
	 break;
      case LoopFreeMemory:
	 pLS->pfSecsFree = DataLocation;
	 break;
      case LoopMemory:
	 pLS->pfSecsTotal= DataLocation;
	 break;
      case Waiting:
	 pLS->pfWaiting= DataLocation;
	 break;
      case TrueRate:
	 pLS->pfRateOutput= DataLocation;
	 break;
      case NextState:
	 pLS->pfNextStateOut = DataLocation;
	 break;

   }
}



static inline void fillLoops(SooperLooperI *pLS, LoopChunk *mloop, unsigned long lCurrPos, bool leavemarks)
{
   LoopChunk *loop=NULL, *nloop, *srcloop;
   
   // descend to the oldest valid unfilled loop
   for (nloop=mloop; nloop; nloop = nloop->srcloop)
   {
      if (nloop->valid && (nloop->frontfill || nloop->backfill)) {
	 loop = nloop;
	 continue;
      }
      
      break;
   }

   // everything is filled!
   if (!loop) return;

   // do filling from earliest to latest
   for (; loop; loop=loop->next)
   {
      srcloop = loop->srcloop;

      // leavemarks is a special hack
      if (leavemarks || (loop->frontfill && lCurrPos<=loop->lMarkH && lCurrPos>=loop->lMarkL))
      {
	      if (!srcloop->valid) {
		      // if src is not valid, fill with silence
		      pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask] = 0.0f;
		      //DBG(fprintf(stderr, "srcloop invalid\n"));
	      }
	      else if (srcloop->lLoopLength) {
		      // we need to finish off a previous
		      pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask] = 
			      pLS->pSampleBuf[(srcloop->lLoopStart + (lCurrPos % srcloop->lLoopLength)) & pLS->lBufferSizeMask];
	      }

	      if (!leavemarks) {
		      // move the right mark according to rate
		      if (pLS->fCurrRate > 0) {
			      loop->lMarkL = lCurrPos;
		      }
		      else {
			      loop->lMarkH = lCurrPos;
		      }
		      
		      // ASSUMPTION: our overdub rate is +/- 1 only
		      if (loop->lMarkL == loop->lMarkH) {
			      // now we take the input from ourself
			      DBG(fprintf(stderr,"%u:%u  front segment filled for %08x for %08x in at %lu\n", pLS->lLoopIndex, pLS->lChannelIndex,
					  (unsigned)loop, (unsigned) srcloop, loop->lMarkL););
			      loop->frontfill = 0;
			      loop->lMarkL = loop->lMarkH = LONG_MAX;
		      }
	      }
      }
      else if (loop->backfill && lCurrPos<=loop->lMarkEndH && lCurrPos>=loop->lMarkEndL)		
      {

	      if (srcloop && !srcloop->valid) {
		      // if src is not valid, fill with silence
		      pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask] = 0.0f;
		      //DBG(fprintf(stderr, "srcloop invalid\n"));
	      }
	      else if (srcloop && srcloop->lLoopLength) {
		      // we need to finish off a previous
		      pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask] =
			      pLS->pSampleBuf[(srcloop->lLoopStart +
				       ((lCurrPos  + loop->lStartAdj - loop->lEndAdj) % srcloop->lLoopLength)) & pLS->lBufferSizeMask];
	      }

	      if (!leavemarks) {
		      // move the right mark according to rate
		      if (pLS->fCurrRate > 0) {
			      loop->lMarkEndL = lCurrPos;
		      }
		      else {
			      loop->lMarkEndH = lCurrPos;
			      
		      }
		      // ASSUMPTION: our overdub rate is +/- 1 only
		      if (loop->lMarkEndL == loop->lMarkEndH) {
			      // now we take the input from ourself
			      DBG(fprintf(stderr,"%u:%u  back segment filled in for %08x from %08x at %lu\n", pLS->lLoopIndex, pLS->lChannelIndex,
					  (unsigned)loop, (unsigned)srcloop, loop->lMarkEndL););
			      loop->backfill = 0;
			      loop->lMarkEndL = loop->lMarkEndH = LONG_MAX;
		      }
	      }
      }
      
      if (mloop == loop) break;
   }

}


static LoopChunk* transitionToNext(SooperLooperI *pLS, LoopChunk *loop, int nextstate);


//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
// useTriggerLag: as beginOverdub()'s (TL_OVER_IL_LAYER_FIX, plugin.hpp).
static LoopChunk* beginMultiply(SooperLooperI *pLS, LoopChunk *loop, bool useTriggerLag = false)
#else
static LoopChunk* beginMultiply(SooperLooperI *pLS, LoopChunk *loop)
#endif
//------ albEnd ----------------------------------
{
   LoopChunk * srcloop = loop;
   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;
   int prevstate = pLS->state;
   
   // first check if this is a multi-increase
   if (loop && loop->mult_out == (int)loop->lCycles && loop->backfill) {
	   //fprintf(stderr, "got a first multi-increase\n");
	   // increase by one cycle length

	   loop->mult_out += 1;
	   pLS->state = STATE_MULTIPLY;
	   
	   pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
	   pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
	   
	   pLS->nextState = STATE_PLAY;

	   return loop;
   }
   else {
	   // make new loop chunk
	   loop = pushNewLoopChunk(pLS, loop->lCycleLength, loop);
   }
	   
   if (loop) {
      // if there is no source bail
      if (!loop->prev) {
	      // lets just back out and revalidate the passed in loop
	      loop->next = NULL;

	      DBG(fprintf(stderr, "begin mult: there is no source\n"));
	      
	      loop = srcloop;
	      pLS->headLoopChunk = pLS->tailLoopChunk = loop;
	      loop->next = NULL;
	      loop->prev = NULL;
	      loop->valid = 1;

	      // no mult op
	      return NULL;
	      // this is already set if we have no srcloop anymore
      }
      else {
	      loop->srcloop = srcloop = loop->prev;
      }

      int prevstate = pLS->state;

      pLS->state = STATE_MULTIPLY;

      if (prevstate == STATE_TRIG_STOP || prevstate == STATE_RECORD) {
	      // coming out of record we can actually 
	      // set the loopfadeatten to 0 without harm
	      // since we're fading out the loopsrcfade
	       pLS->fLoopFadeAtten = 0.0f;
      }

//      if (*pLS->pfQuantMode == 0.0f) {
	      // we'll do this later if we are quantizing
	      pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
	      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
//      }

      pLS->fPlayFadeDelta = 1.0f / xfadeSamples; // added by jlc 20120709

      //pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
      long lInputLatency = (long) (*pLS->pfInputLatency);
      long lOutputLatency = (long) (*pLS->pfOutputLatency);
      long lTriggerLatency = (long) (*pLS->pfTriggerLatency);
      pLS->lFramesUntilInput = (long) lInputLatency - lTriggerLatency;
      //------ albBegin ----------------------------------
      // lLag: how much further back than OL + IL the multiply writes.
#if TL_OVER_IL_LAYER_FIX
      // As beginOverdub(): it reads the input -lFramesUntilInput back, so
      // it writes that much further back too.
      pLS->lLayerLag = (useTriggerLag && prevstate != STATE_RECORD
			&& prevstate != STATE_TRIG_STOP && lTriggerLatency > lInputLatency)
	      ? lTriggerLatency - lInputLatency : 0;
      const long lLag = pLS->lLayerLag;
#else
      const long lLag = 0;
#endif
      (void) lLag;
      //------ albEnd ----------------------------------

      // start out with the single cycle as our length as a marker
      loop->lLoopLength = srcloop->lCycleLength;
      //loop->lLoopLength = srcloop->lLoopLength;

      // start out at same pos
      loop->dCurrPos = srcloop->dCurrPos;
      loop->lCycles = 1; // start with 1 by default
      //loop->lCycles = srcloop->lCycles; 

      loop->lCycleLength = srcloop->lCycleLength;

      loop->lSyncPos = srcloop->lSyncPos;
      loop->lOrigSyncPos = srcloop->lOrigSyncPos;
      loop->lSyncOffset = srcloop->lSyncOffset;
		       
      // force rate to +1
      pLS->fCurrRate = 1.0;
      loop->frontfill = 1;
      loop->backfill = 0;
      loop->firsttime = 1;
      loop->lStartAdj = 0;
      loop->lEndAdj = 0;
      pLS->nextState = -1;
      loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);
		       
      // handle the case where the src loop
      // is already multiplied
      if (srcloop->lCycles > 1) {
	      // we effectively remove the first cycles from our new one
	      if (*pLS->pfQuantMode == QUANT_CYCLE) {
		      //loop->lStartAdj = ((int)floor(fabs((srcloop->dCurrPos-1) / srcloop->lCycleLength))
		      //		 + 1) * srcloop->lCycleLength; 
		      loop->lStartAdj = ((int)floorf(fabs((srcloop->dCurrPos) / srcloop->lCycleLength))) * srcloop->lCycleLength; 

		      loop->frontfill = 0; // no need.
		      DBG(fprintf(stderr, "%u:%u  start adj = %lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lStartAdj));
	      }
	      else {
		      // whatever cycle we are in will become the first
		      loop->lStartAdj = ((int)floorf(fabs((srcloop->dCurrPos) / srcloop->lCycleLength))) * srcloop->lCycleLength; 

	      }

	      // adjust dCurrPos by start adj.
	      // we handle this properly in the processing section
	      loop->dCurrPos = loop->dCurrPos - loop->lStartAdj;
	      
	      // start with 1 because we could end up with none!
	      // which will be subtracted at the end
	      loop->lCycles = 1;
	      loop->lLoopLength = srcloop->lCycleLength;
	      //loop->lLoopLength = 0;
	      DBG(fprintf(stderr,"%u:%u  Quantize ignoring first %d cycles.  Orig length %lu\n", pLS->lLoopIndex, pLS->lChannelIndex,
			  ((int)floor(srcloop->dCurrPos / srcloop->lCycleLength)+ 1),
			  srcloop->lLoopLength));
			  
      }

      double rCurrPos = srcloop->dCurrPos;

      if (prevstate != STATE_RECORD && prevstate != STATE_TRIG_STOP) {
	      //------ albBegin ----------------------------------
#if !MULTIPLY_FRONTFILL_FIX
	      rCurrPos -= lInputLatency - lOutputLatency;
#endif
	      //------ albEnd ----------------------------------
	      
	      if (rCurrPos < 0) {
		      rCurrPos += loop->lLoopLength;
	      }
	      rCurrPos = fmod (rCurrPos, loop->lLoopLength);
      }
      
      if (loop->dCurrPos > 0) {
		  loop->lMarkL = 0;
		  //loop->lMarkH = (unsigned long) rCurrPos - 1;
		  //loop->lMarkH = (unsigned long) max (rCurrPos - (lOutputLatency + lInputLatency), 1.0) - 1;
		  //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		  long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency + lLag), loop->lLoopLength) - 1;
#else
		  long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency), loop->lLoopLength) - 1;
#endif
		  //------ albEnd ----------------------------------
		  //cerr << "Low MARK H: " << markh << endl;
		  if (markh < 0) markh = 0;
		  loop->lMarkH = markh;
		  if (loop->lMarkH == 0) {
			  loop->frontfill = 0;
			  loop->lMarkL = loop->lMarkH = LONG_MAX;
		  }
      }
      else {
	      DBG(cerr << "no frontfill, dcurrpos: " << loop->dCurrPos << endl);
		  // no need to frontfill
		  loop->frontfill = 0;
		  loop->lMarkL = loop->lMarkH = LONG_MAX;
      }
      
      loop->lMarkEndL = loop->lMarkEndH = LONG_MAX;

      
      DBG(fprintf(stderr,"%u:%u  Mark at L:%lu  h:%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,loop->lMarkL, loop->lMarkH);
	      fprintf(stderr,"%u:%u  EndMark at L:%lu  h:%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,loop->lMarkEndL, loop->lMarkEndH);
		  fprintf(stderr,"%u:%u  Entering MULTIPLY state  with cyclecount=%lu   curpos=%g   looplen=%lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lCycles, loop->dCurrPos, loop->lLoopLength));

      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
	      // input always immediately available
	      pLS->lFramesUntilInput = 0;
	      // rest is handled elsewhere
	      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
	      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
      }

   }

   return loop;

}


// encapsulates what happens when a multiply is ended
static LoopChunk * endMultiply(SooperLooperI *pLS, LoopChunk *loop, int nextstate)
{
   LoopChunk *srcloop;
   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;
   
   srcloop = loop->srcloop;
		    
   if (*pLS->pfQuantMode != 0 && srcloop->lCycles > 1 && loop->lCycles<1)
   {
      DBG(fprintf(stderr,"%u:%u  Zero length loop now: at %d!\n", pLS->lLoopIndex, pLS->lChannelIndex, (int)loop->dCurrPos));
      DBG(fprintf(stderr,"%u:%u  Entering %d from MULTIPLY\n", pLS->lLoopIndex, pLS->lChannelIndex, nextstate));
      pLS->state = nextstate;

      loop->backfill = 0;
      loop->lLoopLength = 0;
   }
   else
   {
      if (*pLS->pfQuantMode != 0 && srcloop->lCycles > 1) {
	 // subtract a cycle right off?
	 //loop->lCycles -= 1;

      }

      
      long lInputLatency = (long) (*pLS->pfInputLatency);
      long lOutputLatency = (long) (*pLS->pfOutputLatency);
      //------ albBegin ----------------------------------
#if ROUNDED_MULTIPLY_FILL_FIX
      // Leftover fill only for an unrounded end: a rounded multiply
      // records on to the end (ROUNDED_MULTIPLY_FILL_FIX).
      if (*pLS->pfRoundMode == 0) pLS->lFramesUntilFilled = lInputLatency + lOutputLatency;
#else
      pLS->lFramesUntilFilled = lInputLatency + lOutputLatency; //  to take care of leftover filling
#endif
      //------ albEnd ----------------------------------
      
      if (*pLS->pfRoundMode == 0) {
	 // calculate loop length
	 loop->lLoopLength = loop->lCycles * loop->lCycleLength;
	 loop->backfill = 1;

	 // adjust curr position
	 //loop->dCurrPos -= loop->lStartAdj;
	 loop->lMarkEndL = (unsigned long) loop->dCurrPos;
	 loop->lMarkEndH = loop->lLoopLength - 1;
	 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
	 // A lagged multiply wrote up to OL + IL + lLayerLag behind the play
	 // position; the leftover fill above covers only the last OL + IL of
	 // that (at the next state's unlagged record position).  Fill the
	 // lLayerLag before it from the source now.
	 if (pLS->lLayerLag > 0 && loop->lLoopLength > 0) {
		 const long L = (long) loop->lLoopLength;
		 const long w = (long) loop->dCurrPos - (lInputLatency + lOutputLatency);
		 for (long j = 1; j <= pLS->lLayerLag; ++j) {
			 fillLoops(pLS, loop, (unsigned long) ((((w - j) % L) + L) % L), true);
		 }
	 }
#endif
	 //------ albEnd ----------------------------------

	 DBG(fprintf(stderr,"%u:%u  Entering %d from MULTIPLY. Length %lu.  %lu cycles\n", pLS->lLoopIndex, pLS->lChannelIndex,nextstate,
		 loop->lLoopLength, loop->lCycles)); 
	 DBG(fprintf(stderr,"%u:%u  EndMark at L:%lu  h:%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,loop->lMarkEndL, loop->lMarkEndH));

   
	 pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
	 pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
	 
	 loop = transitionToNext(pLS, loop, nextstate);

      }
      else {
	 // in round mode we need to wait it out
	 // and keep recording till the end
	 DBG(fprintf(stderr,"%u:%u  Finishing MULTIPLY rounded\n", pLS->lLoopIndex, pLS->lChannelIndex)); 
	 loop->lMarkEndL = (unsigned long)loop->dCurrPos;
//	 loop->lMarkEndH = loop->lLoopLength + loop->lStartAdj - 1;
	 loop->lMarkEndH = loop->lLoopLength - 1;
	 pLS->nextState = nextstate;
	 pLS->rounding = true;
      }
   }

   
   return loop;
}

//------ albBegin ----------------------------------
// MULTIPLY_END_FIX (see its comment block near the top of this file):
// finish a rounded multiply whose record position has reached the end.
// The play position carries its overshoot past the end on, rather than
// starting the loop again at 0.  Was inline in the STATE_MULTIPLY loop
// (with dCurrPos = 0); also used by the late multiply end.
//
// A multiply that started mid-cycle leaves the new loop's start to be
// filled from the source as the play position passes it (frontfill, in
// fillLoops); the play position now skips [0, overshoot), so fill that
// stretch here, in order, as it would have.
#if MULTIPLY_END_FIX
static LoopChunk * finishRoundedMultiply(SooperLooperI *pLS, LoopChunk *loop)
{
	const double overshoot = loop->dCurrPos - loop->lLoopLength;
	for (unsigned long i = 0; i < (unsigned long) overshoot; ++i) {
		fillLoops(pLS, loop, i, false);
	}
	loop->lMarkEndH = LONG_MAX;
	loop->backfill = 0;
	loop->lLoopLength = loop->lCycles * loop->lCycleLength;
	loop->dCurrPos = fmod(overshoot, loop->lLoopLength);
	pLS->rounding = false;
	return transitionToNext(pLS, loop, pLS->nextState);
}
#endif
//------ albEnd ----------------------------------


//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
// useTriggerLag: as beginOverdub()'s (TL_OVER_IL_LAYER_FIX, plugin.hpp).
static LoopChunk * beginInsert(SooperLooperI *pLS, LoopChunk *loop, bool useTriggerLag = false)
#else
static LoopChunk * beginInsert(SooperLooperI *pLS, LoopChunk *loop)
#endif
//------ albEnd ----------------------------------
{
   LoopChunk *srcloop = loop;
   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;
   int prevstate = pLS->state;
   
   // try to get a new one with at least 1 cycle more length
   loop = pushNewLoopChunk(pLS, loop->lLoopLength + loop->lCycleLength, loop);

   if (loop) {
      DBG(fprintf(stderr,"%u:%u  Entering INSERT state\n", pLS->lLoopIndex, pLS->lChannelIndex));

      if (!loop->prev) {
	      // lets just back out and revalidate the passed in loop
	      loop->next = NULL;

	      DBG(fprintf(stderr, "begin ins: there is no source\n"));
	      
	      loop = srcloop;
	      pLS->headLoopChunk = pLS->tailLoopChunk = loop;
	      loop->next = NULL;
	      loop->prev = NULL;
	      loop->valid = 1;

	      // no mult op
	      return NULL;
	      // this is already set if we have no srcloop anymore
      }
      else {
	      loop->srcloop = srcloop = loop->prev;
      }

      pLS->state = STATE_INSERT;

      if (*pLS->pfQuantMode == 0.0f) {
	      // we'll do this later if we are quantizing
	      pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
	      pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
      }

      pLS->fPlayFadeDelta = -1.0f / xfadeSamples;

      long lInputLatency = (long) (*pLS->pfInputLatency);
      long lOutputLatency = (long) (*pLS->pfOutputLatency);
      long lTriggerLatency = (long) (*pLS->pfTriggerLatency);
      
      pLS->lFramesUntilInput = (long) lInputLatency - lTriggerLatency;
      //------ albBegin ----------------------------------
      // lLag: how much further back than OL + IL the insert writes.
#if TL_OVER_IL_LAYER_FIX
      // As beginOverdub(): it reads the input -lFramesUntilInput back, so
      // it writes that much further back too.
      pLS->lLayerLag = (useTriggerLag && prevstate != STATE_RECORD
			&& prevstate != STATE_TRIG_STOP && lTriggerLatency > lInputLatency)
	      ? lTriggerLatency - lInputLatency : 0;
      const long lLag = pLS->lLayerLag;
#else
      const long lLag = 0;
#endif
      (void) lLag;
      //------ albEnd ----------------------------------
      
      // start out with the single cycle extra as our length
      loop->lLoopLength = srcloop->lLoopLength + srcloop->lCycleLength;
      loop->dCurrPos = srcloop->dCurrPos;
      loop->lCycles = srcloop->lCycles + 1; // start with src by default
      loop->lCycleLength = srcloop->lCycleLength;
      loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);
		       
      // force rate to +1
      pLS->fCurrRate = 1.0;
      loop->frontfill = 1;
      loop->backfill = 0;
      loop->firsttime = 1;
      loop->lStartAdj = 0;
      loop->lEndAdj = 0;
		       
      if (*pLS->pfQuantMode == QUANT_CYCLE) {
	 // the next cycle boundary
	 loop->lInsPos =
	    ((int)floor(fabs(srcloop->dCurrPos-1) / srcloop->lCycleLength)
	     + 1) * srcloop->lCycleLength; 
      }
      else {
	 loop->lInsPos = (unsigned long) loop->dCurrPos;
      }


      if (*pLS->pfRoundMode != 0) {
	      loop->lRemLen = (unsigned long) (srcloop->lLoopLength - loop->lInsPos);
      }
      else {
	      loop->lRemLen = (unsigned long) (srcloop->lLoopLength - loop->dCurrPos);
      }
      pLS->nextState = -1;

      double rCurrPos = srcloop->dCurrPos;

      if (prevstate != STATE_RECORD && prevstate != STATE_TRIG_STOP) {
	      //rCurrPos -= lInputLatency + lOutputLatency;
      
	      //if (rCurrPos < 0) {
	      //	      rCurrPos += loop->lLoopLength;
	      //}
	      //rCurrPos = fmod (rCurrPos, loop->lLoopLength);
      }
      
      if (loop->dCurrPos > 0) {
	      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
	      long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency + lLag), loop->lLoopLength) - 1;
#else
	      long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency), loop->lLoopLength) - 1;
#endif
	      //------ albEnd ----------------------------------
	      if (markh < 0) markh = 0;
	      loop->lMarkL = 0;
	      loop->lMarkH = (unsigned long) markh;
      }
      else {
	      loop->frontfill = 0; 
	      loop->lMarkL = loop->lMarkH = LONG_MAX;
      }
      
      loop->lMarkEndL = loop->lMarkEndH = LONG_MAX;
		       
      DBG(fprintf(stderr, "%u:%u  InsPos=%lu  RemLen=%lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lInsPos, loop->lRemLen));
      DBG(fprintf(stderr,"%u:%u  Total cycles now=%lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lCycles));
   }

   return loop;

}

// encapsulates what happens when a insert is ended
static LoopChunk * endInsert(SooperLooperI *pLS, LoopChunk *loop, int nextstate)
{
   LoopChunk *srcloop;

   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;
   
   srcloop = loop->srcloop;

   // we wait it out in insert until the end of the loop
   // in round mode we need to wait it out
   // and keep recording till the end

   // the end adjust is the difference in length between us and our source
   // now set our end adjustment for later backfill
   loop->lEndAdj = loop->lLoopLength - srcloop->lLoopLength;

   long lInputLatency = (long) (*pLS->pfInputLatency);
   long lOutputLatency = (long) (*pLS->pfOutputLatency);
   pLS->lFramesUntilFilled = lInputLatency + lOutputLatency; //  to take care of leftover filling
   
   loop->lMarkEndL = (unsigned long)loop->dCurrPos;
   loop->lMarkEndH = loop->lLoopLength - loop->lRemLen;

   DBG(fprintf(stderr,"%u:%u  Finishing INSERT... lMarkEndL=%lu  lMarkEndH=%lu  ll=%lu  rl=%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,
	       loop->lMarkEndL, loop->lMarkEndH, loop->lLoopLength, loop->lRemLen)); 

   pLS->nextState = nextstate;

   if (*pLS->pfRoundMode == 0.0f) {
	   pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
	   pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
   }

   pLS->rounding = true;
   
   return loop;
   
}

//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
// useTriggerLag: a layer started at once by its own command, which writes
// trigger_latency - input_latency further back when that is positive (see
// TL_OVER_IL_LAYER_FIX in plugin.hpp).  False for one started at a sync
// pulse (transitionToNext()) or out of a take.
static LoopChunk * beginOverdub(SooperLooperI *pLS, LoopChunk *loop, bool useTriggerLag = false)
#else
static LoopChunk * beginOverdub(SooperLooperI *pLS, LoopChunk *loop)
#endif
//------ albEnd ----------------------------------
{
   LoopChunk * srcloop = loop;
   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;

   // make new loop chunk
   //loop = pushNewLoopChunk(pLS, loop->lLoopLength, loop);
   loop = pushNewLoopChunk(pLS, 0, loop);
   if (loop) {
	   int prevstate = pLS->state;

      pLS->state = STATE_OVERDUB;
      // always the same length as previous loop

      if (prevstate == STATE_TRIG_STOP || prevstate == STATE_RECORD) {
	      // coming out of record we can actually 
	      // set the loopfadeatten to 0 without harm
	      // since we're fading out the loopsrcfade
              pLS->fLoopFadeAtten = 0.0f;
      }

      pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
      long lInputLatency = (long) (*pLS->pfInputLatency);
      long lOutputLatency = (long) (*pLS->pfOutputLatency);
      long lTriggerLatency = (long) (*pLS->pfTriggerLatency);

      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;

      pLS->fPlayFadeDelta = 1.0f / xfadeSamples; // added by jlc 20120709

      pLS->lFramesUntilInput = (long) lInputLatency - lTriggerLatency;
      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
      //------ albBegin ----------------------------------
      // lLag: how much further back than OL + IL the layer writes.
#if TL_OVER_IL_LAYER_FIX
      // It reads the input -lFramesUntilInput back (the run loop), so it
      // writes that much further back too (and fills the stretch behind
      // the play position at once, below).
      pLS->lLayerLag = (useTriggerLag && prevstate != STATE_RECORD
			&& prevstate != STATE_TRIG_STOP && lTriggerLatency > lInputLatency)
	      ? lTriggerLatency - lInputLatency : 0;
      const long lLag = pLS->lLayerLag;
#else
      const long lLag = 0;
#endif
      //------ albEnd ----------------------------------
      DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex << "   frames until input: " << pLS->lFramesUntilInput << "  xfade samps: " << xfadeSamples << endl);

      if (!loop->prev) {
	      // then we are overwriting our own source
	      // lets just back out and revalidate the passed in loop
	      loop->next = NULL;

	      DBG(fprintf(stderr, "%u:%u  OVERdub using self\n", pLS->lLoopIndex, pLS->lChannelIndex));
	      
	      loop = srcloop;
	      pLS->headLoopChunk = pLS->tailLoopChunk = loop;
	      loop->next = NULL;
	      loop->prev = NULL;
	      loop->valid = 1;
	      loop->srcloop = loop; // !!!

	      pLS->nextState = -1;

	      return loop;
	      // this is already set if we have no srcloop anymore
      }
      else {
	      loop->srcloop = srcloop = loop->prev;
      }

      loop->lCycles = srcloop->lCycles;
      loop->lCycleLength = srcloop->lCycleLength;
      loop->lLoopLength = srcloop->lLoopLength;
      loop->dCurrPos = srcloop->dCurrPos;
      loop->lSyncPos = srcloop->lSyncPos;
      loop->lOrigSyncPos = srcloop->lOrigSyncPos;
      loop->lSyncOffset = srcloop->lSyncOffset;
    
      loop->lStartAdj = 0;
      loop->lEndAdj = 0;
      pLS->nextState = -1;
      
      loop->dOrigFeedback = LIMIT_BETWEEN_0_AND_1(*pLS->pfFeedback);

      //double rCurrPos = loop->dCurrPos - lInputLatency - lOutputLatency;
      double rCurrPos = loop->dCurrPos;
      if (rCurrPos < 0) {
	      rCurrPos += loop->lLoopLength;
      }
      rCurrPos = fmod (rCurrPos, loop->lLoopLength);
      
      if (rCurrPos > 0) 
	 loop->frontfill = 1;
      else
	 loop->frontfill = 0;

      loop->backfill = 1;
      // logically we need to fill in the cycle up to the
      // srcloop's current position.
      // we let the overdub loop itself do this when it gets around to it
      
      if (pLS->fCurrRate < 0) {
	 pLS->fCurrRate = -1.0;
	 // negative rate
	 // need to fill in between these values 
	 loop->lMarkL = (unsigned long) rCurrPos + 1;		       
	 loop->lMarkH = loop->lLoopLength - 1;
	 loop->lMarkEndL = 0;
	 //loop->lMarkEndH = (unsigned long) rCurrPos;
	 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
	 loop->lMarkEndH = (unsigned long) fmod (rCurrPos + (lOutputLatency + lInputLatency + lLag), loop->lLoopLength);
#else
	 loop->lMarkEndH = (unsigned long) fmod (rCurrPos + (lOutputLatency + lInputLatency), loop->lLoopLength);
#endif
	 //------ albEnd ----------------------------------

	 if (loop->lMarkEndH < loop->lMarkL) {
		 DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex << "  Border line rev" << endl);
		 loop->lMarkL = loop->lMarkEndH;
		 loop->lMarkEndH = (unsigned long) rCurrPos + 1;
	 }

      } else {
	 pLS->fCurrRate = 1.0;
	 loop->lMarkL = 0;
	 //loop->lMarkH = (unsigned long) fmod (rCurrPos - (lOutputLatency + lInputLatency), loop->lLoopLength)) - 1;
	 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
	 long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency + lLag), loop->lLoopLength) - 1;
#else
	 long markh = (long) fmod (rCurrPos - (lOutputLatency + lInputLatency), loop->lLoopLength) - 1;
#endif
	 //------ albEnd ----------------------------------
	 if (markh < 0) markh = 0;
	 loop->lMarkH = (unsigned long) markh;
	 loop->lMarkEndL = (unsigned long) rCurrPos;
	 loop->lMarkEndH = loop->lLoopLength - 1;

	 if (loop->lMarkH > loop->lMarkEndL) {
		 DBG(cerr << "Border line" << endl);
		 loop->lMarkEndL = loop->lMarkH;
		 loop->lMarkH = (unsigned long) rCurrPos;
	 }
	 //------ albBegin ----------------------------------
#if OVERDUB_WRAP_FILL_FIX
	 // The stretch written OL + IL (+ lLag) behind the play position
	 // wraps to the loop's end, or starts at 0: no front fill, and the
	 // back fill stops where the stretch begins (it is filled as it is
	 // written).  See OVERDUB_WRAP_FILL_FIX.
	 {
		 const long gapStart = (long) rCurrPos - (lOutputLatency + lInputLatency + lLag);
		 if (gapStart <= 0 && (long) loop->lLoopLength + gapStart > (long) rCurrPos) {
			 loop->frontfill = 0;
			 loop->lMarkL = loop->lMarkH = LONG_MAX;
			 loop->lMarkEndL = (unsigned long) rCurrPos;
			 loop->lMarkEndH = (unsigned long) ((long) loop->lLoopLength + gapStart - 1);
		 }
	 }
#endif
	 //------ albEnd ----------------------------------
      }
      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
      // A lagged layer: copy the previous layer into the stretch between
      // the write position and the play position now, as the run loop's
      // lFramesUntilFilled pass would as the write position crosses it.
      // That pass runs on in the next state at the unlagged position if
      // the layer ends first, which would leave lLag samples unfilled.
      // Bounded: OL + IL + lLag samples.  Lag 0 keeps the stock pass.
      if (lLag > 0 && loop->lLoopLength > 0) {
	      const long span = lOutputLatency + lInputLatency + lLag;
	      const long L = (long) loop->lLoopLength;
	      for (long j = 0; j <= span; ++j) {
		      const long p = (pLS->fCurrRate < 0) ? (long) rCurrPos + j : (long) rCurrPos - j;
		      fillLoops(pLS, loop, (unsigned long) (((p % L) + L) % L), true);
	      }
	      pLS->lFramesUntilFilled = 0;
      }
#endif
      //------ albEnd ----------------------------------

      DBG(fprintf(stderr,"%u:%u  Mark at L:%lu  h:%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,loop->lMarkL, loop->lMarkH));
      DBG(fprintf(stderr,"%u:%u  EndMark at L:%lu  h:%lu\n", pLS->lLoopIndex, pLS->lChannelIndex,loop->lMarkEndL, loop->lMarkEndH));
      DBG(fprintf(stderr,"%u:%u  Entering OVERDUB/repl/subs state: srcloop is %08x\n", pLS->lLoopIndex, pLS->lChannelIndex, (unsigned)srcloop));
   }

   return loop;
}

//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
// useTriggerLag: as beginOverdub()'s.
static LoopChunk * beginSubstitute(SooperLooperI *pLS, LoopChunk *loop, bool useTriggerLag = false)
{
	LoopChunk * tloop = beginOverdub(pLS, loop, useTriggerLag);
#else
static LoopChunk * beginSubstitute(SooperLooperI *pLS, LoopChunk *loop)
{
	LoopChunk * tloop = beginOverdub(pLS, loop);
#endif
//------ albEnd ----------------------------------
	int xfadeSamples = (int) (*pLS->pfXfadeSamples);
	if (xfadeSamples < 1) xfadeSamples = 1;

	if (tloop) {
		pLS->state = STATE_SUBSTITUTE;
		pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
		pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
	}

	return tloop;
}

//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
// useTriggerLag: as beginOverdub()'s.
static LoopChunk * beginReplace(SooperLooperI *pLS, LoopChunk *loop, bool useTriggerLag = false)
{
	LoopChunk * tloop = beginOverdub(pLS, loop, useTriggerLag);
#else
static LoopChunk * beginReplace(SooperLooperI *pLS, LoopChunk *loop)
{
	LoopChunk * tloop = beginOverdub(pLS, loop);
#endif
//------ albEnd ----------------------------------
	int xfadeSamples = (int) (*pLS->pfXfadeSamples);
	if (xfadeSamples < 1) xfadeSamples = 1;

	if (tloop) {
		pLS->state = STATE_REPLACE;
		pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
		pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
	}

	return tloop;
}


static LoopChunk * transitionToNext(SooperLooperI *pLS, LoopChunk *loop, int nextstate)
{
   LoopChunk * newloop = loop;

   int xfadeSamples = (int) (*pLS->pfXfadeSamples);
   if (xfadeSamples < 1) xfadeSamples = 1;

   unsigned int eighthSamples = 0;
   unsigned int syncSamples = 0;
   if (*pLS->pfTempo > 0.0f) {
	   eighthSamples = (unsigned int) (pLS->fSampleRate * 30.0 / *pLS->pfTempo);
	   
	   if (*pLS->pfQuantMode == QUANT_CYCLE || *pLS->pfQuantMode == QUANT_LOOP) {
		   syncSamples = (unsigned int) (eighthSamples * *pLS->pfEighthPerCycle / 2);
	   }
	   else if (*pLS->pfQuantMode == QUANT_8TH) {
		   syncSamples = (eighthSamples / 2);
	   }
   }
   

   
   switch(nextstate)
   {
      case STATE_PLAY:
	      pLS->fLoopFadeDelta = -1.0f / (xfadeSamples);
	      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
	      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
	      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
	      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
	      pLS->wasMuted = false;
	      if (pLS->state == STATE_PAUSED && loop) {
		      // set current loop position to paused position
		      loop->dCurrPos = pLS->dPausedPos;
		      DBG(fprintf(stderr, "%u:%u  t2n setting loop pos to pause pos: %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->dPausedPos));
	      }
	      break;
      case STATE_MUTE:
      case STATE_PAUSED:
	      pLS->fLoopFadeDelta = -1.0f / (xfadeSamples);
	      pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
	      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
	      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
	      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
	      pLS->wasMuted = true;
	      if (nextstate == STATE_PAUSED && loop) {
		      pLS->dPausedPos = loop->dCurrPos;
		      //------ albBegin ----------------------------------
		      // See PAUSE_FROM_MUTE_FIX's comment block near the top of
		      // this file. pLS->state
		      // still holds the pre-transition value here (only
		      // overwritten below, after this whole switch), so this is
		      // exactly the information needed to mirror the immediate
		      // path's own wasMuted semantics -- true only when actually
		      // coming from STATE_MUTE. Provably zero-regression: before
		      // PAUSE_FROM_MUTE_FIX part 1 (above), nothing in this file
		      // ever set nextState/nextstate to STATE_PAUSED, so this
		      // branch had no caller to have any observed behaviour yet.
#if PAUSE_FROM_MUTE_FIX
		      pLS->wasMuted = (pLS->state == STATE_MUTE);
#else
		      pLS->wasMuted = false;
#endif
		      //------ albEnd ----------------------------------
	      }
	      break;

      case STATE_OVERDUB:
	 newloop = beginOverdub(pLS, loop);
	 break;

      case STATE_REPLACE:
	 newloop = beginReplace(pLS, loop);
	 break;

      case STATE_SUBSTITUTE:
	 newloop = beginSubstitute(pLS, loop);
	 break;
	 
      case STATE_INSERT:
	 newloop = beginInsert(pLS, loop);
	 if (!newloop) {
		 nextstate = -1;
	 }
	 break;

      case STATE_MULTIPLY:
	 newloop = beginMultiply(pLS, loop);
	 if (!newloop) {
		 nextstate = -1;
	 }
	 break;

      case STATE_TRIGGER_PLAY:
	      pLS->fLoopFadeDelta = -1.0f / (xfadeSamples);
	      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
	      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
	      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
	      //------ albBegin ----------------------------------
#if TRIGGER_FEED_FADE_FIX
	      // Fade the feedback back in, as STATE_PLAY does (a Replace or
	      // Substitute left it at 0).
	      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
#endif
	      //------ albEnd ----------------------------------
	      if (loop) {
		      //------ albBegin ----------------------------------
		      // RESTART_POSITION_FIX: the state before this transition
		      // (overwritten on the next line).
		      const int prevstate = pLS->state;
		      (void) prevstate;
		      //------ albEnd ----------------------------------
		      pLS->state = STATE_PLAY;
		      nextstate = STATE_PLAY;
		      pLS->wasMuted = false;
		      if (pLS->fCurrRate > 0) {

			      DBG(fprintf(stderr, "%u:%u  trigger play : pre loop pos: %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));

			      loop->dCurrPos = (double) loop->lLoopLength - loop->lSyncPos;
			      DBG(fprintf(stderr, "%u:%u  trigger play %08x: syncpos: %ld\n", pLS->lLoopIndex, pLS->lChannelIndex, (unsigned)pLS, loop->lSyncPos));

			      //loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength); // wrapx

			      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			      // Increment (e): the slack window, from the press.
			      if (*pLS->pfSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)) {
#else
			      if (*pLS->pfSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
			      //------ albEnd ----------------------------------
				      // if rel sync is on, shift it forward to account for not being quantized
				      loop->dCurrPos += pLS->lSamplesSinceSync; // jlc
				      DBG(fprintf(stderr, "%u:%u  after adding %u  pos = %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lSamplesSinceSync, loop->dCurrPos));
			      }
		      }
		      else {
			      loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1;
			      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			      // Increment (e): the slack window, from the press.
			      if (*pLS->pfSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)) {
#else
			      if (*pLS->pfSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
			      //------ albEnd ----------------------------------
				      // if rel sync is on, shift it forward to account for not being quantized
				      loop->dCurrPos -= pLS->lSamplesSinceSync; // jlc
				      DBG(fprintf(stderr, "%u:%u  after sub %u  pos = %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lSamplesSinceSync, loop->dCurrPos));
			      }
		      }
		      //------ albBegin ----------------------------------
#if RESTART_POSITION_FIX
		      // Start IL + OL into the loop, as playback after a record
		      // does, so a restart is heard in time.  From Record and
		      // TrigStop, MULTI_TRIGGER adds it after this transition.
		      // No wrap needed: the play loop wraps it, as it does the
		      // Record path's lLoopLength + IL + OL.  Reverse subtracts.
		      if (prevstate != STATE_RECORD && prevstate != STATE_TRIG_STOP) {
			      loop->dCurrPos += ((long) *pLS->pfInputLatency
						 + (long) *pLS->pfOutputLatency) * pLS->fCurrRate;
		      }
#endif
		      //------ albEnd ----------------------------------


	      }
	      break;
      case STATE_ONESHOT:
	      // play the loop one_shot mode
	      pLS->fLoopFadeDelta = -1.0f / (xfadeSamples);
	      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
	      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
	      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
	      //------ albBegin ----------------------------------
#if TRIGGER_FEED_FADE_FIX
	      // Fade the feedback back in, as STATE_PLAY does (a Replace or
	      // Substitute left it at 0).
	      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
#endif
	      //------ albEnd ----------------------------------
	      if (loop) {
		      DBG(fprintf(stderr,"%u:%u  Starting ONESHOT state\n", pLS->lLoopIndex, pLS->lChannelIndex));
		      //------ albBegin ----------------------------------
		      // RESTART_POSITION_FIX: the state before this transition
		      // (overwritten on the next line).
		      const int prevstate = pLS->state;
		      (void) prevstate;
		      //------ albEnd ----------------------------------
		      pLS->state = STATE_ONESHOT;
		      if (pLS->fCurrRate > 0) {
			      loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) + *pLS->pfSyncOffsetSamples;

			      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			      // Increment (e): the slack window, from the press.
			      if (*pLS->pfSyncMode == 2.0f && inSlackWindow(pLS, eighthSamples)) {
#else
			      if (*pLS->pfSyncMode == 2.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
			      //------ albEnd ----------------------------------
				      // if rel sync is on, shift it forward to account for not being quantized
				      loop->dCurrPos += pLS->lSamplesSinceSync; // jlc
				      DBG(fprintf(stderr, "%u:%u  after adding %u  pos = %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lSamplesSinceSync, loop->dCurrPos));
			      }
		      }
		      else {
			      loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1;
			      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			      // Increment (e): the slack window, from the press.
			      if (*pLS->pfSyncMode == 2.0f && inSlackWindow(pLS, eighthSamples)) {
#else
			      if (*pLS->pfSyncMode == 2.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
			      //------ albEnd ----------------------------------
				      // if rel sync is on, shift it forward to account for not being quantized
				      loop->dCurrPos -= pLS->lSamplesSinceSync; // jlc
				      DBG(fprintf(stderr, "%u:%u  after sub %u  pos = %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lSamplesSinceSync, loop->dCurrPos));
			      }
		      }


		      // wrap
		      loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		      //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
		      // One loop length, less the count added above (Rel Sync).
		      startOneshotPass(pLS, loop,
			      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			      // Increment (e): the slack window, from the press.
			      (*pLS->pfSyncMode == 2.0f && inSlackWindow(pLS, eighthSamples))
#else
			      (*pLS->pfSyncMode == 2.0f && pLS->lSamplesSinceSync < eighthSamples)
#endif
			      //------ albEnd ----------------------------------
			      ? (double) pLS->lSamplesSinceSync : 0.0);
#endif
		      //------ albEnd ----------------------------------
		      //------ albBegin ----------------------------------
#if RESTART_POSITION_FIX
		      // Start IL + OL in, as in the STATE_TRIGGER_PLAY case above.
		      // Record and TrigStop are left as stock.
		      if (prevstate != STATE_RECORD && prevstate != STATE_TRIG_STOP) {
			      loop->dCurrPos += ((long) *pLS->pfInputLatency
						 + (long) *pLS->pfOutputLatency) * pLS->fCurrRate;
		      }
#endif
		      //------ albEnd ----------------------------------

		      DBG(fprintf(stderr,"%u:%u  Starting ONESHOT state, currpos now: %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));
	      }
	      break;
   }

   if (nextstate != -1) {
	   if (loop) {
		   DBG(fprintf(stderr,"%u:%u  Entering state %d from %d at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, nextstate, pLS->state, loop->dCurrPos));
	   }
	   pLS->state = nextstate;

   }
   else {
      DBG(fprintf(stderr,"%u:%u  Next state is -1?? Why?\n", pLS->lLoopIndex, pLS->lChannelIndex));
      pLS->state = STATE_PLAY;
      pLS->wasMuted = false;
   }

   // never wait for input when transitioning
   pLS->lFramesUntilInput = 0;
		      

   return newloop;
}


/*****************************************************************************/

/* Run the sampler  for a block of SampleCount samples. */
void 
runSooperLooper(LADSPA_Handle Instance,
	       unsigned long SampleCount)
{

  LADSPA_Data * pfBuffer;
  LADSPA_Data * pfInput;
  LADSPA_Data * pfOutput;
  LADSPA_Data * pfSyncInput;
  LADSPA_Data * pfSyncOutput;
  LADSPA_Data * pfInputLatencyBuf;
  LADSPA_Data fDry=1.0f, fWet=1.0f, tmpWet;
  LADSPA_Data dryDelta=0.0f, wetDelta=0.0f, dryTarget=1.0f, wetTarget=1.0f;
  LADSPA_Data fInputSample;
  LADSPA_Data fOutputSample;

  LADSPA_Data fRate = 1.0f;
  LADSPA_Data fScratchPos = 0.0f;
  //LADSPA_Data rateDelta=0.0f, rateTarget=1.0f;
  LADSPA_Data scratchDelta=0.0f, scratchTarget=0.0f;
  LADSPA_Data fTrigThresh = 0.0f;
  LADSPA_Data fTempo;
  unsigned int eighthSamples = 1;
  unsigned int syncSamples = 0;
  unsigned int eighthPerCycle = 8;
  
  int lMultiCtrl=-1;  
  bool useFeedbackPlay = false;
  LADSPA_Data fTapTrig = 0.0f;
  
  LADSPA_Data fFeedback = 1.0f;
  LADSPA_Data feedbackDelta=0.0f, feedbackTarget=1.0f;
  unsigned int lCurrPos = 0;
  unsigned int xCurrPos = 0;
  unsigned int lpCurrPos = 0;  
  LADSPA_Data *pLoopSample, *spLoopSample, *rLoopSample, *rpLoopSample, *xLoopSample;
  long slCurrPos;
  double rCurrPos;
  double rpCurrPos;
  double dDummy;
  int firsttime, backfill;
  int useDelay = 0;
  int prevstate;
  
  float fPosRatio;
  int xfadeSamples = XFADE_SAMPLES;
  
  SooperLooperI * pLS;
  LoopChunk *loop, *srcloop=0;
  LoopChunk *lastloop, *prevloop, *nextloop;
  
  LADSPA_Data fSyncMode = 0.0f;
  LADSPA_Data fQuantizeMode = 0.0f;
  LADSPA_Data fPlaybackSyncMode = 0.0f;
  LADSPA_Data fMuteQuantized = 0.0f;
  LADSPA_Data fOverdubQuantized = 0.0f;
  bool        bReplaceQuantized = true;
  LADSPA_Data fSyncOffsetSamples = 0.0f;
  bool bRoundIntegerTempo = false;

  unsigned long lSampleIndex;

  LADSPA_Data fSafetyFeedback;
  
  pLS = (SooperLooperI *)Instance;

  if (!pLS || !pLS->pfInput || !pLS->pfOutput) {
     // something is badly wrong!!!
     return;
  }
  
  pfInput = pLS->pfInput;
  pfOutput = pLS->pfOutput;
  pfBuffer = (LADSPA_Data *)pLS->pSampleBuf;
  pfSyncOutput = pLS->pfSyncOutput;
  pfSyncInput = pLS->pfSyncInput;
  pfInputLatencyBuf = (LADSPA_Data *) pLS->pInputBuf;

  xfadeSamples = (int) (*pLS->pfXfadeSamples);
  if (xfadeSamples < 1) xfadeSamples = 1;
  
  fTrigThresh = *pLS->pfTrigThresh;

  pLS->bRateCtrlActive = (int) *pLS->pfRateCtrlActive;

  fSyncMode = *pLS->pfSyncMode;

  //------ albBegin ----------------------------------
  // SYNC_MODE_LATE_HIT_RESET_FIX: see this macro's comment block near
  // the top of this file. Detects the Sync port's own off->on
  // transition and resets the same three fields
  // Looper::reset_late_hit_state() already resets for a sync SOURCE
  // change, so a stale value from before sync was disabled cannot
  // survive into the first button press after it is re-enabled.
#if SYNC_MODE_LATE_HIT_RESET_FIX
  if (fSyncMode >= 1.0f && pLS->fSyncModeLastBuffer < 1.0f) {
	  pLS->lGlobalSamplesSinceSync = 0;
	  pLS->bSyncPulseReceived      = false;
	  pLS->lLastSyncPeriodSamples  = ULONG_MAX;
  }
#endif
  // REL_SYNC_STALE_PHASE_FIX (see its comment block near the top of this
  // file): under Rel Sync the loop's counter must match the pulses.
#if REL_SYNC_STALE_PHASE_FIX
  {
	  // A Quantize change changes the pulse spacing: the tracked count
	  // is not known again until the next pulse.
	  const bool quantChanged = (*pLS->pfQuantMode != pLS->fQuantModeLastBuffer);
	  pLS->fQuantModeLastBuffer = *pLS->pfQuantMode;
	  if (quantChanged) {
		  pLS->lTrackedSinceSync  = REL_SYNC_PHASE_UNKNOWN;
		  pLS->lTrackedSyncPeriod = 0;
	  }
	  // Take the tracked count on entering Rel Sync or a Quantize change,
	  // and whenever the tracked count is unknown (no pulse yet since a
	  // source change, or none for over two spacings).
	  if (fSyncMode == 2.0f
	      && (pLS->fSyncModeLastBuffer != 2.0f || quantChanged
	          || pLS->lTrackedSinceSync >= REL_SYNC_PHASE_UNKNOWN)) {
		  pLS->lSamplesSinceSync = pLS->lTrackedSinceSync;
	  }
  }
#endif
#if SYNC_MODE_LATE_HIT_RESET_FIX || REL_SYNC_STALE_PHASE_FIX
  pLS->fSyncModeLastBuffer = fSyncMode;
#endif
#if REL_SYNC_STALE_PHASE_FIX
  // Track the source's pulses whatever the state or Sync setting (only
  // channel 0's input is the source; see Looper::run()).  Same counting
  // as the states' own: ++, then 0 on a pulse.
  if (pLS->lChannelIndex == 0) {
	  // The spacing kept is the longest since the count became known: a
	  // loop as the source also sends a pulse at its state changes, which
	  // can only make a spacing shorter, so it cannot make the stop test
	  // below fire early.  Such pulses still reset the count, as they
	  // reset the states' own counters.
	  for (unsigned long n = 0; n < SampleCount; ++n) {
		  if (pLS->lTrackedSinceSync < REL_SYNC_PHASE_UNKNOWN) {
			  pLS->lTrackedSinceSync++;
		  }
		  if (pfSyncInput[n] > 1.5f) {
			  if (pLS->lTrackedSinceSync >= REL_SYNC_PHASE_UNKNOWN) {
				  pLS->lTrackedSyncPeriod = 0;
			  } else if (pLS->lTrackedSinceSync > pLS->lTrackedSyncPeriod) {
				  pLS->lTrackedSyncPeriod = pLS->lTrackedSinceSync;
			  }
			  pLS->lTrackedSinceSync = 0;
		  }
	  }
	  // No pulse for over two spacings (the transport stopped): a count
	  // from before the stop must not be used when the pulses resume.
	  if (pLS->lTrackedSyncPeriod != 0
	      && pLS->lTrackedSinceSync < REL_SYNC_PHASE_UNKNOWN
	      && pLS->lTrackedSinceSync > 2 * pLS->lTrackedSyncPeriod) {
		  pLS->lTrackedSinceSync  = REL_SYNC_PHASE_UNKNOWN;
		  pLS->lTrackedSyncPeriod = 0;
	  }
  }
#endif
  //------ albEnd ----------------------------------

  fPlaybackSyncMode = *pLS->pfPlaybackSyncMode;

  fQuantizeMode = *pLS->pfQuantMode;

  fMuteQuantized = *pLS->pfMuteQuantized;
  fOverdubQuantized = *pLS->pfOverdubQuantized;
  bReplaceQuantized = pLS->bReplaceQuantized;
  fSyncOffsetSamples = *pLS->pfSyncOffsetSamples;

  bRoundIntegerTempo = *pLS->pfRoundIntegerTempo;

  eighthPerCycle = (unsigned int) *pLS->pfEighthPerCycle;

  fSafetyFeedback = (*pLS->pfUseSafetyFeedback != 0.0f) ? SAFETY_FEEDBACK : 1.0f;
  
  fTempo = *pLS->pfTempo;
  if (fTempo > 0.0f) {
	  eighthSamples = (unsigned int) (pLS->fSampleRate * 30.0 / fTempo);

	  if (fQuantizeMode == QUANT_CYCLE || fQuantizeMode == QUANT_LOOP) {
		  syncSamples = (unsigned int) (eighthSamples * eighthPerCycle / 2);
	  }
	  else if (fQuantizeMode == QUANT_8TH) {
		  syncSamples = (eighthSamples / 2);
	  }

  }

  //------ albBegin ----------------------------------
  // Seed lLastSyncPeriodSamples from the tempo-derived syncSamples whenever no
  // real inter-pulse measurement has been taken yet (sentinel == ULONG_MAX).
  // syncSamples is > 0 only when fTempo > 0 and the quantize mode is
  // QUANT_CYCLE, QUANT_LOOP, or QUANT_8TH; the guard handles all other cases
  // gracefully by leaving the sentinel unchanged.
  //
  // Once the second sync pulse arrives it overwrites this estimate with the
  // actual measured interval and the guard never fires again.  The net effect:
  // the fraction cap in effectiveLateHitWindow() is active from the very first
  // buffer, not only after two pulses have been observed.
  if (pLS->lLastSyncPeriodSamples == ULONG_MAX && syncSamples > 0) {
      pLS->lLastSyncPeriodSamples = syncSamples;
  }
  // C1 fix: when fTempo is known and sync is active, override
  // lLastSyncPeriodSamples with the authoritative tempo-derived value.
  // The pulse scanner (the lGlobalSamplesSinceSync loop further on) can be
  // polluted by loop-coordination 2.0f events that arrive in quick
  // succession at state transitions (recording
  // end, undo, etc.) and can leave lLastSyncPeriodSamples at a tiny value
  // that effectively disables the late-hit window for subsequent presses.
  // Computing directly from fTempo is immune to this and tracks live
  // tempo changes automatically (runs every buffer).
  //
  // Formula: eighthSamples * eighthPerCycle (full cycle period, matching
  // what the pulse scanner measures between cycle boundaries).  Note: NOT
  // divided by 2 — syncSamples uses /2 for quantize-period purposes, but
  // lLastSyncPeriodSamples must be the full cycle for effectiveLateHitWindow
  // to compute fractionLimit = g_late_hit_max_quant_unit_fraction * one_cycle.
  //
  // Gated on fSyncMode: completely inactive when sync is disabled.  This
  // means zero impact on JACK-mode usage without the sync checkbox.
  if (fSyncMode >= 1.0f && fTempo > 0.0f && eighthPerCycle > 0) {
      // The Quantize unit: one eighth with Quantize 8th, one cycle otherwise.
      const float unitEighths = (fQuantizeMode == QUANT_8TH) ? 1.0f : (float) eighthPerCycle;
      pLS->lLastSyncPeriodSamples =
          (unsigned long)(pLS->fSampleRate * 30.0f / fTempo * unitEighths);
  }
  //------ albEnd ----------------------------------

  lMultiCtrl = (int) *(pLS->pfMultiCtrl);

  useFeedbackPlay = (bool) *(pLS->pfUseFeedbackPlay);
  
  fTapTrig = *(pLS->pfTapCtrl);

  
  if (lMultiCtrl == pLS->lLastMultiCtrl)
  {
     // ignore it, we must have a change to trigger an action
     // and it must be the right 10s.
     lMultiCtrl = -1;
  }
  else {
          DBG(fprintf(stderr, "Multi change from %d to %d\n", pLS->lLastMultiCtrl, lMultiCtrl));
     pLS->lLastMultiCtrl = lMultiCtrl;
  }

  //------ albBegin ----------------------------------
  // RECORD_FAILSAFE -- auto-fire CANCEL_RECORD_MUTE if a record has been
  // left running in STATE_RECORD longer than g_record_failsafe_seconds,
  // instead of leaving it recording indefinitely because the performer
  // never got back to press anything.  See "New Feature: record
  // failsafe" in mod_docs/late_hit_patch_summary.rst.
  //
  // Computed here, as a plain statement pair ahead of the if/else-if chain
  // below, rather than inline at the CANCEL_RECORD_MUTE branch itself: that
  // chain is one long if/else-if sequence, and splicing a self-contained
  // if/else in the middle of it (as a first attempt at this did) breaks it
  // -- the next "else if" has nothing open to attach to.  bRecordFailsafeFired
  // is then just one more term ORed into that branch's existing condition.
  //
  // lRecordFailsafeSamples accumulates SampleCount once per buffer while
  // state is STATE_RECORD and resets to 0 the instant it is not -- so it
  // measures unbroken time in STATE_RECORD only, never leaking across
  // separate takes.  bRecordFailsafeFired can therefore only be true when
  // pLS->state == STATE_RECORD (the counter is 0 otherwise), so it always
  // lands in the CANCEL_RECORD_MUTE branch's STATE_RECORD case -- never its
  // STATE_TRIG_START or no-op cases.
  //
  // Specialized 2026-09-01: only accumulate when this STATE_RECORD span
  // interrupted an actively playing, unmuted loop (bRecordEnteredFromActivePlay,
  // set once at RECORD press time in the MULTI_RECORD case above -- see
  // plugin.hpp's comment on the field). A first/fresh recording from an
  // empty loop, or one started from STATE_MUTE/STATE_PAUSED, never
  // accumulates and so can never auto-cancel -- the failsafe now covers
  // only the case where getting distracted mid-record means an existing,
  // audible take stays silently replaced.
  if (pLS->state == STATE_RECORD) {
      if (pLS->bRecordEnteredFromActivePlay) {
          pLS->lRecordFailsafeSamples += SampleCount;
      }
  } else {
      pLS->lRecordFailsafeSamples = 0;
  }
  bool bRecordFailsafeFired =
      g_record_failsafe_seconds > 0.0f &&
      pLS->lRecordFailsafeSamples >
          (unsigned long)(g_record_failsafe_seconds * pLS->fSampleRate);
  //------ albEnd ----------------------------------


  // force use delay
  if (lMultiCtrl == MULTI_REDO && *pLS->pfRedoTapMode != 0)
  {
     useDelay = 1;
  }
  else if (lMultiCtrl == MULTI_MUTE_ON) {
	  if ((pLS->state != STATE_MUTE) && (pLS->state != STATE_OFF_MUTE)) {
		  // lets mute it
		  lMultiCtrl = MULTI_MUTE;
	  } else {
		  // already muted, do nothing
		  lMultiCtrl = -1;
	  }
  }
  else if (lMultiCtrl == MULTI_MUTE_OFF) {
	  if ((pLS->state == STATE_MUTE) || (pLS->state == STATE_OFF_MUTE)) {
		  // lets unmute it
		  lMultiCtrl = MULTI_MUTE;
	  } else {
		  // already not muted, do nothing
		  lMultiCtrl = -1;
	  }
  }
  else if (lMultiCtrl == MULTI_MUTE_TRIGGER) {
	  if (pLS->state != STATE_MUTE) {
		  // lets mute it
		  lMultiCtrl = MULTI_MUTE;
	  } else {
		  // already muted, Trigger
		  lMultiCtrl = MULTI_TRIGGER;
	  }
  }
  //------ albBegin ----------------------------------
  // CANCEL_RECORD_MUTE -- cancel an in-progress or pending record, keep the
  // prior take, and mute only if audio was actually captured.  No-op in every
  // other state.  See "New Command: cancel_record_mute" in
  // mod_docs/late_hit_patch_summary.rst.
  // The "|| bRecordFailsafeFired" below is the only change from that design:
  // a second, automatic way to reach this same branch.  See the
  // RECORD_FAILSAFE block just above.
  //
  // Implemented as a state-based remap onto MULTI_UNDO rather than as its own
  // dispatch case, so the existing (well-tested) undo path does all the work.
  // The only extra input is wasMuted, which MULTI_UNDO reads to decide where
  // to land: pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY.
  //
  // Deliberately does NOT route through mute_on.  This rig runs mute quantize
  // whenever sync is on, and a quantized mute is deferred to the next boundary
  // (see the MULTI_MUTE else-branch): it would convert STATE_RECORD into
  // STATE_TRIG_STOP, commit the take, and leave wasMuted false -- so a
  // following undo would land in STATE_PLAY, audible.  Setting wasMuted here
  // bypasses the mute state machine entirely and is immediate.
  else if (lMultiCtrl == MULTI_CANCEL_RECORD_MUTE || bRecordFailsafeFired) {
	  //------ albBegin ----------------------------------
	  // Only from play: ignore the command unless the RECORD press that
	  // started this record interrupted a playing loop (STATE_PLAY or
	  // STATE_ONESHOT, the bRecordEnteredFromActivePlay flag that also
	  // gates record_failsafe).  For a record started from Mute, Paused or
	  // an empty loop it is a no-op, and the record carries on.
	  //
	  // Why: the command is the long-press partner of an eager-single
	  // record button, so it can only ever reach the record its own press
	  // started, a few hundred ms in.  From a playing loop that is the
	  // hold-to-stop gesture: the press fires record, the hold takes it
	  // back and leaves the loop muted.  From silence there is nothing to
	  // stop, so the only way a hold happens is a lazy press -- and
	  // cancelling then throws away the take the performer meant to start.
	  //
	  // The failsafe path is excluded explicitly, although its counter
	  // already runs only when the same flag is set.  So a cancelled
	  // record always has a previous take under it, which the redo unlink
	  // below relies on.  Regression tests: testdef_cancel_record_mute.py.
	  const bool bCancelIgnored =
	      !bRecordFailsafeFired && !pLS->bRecordEnteredFromActivePlay;
	  //------ albEnd ----------------------------------
	  if (bCancelIgnored) {
		  // Record started from silence: the hold was a lazy press.
		  lMultiCtrl = -1;
	  }
	  else if (pLS->state == STATE_RECORD) {
		  // Committed record: audio was captured, so undo it and stay silent.
		  // Forcing wasMuted makes MULTI_UNDO take its STATE_MUTE branch
		  // (undoLoop with no crossfade back to play).
		  //
		  // Silence relies on the play fade already being at zero: entering
		  // STATE_RECORD sets fPlayFadeDelta = -1/xfadeSamples, and the undo
		  // path only *holds* the attenuation (fPlayFadeDelta = 0), it does
		  // not ramp down.  That holds because the long-press threshold is
		  // hundreds of ms against a crossfade of a few ms.  If the threshold
		  // is ever shortened to near the crossfade, set fPlayFadeDelta here
		  // explicitly instead of relying on the record entry.
		  //
		  // Safe to mutate wasMuted here: STATE_RECORD is in MULTI_UNDO's
		  // case list, so the undo that consumes it always runs.  Do not set
		  // it on any path where the remapped command might not execute.
		  pLS->wasMuted = true;

		  // Stop writing input into the loop buffer.  Entering STATE_RECORD
		  // ramped fLoopFadeAtten up to 1.0, and MULTI_UNDO's STATE_MUTE
		  // branch only touches fPlayFadeDelta -- it assumes the caller was
		  // already muted, with the write gain long since at zero.  Arriving
		  // there straight out of STATE_RECORD leaves the write gain pinned
		  // at 1.0, and plugin.cc:5262 (in the play block, whose case group
		  // at 5008-5017 includes STATE_MUTE) then runs
		  //     loop = loop * fFeedFadeAtten + fLoopFadeAtten * input
		  // every sample.  With both attenuations at 1.0 that is loop += input,
		  // an accumulator with no decay: the restored take runs away into
		  // gross clipping while muted, unnoticed until unmute.  Measured at
		  // -10.3 with an input of -0.9 before this was added.
		  //
		  // These mirror transitionToNext()'s STATE_MUTE case (~1852-59),
		  // which is the canonical "we are becoming muted" fade set.  Match
		  // it exactly rather than picking a subset: an earlier version set
		  // only fLoopFadeDelta/fFeedFadeDelta, which left the *Src* pair
		  // stale.  Those two do not affect the play/mute block, but they
		  // govern the identical accumulator on the source loop at 4464 /
		  // 4712 / 4897, which runs in OVERDUB / REPLACE / SUBSTITUTE /
		  // MULTIPLY / INSERT.  Entering any of those after a cancel would
		  // start writing input into the source loop at a stale gain.
		  //
		  // Set here rather than in the shared STATE_MUTE branch of
		  // MULTI_UNDO so no other undo caller changes behaviour.  Harmless
		  // in the no-prev sub-case, which reaches UNDO_ALL and sets the
		  // non-Src pair identically.
		  // fPlayFadeDelta is deliberately NOT set here: MULTI_UNDO's
		  // STATE_MUTE branch (~3620) assigns it 0.0f right after this,
		  // so setting it would be dead code.  The play fade is already at
		  // zero from the record entry, so holding it is correct.
		  // Regression test: testResidueAfterCancelOverExistingTake.
		  pLS->fLoopFadeDelta    = -1.0f / xfadeSamples;
		  pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
		  pLS->fFeedFadeDelta    =  1.0f / xfadeSamples;
		  pLS->fFeedSrcFadeDelta =  1.0f / xfadeSamples;

		  // Unlink the chunk about to be discarded, so a later redo cannot
		  // restore it.  Undo keeps each chunk's `next` for redo
		  // (popHeadLoop: "leave the next where is is for redo"), so
		  // without this a redo after the cancel would bring back the few
		  // hundred ms recorded before the long press, replacing the take
		  // -- silently if muted.  Nothing legitimate is lost: the record
		  // press already overwrote prev->next with this chunk, as any new
		  // take does.  Done before the undo, which finds the previous take
		  // through dead->prev and never reads prev->next.  The
		  // pending-record cancel below creates no chunk and is untouched.
		  // Regression tests: cancelThenRedo in testdef_cancel_record_mute.py.
		  {
			  LoopChunk *dead = pLS->headLoopChunk;
			  if (dead && dead->prev && dead->prev->next == dead) {
				  dead->prev->next = NULL;
			  }
		  }

		  lMultiCtrl = MULTI_UNDO;
	  }
	  else if (pLS->state == STATE_TRIG_START) {
		  // Pending record, nothing captured yet: drop the pending request
		  // and carry on.  No mute -- there is no blip to hide.
		  //
		  // Cancelled in place rather than remapped to MULTI_UNDO.  The
		  // late-hit path creates a fresh chunk when it starts recording
		  // immediately (pushNewLoopChunk, ~2697), but the normal pending
		  // path does not -- the TRIG_START fallback (~2808) only sets the
		  // state.  So here headLoopChunk is still the *existing* take with
		  // prev == NULL, and MULTI_UNDO would pop that take and fall
		  // through to UNDO_ALL, i.e. destroy exactly what this command
		  // exists to protect.  Measured before this was corrected: a
		  // recorded take plus a pending record plus cancel left the looper
		  // Off with an empty loop.
		  //
		  // Keyed on STATE_TRIG_START, NOT on waitingForSync: that flag is
		  // also set for deferred mute and play transitions, which have
		  // nothing to do with recording.
		  //
		  // Regression test: testPendingCancelKeepsExistingTake.
		  pLS->waitingForSync = 0;
		  pLS->nextState = -1;
		  if (pLS->headLoopChunk && pLS->headLoopChunk->lLoopLength) {
			  pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY;
		  } else {
			  pLS->state = pLS->wasMuted ? STATE_OFF_MUTE : STATE_OFF;
		  }
		  lMultiCtrl = -1;   // handled here; nothing left to dispatch
	  }
	  else {
		  // Everything else is a no-op, and that is the point of the command.
		  // In particular STATE_TRIG_STOP -- a quantized record *end* waiting
		  // on its boundary -- must not be cancelled: the take is being kept.
		  lMultiCtrl = -1;
	  }
  }
  //------ albEnd ----------------------------------
  //------ albBegin ----------------------------------
  // CANCEL_OVERDUB_UNDO -- the overdub button's long/double-press companion:
  // undo, as if the eager overdub press just before it had not happened.
  // Acts only when the command immediately before it was an overdub press
  // (lLastOverdubFrom, set at the start of every dispatch below), and never
  // removes a loop's first take -- only layers on top of it.  A layer the
  // eager press started is discarded and unlinked (no redo); a real
  // overdub that gets undone stays redo-able, as with plain undo.  The cases are
  // in mod_docs/late_hit_patch_summary.rst, "cancel_overdub_undo Implementation".
  else if (lMultiCtrl == MULTI_CANCEL_OVERDUB_UNDO) {
	  const long from = pLS->lLastOverdubFrom;
	  LoopChunk *head = pLS->headLoopChunk;
	  pLS->lLastOverdubFrom = -1;
	  lMultiCtrl = -1;                       // no-op unless a case applies

	  if (from == STATE_OVERDUB) {
		  // The eager press was a stop.
		  if (pLS->state == STATE_OVERDUB && pLS->waitingForSync
		      && pLS->nextState == STATE_PLAY) {
			  // Stop still queued: cancel it.  The undo then discards
			  // the running overdub's layer, as plain undo does while
			  // overdubbing.
			  pLS->waitingForSync = 0;
			  pLS->nextState = -1;
			  lMultiCtrl = MULTI_UNDO;
		  }
		  else if (pLS->state == STATE_PLAY && head && head->prev) {
			  // Stopped at once: undo removes the layer just committed.
			  lMultiCtrl = MULTI_UNDO;
		  }
	  }
	  else if (from == STATE_PLAY) {
		  // The eager press was a start, from playing.
		  if (pLS->state == STATE_OVERDUB && !pLS->waitingForSync
		      && head && head->prev) {
			  discardEagerOverdubLayer(pLS);
			  pLS->state = STATE_PLAY;
			  pLS->wasMuted = false;
			  if (pLS->headLoopChunk && pLS->headLoopChunk->prev) {
				  lMultiCtrl = MULTI_UNDO;   // and undo the layer under it
			  }
		  }
		  else if (pLS->state == STATE_PLAY && pLS->waitingForSync
			   && pLS->nextState == STATE_OVERDUB) {
			  // Start still queued: drop it, then undo the last layer.
			  pLS->waitingForSync = 0;
			  pLS->nextState = -1;
			  if (head && head->prev) {
				  lMultiCtrl = MULTI_UNDO;
			  }
		  }
	  }
	  else if (from == STATE_MUTE) {
		  // The eager press was a start, from muted (the overdub made the
		  // loop audible).  Handled here rather than through MULTI_UNDO,
		  // whose muted branch only holds the play level: back to muted
		  // needs an explicit fade-out.
		  if (pLS->state == STATE_OVERDUB && !pLS->waitingForSync
		      && head && head->prev) {
			  discardEagerOverdubLayer(pLS);
			  if (pLS->headLoopChunk && pLS->headLoopChunk->prev) {
				  undoLoop(pLS, false);      // the layer under it; redo-able
			  }
			  pLS->state = STATE_MUTE;
			  pLS->wasMuted = true;
			  pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
		  }
		  else if (pLS->state == STATE_MUTE && pLS->waitingForSync
			   && pLS->nextState == STATE_OVERDUB) {
			  pLS->waitingForSync = 0;
			  pLS->nextState = -1;
			  if (head && head->prev) {
				  lMultiCtrl = MULTI_UNDO;   // silent: already muted
			  }
		  }
	  }
	  else if (from == STATE_RECORD || from == STATE_TRIG_STOP) {
		  // The eager press ended a record into an overdub: drop the
		  // overdub, keep the take.
		  if (pLS->state == STATE_OVERDUB && !pLS->waitingForSync
		      && head && head->prev) {
			  discardEagerOverdubLayer(pLS);
			  pLS->state = STATE_PLAY;
			  pLS->wasMuted = false;
		  }
		  else if (pLS->state == STATE_TRIG_STOP && pLS->waitingForSync
			   && pLS->nextState == STATE_OVERDUB) {
			  pLS->nextState = STATE_PLAY;   // the record ends as usual
		  }
	  }
	  // Anything else -- another origin, or the state has moved on in a
	  // way these cases do not cover -- is left alone.
  }
  //------ albEnd ----------------------------------
  else if (lMultiCtrl == MULTI_RECORD_OR_OVERDUB) {
	  if (!pLS->headLoopChunk || pLS->state == STATE_OFF || pLS->state == STATE_OFF_MUTE || pLS->state == STATE_RECORD 
	      || pLS->state == STATE_TRIG_START || pLS->state == STATE_TRIG_STOP 
	      || pLS->state == STATE_DELAY) {
		  // we record
		  lMultiCtrl = MULTI_RECORD;
	  } else {
		  // otherwise overdub
		  lMultiCtrl = MULTI_OVERDUB;
	  }
  }
  else if (lMultiCtrl == MULTI_RECORD_OVERDUB_END) {
	  if (!pLS->headLoopChunk || pLS->state == STATE_OFF || pLS->state == STATE_OFF_MUTE
	      || pLS->state == STATE_DELAY) {
		  // we record
		  lMultiCtrl = MULTI_RECORD;
	  } else {
		  // otherwise overdub
		  lMultiCtrl = MULTI_OVERDUB;
	  }
  }
  else if (lMultiCtrl == MULTI_PAUSE_ON) {
	  if (pLS->state != STATE_PAUSED) {
		  // lets pause it
		  lMultiCtrl = MULTI_PAUSE;
	  } else {
		  // already paused, do nothing
		  lMultiCtrl = -1;
	  }
  }
  else if (lMultiCtrl == MULTI_PAUSE_OFF) {
	  if (pLS->state == STATE_PAUSED) {
		  // lets unpause it
		  lMultiCtrl = MULTI_PAUSE;
	  } else {
		  // already not paused, do nothing
		  lMultiCtrl = -1;
	  }
  }
  
  if (fTapTrig == pLS->fLastTapCtrl) {
     // ignore it, we must have a change to trigger a tap
     
  } else if (pLS->lTapTrigSamples >= TRIG_SETTLE) {
     // signal to below to trigger the delay tap command
     if (pLS->bPreTap) {
	// ignore the first time
	pLS->bPreTap = 0;
     }
     else {
	DBG(fprintf(stderr, "%u:%u  Tap triggered\n", pLS->lLoopIndex, pLS->lChannelIndex));
	lMultiCtrl = MULTI_REDO;
	useDelay = 1;
     }
  }
  pLS->fLastTapCtrl = fTapTrig;
 
  //fRateSwitch = *(pLS->pfRateSwitch);


  if (pLS->pfScratchPos) {
	  scratchTarget = LIMIT_BETWEEN_0_AND_1(*(pLS->pfScratchPos));
	  fScratchPos =  LIMIT_BETWEEN_0_AND_1(pLS->fScratchPosCurr);
	  scratchDelta = (scratchTarget - fScratchPos) / MAX(1, (SampleCount - 1));
  }
  
  // the rate switch is ON if it is below 1 but not 0
  // rate is 1 if rate switch is off
  //if (fRateSwitch > 1.0 || fRateSwitch==0.0) {
  //  fRate = 1.0;
  //}
  //else {
     //fprintf(stderr, "rateswitch is 1.0: %f!\n", fRate);
  //}

  wetTarget = LIMIT_BETWEEN_0_AND_1(*(pLS->pfWet));
  fWet =  LIMIT_BETWEEN_0_AND_1(pLS->fWetCurr);
  wetDelta = (wetTarget - fWet) / MAX(1, (SampleCount - 1));
  
// 	  wetTarget += wetDelta;
// 	  fWet = fWet * 0.1f + wetTarget * 0.9f;
  
  dryTarget = LIMIT_BETWEEN_0_AND_1(*(pLS->pfDry));
  fDry =  LIMIT_BETWEEN_0_AND_1(pLS->fDryCurr);
  dryDelta = (dryTarget - fDry) / MAX(1, (SampleCount - 1));

  feedbackTarget = LIMIT_BETWEEN_0_AND_1(*(pLS->pfFeedback));
  fFeedback =  LIMIT_BETWEEN_0_AND_1(pLS->fFeedbackCurr);
  feedbackDelta = (feedbackTarget - fFeedback) / MAX(1, (SampleCount - 1));
  
  // probably against the rules, but I'm doing it anyway
  *pLS->pfFeedback = feedbackTarget;


  loop = pLS->headLoopChunk;

  // clear sync out
  if (fSyncMode == 0.0f || pfSyncInput != pfSyncOutput) {
	  memset(pfSyncOutput, 0, SampleCount * sizeof(LADSPA_Data));
  }
  

  unsigned long lInputLatency = (unsigned long) (*pLS->pfInputLatency);
  unsigned long lOutputLatency = (unsigned long) (*pLS->pfOutputLatency);

  
  // copy input signal to input latency buffer
  unsigned long lbuf_wpos = pLS->lInputBufWritePos;
  for (unsigned long n=0; n < SampleCount; ++n) {
	  pfInputLatencyBuf[lbuf_wpos]  = pfInput[n];
	  lbuf_wpos = (lbuf_wpos+1) & pLS->lInputBufMask;
  }

  // calculate initial offset for reading for input

  unsigned long lInputReadPos = (lInputLatency <= pLS->lInputBufWritePos)
	  ? (pLS->lInputBufWritePos - lInputLatency)
	  : (pLS->lInputBufSize - (lInputLatency - pLS->lInputBufWritePos)) ;
  
	  
  //------ albBegin ----------------------------------
  // Global sync counter: lGlobalSamplesSinceSync increments every sample in
  // ALL states (including STATE_OFF and STATE_TRIG_START) and resets on each
  // incoming sync pulse.  Unlike the existing lSamplesSinceSync, it is
  // therefore valid to read at button-press time in MULTI_RECORD.
  //
  // lGlobalSyncAtButtonPress is a snapshot taken BEFORE scanning the current
  // buffer.  LADSPA control events (button presses) are processed at buffer
  // boundaries, so the button conceptually arrived at the start of this buffer,
  // before any sync pulses it may contain.  Using the pre-scan snapshot avoids
  // a bug where a mid-buffer sync pulse would reset the counter and make a
  // genuinely-late press appear to be on-time (or vice versa).
  // C5 fix: widened from unsigned int to avoid 32-bit wrap-around at ~25h
  // (2^32 samples / 48000 Hz = 89478 s).  lGlobalSyncAtButtonPress must
  // match so T comparisons remain consistent.
  unsigned long long lGlobalSyncAtButtonPress = pLS->lGlobalSamplesSinceSync;

  if (fSyncMode >= 1.0f) {
	  // Sanity floor for period capture (C1 defence-in-depth).
	  // Reject inter-pulse intervals shorter than one 32nd note at 300 BPM
	  // (~1200 samples at 48 kHz).  Pulses arriving faster than this are
	  // almost certainly loop-coordination 2.0f events at state transitions
	  // (recording end, undo, etc.), not true tempo-clock boundaries.  The
	  // floor prevents those from corrupting lLastSyncPeriodSamples.
	  // When fTempo > 0 the tempo-derived value (C1 fix, above) is set again
	  // at the start of the next buffer, replacing anything written here, so
	  // the floor only matters when fTempo == 0.  (Within this buffer a
	  // press still sees the pulse-measured value: review item C1a, open.)
	  const unsigned long lMinPlausiblePeriod =
	      (unsigned long)(pLS->fSampleRate / 40.0f);
	  for (unsigned long n = 0; n < SampleCount; ++n) {
		  pLS->lGlobalSamplesSinceSync++;
		  if (pfSyncInput[n] > 1.5f) {
			  if (pLS->lGlobalSamplesSinceSync >= lMinPlausiblePeriod) {
				  pLS->lLastSyncPeriodSamples = pLS->lGlobalSamplesSinceSync;
			  }
			  pLS->lGlobalSamplesSinceSync = 0; // always reset on any pulse
			  pLS->bSyncPulseReceived = true;   // always set on any pulse
		  }
	  }
  }
  //------ albEnd ----------------------------------

  // transitions due to control triggering
  
  if (lMultiCtrl >= 0 && lMultiCtrl <= 127)
  {
	  //------ albBegin ----------------------------------
	  // CANCEL_OVERDUB_UNDO bookkeeping: remember whether this command is
	  // an overdub press, and the state it found (before the dispatch below
	  // changes it).  Any other command clears it, so cancel_overdub_undo
	  // only ever acts on an overdub press immediately before it.  Placed
	  // after the remaps above, so record_or_overdub resolving to an
	  // overdub counts too.
	  pLS->lLastOverdubFrom = (lMultiCtrl == MULTI_OVERDUB) ? pLS->state : -1;
#if TRIGGER_LATENCY_ALL_EDGES
	  // A new command clears a pending deferred edge; the site that wants
	  // one sets it again below.
	  pLS->lEdgeDelay = 0;
	  pLS->lEdgeEarly = 0;
	  pLS->lEdgeTrim = 0;
#if FREE_PREROLL
	  pLS->lFreePrerollNext = 0;
#endif
	  pLS->bEdgeStopDeferred = false;
	  pLS->fEdgeStopPulse = 0.0f;
	  pLS->bEdgeEndDeferred = false;
	  pLS->lEdgeEndNext = STATE_PLAY;
	  pLS->bEdgeStopReverse = false;
	  pLS->fEdgeEndPulse = 0.0f;
	  pLS->bEdgeStopScratch = false;
#endif
	  //------ albEnd ----------------------------------
          DBG(fprintf(stderr, "Multictrl val is %ld\n", lMultiCtrl));

     //lMultiCtrl = lMultiCtrl;

     // change the value if necessary

     if (lMultiCtrl==MULTI_REDO && useDelay) {
	lMultiCtrl = MULTI_DELAY;
     }
     
     switch(lMultiCtrl)
     {
	case MULTI_RECORD:
	{
	   //------ albBegin ----------------------------------
	   // Capture whether this press is interrupting an actively playing,
	   // unmuted loop -- BEFORE the switch below changes pLS->state to
	   // STATE_RECORD or STATE_TRIG_START.  Read later by the
	   // record_failsafe counter ahead of the remap chain.
	   // See plugin.hpp's comment on the field.
	   pLS->bRecordEnteredFromActivePlay =
	       (pLS->state == STATE_PLAY || pLS->state == STATE_ONESHOT);
	   //------ albEnd ----------------------------------

	   switch(pLS->state) {
	      case STATE_RECORD:
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Unsynced stop: end input_latency - trigger_latency after the
		      // command, on the press, through STATE_TRIG_STOP.  With Sync
		      // off it keeps recording and ends on the sample its countdown
		      // reaches 0, with the same transition to Play and playback
		      // offset as the immediate stop below.  At 0 the stock branch
		      // runs.
		      if (fSyncMode == 0.0f && (fTrigThresh==0.0f) && !bRoundIntegerTempo
		          && loop && takeEdgeDelaySamples(pLS, takePreroll(loop)) > 0) {
			      // (FREE_PREROLL: the take's pre-roll counts as command
			      // latency, here and in the trim below.)
			      pLS->state = STATE_TRIG_STOP;
			      pLS->nextState = STATE_PLAY;
			      pLS->lEdgeDelay = takeEdgeDelaySamples(pLS, takePreroll(loop));
			      pLS->bEdgeStopDeferred = true;
			      pLS->fEdgeStopPulse = 2.0f;
			      DBG(fprintf(stderr,"%u:%u  Record stop deferred %ld samples\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lEdgeDelay));
			      break;
		      }
		      // The other way (trigger_latency > input_latency, increment
		      // (c)): the press was edgeAdvanceSamples() ago in the input,
		      // so end the take that much back (the samples past it fade
		      // into the loop's start), then stop as below: the position
		      // carries on that much past the new end, so playback stays
		      // in phase.
		      if (fSyncMode == 0.0f && (fTrigThresh==0.0f) && !bRoundIntegerTempo
		          && loop && takeTrimWanted(pLS, loop)) {
			      loop->lLoopLength = (unsigned long) loop->dCurrPos;
			      pLS->fLoopFadeAtten = endTakeTrimmed(pLS, loop,
				      (unsigned long) takeEdgeAdvanceSamples(pLS, takePreroll(loop)),
				      xfadeSamples);
		      }
#endif
		      //------ albEnd ----------------------------------

		      if (fSyncMode == 0.0f && (fTrigThresh==0.0f) && !bRoundIntegerTempo) {
			      // no sync: stop immediately
			      pLS->state = STATE_PLAY;
			      pLS->wasMuted = false;
			      pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
			      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
			      DBG(fprintf(stderr,"%u:%u  from rec Entering PLAY state loop len: %lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lLoopLength));

			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 2.0f;
			      
			      if (loop) {
				      // we need to increment loop position by output latency (+ IL ?)
				      loop->dCurrPos = fmod(loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate, loop->lLoopLength);
				      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
				      DBG(fprintf(stderr,"%u:%u  from rec Entering PLAY state at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));
			      }

		      }
		      else {
			      //------ albBegin ----------------------------------
			      // STOCK (sooperlooper 1.7.9) — original else branch for sync-mode stop:
			      //   } else {
			      //     pLS->state = STATE_TRIG_STOP;
			      //     pLS->nextState = STATE_PLAY;
			      //     DBG(fprintf(stderr,"%u:%u  Entering TRIG_STOP state\n", ...));
			      //   }
			      // CHANGE: else branch is now wrapped in a late-hit window check.
			      // STATE_TRIG_STOP path is preserved as the out-of-window fallback.
			      //
			      // Late-stop window check: when RECORD is pressed during STATE_RECORD
			      // (i.e. to stop), check if stop arrived within LATE_HIT_WINDOW_MS
			      // to anchor the loop end at the previous boundary (discarding the
			      // overrun audio), then go directly to STATE_PLAY with dCurrPos set
			      // identically to the normal STATE_TRIG_STOP path (lLoopLength + latency).
			      // The trim alone corrects the loop length; no first-play offset needed.
			      // check late-hit window for stop button
			      unsigned long shiftSamples = effectiveLateHitWindow(pLS, eighthPerCycle);
			      unsigned long T = lGlobalSyncAtButtonPress;
			      // fSyncMode == 1 is the essential guard: the loop's Sync is on
			      // and pfSyncInput carries the source's pulses, so
			      // lGlobalSamplesSinceSync (T) has a meaningful value.  The
			      // source only produces pulses with Quantize Cycle or 8th (JACK
			      // and Internal give none for Off or Loop; MIDI clock gives one
			      // per beat), so with no pulse arriving T grows without bound
			      // and the window never fires.
			      // Rel Sync fix: fSyncMode == 1 only.  Under Rel Sync (2) the
			      // stop waits for the recorded phase offset, not the pulse, so
			      // trimming to the pulse gave a loop that was not a whole number
			      // of cycles.
			      // C2 fix: bLateHitBypassed prevents late-hit firing on slave
			      // channels in internal sync mode; it is set in Looper::run().
			      bool stopWithinWindow = (T > 0
			                               && T <= shiftSamples
			                               && fSyncMode == 1.0f
			                               && pLS->bSyncPulseReceived
			                               && loop != NULL
			                               && !pLS->bLateHitBypassed);
			      // Late enough for the settings, but the window was limited
			      // to the input history.
			      if (!stopWithinWindow && T > shiftSamples && T <= lateHitWindowWanted (pLS)
			          && fSyncMode == 1.0f && pLS->bSyncPulseReceived && loop != NULL
			          && !pLS->bLateHitBypassed)
				      noteHistoryShort (pLS, T + 1);

			      SYNCWDBG(SL_LOG_RT(
					  "\n%u:%u  STOP button: lGlobalSyncAtButtonPress=%llu  windowSamples=%lu"
			          "  withinWindow=%d  (%.2f ms since boundary, window=%.1f ms)  dCurrPos=%g\n",
			          pLS->lLoopIndex, pLS->lChannelIndex,
			          lGlobalSyncAtButtonPress, shiftSamples,
			          (int)stopWithinWindow,
			          T * 1000.0f / pLS->fSampleRate,
			          shiftSamples * 1000.0f / pLS->fSampleRate,
					  loop ? loop->dCurrPos : -1.0));

			      if (stopWithinWindow) {
				      // trim from the loop end and finalize immediately.
				      // T reads one less than the samples since the pulse sample
				      // (the counter resets after that sample's own increment), and a
				      // normal stop ends the loop just before the pulse sample, so the
				      // trim is T + 1.  Trimming T left the loop one sample long.
				      //------ albBegin ----------------------------------
#if SYNC_PREROLL
				      // ... and P more after a pre-rolled start.
				      const unsigned long lateSamples = T + 1 + loop->lStartEarly;
#else
				      const unsigned long lateSamples = T + 1;
#endif
				      //------ albEnd ----------------------------------
				      // guard against underflow — require at least xfadeSamples.
				      unsigned long trimmedLength = 0;
				      if ((unsigned long)loop->dCurrPos > lateSamples + (unsigned long)xfadeSamples)
					      trimmedLength = (unsigned long)(loop->dCurrPos - lateSamples);
				      else {
					      // too close to trim safely — fall through to normal path
					      pLS->state = STATE_TRIG_STOP;
					      pLS->nextState = STATE_PLAY;
					      SYNCWDBG(SL_LOG_RT(
					          "%u:%u  Late-stop: T=%lu too large to trim safely"
					          " (dCurrPos=%g), using TRIG_STOP\n",
					          pLS->lLoopIndex, pLS->lChannelIndex,
					          T, loop->dCurrPos));
					      break;
				      }

				      loop->lLoopLength  = trimmedLength;
				      loop->lCycleLength = trimmedLength;
				      loop->lCycles = 1;

				      SYNCWDBG(SL_LOG_RT(
				          "%u:%u  Late-stop: trimmed %lu samples (%.2f ms),"
				          " lLoopLength=%lu\n",
				          pLS->lLoopIndex, pLS->lChannelIndex,
				          T, T * 1000.0f / pLS->fSampleRate,
				          loop->lLoopLength));

				      // The seam crossfade.  After a normal stop, STATE_PLAY writes
				      // the input that follows the pulse into the loop's start
				      // (through rCurrPos), fading it out: index i gets gain
				      // 1 - (i + 1) / xfadeSamples.  Here the first lateSamples of
				      // that input are already in the buffer just past the trimmed
				      // end, so fade them in now, and let STATE_PLAY carry the fade
				      // on from index lateSamples (its first rCurrPos below).
				      // Without this the fade-out started at full level at index
				      // lateSamples, laid over the take, and the seam had none.
				      for (unsigned long i = 0; i < lateSamples && i + 1 < (unsigned long) xfadeSamples; ++i) {
					      const float fGain = 1.0f - (float) (i + 1) / xfadeSamples;
					      pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask] +=
						      fGain * pLS->pSampleBuf[(loop->lLoopStart + trimmedLength + i) & pLS->lBufferSizeMask];
				      }

				      pLS->state = STATE_PLAY;
				      pLS->wasMuted = false;
				      pLS->fLoopFadeAtten   = LIMIT_BETWEEN_0_AND_1 (1.0f - (float) lateSamples / xfadeSamples);
				      pLS->fLoopFadeDelta   = -1.0f / xfadeSamples;
				      pLS->fPlayFadeDelta   =  1.0f / xfadeSamples;
				      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
				      // The feedback fades as the normal stop sets them
				      // (transitionToNext(STATE_PLAY)).  This used to set
				      // fFeedSrcFadeDelta to -1, the record start's value, and
				      // leave fFeedFadeDelta alone: the source-loop fade went to
				      // 0 and stayed there, and the next Multiply, which rewrites
				      // the source loop through it, wiped the take
				      // (testdef_multiply_after_late_stop.py).
				      pLS->fFeedFadeDelta    =  1.0f / xfadeSamples;
				      pLS->fFeedSrcFadeDelta =  1.0f / xfadeSamples;

				      pfSyncOutput[0] = 2.0f;

				      // mirror STATE_TRIG_STOP latency compensation exactly,
				      // then advance by lateSamples to compensate for entering
				      // STATE_PLAY that many samples after the pulse sample, where a
				      // normal stop enters it.  This shifts the first play ahead in
				      // content, exactly cancelling the late real-time entry.  After
				      // the first natural wrap, position 0 plays at exactly the same
				      // real time as a normal on-time loop; no rewind needed.
				      loop->dCurrPos = (double)loop->lLoopLength;
				      loop->dCurrPos += (lOutputLatency + lInputLatency) * fRate;
				      loop->dCurrPos += (double)lateSamples;
				      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;

			      } else {
				      pLS->state = STATE_TRIG_STOP;
				      pLS->nextState = STATE_PLAY;
				      DBG(fprintf(stderr,"%u:%u  Entering TRIG_STOP state\n", pLS->lLoopIndex, pLS->lChannelIndex));
			      }
      		      //------ albEnd ----------------------------------
		      }
		      break;

	      case STATE_MULTIPLY:
		 // special ending for multiply
		 // the loop ends immediately and the cycle length changes
		 // and #cycles becomes 1
		 if (loop) {
		    loop->backfill = 0;
		    loop->lLoopLength = (unsigned long) (loop->dCurrPos - loop->lStartAdj);
		    loop->lCycleLength = loop->lLoopLength;
		    loop->lCycles = 1;

		    pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		    pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
		    pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		    pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
		    pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
			      
		    pLS->state = STATE_PLAY;
		    pLS->wasMuted = false;
		    DBG(fprintf(stderr,"%u:%u  Entering PLAY state after Multiply NEW loop\n", pLS->lLoopIndex, pLS->lChannelIndex));

		    // we need to increment loop position by output latency (+ IL ?)
		    loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
		    pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		    
		 }
		 break;

	      case STATE_INSERT:
		 // special ending for insert
		 // the loop ends immediately and the cycle length changes
		 // and #cycles becomes 1
		 if (loop) {
		    // how much loop was remaining when the insert started
		    loop->lEndAdj = loop->lRemLen;
		    loop->lLoopLength = (unsigned long) (loop->dCurrPos + loop->lRemLen);

		    loop->backfill = 1;
		    loop->lMarkEndL = (unsigned long) loop->dCurrPos;
		    loop->lMarkEndH = loop->lLoopLength - 1;
		    loop->lCycleLength = loop->lLoopLength;
		    loop->lCycles = 1;

		    pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		    pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
		    pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		    pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
		    pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
			      
		    pLS->state = STATE_PLAY;
		    pLS->wasMuted = false;
		    DBG(fprintf(stderr,"%u:%u  Entering PLAY state after Multiply NEW loop\n", pLS->lLoopIndex, pLS->lChannelIndex));

		    // we need to increment loop position by output latency (+ IL ?)
		    loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
		    pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		    
		 }
		 break;
		 
	      case STATE_DELAY:
		 // goes back to play mode
		 // pop the delay loop off....
		 if (loop) {
		    undoLoop(pLS, false);
		 }
		 // fall through to default for late-hit / normal handling
		 
	      default:
		 //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		 // Record pressed in a layer, Sync off, no threshold (where
		 // increment 1 starts the take on the press): keep the layer
		 // going until then, then start the take at once (2b-iii).
		 if (fSyncMode == 0.0f && fTrigThresh == 0.0f
		     && deferLayerEnd(pLS, loop, STATE_TRIG_START, 0.0f))
			 break;
#endif
		 //------ albEnd ----------------------------------
		 //------ albBegin ----------------------------------
		 // STOCK (sooperlooper 1.7.9) — original default case in MULTI_RECORD state switch:
		 //   default:
		 //     pLS->state = STATE_TRIG_START;
		 //     DBG(fprintf(stderr,"%u:%u  Entering TRIG_START state\n", ...));
		 // CHANGE: default case now first checks the late-hit window.  On a within-
		 // window press the loop is backfilled and recording starts immediately.
		 // STATE_TRIG_START remains the fallback for out-of-window presses,
		 // preserving stock behaviour for normal (on-time) presses.
		 // NOTE: the { on the next line adds the scope that the late-start
		 // block below requires for its local variables.  Stock default: had no surrounding brace.
		 {
		 // Late-start window check: when RECORD is first pressed (from any state
		 // other than STATE_RECORD/STATE_MULTIPLY/etc.), check if the press arrived
		 // within LATE_HIT_WINDOW_MS after the last sync boundary.
		 // If yes: create the loop immediately, backfill T samples from the
		 // continuous input buffer into [0..T) of the loop buffer so the loop is
		 // anchored at the boundary, and enter STATE_RECORD with dCurrPos=T.
		 // Backfill alone corrects the start; no per-play offset is needed.
		 // If no: use normal STATE_TRIG_START and wait for the next boundary.
		 // Check whether the button arrived within the late-hit window after
		 // the last sync boundary.  If so, skip STATE_TRIG_START (which would
		 // wait for the NEXT boundary) and begin recording immediately, backfilling
		 // the missed samples from pInputBuf so the loop is anchored at the boundary.
		 unsigned long shiftSamples = effectiveLateHitWindow(pLS, eighthPerCycle);
		 unsigned long T = lGlobalSyncAtButtonPress;
		 // fSyncMode == 1 is the essential guard: the loop's Sync is on and
		 // the source's pulses arrive.  The source only produces pulses with
		 // Quantize Cycle or 8th (JACK and Internal give none for Off or
		 // Loop; MIDI clock gives one per beat); with no pulse T grows
		 // without bound and the window never fires.
		 // Rel Sync fix: fSyncMode == 1 only.  Under Rel Sync (2) a record
		 // starts where it is pressed and its offset from the pulse is the
		 // point; moving the start back to the pulse would defeat that.
		 // C2 fix: bLateHitBypassed prevents late-hit firing on slave
		 // channels in internal sync mode; it is set in Looper::run().
		 // The backfill reads T + 1 samples back from this block in the
		 // input ring, which this block has already partly overwritten.
		 const bool lateCanFire = (T > 0
		                           && fSyncMode == 1.0f
		                           && pLS->bSyncPulseReceived
		                           && !pLS->bLateHitBypassed);
		 bool withinWindow = (lateCanFire
		                      && T <= shiftSamples
		                      && T + 1 + SampleCount <= pLS->lInputBufSize);
		 // Late enough for the settings, but the input history is too short.
		 if (lateCanFire && !withinWindow && T <= lateHitWindowWanted (pLS))
			 noteHistoryShort (pLS, T + 1 + SampleCount);

		 SYNCWDBG(SL_LOG_RT(
		     "\n%u:%u  RECORD button: lGlobalSyncAtButtonPress=%llu  windowSamples=%lu"
		     "  withinWindow=%d  syncMode=%g  quantMode=%g"
		     "  (%.2f ms since boundary, window=%.1f ms)\n",
		     pLS->lLoopIndex, pLS->lChannelIndex,
		     lGlobalSyncAtButtonPress, shiftSamples,
		     (int)withinWindow, fSyncMode, fQuantizeMode,
		     T * 1000.0f / pLS->fSampleRate,
		     shiftSamples * 1000.0f / pLS->fSampleRate));

		 if (withinWindow) {
			 loop = pushNewLoopChunk(pLS, 0, NULL);
			 if (loop) {
				 SYNCWDBG(SL_LOG_RT(
				     "%u:%u  Late-hit path: backfilling %lu samples (%.2f ms),"
				     " lLoopStart=%lu, entering RECORD directly\n",
				     pLS->lLoopIndex, pLS->lChannelIndex,
				     T, T * 1000.0f / pLS->fSampleRate,
				     loop->lLoopStart));
				 DBG(fprintf(stderr, "%u:%u  Late-hit record: backfilling %lu samples, entering RECORD\n",
				             pLS->lLoopIndex, pLS->lChannelIndex, T));
				 pLS->state = STATE_RECORD;
				 fRate = pLS->fCurrRate = 1.0f;
				 loop->lLoopLength = 0;
				 loop->lStartAdj = 0;
				 loop->lEndAdj = 0;
				 loop->dCurrPos = 0.0;
				 loop->firsttime = 0;
				 loop->lMarkL = loop->lMarkEndL = LONG_MAX;
				 loop->frontfill = loop->backfill = 0;
				 loop->lCycles = 1;
				 loop->srcloop = NULL;
				 pLS->nextState = -1;
				 loop->dOrigFeedback = fFeedback;
				 loop->lSyncOffset = 0;
				 loop->lOrigSyncPos = loop->lSyncPos = 0;

				 // Backfill T samples from the input latency buffer into the loop
				 // buffer.  lInputBufWritePos points to the START of the current
				 // block (the current block has been written to the latency buffer
				 // already, but lInputBufWritePos is not advanced until end of run).
				 //
				 // STATE_RECORD writes pfInput[] directly to the loop buffer; it
				 // does NOT read from pfInputLatencyBuf.  The latency buffer is
				 // simply a verbatim ring-buffer copy of pfInput:
				 //
				 //   pfInputLatencyBuf[(lInputBufWritePos + m) & mask]
				 //       == physical audio at JACK time (T_now - lInputLatency + m)
				 //
				 // The pulse sample is M = T + 1 samples before this block: the
				 // counter is reset after the pulse sample's own increment, so it
				 // reads one less than the samples since then.  A normal start
				 // records the pulse sample first (STATE_TRIG_START enters RECORD
				 // on it), so the backfill starts there too.  (Backfilling T
				 // started the loop one sample late and one sample short.)
				 //
				 // Normal recording from the sync boundary (T_sync = T_now - M)
				 // would have written:
				 //
				 //   loop[i] = pfInput at T_sync + i
				 //           = physical audio at T_sync - lInputLatency + i
				 //           = physical audio at T_now  - lInputLatency + (i - M)
				 //           = pfInputLatencyBuf[lInputBufWritePos + (i - M)]
				 //           = pfInputLatencyBuf[lInputBufWritePos - M + i]
				 //
				 // Therefore:  readPos = lInputBufWritePos - M   (no lInputLatency term)
				 //
				 // Continuity check: the last backfill sample (i = M-1) reads from
				 // lInputBufWritePos - 1, which is pfInput[SampleCount-1] of the
				 // previous block = physical audio at T_now - lInputLatency - 1.
				 // Live recording begins at pfInput[0] of the current block =
				 // physical audio at T_now - lInputLatency.  The two streams are
				 // perfectly adjacent with no gap or overlap.
				 //
				 // Constraint: M plus this block must fit in lInputBufSize (32768),
				 // checked in withinWindow.  With the default 100 ms window,
				 // M <= ~4800 samples at 48 kHz.  The modular arithmetic below
				 // handles ring-buffer wrap-around correctly.
				 unsigned long backfillOffset = T + 1;
				 //------ albBegin ----------------------------------
#if SYNC_PREROLL
				 // The plain Sync pre-roll goes back P before the pulse too
				 // (0 if the history does not hold it as well).
				 const unsigned long preRoll = syncPreRollSamples(pLS, T + 1, SampleCount);
				 backfillOffset += preRoll;
				 loop->lStartEarly = preRoll;
				 loop->lOrigSyncPos = loop->lSyncPos = -(long) preRoll;
#endif
				 //------ albEnd ----------------------------------
				 unsigned long readPos = (pLS->lInputBufWritePos >= backfillOffset)
					 ? (pLS->lInputBufWritePos - backfillOffset)
					 : (pLS->lInputBufSize - (backfillOffset - pLS->lInputBufWritePos));
				 SYNCWDBG(SL_LOG_RT(
				     "%u:%u  Backfill: lInputBufWritePos=%lu SampleCount=%lu"
				     " T=%lu lInputLatency=%lu readPos=%lu\n",
				     pLS->lLoopIndex, pLS->lChannelIndex,
				     pLS->lInputBufWritePos, SampleCount,
				     T, lInputLatency, readPos));
				 // C10 safety invariant:
				 //   pushNewLoopChunk() always returns a freshly allocated chunk
				 //   whose [lLoopStart, lLoopStart + T) range is exclusive to this
				 //   chunk (ensureLoopSpace places new chunks contiguously after
				 //   the previous tail).  No other live chunk occupies this range,
				 //   so the destination write is safe.
				 //   lBufferSize is always a power of two (see instantiate()), so
				 //   lBufferSizeMask = lBufferSize - 1 is a valid wrap mask.
				 //   withinWindow requires M + SampleCount <= lInputBufSize, so
				 //   the source read from pfInputLatencyBuf never reaches this
				 //   block's samples (already written over the oldest ones)
				 //   regardless of g_late_hit_window_ms.
				 //
				 // The record fade-in goes on the backfill, at the loop's start,
				 // with the gain a normal start gives index i: (i + 1) / xfade.
				 // (It used to start where live recording takes over, leaving a
				 // fade from silence at index T, inside the loop, on every pass.)
				 for (unsigned long i = 0; i < backfillOffset; ++i) {
					 const float fGain = (i + 1 < (unsigned long) xfadeSamples)
						 ? (float) (i + 1) / xfadeSamples : 1.0f;
					 pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask] =
						 fGain * pfInputLatencyBuf[(readPos + i) & pLS->lInputBufMask];
				 }

				 // Start recording from position M: backfill covers [0..M), live
				 // recording continues from M onward to the next sync boundary.
				 // The loop content is now identical to what an on-time press would
				 // have produced, so the standard TRIG_STOP latency formula
				 // (dCurrPos += lOutputLatency + lInputLatency) applies unchanged.
				 loop->dCurrPos = (double)backfillOffset;

				 // fade setup (mirrors STATE_TRIG_START transition), except that
				 // the input-to-loop fade-in carries on from where the backfill
				 // left it (STATE_RECORD steps the gain before each sample)
				 pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 ((float) backfillOffset / xfadeSamples);
				 pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
				 pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
				 pLS->fLoopSrcFadeDelta = 1.0f / xfadeSamples;
				 pLS->fFeedSrcFadeDelta = -1.0f / xfadeSamples;
			 } else {
				 // Bug fix (albBegin): entering STATE_PLAY with loop==NULL crashes the DSP
				 // loop dispatcher (srcloop = loop->srcloop is the first dereference).
				 // pushNewLoopChunk returned NULL here (OOM or LoopChunk pool exhausted);
				 // the local loop variable is therefore NULL.  STATE_OFF passes audio
				 // through without looping — safe graceful degradation on allocation failure.
				 DBG(fprintf(stderr, "%u:%u  out of memory in late-hit path, entering STATE_OFF\n",
				             pLS->lLoopIndex, pLS->lChannelIndex));
				 pLS->state = STATE_OFF;
				 pLS->wasMuted = false;
			 }
		 } else {
			 pLS->state = STATE_TRIG_START;
			 //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			 // An unsynced take with no threshold starts on
			 // STATE_TRIG_START's first sample: start it input_latency -
			 // trigger_latency later instead, on the press (stock's TODO
			 // there).
#if FREE_PREROLL
			 // FREE_PREROLL: the free pre-roll counts as command latency
			 // here too, so the take starts that much before the press
			 // (a shorter wait, or more history); none if the history
			 // does not hold it, as without it.
			 if (fSyncMode == 0.0f && fTrigThresh == 0.0f) {
				 unsigned long pre = freePrerollSamples(pLS);
				 if (pre > 0 && (unsigned long) takeEdgeAdvanceSamples(pLS, pre) + SampleCount
				     > pLS->lInputBufSize) {
					 noteHistoryShort (pLS, takeEdgeAdvanceSamples(pLS, pre) + SampleCount);
					 pre = 0;
				 }
				 pLS->lFreePrerollNext = pre;
				 pLS->lEdgeDelay = takeEdgeDelaySamples(pLS, pre);
				 const unsigned long early = (unsigned long) takeEdgeAdvanceSamples(pLS, pre);
				 if (early > 0) {
					 if (early + SampleCount <= pLS->lInputBufSize)
						 pLS->lEdgeEarly = early;
					 else
						 noteHistoryShort (pLS, early + SampleCount);
				 }
			 }
#else
			 if (fSyncMode == 0.0f && fTrigThresh == 0.0f)
				 pLS->lEdgeDelay = edgeDelaySamples(pLS);
			 // The other way (trigger_latency > input_latency): start
			 // it that much early instead, from the input history, if
			 // the history holds it as well as this block.
			 if (fSyncMode == 0.0f && fTrigThresh == 0.0f && edgeAdvanceSamples(pLS) > 0) {
				 const unsigned long early = (unsigned long) edgeAdvanceSamples(pLS);
				 if (early + SampleCount <= pLS->lInputBufSize)
					 pLS->lEdgeEarly = early;
				 else
					 noteHistoryShort (pLS, early + SampleCount);
			 }
#endif
#endif
			 //------ albEnd ----------------------------------
#if REL_SYNC_LATE_HIT
			 // A Rel Sync record that starts at the press (STATE_TRIG_START
			 // decides on its first sample) starts this much earlier, if the
			 // input history holds it as well as this block.
			 {
				 const float fTrigLat = *pLS->pfTriggerLatency;
				 const unsigned long early =
					 (unsigned long) (fTrigLat > 0.0f ? fTrigLat : 0.0f)
					 + (unsigned long) (g_rel_sync_preroll_ms * 0.001f * pLS->fSampleRate);
				 pLS->lRelSyncPressEarly =
					 (fSyncMode == 2.0f && early + SampleCount <= pLS->lInputBufSize) ? early : 0;
				 if (fSyncMode == 2.0f && early > 0 && pLS->lRelSyncPressEarly == 0)
					 noteHistoryShort (pLS, early + SampleCount);
			 }
#endif
			 SYNCWDBG(SL_LOG_RT(
			     "%u:%u  Normal path: entering TRIG_START, will wait for next sync"
			     " (%.2f ms since last boundary, lGlobalSyncAtButtonPress=%llu)\n",
			     pLS->lLoopIndex, pLS->lChannelIndex,
			     T * 1000.0f / pLS->fSampleRate,
			     lGlobalSyncAtButtonPress));
			 DBG(fprintf(stderr, "%u:%u  Entering TRIG_START state\n",
			             pLS->lLoopIndex, pLS->lChannelIndex));
		 }
		 }
		 //------ albEnd ----------------------------------
	   }
	} break;

	case MULTI_OVERDUB:
	{
	   switch(pLS->state) {
	      case STATE_OVERDUB:
		      // don't sync overdub ops unless asked to
		      if ((fOverdubQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF)) {
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // End input_latency - trigger_latency after the command, on
		      // the press: the layer goes on, and its sample loop ends it
		      // as below when the countdown runs out.  At 0, as below now.
		      if (edgeDelaySamples(pLS) > 0) {
			      pLS->lEdgeDelay = edgeDelaySamples(pLS);
			      pLS->bEdgeEndDeferred = true;
			      pLS->lEdgeEndNext = STATE_PLAY;
			      pLS->fEdgeEndPulse = 1.0f;
			      break;
		      }
#endif
		      //------ albEnd ----------------------------------

			      pLS->state = STATE_PLAY;
			      pLS->wasMuted = false;
			      DBG(fprintf(stderr,"%u:%u  Entering PLAY state\n", pLS->lLoopIndex, pLS->lChannelIndex));
			      pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
		      }
		      else {
			      pLS->nextState = STATE_PLAY;
			      pLS->waitingForSync = 1;
		      }
		      break;
	      case STATE_MULTIPLY:
		 if (loop) {
		    loop = endMultiply(pLS, loop, STATE_OVERDUB);
		 }
		 break;
		 
	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, STATE_OVERDUB);
		 }
		 break;
		 
		 
	   case STATE_RECORD:
	   case STATE_TRIG_STOP:
		   // lets sync on ending record with overdub
		   if (fSyncMode == 0.0f && !bRoundIntegerTempo) {
			   //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
			   // End the take input_latency - trigger_latency after the
			   // command, on the press: through STATE_TRIG_STOP, which with
			   // Sync off ends when the countdown runs out and goes into the
			   // overdub as below (the stock synced record-into-overdub path).
			   // (FREE_PREROLL: the take's pre-roll counts as command
			   // latency.)
			   if (pLS->state == STATE_RECORD && loop
			       && (takeEdgeDelaySamples(pLS, takePreroll(loop)) > 0
				   || takeTrimWanted(pLS, loop))) {
				   pLS->state = STATE_TRIG_STOP;
				   pLS->nextState = STATE_OVERDUB;
				   pLS->lEdgeDelay = takeEdgeDelaySamples(pLS, takePreroll(loop));
				   // The other way (increment (c)): at once, trimmed back
				   // to the press.
				   pLS->lEdgeTrim = (pLS->lEdgeDelay > 0) ? 0
					   : (unsigned long) takeEdgeAdvanceSamples(pLS, takePreroll(loop));
				   break;
			   }
#endif
			   //------ albEnd ----------------------------------
			   if (loop) {

				   loop = beginOverdub(pLS, loop);
				   if (loop)
					   srcloop = loop->srcloop;
				   else
					   srcloop = NULL;

				   // we need to increment loop position by output latency (+ IL ?)
				   loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
				   DBG(fprintf(stderr,"%u:%u  from rec Entering overdub state at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));
				   pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;

				   // input always immediately available
				   pLS->lFramesUntilInput = 0;

				   pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
				   pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;

				   // then send out a sync here for any slaves
				   //pfSyncOutput[0] = 1.0f;
			   }
		   } else {
			   DBG(fprintf(stderr, "%u:%u  starting syncwait for overdub:  %f\n", pLS->lLoopIndex, pLS->lChannelIndex, fSyncMode));
			   pLS->state = STATE_TRIG_STOP;
			   pLS->nextState = STATE_OVERDUB;
			   pLS->waitingForSync = 1;
		   }
		   
		   break;
	   case STATE_DELAY:
		   // goes back to overdub mode
		   // pop the delay loop off....
		   if (loop) {
			   undoLoop(pLS, false);
			   loop = pLS->headLoopChunk;
		   }
		   // continue through to default

	   default:
		   //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		   // Ending a Replace or Substitute when the overdub starts at
		   // once: end it on the press, then start the overdub (2b-iii).
		   if (((fOverdubQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
		       && deferLayerEnd(pLS, loop, STATE_OVERDUB, 1.0f))
			   break;
#endif
		   //------ albEnd ----------------------------------
		   if ((fOverdubQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF)) {

			   if (loop) {
				   //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
				   loop = beginOverdub(pLS, loop, true);
#else
				   loop = beginOverdub(pLS, loop);
#endif
				   //------ albEnd ----------------------------------
				   if (loop)
					   srcloop = loop->srcloop;
				   else
					   srcloop = NULL;
			   }
			   // then send out a sync here for any slaves
			   pfSyncOutput[0] = 1.0f;
		   }
		   else {
			   if (pLS->state == STATE_RECORD) {
				   pLS->state = STATE_TRIG_STOP;
			   }
			   DBG(fprintf(stderr, "%u:%u  starting syncwait for overdub\n", pLS->lLoopIndex, pLS->lChannelIndex));
			   pLS->nextState = STATE_OVERDUB;
			   pLS->waitingForSync = 1;
		   }
	   }
	} break;

	case MULTI_MULTIPLY:
	{
	   switch(pLS->state) {
	      case STATE_MULTIPLY:
		 // set mark
		      if (fSyncMode == 0.0f) {
			      if (loop) {
				      if (loop->mult_out > 0) {
					      // we're in a multiincrease
					      loop->mult_out += 1;
					      //fprintf(stderr, "mult out added for %d\n", loop->mult_out);
				      }
				      else {
					      loop->mult_out = loop->lCycles;
					      //fprintf(stderr, "mult out initiated\n");
					      loop = endMultiply(pLS, loop, STATE_PLAY);
				      }
			      }

			      // put a sync marker at the beginning here
			      // mostly for slave purposes, it might screw up others
			      pfSyncOutput[0] = 1.0f;
			      
		      } else {
			      if (loop) {
				      if (loop->mult_out > 0) {
					      // we're in a multiincrease
					      loop->mult_out += 1;
					      //fprintf(stderr, "mult out added for %d\n", loop->mult_out);
				      }
				      else {
					      loop->mult_out = loop->lCycles;
					      //fprintf(stderr, "mult out initiated\n");
					      //------ albBegin ----------------------------------
					      // Multiply-end late-hit correction
#if LATE_HIT_MULTIPLY_CORRECTION
					      // T: samples elapsed since last sync pulse (same variable as
					      // in the MULTI_RECORD paths; declared locally here because each
					      // case block has its own scope).
					      unsigned long T = lGlobalSyncAtButtonPress;
					      // If this press arrived within the late-hit window after the
					      // most recent sync boundary, correct it back to that boundary
					      // instead of waiting for the next one.  The boundary T samples
					      // ago already caused lCycles to increment, so we decrement it
					      // back before calling endMultiply.  lLoopLength is corrected
					      // too so endMultiply sees a consistent state regardless of
					      // Round mode.
					      //
					      // Guard: lCycles >= 2 ensures we never decrement to 0.
					      // Same g_late_hit_window_ms and T as the Record stop path.
					      //
					      // The boundary must also be where the current cycle began:
					      // sync pulses can come several times per cycle (Eighths/Cycle
					      // shorter than the loop's cycle), and only the one at a cycle
					      // start added a cycle.  The play position runs IL + OL ahead
					      // of the sync grid, so a cycle that began at that boundary is
					      // T + 1 + IL + OL samples old (the + 1 as for the record
					      // late-hit); a boundary inside the cycle is at least one sync
					      // period off, more than half the window.
					      {
						      unsigned long shiftSamples = effectiveLateHitWindow(pLS, eighthPerCycle);
						      const long sinceCycle = (long) loop->dCurrPos
							      - (long) (loop->lLoopLength - loop->lCycleLength);
						      long cycleFromBoundary = (long) T + 1 + (long) (lInputLatency + lOutputLatency);
						      //------ albBegin ----------------------------------
#if SYNC_PREROLL
						      // A pre-rolled loop's cycles start -lSyncPos before the
						      // pulse (lSyncPos is 0 otherwise under plain Sync, or
						      // positive after set_sync_pos, left as before).
						      if (loop->lSyncPos < 0) cycleFromBoundary -= loop->lSyncPos;
#endif
						      //------ albEnd ----------------------------------
						      const bool boundaryIsCycleStart =
							      labs (sinceCycle - cycleFromBoundary) <= (long) (shiftSamples / 2);
						      // C2 fix: bLateHitBypassed prevents late-hit firing on slave
						      // channels in internal sync mode; it is set in Looper::run().
						      // Rel Sync fix: fSyncMode == 1 only, as for the record
						      // start and stop (Rel Sync is 2).
						      bool multStopInWindow = (T > 0
							                              && T <= shiftSamples
							                              && fSyncMode == 1.0f
							                              && pLS->bSyncPulseReceived
							                              && loop->lCycles >= 2
							                              && boundaryIsCycleStart
							                              && !pLS->bLateHitBypassed);
						      SYNCWDBG(SL_LOG_RT(
							      "%u:%u  MultiplyEnd: T=%lu shiftSamples=%lu lCycles=%lu"
							      " sinceCycle=%ld (cycle start at the boundary: %ld)"
							      " withinWindow=%d syncMode=%g\n",
							      pLS->lLoopIndex, pLS->lChannelIndex,
							      T, shiftSamples, loop->lCycles,
							      sinceCycle, cycleFromBoundary,
							      (int)multStopInWindow, fSyncMode));
						      if (multStopInWindow) {
							      loop->lCycles -= 1;
							      loop->lLoopLength = loop->lCycles * loop->lCycleLength;
							      loop->mult_out = loop->lCycles;
							      SYNCWDBG(SL_LOG_RT(
								      "%u:%u  MultiplyEnd late-hit: trimmed to %lu cycles"
								      " lLoopLength=%lu T=%lu\n",
								      pLS->lLoopIndex, pLS->lChannelIndex,
								      loop->lCycles, loop->lLoopLength, T));
							      loop = endMultiply(pLS, loop, STATE_PLAY);
#if MULTIPLY_END_FIX
							      // MULTIPLY_END_FIX (see its comment block near the
							      // top of this file).  With Round on, endMultiply only
							      // marks the end; the record position is already
							      // past it, so finish now.  The input since the
							      // boundary (T + 1 samples: the counter reads one less,
							      // as for the record late-hit) went past the new end;
							      // fade it into the loop's start as an on-time end's
							      // Play does, and let Play carry the fade on.
							      if (loop && pLS->rounding) {
								      const unsigned long lateSamples = T + 1;
								      const unsigned long readPos =
									      (pLS->lInputBufWritePos + pLS->lInputBufSize - (lateSamples % pLS->lInputBufSize))
									      % pLS->lInputBufSize;
								      // Finish first: it fills the loop's start (which
								      // the crossfade then adds to).
								      loop = finishRoundedMultiply(pLS, loop);
								      for (unsigned long i = 0; loop && i < lateSamples && i + 1 < (unsigned long) xfadeSamples; ++i) {
									      const float fGain = 1.0f - (float) (i + 1) / xfadeSamples;
									      pLS->pSampleBuf[(loop->lLoopStart + i) & pLS->lBufferSizeMask] +=
										      fGain * pfInputLatencyBuf[(readPos + i) & pLS->lInputBufMask];
								      }
								      pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (1.0f - (float) lateSamples / xfadeSamples);
							      }
#endif
							      pfSyncOutput[0] = 1.0f;
							      break;
						      }
					      }
#endif
					      //------ albEnd ----------------------------------
					      // loop = endMultiply(pLS, loop, STATE_PLAY);
					      
					      DBG(fprintf(stderr, "%u:%u  waiting for sync multi end\n", pLS->lLoopIndex, pLS->lChannelIndex));
					      pLS->nextState = STATE_PLAY;
					      pLS->waitingForSync = 1;
				      }
			      }
		      }
		      break;

	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, STATE_MULTIPLY);
		 }
		 break;
		 
	      case STATE_DELAY:
		 // goes back to play mode
		 // pop the delay loop off....
		 if (loop) {
		    undoLoop(pLS, false);
		    loop = pLS->headLoopChunk;
		 }
		 // continue through to default

	      default:
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Ending a take, Sync off: end it on the press (2b-i).
		      if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_MULTIPLY,
			       fQuantizeMode == QUANT_OFF ? 1.0f : 0.0f))
			      break;
		      // Ending a layer, Sync and Quantize off: end it on the press,
		      // then start this one (2b-iii).
		      if (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF
			  && deferLayerEnd(pLS, loop, STATE_MULTIPLY, 1.0f))
			      break;
#endif
		      //------ albEnd ----------------------------------
		      if (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) {
			      if (loop) {
				      prevstate = pLS->state;

				      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
				      loop = beginMultiply(pLS, loop, true);
#else
				      loop = beginMultiply(pLS, loop);
#endif
				      //------ albEnd ----------------------------------
				      if (loop)
					      srcloop = loop->srcloop;
				      else
					      srcloop = NULL;
				      
				      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
					      // input always immediately available
					      pLS->lFramesUntilInput = 0;
					      // we need to increment loop position by output latency (+ IL ?)
					      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
					      DBG(fprintf(stderr,"%u:%u  from rec Entering multiply state at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));
					      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;

					      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
					      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
				      }
				      
			      }

			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
		      } else {
			      if (pLS->state == STATE_RECORD) {
				      pLS->state = STATE_TRIG_STOP;
			      }
			      
			      DBG(fprintf(stderr, "%u:%u  starting syncwait for multiply\n", pLS->lLoopIndex, pLS->lChannelIndex));
			      pLS->nextState = STATE_MULTIPLY;
			      pLS->waitingForSync = 1;
		      }
	   }
	} break;

	case MULTI_INSERT:
	{

	   switch(pLS->state) {
	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, STATE_PLAY);
		 }
		 break;
	      case STATE_MUTE:
	      case STATE_ONESHOT:
		 // play the loop one_shot mode
		 if (loop) {
		    DBG(fprintf(stderr,"Starting ONESHOT state\n"));
		    pLS->state = STATE_ONESHOT;
		    if (pLS->fCurrRate > 0)
			    loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) ;
		    else
			    loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1;

		    // wrap
		    loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		    //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
		    startOneshotPass(pLS, loop, 0.0);
#endif
		    //------ albEnd ----------------------------------

		 }
		 break;
	      case STATE_MULTIPLY:
		 if (loop) {
		    loop = endMultiply(pLS, loop, STATE_INSERT);
		 }
		 break;

	      case STATE_DELAY:
		 // goes back to play mode
		 // pop the delay loop off....
		 if (loop) {
		    undoLoop(pLS, false);
		    loop = pLS->headLoopChunk;
		 }
		 // continue through to default

	      default:
		 // make new loop chunk
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Ending a take, Sync off: end it on the press (2b-i).
		      if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_INSERT,
			       fQuantizeMode == QUANT_OFF ? 1.0f : 0.0f))
			      break;
		      // Ending a layer, Sync and Quantize off: end it on the press,
		      // then start this one (2b-iii).
		      if (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF
			  && deferLayerEnd(pLS, loop, STATE_INSERT, 1.0f))
			      break;
#endif
		      //------ albEnd ----------------------------------
		      if (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) 
		      {
			      if (loop)
			      {
				      prevstate = pLS->state;
				      
				      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
				      loop = beginInsert(pLS, loop, true);
#else
				      loop = beginInsert(pLS, loop);
#endif
				      //------ albEnd ----------------------------------
				      if (loop) srcloop = loop->srcloop;
				      else srcloop = NULL;

				      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
					      // input always immediately available
					      pLS->lFramesUntilInput = 0;
					      // we need to increment loop position by output latency (+ IL ?)
					      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
					      DBG(fprintf(stderr,"from rec Entering insert state at %g\n", loop->dCurrPos));
					      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
					      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;					      
					      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
					      
				      }

			      }
			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
		      } else {
			      if (pLS->state == STATE_RECORD) {
				      pLS->state = STATE_TRIG_STOP;
			      }

			      DBG(fprintf(stderr, "starting syncwait for insert\n"));
			      pLS->nextState = STATE_INSERT;
			      pLS->waitingForSync = 1;
		      }
	   }

	} break;

	case MULTI_REPLACE: // also MULTI_TOG_REDO in mute mode
	{
	   
	   switch(pLS->state) {
	      case STATE_REPLACE:
		      if ((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF)) {
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // End input_latency - trigger_latency after the command, on
		      // the press: the layer goes on, and its sample loop ends it
		      // as below when the countdown runs out.  At 0, as below now.
		      if (edgeDelaySamples(pLS) > 0) {
			      pLS->lEdgeDelay = edgeDelaySamples(pLS);
			      pLS->bEdgeEndDeferred = true;
			      pLS->lEdgeEndNext = STATE_PLAY;
			      pLS->fEdgeEndPulse = 1.0f;
			      break;
		      }
#endif
		      //------ albEnd ----------------------------------
			      pLS->state = STATE_PLAY;
			      pLS->wasMuted = false;
			      DBG(fprintf(stderr,"%u:%u  Entering PLAY state\n", pLS->lLoopIndex, pLS->lChannelIndex));
			      pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;

			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
		      } else {
			      pLS->nextState = STATE_PLAY;
			      pLS->waitingForSync = 1;
		      }
		      break;


	      case STATE_MULTIPLY:
		 if (loop) {
		    loop = endMultiply(pLS, loop, STATE_REPLACE);
		 }
		 break;

	      case STATE_DELAY:
		 // this toggles HOLD mode
		 pLS->bHoldMode = !pLS->bHoldMode;
		 DBG(fprintf(stderr, "HoldMode is now %d\n", pLS->bHoldMode));
		 break;

	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, STATE_REPLACE);
		 }
		 break;
		 
	      default:
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Ending a take, Sync off: end it on the press (2b-i).
		      if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_REPLACE,
			       (!bReplaceQuantized || fQuantizeMode == QUANT_OFF) ? 1.0f : 0.0f))
			      break;
		      // Ending a layer when this one starts at once: end it on the
		      // press, then start this one (2b-iii).
		      if (((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
			  && deferLayerEnd(pLS, loop, STATE_REPLACE, 1.0f))
			      break;
		      // Ending a take at once with Sync on (replace_quantized off):
		      // end it on the press (2b-iii).
		      if (bReplaceQuantized == 0.0f && fSyncMode != 0.0f
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)
			  && deferTakeEndNow(pLS, loop, STATE_REPLACE, 1.0f))
			      break;
#endif
		      //------ albEnd ----------------------------------
		      if (((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) {
			      if (loop)
			      {
				      prevstate = pLS->state;
				      
				      DBG(fprintf(stderr, "starting replace immediately\n"));
				      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
				      loop = beginReplace(pLS, loop, true);
#else
				      loop = beginReplace(pLS, loop);
#endif
				      //------ albEnd ----------------------------------
				      if (loop) srcloop = loop->srcloop;
				      else srcloop = NULL;

				      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
					      // input always immediately available
					      pLS->lFramesUntilInput = 0;
					      // we need to increment loop position by output latency (+ IL ?)
					      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
					      DBG(fprintf(stderr,"from rec Entering multiply state at %g\n", loop->dCurrPos));
					      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
					      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
					      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
				      }
			      }

			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
		      }
		      else {
			      if (pLS->state == STATE_RECORD) {
				      pLS->state = STATE_TRIG_STOP;
			      }
			      DBG(fprintf(stderr, "starting syncwait for replace\n"));
			      pLS->nextState = STATE_REPLACE;
			      pLS->waitingForSync = 1;
		      }
	   }

	} break;

	case MULTI_SUBSTITUTE: // also MULTI_TOG_REDO in mute mode
	{
	   
	   switch(pLS->state) {
	      case STATE_SUBSTITUTE:
		      if ((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF)) {
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // End input_latency - trigger_latency after the command, on
		      // the press: the layer goes on, and its sample loop ends it
		      // as below when the countdown runs out.  At 0, as below now.
		      if (edgeDelaySamples(pLS) > 0) {
			      pLS->lEdgeDelay = edgeDelaySamples(pLS);
			      pLS->bEdgeEndDeferred = true;
			      pLS->lEdgeEndNext = STATE_PLAY;
			      pLS->fEdgeEndPulse = 1.0f;
			      break;
		      }
#endif
		      //------ albEnd ----------------------------------
			      pLS->state = STATE_PLAY;
			      pLS->wasMuted = false;
			      DBG(fprintf(stderr,"Entering PLAY state\n"));
			      pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;

			      // then send out a sync here for any slaves
			      pfSyncOutput[0] = 1.0f;
			      
		      } else {
			      pLS->nextState = STATE_PLAY;
			      pLS->waitingForSync = 1;
		      }
		      break;


	      case STATE_MULTIPLY:
		 if (loop) {
		    loop = endMultiply(pLS, loop, STATE_SUBSTITUTE);
		 }
		 break;

	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, STATE_SUBSTITUTE);
		 }
		 break;
		 
	      default:
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Ending a take, Sync off: end it on the press (2b-i).
		      if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_SUBSTITUTE,
			       fQuantizeMode == QUANT_OFF ? 1.0f : 0.0f))
			      break;
		      // Ending a layer when this one starts at once: end it on the
		      // press, then start this one (2b-iii).
		      if (((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
			  && deferLayerEnd(pLS, loop, STATE_SUBSTITUTE, fQuantizeMode == QUANT_OFF ? 1.0f : 0.0f))
			      break;
		      // Ending a take at once with Sync on (replace_quantized off):
		      // end it on the press (2b-iii).
		      if (bReplaceQuantized == 0.0f && fSyncMode != 0.0f
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)
			  && deferTakeEndNow(pLS, loop, STATE_SUBSTITUTE, fQuantizeMode == QUANT_OFF ? 1.0f : 0.0f))
			      break;
#endif
		      //------ albEnd ----------------------------------
		      if (((bReplaceQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
			  && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) {
			      if (loop)
			      {
				      prevstate = pLS->state;

				      DBG(fprintf(stderr, "starting subst immediately\n"));
				      //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
				      loop = beginSubstitute(pLS, loop, true);
#else
				      loop = beginSubstitute(pLS, loop);
#endif
				      //------ albEnd ----------------------------------
				      if (loop) srcloop = loop->srcloop;
				      else srcloop = NULL;

				      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
					      // input always immediately available
					      pLS->lFramesUntilInput = 0;
					      // we need to increment loop position by output latency (+ IL ?)
					      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
					      DBG(fprintf(stderr,"from rec Entering multiply state at %g\n", loop->dCurrPos));
					      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
					      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
					      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
				      }
			      }

			      if (fQuantizeMode == QUANT_OFF) {
				      // then send out a sync here for any slaves
				      pfSyncOutput[0] = 1.0f;
			      }
		      }
		      else {
			      if (pLS->state == STATE_RECORD) {
				      pLS->state = STATE_TRIG_STOP;
			      }
			      DBG(fprintf(stderr, "starting syncwait for substitute\n"));
			      pLS->nextState = STATE_SUBSTITUTE;
			      pLS->waitingForSync = 1;
		      }
	   }

	} break;
	
	case MULTI_MUTE:
	case MULTI_PAUSE:
	{
	   //------ albBegin ----------------------------------
	   // Late-hit window check for mute/unmute -- computed once here, used
	   // by both the unmute branch (STATE_MUTE case below) and the
	   // entering-mute branch (default: case below). Same T
	   // (lGlobalSyncAtButtonPress) and guard shape as Record's own
	   // late-hit correction; deliberately NOT effectiveLateHitWindow() --
	   // see g_mute_late_hit_ms's comment block near the top of this file
	   // for why the cycle-fraction cap does not apply here.
	   unsigned long muteShiftSamples =
	       (unsigned long)(g_mute_late_hit_ms * 0.001f * pLS->fSampleRate);
	   // At most half the Quantize unit, as for Record: the nearer
	   // boundary wins.
	   if (pLS->lLastSyncPeriodSamples != ULONG_MAX
	       && muteShiftSamples > pLS->lLastSyncPeriodSamples / 2)
		   muteShiftSamples = pLS->lLastSyncPeriodSamples / 2;
#if TRIGGER_LATENCY_ALL_EDGES
	   // Increment (e): the window (and its cap) are the press's; the
	   // command arrives trigger_latency later.
	   muteShiftSamples += commandWindowExtra (pLS);
#endif
	   unsigned long muteT = lGlobalSyncAtButtonPress;
	   bool withinMuteWindow = (muteT > 0
	                            && muteT <= muteShiftSamples
	                            && fSyncMode > 0.0f
	                            && pLS->bSyncPulseReceived
	                            && !pLS->bLateHitBypassed);

	   SYNCWDBG(SL_LOG_RT(
	       "\n%u:%u  MUTE/PAUSE button: lGlobalSyncAtButtonPress=%llu"
	       "  windowSamples=%lu  withinMuteWindow=%d  fromState=%d"
	       "  multiCtrl=%d  (%.2f ms since boundary, window=%.1f ms)\n",
	       pLS->lLoopIndex, pLS->lChannelIndex,
	       lGlobalSyncAtButtonPress, muteShiftSamples,
	       (int)withinMuteWindow, (int)pLS->state, lMultiCtrl,
	       muteT * 1000.0f / pLS->fSampleRate,
	       muteShiftSamples * 1000.0f / pLS->fSampleRate));
	   //------ albEnd ----------------------------------

	   switch(pLS->state) {

	       case STATE_OFF_MUTE:
             if (lMultiCtrl == MULTI_MUTE) {
                 pLS->state = STATE_OFF;
                 pLS->wasMuted = false;
             }
	         break;
	       case STATE_OFF:
             if (lMultiCtrl == MULTI_MUTE) {
                 pLS->state = STATE_OFF_MUTE;
                 pLS->wasMuted = true;
             }
	         break;
	       case STATE_MUTE:
		      // reset for audio ramp
		      //pLS->lRampSamples = xfadeSamples;
	       case STATE_ONESHOT:
	       case STATE_PAUSED:
	       // this enters play mode but from the continuous position
	       {
		       //------ albBegin ----------------------------------
		       // Late-hit unmute: scoped to STATE_MUTE + MULTI_MUTE
		       // specifically, so it cannot affect STATE_ONESHOT,
		       // STATE_PAUSED, or the MULTI_PAUSE (pause/resume) paths
		       // sharing this case. Wrapped in its own block so this
		       // declaration's scope ends before the STATE_MULTIPLY/
		       // STATE_INSERT/STATE_DELAY/default: labels further down
		       // this same switch(pLS->state) -- otherwise a direct jump
		       // to one of those (e.g. pLS->state == STATE_MULTIPLY)
		       // would illegally cross this initializer.
		       bool unmuteLateHit = (pLS->state == STATE_MUTE
		                            && lMultiCtrl == MULTI_MUTE
		                            && withinMuteWindow);
		       //------ albEnd ----------------------------------
		       if (loop && ((fMuteQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF) || pLS->state == STATE_PAUSED || unmuteLateHit)) {
			       if (pLS->state == STATE_MUTE && lMultiCtrl == MULTI_PAUSE) {
				       pLS->state = STATE_PAUSED;
				       pLS->wasMuted = true;
				       DBG(fprintf(stderr,"%u:%u  Entering PAUSED state from mute\n", pLS->lLoopIndex, pLS->lChannelIndex));
				       pLS->dPausedPos = loop->dCurrPos;
			       }
			       else if (pLS->state == STATE_PAUSED && lMultiCtrl == MULTI_MUTE) {
				       pLS->state = STATE_MUTE;
				       pLS->wasMuted = true;
				       DBG(fprintf(stderr,"%u:%u  Entering MUTED state from pause\n", pLS->lLoopIndex, pLS->lChannelIndex));
				       pLS->dPausedPos = loop->dCurrPos;
			       }
			       else {
				       pLS->state = STATE_PLAY;
				       pLS->wasMuted = false;
				       DBG(fprintf(stderr,"%u:%u  Entering PLAY state continuous\n", pLS->lLoopIndex, pLS->lChannelIndex));
				       pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
				       
				       if (lMultiCtrl == MULTI_PAUSE && loop) {
					       // set current loop position to paused position
					       loop->dCurrPos = pLS->dPausedPos;
					       DBG(fprintf(stderr, "%u:%u  setting loop pos to pause pos: %g\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->dPausedPos));
				       }
				       
				       // then send out a sync here for any slaves
				       pfSyncOutput[0] = 1.0f;
			       }
		       }
		       else if (loop) {
			       // wait for sync
			       if (pLS->state == STATE_RECORD) {
				       pLS->state = STATE_TRIG_STOP;
			       }
			       DBG(fprintf(stderr, "%u:%u  starting syncwait for play from mute\n", pLS->lLoopIndex, pLS->lChannelIndex));
			       if (pLS->state == STATE_PAUSED && lMultiCtrl == MULTI_MUTE) {
				       pLS->nextState = STATE_MUTE;
			       }
			       //------ albBegin ----------------------------------
			       // See PAUSE_FROM_MUTE_FIX's comment block near the top
			       // of this file. Missing
			       // case in stock 1.7.9: waiting in STATE_MUTE to resolve
			       // into STATE_PAUSED was never handled, so it fell through
			       // to the generic STATE_PLAY branch below.
#if PAUSE_FROM_MUTE_FIX
			       else if (pLS->state == STATE_MUTE && lMultiCtrl == MULTI_PAUSE) {
				       pLS->nextState = STATE_PAUSED;
			       }
#endif
			       //------ albEnd ----------------------------------
			       else {
				       pLS->nextState = STATE_PLAY;
			       }
			       pLS->waitingForSync = 1;
		       }
	       }
		 break;

	      case STATE_MULTIPLY:
		 if (loop) {
		    loop = endMultiply(pLS, loop, lMultiCtrl == MULTI_MUTE ? STATE_MUTE: STATE_PAUSED);
		 }
		 break;


	      case STATE_INSERT:
		 if (loop) {
		    loop = endInsert(pLS, loop, lMultiCtrl == MULTI_MUTE ? STATE_MUTE: STATE_PAUSED);
		 }
		 break;

		 
	      case STATE_DELAY:
		 // pop the delay loop off....
		 if (loop) {
		    undoLoop(pLS, false);
		    loop = pLS->headLoopChunk;
		 }
		 // continue through to default

	      default:
		   //------ albBegin ----------------------------------
		   // Late-hit correction for entering mute -- the symmetric case
		   // to unmute above. Scoped to STATE_PLAY specifically, so it
		   // structurally cannot affect STATE_RECORD/STATE_TRIG_STOP (the
		   // output-latency compensation branch just below, which needs
		   // its own separate timing story), STATE_DELAY (which falls
		   // through into this default: case from above), or MULTI_PAUSE.
		   bool muteLateHit = (pLS->state == STATE_PLAY
		                       && lMultiCtrl == MULTI_MUTE
		                       && withinMuteWindow);
		   bool muteImmediateOK =
		       ((fMuteQuantized == 0.0f) || (fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF))
		       && !(pLS->state == STATE_RECORD && bRoundIntegerTempo);
		   //------ albEnd ----------------------------------
		   //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		   // Ending a take, Sync off: end it on the press (2b-i).
		   if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo,
				    lMultiCtrl == MULTI_MUTE ? STATE_MUTE : STATE_PAUSED,
				    muteImmediateOK ? 1.0f : 0.0f))
			   break;
		   // Ending a layer, when the mute is immediate: end it on the
		   // press (2b-ii).
		   if (muteImmediateOK
		       && deferLayerEnd(pLS, loop,
					lMultiCtrl == MULTI_MUTE ? STATE_MUTE : STATE_PAUSED, 1.0f))
			   break;
		   // Ending a take at once with Sync on (mute_quantized off): end
		   // it on the press (2b-iii).
		   if (muteImmediateOK && fSyncMode != 0.0f
		       && deferTakeEndNow(pLS, loop,
					  lMultiCtrl == MULTI_MUTE ? STATE_MUTE : STATE_PAUSED, 1.0f))
			   break;
#endif
		   //------ albEnd ----------------------------------
		   if (loop && (muteImmediateOK || muteLateHit))
		   {
		     if (pLS->state == STATE_RECORD || pLS->state == STATE_TRIG_STOP) {
			 // we need to increment loop position by output latency (+ IL ?)
			 loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
			 DBG(fprintf(stderr,"%u:%u  from rec Entering mute state at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->dCurrPos));
			 pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		     }
		 
	
		     pLS->state = (lMultiCtrl==MULTI_MUTE ? STATE_MUTE: STATE_PAUSED);
		     DBG(fprintf(stderr,"%u:%u   Entering MUTE/pause state\n", pLS->lLoopIndex, pLS->lChannelIndex));
		     // reset for audio ramp
		     //pLS->lRampSamples = xfadeSamples;
		     pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
		     
		     pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		     pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		     pLS->wasMuted = true;
		     
		     if (lMultiCtrl == MULTI_PAUSE) {
			     pLS->dPausedPos = loop->dCurrPos;
			     pLS->wasMuted = false;
		     }

		      // then send out a sync here for any slaves
		      pfSyncOutput[0] = 1.0f;
		 }
		   else if (loop) {
			 // wait for sync
			 if (pLS->state == STATE_RECORD) {
				 pLS->state = STATE_TRIG_STOP;
			 }
			 DBG(fprintf(stderr, "%u:%u  starting syncwait for mute\n", pLS->lLoopIndex, pLS->lChannelIndex));
			 pLS->nextState = lMultiCtrl==MULTI_MUTE ? STATE_MUTE: STATE_PAUSED;
			 pLS->waitingForSync = 1;	 
		 }
	   }

	} break;

	case MULTI_DELAY:
	{
	   
	   switch(pLS->state) {
	      case STATE_DELAY:
		 // this is new tap
		 // calculate new length
		 if (loop) {
		    loop->lLoopLength = pLS->lTapTrigSamples;
		    loop->lCycleLength = loop->lLoopLength;

		    
		    if (loop->lLoopLength > (pLS->lBufferSize)) {
		       // ignore this tap, it's too long
		       // just treat as first
		       loop->lLoopLength = 0;
		       loop->lCycleLength = 0;
		       DBG(fprintf(stderr, "Long tap delay ignored....\n"));
		    }
		    
		    pLS->lTapTrigSamples = 0;

		    loop->dCurrPos = 0;
		    // need to clean the whole thing the first time

		    loop->backfill = 1;
		    loop->lMarkEndL = 0;
		    loop->lMarkEndH = loop->lLoopLength - 1;
		 
		    DBG(fprintf(stderr,"New delay length of %g secs\n",
				loop->lLoopLength / pLS->fSampleRate));
		 }
		 break;


		 
	      case STATE_REPLACE:
	      case STATE_SUBSTITUTE:
	      case STATE_RECORD:		 
	      case STATE_INSERT:
	      case STATE_OVERDUB:
	      case STATE_MULTIPLY:
		 // nothing happens
		 break;
		 
	      default:


		 loop = pushNewLoopChunk(pLS, 0, NULL);
		 if (loop)
		 {
		    pLS->state = STATE_DELAY;
		    loop->srcloop = NULL;
		    
		    // initial delay length 0 until next tap
		    loop->lLoopLength = 0;
		    
		    
		    pLS->bHoldMode = 0;
		    pLS->lTapTrigSamples = 0;
		    
		    
		    DBG(fprintf(stderr,"Entering DELAY state: waiting for second tap...\n"));
		 }
	   }
	} break;

	case MULTI_SCRATCH:
	{
	   switch(pLS->state) {
	      case STATE_SCRATCH:
		 pLS->state = STATE_PLAY;
		 pLS->wasMuted = false;
		 DBG(fprintf(stderr,"Entering PLAY state\n"));
		 break;

	      case STATE_MUTE:
	      case STATE_ONESHOT:		 
		 // this restarts the loop from beginnine
		 pLS->state = STATE_PLAY;
		 pLS->wasMuted = false;
		 if (loop) {
			 loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) + fSyncOffsetSamples;
			 // wrap
			 loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		 }
		 DBG(fprintf(stderr,"Entering PLAY state from top\n"));
		 break;

		 
	      case STATE_PLAY:
		 if (loop && !loop->frontfill && !loop->backfill)
		 {
		    // good to go, not filling in a loop beginning
		    pLS->state = STATE_SCRATCH;
		    DBG(fprintf(stderr,"Entering SCRATCH state\n"));
		 }
		 break;

	      case STATE_RECORD:
	      case STATE_TRIG_STOP:
		 //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		 // Ending a take (at once, whatever the Sync): end it on the
		 // press, then the reversed oneshot below (STATE_TRIG_STOP, 2b-iii).
		 if (deferTakeEndNow(pLS, loop, STATE_ONESHOT, 0.0f)) {
			 pLS->bEdgeStopScratch = true;
			 break;
		 }
#endif
		 //------ albEnd ----------------------------------
		 // THIS puts it into reverse and does a ONE_SHOT after TRIG_STOP
		 if (loop) {
		    fRate = pLS->fCurrRate = -1.0f;
		    pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		    pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
		    pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		    pLS->state = STATE_ONESHOT;
		    if (pLS->fCurrRate > 0)
			    loop->dCurrPos = (loop->lLoopLength -  loop->lSyncPos) + fSyncOffsetSamples;
		    else
			    loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1 - fSyncOffsetSamples;

		    // we need to increment loop position by output latency (+ IL ?)
		    loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
		    DBG(fprintf(stderr,"from rec Entering REV ONCE state at %g\n", loop->dCurrPos));
		    pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		    
		    // wrap
		    loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		    //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
		    startOneshotPass(pLS, loop, 0.0);
#endif
		    //------ albEnd ----------------------------------

		    DBG(fprintf(stderr,"Enter reversed ONESHOT state\n"));
		 }
		 break;
		 
	      case STATE_OVERDUB:
	      case STATE_MULTIPLY:
	      case STATE_REPLACE:
	      case STATE_SUBSTITUTE:
	      case STATE_INSERT:
	      case STATE_DELAY:
	      case STATE_OFF:
	      case STATE_OFF_MUTE:
		 // nothing happens
		 break;
		 
	      default:
		 pLS->state = STATE_SCRATCH;
		 DBG(fprintf(stderr,"Entering SCRATCH state\n"));
	   }

	} break;

	case MULTI_UNDO:
	case MULTI_UNDO_TWICE:
	{


	   switch(pLS->state) {
	      case STATE_PLAY:
	      case STATE_RECORD:
	      case STATE_OVERDUB:
	      case STATE_MULTIPLY:
	      case STATE_INSERT:
	      case STATE_TRIG_START:
	      case STATE_TRIG_STOP:		 
	      case STATE_REPLACE:
	      case STATE_SUBSTITUTE:
	      case STATE_DELAY:
	      case STATE_MUTE:
	      case STATE_PAUSED:
			   
			   // cancel whatever mode, back to play mode (or mute)
			   pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY;
		      
		      if (pLS->waitingForSync) {
			      // don't undo loop
			      // just return to play (or mute)
			      pLS->waitingForSync = 0;
			      pLS->nextState = -1;
		      }
		      else if (pLS->fNextCurrRate != 0.0f) {
			      // undo pending reverse
			      pLS->fNextCurrRate = 0.0f;
		      }

					if ( loop && loop->prev) {
						if (pLS->state == STATE_MUTE) {
							undoLoop(pLS, false);;
						} else { //undo with proper xfade
							pLS->state = STATE_UNDO;
							pLS->nextState = STATE_PLAY;
						}
					} else {
							LoopChunk *dead;
   						dead = pLS->headLoopChunk;
							//only go back into off if this is the only action undone
   						if (dead && (!dead->next)) {
								pLS->state = STATE_UNDO_ALL;
   						}
					}

			   
                           if (pLS->state == STATE_UNDO) {
                                   pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
                                   pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
                                   
                                   pLS->fPlayFadeAtten = 0.0f;
                                   pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			   }
                           else if (pLS->state == STATE_MUTE) {
				   pLS->fPlayFadeDelta = 0.0f;
			   } else if (pLS->state == STATE_UNDO_ALL) {
                                   pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
                                   pLS->fFeedFadeDelta = 1.0f / xfadeSamples;

				   pLS->fPlayFadeAtten = 1.0f;
				   pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
			   }

		      DBG(fprintf(stderr,"%u:%u  Undoing and reentering PLAY state from UNDO\n", pLS->lLoopIndex, pLS->lChannelIndex));
		      break;
	   }
	   
	} break;

	case MULTI_UNDO_ALL:
	{
		if (pLS->state != STATE_OFF && pLS->state != STATE_OFF_MUTE) {
			DBG(fprintf(stderr,"%u:%u  UNDO all loops\n", pLS->lLoopIndex, pLS->lChannelIndex));
			pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
			
			pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
			
			pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY;
			
			if (loop && loop->lLoopLength) {
				pLS->state = STATE_UNDO_ALL;
				
				pLS->fLoopFadeDelta = -1.0f / (xfadeSamples);
				pLS->fPlayFadeDelta = -1.0f / xfadeSamples;// fade out for undo all
			}			
		}
	} break;

	case MULTI_REDO_ALL:
	{

		if (pLS->state == STATE_OFF) {
			pLS->state = STATE_PLAY;
			pLS->wasMuted = false;
		} else if (pLS->state == STATE_OFF_MUTE) {
			pLS->state = STATE_MUTE;
		} 
		//else {
		//	pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY;
		//}
		
		if (!loop || (pLS->state == STATE_MUTE)) {
			lastloop = pLS->headLoopChunk;
			redoLoop(pLS);
			 
			while (pLS->headLoopChunk != lastloop) {
			lastloop = pLS->headLoopChunk;
			redoLoop(pLS);
			}
			if (!pLS->headLoopChunk) {
			    pLS->state = pLS->wasMuted ? STATE_OFF_MUTE : STATE_OFF;
			}
		} else {
			if (loop->next) {
				pLS->state = STATE_REDO_ALL;
				pLS->nextState = STATE_PLAY;
			}
		}
		
		pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		
		pLS->fPlayFadeAtten = 0.0f;
		pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
		
		if (pLS->state == STATE_MUTE) {
			pLS->fPlayFadeDelta = 0.0f;
		}

		DBG(fprintf(stderr,"REDO all loops\n"));

	} break;
	
	case MULTI_REDO:
	{
	   switch (pLS->state) {
		   case STATE_PLAY:
		   case STATE_RECORD:
		   case STATE_TRIG_START:
		   case STATE_TRIG_STOP:		 
		   case STATE_OVERDUB:
		   case STATE_MULTIPLY:
		   case STATE_INSERT:
		   case STATE_REPLACE:
		   case STATE_SUBSTITUTE:
		   case STATE_MUTE:
		   case STATE_PAUSED:
		   case STATE_OFF:
		   case STATE_OFF_MUTE:
			   
			   if (pLS->state == STATE_OFF) {
				   pLS->state = STATE_PLAY;
				   pLS->wasMuted = false;
			   } else if(pLS->state == STATE_OFF_MUTE) {
				   pLS->state = STATE_MUTE;
				   //pLS->state = pLS->wasMuted ? STATE_MUTE : STATE_PLAY;
			   }
			   
			   if (!loop || pLS->state == STATE_MUTE) {
				   // we don't need a fadeout
				   redoLoop(pLS);
				   if (!pLS->headLoopChunk) {
                       pLS->state = pLS->wasMuted ? STATE_OFF_MUTE : STATE_OFF;
				   }
			   } else {
				   // we need a x-fade
				   if (loop->next) {
					   pLS->state = STATE_REDO;
					   pLS->nextState = STATE_PLAY;
					   pLS->fPlayFadeAtten = 0.0f;
				   }
			   }
			   
			   pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
			   pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
			   			   
			   if (pLS->state == STATE_MUTE) {
				   // we don't need a fade in
				   pLS->fPlayFadeDelta = 0.0f;
			   } else {
				   pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
			   }

			break;
	   }

	} break;

	
	case MULTI_REVERSE: // ALSO MULTI_ROUND toggle in Mute mode
	{
	   switch(pLS->state) {
	      case STATE_RECORD:
	      case STATE_TRIG_STOP:
		 //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		 // Ending a take (at once, whatever the Sync): end it on the
		 // press, then play in reverse (STATE_TRIG_STOP negates the rate
		 // when it fires).
		 if (deferTakeEndNow(pLS, loop, STATE_PLAY, 0.0f)) {
			 pLS->bEdgeStopReverse = true;
			 break;
		 }
#endif
		 //------ albEnd ----------------------------------
		 // ends the record operation NOW
		 // and starts playing in reverse
		      fRate = pLS->fCurrRate *= -1.0f;
		      pLS->state = STATE_PLAY;
		      pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
		      pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
		      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
		      pLS->wasMuted = false;
		      // we need to increment loop position by output latency (+ IL ?)
		      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
		      DBG(fprintf(stderr,"from rec Entering ONCE state at %g\n", loop->dCurrPos));
		      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		      
		      DBG(fprintf(stderr,"Entering PLAY state by reversing\n"));
		 break;
	       case STATE_MULTIPLY:
	       case STATE_INSERT:
		  // ignore for now
		 break;

	      case STATE_SCRATCH:
		 // reverse button toggles rate control active when in scratch mode
		 //pLS->bRateCtrlActive = ! pLS->bRateCtrlActive;
		 //DBG(fprintf(stderr, "Rate control active state now %d\n", pLS->bRateCtrlActive));
		 break;
		 
	      default:

		 if (pLS->pfQuantMode && *pLS->pfQuantMode != 0) {
		    pLS->fNextCurrRate = pLS->fCurrRate * -1;
		    DBG(fprintf(stderr,"Reversing Rate quantized\n"));
		 }
		 else {
		    // simply negate the current rate
		    fRate = pLS->fCurrRate *= -1.0f;
		    DBG(fprintf(stderr,"Reversing Rate\n"));
		 }
	   }		 
	} break;

	case MULTI_ONESHOT:
	{
		switch(pLS->state) {
		case STATE_MULTIPLY:
			if (loop) {
				loop = endMultiply(pLS, loop, STATE_ONESHOT);
			}
			break;
			
		case STATE_INSERT:
			if (loop) {
				loop = endInsert(pLS, loop, STATE_ONESHOT);
			}
			break;
			
		default:
			// play the loop one_shot mode
			if (loop) {
				//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
				// Ending a take, Sync off: end it on the press (2b-i).
				if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_ONESHOT,
						 2.0f))
					break;
				// Ending a layer when this acts at once (Sync off, or the
				// slack window): end it on the press (2b-ii, 2b-iii).
				const bool restartNow = (fSyncMode == 0.0f
					//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
					// Increment (e): the slack window, from the press.
					|| (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)));
#else
					|| (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples));
#endif
					//------ albEnd ----------------------------------
				if (restartNow && deferLayerEnd(pLS, loop, STATE_ONESHOT, 2.0f))
					break;
				// Ending a take in the slack window, Sync on (2b-iii).
				if (fSyncMode >= 1.0f && restartNow
				    && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)
				    && deferTakeEndNow(pLS, loop, STATE_ONESHOT, 2.0f))
					break;
#endif
				//------ albEnd ----------------------------------
				//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
				// Increment (e): the slack window, from the press.
				if (((fSyncMode == 0.0f) || (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples))) // give it some slack on relsync
#else
				if (((fSyncMode == 0.0f) || (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples)) // give it some slack on relsync
#endif
				//------ albEnd ----------------------------------
				    && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) {
					DBG(fprintf(stderr,"Starting ONESHOT state\n"));
					pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
					pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
					pLS->fFeedFadeDelta = 1.0f / xfadeSamples;

				      prevstate = pLS->state;
				      
				      pLS->state = STATE_ONESHOT;

				      if (pLS->fCurrRate > 0)
					      loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos)  + fSyncOffsetSamples;
				      else
					      loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1 - fSyncOffsetSamples;
				      //------ albBegin ----------------------------------
#if ONESHOT_SLACK_FIX
				      // ONESHOT_SLACK_FIX (see its comment block near the top
				      // of this file): acting at once in the slack window,
				      // start as far in as the press is past the pulse (the
				      // count + 1, read before this block's first sample), as
				      // Trigger does, so the pass is in time with the beat.
				      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
				      // Increment (e): the slack window, from the press.
				      if (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)) {
#else
				      if (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
				      //------ albEnd ----------------------------------
					      if (pLS->fCurrRate > 0)
						      loop->dCurrPos += pLS->lSamplesSinceSync + 1;
					      else
						      loop->dCurrPos -= pLS->lSamplesSinceSync + 1;
				      }
#endif
				      //------ albEnd ----------------------------------
				      
				      // wrap
				      loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);

				      DBG(fprintf(stderr,"Starting ONESHOT state at   %g\n", loop->dCurrPos));

				      if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
					      // we need to increment loop position by output latency (+ IL ?)
					      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
					      DBG(fprintf(stderr,"from rec Entering ONCE state at %g\n", loop->dCurrPos));
					      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
				      }
				      //------ albBegin ----------------------------------
#if RESTART_POSITION_FIX
				      // From any other state, the same IL + OL, so the oneshot
				      // is heard in time (see RESTART_POSITION_FIX).
				      else {
					      loop->dCurrPos += ( lOutputLatency + lInputLatency) * fRate;
				      }
#endif
				      //------ albEnd ----------------------------------
				      //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
				      // One loop length, less the slack window's count
				      // (ONESHOT_SLACK_FIX's start, above).
				      {
					      double skipped = 0.0;
#if ONESHOT_SLACK_FIX
					      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
					      // Increment (e): the slack window, from the press.
					      if (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples))
#else
					      if (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples)
#endif
					      //------ albEnd ----------------------------------
						      skipped = (double) pLS->lSamplesSinceSync + 1.0;
#endif
					      startOneshotPass(pLS, loop, skipped);
				      }
#endif
#if TRIGGER_LATENCY_ALL_EDGES
				      // Increment (d): as Trigger's -- an unsynced oneshot that
				      // ends no take is trigger_latency + OL in, and its pass
				      // that much shorter (it began at the press).
				      if (fSyncMode == 0.0f && prevstate != STATE_RECORD
					  && prevstate != STATE_TRIG_STOP) {
					      const double adv = (double) *pLS->pfTriggerLatency
						      - (double) *pLS->pfInputLatency;
					      loop->dCurrPos += adv * fRate;
#if ONESHOT_ONE_PASS_FIX
					      pLS->dOneshotLeft -= adv;
#endif
				      }
#endif
				      //------ albEnd ----------------------------------
				      
				      // then send out a sync here for any slaves
				      pfSyncOutput[0] = 2.0f;
				      
				}
				else {
					if (pLS->state == STATE_RECORD) {
						pLS->state = STATE_TRIG_STOP;
					}
					DBG(fprintf(stderr, "starting syncwait for ONESHOT\n"));
					pLS->nextState = STATE_ONESHOT;
					pLS->waitingForSync = 1;
				}
			}
		      break;
		}
	} break;

	case MULTI_TRIGGER:
	{
		switch(pLS->state) {
		case STATE_MULTIPLY:
			if (loop) {
				loop = endMultiply(pLS, loop, STATE_PLAY);
			}
			break;
			
		case STATE_INSERT:
			if (loop) {
				loop = endInsert(pLS, loop, STATE_PLAY);
			}
			break;
			
		default:
			// play the loop from the start
			if (loop) {
				//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
				// Ending a take, Sync off: end it on the press (2b-i).
				if (deferTakeEnd(pLS, loop, fSyncMode, bRoundIntegerTempo, STATE_TRIGGER_PLAY,
						 2.0f))
					break;
				// Ending a layer when this acts at once (Sync off, or the
				// slack window): end it on the press (2b-ii, 2b-iii).
				const bool restartNow = (fSyncMode == 0.0f
					//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
					// Increment (e): the slack window, from the press.
					|| (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)));
#else
					|| (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples));
#endif
					//------ albEnd ----------------------------------
				if (restartNow && deferLayerEnd(pLS, loop, STATE_TRIGGER_PLAY, 2.0f))
					break;
				// Ending a take in the slack window, Sync on (2b-iii).
				if (fSyncMode >= 1.0f && restartNow
				    && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)
				    && deferTakeEndNow(pLS, loop, STATE_TRIGGER_PLAY, 2.0f))
					break;
#endif
				//------ albEnd ----------------------------------
				//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
				// Increment (e): the slack window, from the press.
				if ((fSyncMode == 0.0f || (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples))) // give it some slack 
#else
				if ((fSyncMode == 0.0f || (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples)) // give it some slack 
#endif
				//------ albEnd ----------------------------------
				    && !(pLS->state == STATE_RECORD && bRoundIntegerTempo)) 
				{

					prevstate = pLS->state;
					
					transitionToNext (pLS, loop, STATE_TRIGGER_PLAY);
					//------ albBegin ----------------------------------
#if SYNC_COUNT_OFF_BY_ONE_FIX
					// Acting at once in the slack window: transitionToNext
					// added the count, which here, before this block's first
					// sample, is one less than its distance from the pulse.
					//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
					// Increment (e): the slack window, from the press.
					if (fSyncMode >= 1.0f && inSlackWindow(pLS, eighthSamples)) {
#else
					if (fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples) {
#endif
					//------ albEnd ----------------------------------
						loop->dCurrPos += (pLS->fCurrRate > 0) ? 1.0 : -1.0;
					}
#endif
#if TRIGGER_LATENCY_ALL_EDGES
					// Increment (d): an unsynced restart that ends no take
					// happened at the press, trigger_latency before this
					// command: trigger_latency + OL into the loop, not
					// input_latency + OL (RESTART_POSITION_FIX's), so it is
					// heard in time.  A synced restart (slack window) is
					// placed by the grid, not the press.
					if (fSyncMode == 0.0f && prevstate != STATE_RECORD
					    && prevstate != STATE_TRIG_STOP) {
						loop->dCurrPos += ((double) *pLS->pfTriggerLatency
								   - (double) *pLS->pfInputLatency) * pLS->fCurrRate;
					}
#endif
					//------ albEnd ----------------------------------

					if (prevstate == STATE_RECORD || prevstate == STATE_TRIG_STOP) {
						// we need to increment loop position by output latency (+ IL ?)
						loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency) * fRate;
						DBG(fprintf(stderr,"from rec Entering ONCE state at %g\n", loop->dCurrPos));
						pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
					}
					
					// then send out a sync here for any slaves
					pfSyncOutput[0] = 2.0f;
					
				}
				else {
					if (pLS->state == STATE_RECORD) {
						pLS->state = STATE_TRIG_STOP;
					}
					DBG(fprintf(stderr, "starting syncwait for trigger\n"));
					pLS->nextState = STATE_TRIGGER_PLAY;
					pLS->waitingForSync = 1;
				}
			}
		      break;
		}
	} break;

	case MULTI_SET_SYNC_POS:
	{
		// set current loop's sync offset to the current pos
		if (loop) {
			loop->lSyncPos = (unsigned long) (loop->lLoopLength - loop->dCurrPos);
		}

	} break;

	case MULTI_RESET_SYNC_POS:
	{
		// set current loop's sync offset to the current pos
		if (loop) {
			loop->lSyncPos = loop->lOrigSyncPos;
		}

	} break;

	case MULTI_QUANT_TOG:
	{
	   // toggle quantize
	   // changing input param!!! Not strictly allowed!
	   if (pLS->pfQuantMode) {
	      if (*pLS->pfQuantMode == 0)
		 *pLS->pfQuantMode = (LADSPA_Data) QUANT_CYCLE;
	      else
		 *pLS->pfQuantMode = (LADSPA_Data) QUANT_OFF;
	   
	      DBG(fprintf(stderr,"Quantize mode is now %g\n", *pLS->pfQuantMode));
	   }
	} break;

	case MULTI_ROUND_TOG:
	{
	   if (pLS->pfRoundMode) {
	      // toggle roundmode
	      // changing input param!!!   Not strictly allowed!
	      if (*pLS->pfRoundMode == 0)
		 *pLS->pfRoundMode = 1.0f;
	      else
		 *pLS->pfRoundMode = 0;

	      DBG(fprintf(stderr,"Round mode is now %g\n", *pLS->pfRoundMode));
	   }
	} break;

	
	
     }		 

  }
  

  fRate = pLS->fCurrRate;
  
  //fprintf(stderr,"before play\n");  
  //fprintf(stderr, "fRateSwitch: %f\n", fRateSwitch);

  // the run loop
  
  lSampleIndex = 0;

  while (lSampleIndex < SampleCount)
  {
     loop = pLS->headLoopChunk;

     switch(pLS->state)
     {
			 
	case STATE_TRIG_START:
	{
	   //fprintf(stderr,"in trigstart\n");
	   
	   // we are looking for the threshold to actually
	   // start the recording on (while still playing dry signal)
	   
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      fWet += wetDelta;
              fDry += dryDelta;
	      fFeedback += feedbackDelta;
	      fScratchPos += scratchDelta;

	      // TODO: need to possibly wait IL-TL before actually starting
	      
	      fInputSample = pfInput[lSampleIndex];
	      //------ albBegin ----------------------------------
	      // TRIGGER_LATENCY_ALL_EDGES: an unsynced start waits out its
	      // countdown (set by MULTI_RECORD; 0 otherwise) -- the TODO above.
#if TRIGGER_LATENCY_ALL_EDGES
	      const bool edgeDue = (pLS->lEdgeDelay <= 0);
#else
	      const bool edgeDue = true;
#endif
	      //------ albEnd ----------------------------------
	      //------ albBegin ----------------------------------
	      // REL_SYNC_STALE_PHASE_FIX: start at once only when the counter
	      // is known.  The plain-Sync clause leaves Rel Sync out, so an
	      // unknown one cannot start on a non-cycle value; it waits for the
	      // counting at the bottom of this loop to see a cycle pulse.
#if REL_SYNC_STALE_PHASE_FIX
	      if ((fSyncMode == 0.0f && edgeDue && ((fInputSample > fTrigThresh) || (fTrigThresh==0.0f)))
			  || (fSyncMode == 2.0f && pLS->lSamplesSinceSync < REL_SYNC_PHASE_UNKNOWN) // relative sync offset mode
			  || (fSyncMode > 0.0f && fSyncMode != 2.0f && pfSyncInput[lSampleIndex] != 0.0f))
#else
	      if ((fSyncMode == 0.0f && edgeDue && ((fInputSample > fTrigThresh) || (fTrigThresh==0.0f)))
			  || (fSyncMode == 2.0f) // relative sync offset mode 
			  || (fSyncMode > 0.0f && pfSyncInput[lSampleIndex] != 0.0f))
#endif
	      //------ albEnd ----------------------------------
	      {
			  
			  loop = pushNewLoopChunk(pLS, 0, NULL);
			  if (loop) {
				  DBG(fprintf(stderr,"%u:%u  Entering RECORD state: syncmode=%g\n", pLS->lLoopIndex, pLS->lChannelIndex, fSyncMode));
				  
				  pLS->state = STATE_RECORD;
				  // force rate to be 1.0
				  fRate = pLS->fCurrRate = 1.0f;
				  
				  loop->lLoopLength = 0;
				  loop->lStartAdj = 0;
				  loop->lEndAdj = 0;
				  loop->dCurrPos = 0.0;
				  loop->firsttime = 0;
				  loop->lMarkL = loop->lMarkEndL = LONG_MAX;
				  loop->frontfill = loop->backfill = 0;
				  loop->lCycles = 1; // at first just one		 
				  loop->srcloop = NULL;
				  pLS->nextState = -1;
				  loop->dOrigFeedback = fFeedback;
				  
				  if (fSyncMode == 2.0f) {
					  // use sync offset
					  loop->lSyncOffset = pLS->lSamplesSinceSync;
					  // this needs to be adjusted for latency
					  //loop->lOrigSyncPos = loop->lSyncPos = pLS->lSamplesSinceSync + (unsigned long) (( lOutputLatency + lInputLatency) * fRate); 
					  //------ albBegin ----------------------------------
#if REL_SYNC_POS_FIX
					  // REL_SYNC_POS_FIX (see its comment block near the top
					  // of this file): from the nearer boundary, signed.
					  {
						  const long unit = 2 * (long) syncSamples;
#if SYNC_COUNT_OFF_BY_ONE_FIX
						  // The distance since the boundary is the count + 1.
						  const long since = (long) pLS->lSamplesSinceSync + 1;
#else
						  const long since = (long) pLS->lSamplesSinceSync;
#endif
						  loop->lOrigSyncPos = loop->lSyncPos =
							  (unit > 0 && since >= unit / 2) ? since - unit : since;
					  }
#else
					  if (pLS->lSamplesSinceSync < syncSamples/2) {
						  //loop->lOrigSyncPos = loop->lSyncPos = pLS->lSamplesSinceSync + (unsigned long) (( lOutputLatency + lInputLatency) * fRate); 
						  loop->lOrigSyncPos = loop->lSyncPos = pLS->lSamplesSinceSync; 
					  }
					  else {
						  //loop->lOrigSyncPos = loop->lSyncPos = (pLS->lSamplesSinceSync - syncSamples) + (unsigned long) (( lOutputLatency + lInputLatency) * fRate); 
						  loop->lOrigSyncPos = loop->lSyncPos = (pLS->lSamplesSinceSync - syncSamples); 
					  }
#endif
					  //------ albEnd ----------------------------------
					  
					  DBG(cerr <<  pLS->lLoopIndex << ":" << pLS->lChannelIndex << "   sync offset is: " << loop->lSyncOffset << "  syncpos: " << loop->lSyncPos <<endl);
				  }
				  else {
					  loop->lSyncOffset = 0;
					  loop->lOrigSyncPos = loop->lSyncPos = 0;
				  }
				  // cause input-to-loop fade in
				  pLS->fLoopFadeAtten = 0.0f;
				  pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
				  pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
				  
				  // only place this goes up
				  pLS->fLoopSrcFadeDelta = 1.0f / xfadeSamples;
				  // and this goes down
				  pLS->fFeedSrcFadeDelta = -1.0f / xfadeSamples;

				  //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT
				  // Early start: [0, early) is the input before this sample,
				  // from the input history (a verbatim copy of pfInput, this
				  // block included).  The offset stays the press's.
				  if (fSyncMode == 2.0f && pLS->lRelSyncPressEarly > 0) {
					  pLS->fLoopFadeAtten = startTakeEarly(pLS, loop, pfInputLatencyBuf,
						  pLS->lInputBufWritePos + lSampleIndex,
						  pLS->lRelSyncPressEarly, xfadeSamples);
				  }
				  pLS->lRelSyncPressEarly = 0;
#endif
#if TRIGGER_LATENCY_ALL_EDGES
				  // Increment (c): an unsynced take starts lEdgeEarly early
				  // (the press), from the input history.  With Sync off the
				  // press is the loop's start, so the sync position stays 0.
				  if (fSyncMode == 0.0f && pLS->lEdgeEarly > 0) {
					  pLS->fLoopFadeAtten = startTakeEarly(pLS, loop, pfInputLatencyBuf,
						  pLS->lInputBufWritePos + lSampleIndex,
						  pLS->lEdgeEarly, xfadeSamples);
					  loop->lSyncPos = loop->lOrigSyncPos = 0;
					  loop->lStartEarly = 0;
				  }
				  pLS->lEdgeEarly = 0;
#if FREE_PREROLL
				  // FREE_PREROLL: the take keeps its pre-roll, for its end,
				  // and its beat 1 is there, where the press was (as
				  // SYNC_PREROLL's lSyncPos = -P), for restarts.
				  loop->lFreePreroll = (fSyncMode == 0.0f) ? pLS->lFreePrerollNext : 0;
				  if (loop->lFreePreroll > 0)
					  loop->lSyncPos = loop->lOrigSyncPos = -(long) loop->lFreePreroll;
				  pLS->lFreePrerollNext = 0;
#endif
#endif
#if SYNC_PREROLL
				  // Plain Sync pre-roll: starting on a boundary pulse.
				  if (fSyncMode == 1.0f && pfSyncInput[lSampleIndex] > 1.5f) {
					  const unsigned long preRoll = syncPreRollSamples(pLS, 0, SampleCount);
					  if (preRoll > 0) {
						  pLS->fLoopFadeAtten = startTakeEarly(pLS, loop, pfInputLatencyBuf,
							  pLS->lInputBufWritePos + lSampleIndex,
							  preRoll, xfadeSamples);
					  }
				  }
#endif
				  //------ albEnd ----------------------------------
			  }
			  else {
				  DBG(fprintf(stderr, "out of memory! back to PLAY mode\n"));
				  pLS->state = STATE_PLAY;
				  pLS->wasMuted = false;
			  }

			  break;
	      }

	      pfOutput[lSampleIndex] = fDry * fInputSample;

	      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
	      // Count down a deferred start, for samples that stay here.
	      if (pLS->lEdgeDelay > 0) pLS->lEdgeDelay--;
#endif
	      //------ albEnd ----------------------------------

	      //------ albBegin ----------------------------------
	      // REL_SYNC_STALE_PHASE_FIX: count while waiting, as the other
	      // states do.  Only for samples that stay in this state: a start
	      // breaks out above, and STATE_RECORD counts that sample itself.
	      // A cycle pulse makes the counter 0, so an unknown Rel Sync
	      // record starts on the next sample with offset 0, and its stop
	      // comes one sample after a later pulse: whole cycles.  Pass the
	      // sync through too, as the other states do: the other channels
	      // take it from this one's output and must see the same pulse,
	      // which STATE_RECORD never reprocesses here.
#if REL_SYNC_STALE_PHASE_FIX
	      if (fSyncMode == 2.0f) {
		      pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
		      pLS->lSamplesSinceSync++;
		      if (pfSyncInput[lSampleIndex] > 1.5f) {
			      pLS->lSamplesSinceSync = 0;
		      }
	      }
#endif
#if REL_SYNC_LATE_HIT
	      // Waiting, so it will not start at the press: no early start.
	      pLS->lRelSyncPressEarly = 0;
#endif
	      //------ albEnd ----------------------------------
	   }
     
	} break;
	
	case STATE_RECORD:
	{
	   // play the input out while recording it.
	   //fprintf(stderr,"in record\n");

	   if ((loop = ensureLoopSpace (pLS, loop, SampleCount - lSampleIndex, NULL)) == NULL) {
		   DBG(fprintf(stderr, "%u:%u  Entering PLAY state -- END of memory! %08x\n", pLS->lLoopIndex, pLS->lChannelIndex,
			       (unsigned) (pLS->pSampleBuf + pLS->lBufferSize) ));
		   pLS->state = STATE_PLAY;
		   pLS->wasMuted = false;
		   goto passthrough;
   	   }
		
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      fWet += wetDelta;
		  fDry += dryDelta;
	      fFeedback += feedbackDelta;
	      fScratchPos += scratchDelta;

	      pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
	      pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopSrcFadeAtten + pLS->fLoopSrcFadeDelta);
	      pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
	      pLS->fPlayFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fPlayFadeAtten + pLS->fPlayFadeDelta);
	      
// 	      if (pLS->waitingForSync && (fSyncMode == 0.0 || pfSyncInput[lSampleIndex] != 0.0))
// 	      {
// 		      DBG(fprintf(stderr,"Finishing synced record\n"));
// 		      pLS->state = pLS->nextState;
// 		      pLS->nextState = -1;
// 		      pLS->waitingForSync = 0;
// 		      break;
// 	      }

	      //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT
	      // Where the take passes its end point: the check STATE_TRIG_STOP
	      // makes, at the same place in the sample (before this sample's
	      // count).  The start sample is the first.
	      if (fSyncMode == 2.0f && pLS->lSamplesSinceSync == loop->lSyncOffset) {
		      loop->lRelSyncLastPoint = (unsigned long) loop->dCurrPos;
	      }
#endif
	      //------ albEnd ----------------------------------

	      if (fSyncMode != 0.0f) {
		      pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
	      }

	      if (fSyncMode >= 1.0f) {
		      pLS->lSamplesSinceSync++;

		      if (pfSyncInput[lSampleIndex] > 1.5f) {
			      pLS->lSamplesSinceSync = 0;
			      DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex <<" rec reseting sync: " << pLS << " at " << loop->dCurrPos << endl);
		      }
	      }
	      
	      // wrap at the proper loop end
	      lCurrPos = (unsigned int) lrint(loop->dCurrPos);
	      pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];
		      
// 	      if ((char *)(lCurrPos + loop->pLoopStart) >= (pLS->pSampleBuf + pLS->lBufferSize)) {
// 		 // stop the recording RIGHT NOW
// 		 // we don't support loop crossing the end of memory
// 		 // it's easier.
// 		 DBG(fprintf(stderr, "Entering PLAY state -- END of memory! %08x\n",
// 			     (unsigned) (pLS->pSampleBuf + pLS->lBufferSize) ));
// 		 pLS->state = STATE_PLAY;
// 		 break;
// 	      }
		   

	      if (lCurrPos == 0) {
		      pfSyncOutput[lSampleIndex] = 1.0f;
	      }
		   
	      fInputSample = pfInput[lSampleIndex];
	      
	      *pLoopSample = pLS->fLoopFadeAtten * fInputSample;
	      
	      // increment according to current rate
	      loop->dCurrPos = loop->dCurrPos + fRate;
	      
	      
	      pfOutput[lSampleIndex] = fDry * fInputSample;
	   }

	   // update loop values (in case we get stopped by an event)
	   lCurrPos = (unsigned int) lrint(loop->dCurrPos);
	   loop->lLoopLength = (unsigned long) lCurrPos;
	   loop->lCycleLength = loop->lLoopLength;

	} break;

	case STATE_TRIG_STOP:
	{
	   //fprintf(stderr,"in trigstop\n");	   
	   // play the input out.  Keep recording until we go
	   // above the threshold, then go into next state.

	   //------ albBegin ----------------------------------
	   // TRIG_STOP_MEMORY_FIX: see its comment block near the top of this
	   // file.  Was: the call alone, unchecked.
#if TRIG_STOP_MEMORY_FIX
	   if ((loop = ensureLoopSpace (pLS, loop, SampleCount - lSampleIndex, NULL)) == NULL) {
		   // As STATE_RECORD does at the end of memory: keep what was
		   // recorded and play it.  Also drop the pending stop.
		   pLS->state = STATE_PLAY;
		   pLS->wasMuted = false;
		   pLS->nextState = -1;
		   pLS->waitingForSync = 0;
		   goto passthrough;
	   }
#else
	   loop = ensureLoopSpace (pLS, loop, SampleCount - lSampleIndex, NULL);
#endif
	   //------ albEnd ----------------------------------

	   bool roundTempoDone  = false;
	   unsigned int roundedSamples = 0;
	   //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT || LATE_STOP_ANY_COMMAND
	   const unsigned long lateStopWindow = effectiveLateHitWindow(pLS, eighthPerCycle);
#endif
	   //------ albEnd ----------------------------------

	   if (bRoundIntegerTempo) {
		   // logic to figure out if it is time to stop with a rounded tempo
		   float eighths = *pLS->pfEighthPerCycle;
		   double tempo = (30.0 * eighths * pLS->fSampleRate / loop->dCurrPos);
		   while (tempo > 1.0f && (tempo < 60.0f || tempo > 240.0f)) {
			   eighths *= (tempo < 60.0f) ? 2.0f : 0.5f;
			   eighths = max(1.0f, eighths);
			   tempo = (30.0 * eighths * pLS->fSampleRate / loop->dCurrPos);
			   DBG(cerr << "eights adjusted to: " << eighths << "  tempo: " << tempo << endl;)
			   if (eighths == 1.0f) break;
		   } 
		   
		   roundedSamples = (unsigned int) (pLS->fSampleRate * 30.0 * eighths/ floor(tempo));
		   //bool roundTempoDone = bRoundIntegerTempo && abs((long)lCurrPos - (long)roundedSamples) < 1;
		   DBG(cerr << pLS->lLoopIndex<< ":" <<  pLS->lChannelIndex <<  "rounded samples now: " << roundedSamples << "  eighths: " << eighths << " tempo: " << tempo << " currpos: " << loop->dCurrPos << endl;)
	   }
		
	   for (;lSampleIndex < SampleCount;
		lSampleIndex++)
	   {
	      fWet += wetDelta;
              fDry += dryDelta;
	      fFeedback += feedbackDelta;
	      fScratchPos += scratchDelta;
	      pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
	      pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopSrcFadeDelta);
	      pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
	      pLS->fPlayFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fPlayFadeAtten + pLS->fPlayFadeDelta);
		   
	      lCurrPos = (unsigned int) loop->dCurrPos;
	      pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];
	      
	      fInputSample = pfInput[lSampleIndex];
	      
	      
// 	      if ((fSyncMode == 0.0f && ((fInputSample > fTrigThresh) || (fTrigThresh==0.0)))
// 		  || (fSyncMode > 0.0f && pfSyncInput[lSampleIndex] != 0.0))

	      roundTempoDone = bRoundIntegerTempo && lCurrPos == roundedSamples;

	      //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT
	      // REL_SYNC_LATE_HIT late stop: the command that ended the take
	      // (any: Record, Overdub, a quantized Mute, ...) came at most the
	      // late-hit window past the take's last end point, and that point
	      // is a whole Quantize unit or more after the start (the start
	      // itself is a point, at lStartEarly).  End there now instead of
	      // at the next point.  The take's last point only moves while
	      // recording, so this can hold only on the first sample here.
	      const bool relSyncLate = (fSyncMode == 2.0f
	                                && !pLS->bLateHitBypassed
	                                && loop->lRelSyncLastPoint > loop->lStartEarly
	                                && lCurrPos - loop->lRelSyncLastPoint <= lateStopWindow);
#endif
#if LATE_STOP_ANY_COMMAND
	      // LATE_STOP_ANY_COMMAND: plain Sync, the command arrived at the
	      // start of this buffer at most the window after the last pulse,
	      // with the guards of the Record late stop (and no pulse right
	      // here, which ends the take normally).
	      const bool plainSyncLate = (fSyncMode == 1.0f
	                                  && lSampleIndex == 0
	                                  && pfSyncInput[lSampleIndex] == 0.0f
	                                  && pLS->bSyncPulseReceived
	                                  && !pLS->bLateHitBypassed
	                                  && lGlobalSyncAtButtonPress > 0
	                                  && lGlobalSyncAtButtonPress <= lateStopWindow
	                                  && loop->dCurrPos > (double) (lGlobalSyncAtButtonPress + 1 + xfadeSamples
#if SYNC_PREROLL
	                                                                + loop->lStartEarly
#endif
	                                                                ));
#endif
	      //------ albEnd ----------------------------------
	      
	      //------ albBegin ----------------------------------
	      // TRIGGER_LATENCY_ALL_EDGES: a deferred unsynced stop waits out its
	      // countdown (set by MULTI_RECORD, Record -> Overdub, and
	      // deferTakeEnd(); 0 otherwise).
#if TRIGGER_LATENCY_ALL_EDGES
	      const bool stopDue = (pLS->lEdgeDelay <= 0);
	      // A deferred take end (deferTakeEnd(), deferTakeEndNow(), Record's
	      // own stop) fires when its countdown runs out, and only then,
	      // whatever the Sync, and skips what only a synced end does.
	      const bool edgeDeferred = pLS->bEdgeStopDeferred;
#else
	      const bool stopDue = true;
	      const bool edgeDeferred = false;
#endif
	      //------ albEnd ----------------------------------
	      // exit immediately if syncmode is off, or we have a sync
	      //------ albBegin ----------------------------------
	      // STOCK: the condition of stopNow was the if's own.
	      bool stopNow = ((fSyncMode == 0.0f && (!bRoundIntegerTempo || roundTempoDone) && stopDue)
 		  || (fSyncMode == 1.0f && (pfSyncInput[lSampleIndex] != 0.0f))
		  || (fSyncMode == 2.0f && pLS->lSamplesSinceSync == loop->lSyncOffset)
#if REL_SYNC_LATE_HIT
		  || relSyncLate
#endif
#if LATE_STOP_ANY_COMMAND
		  || plainSyncLate
#endif
		  );
	      if (edgeDeferred)
		      stopNow = stopDue;
	      if (stopNow)
	      //------ albEnd ----------------------------------
	      {
		      DBG(fprintf(stderr,"%u:%u   Entering %d state at %u\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->nextState, lCurrPos));
		 //pLS->state = pLS->nextState;
		 // reset for audio ramp
		 //pLS->lRampSamples = xfadeSamples;
		 //loop->dCurrPos = 0.0f;
		      DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex << "Round tempo: " << bRoundIntegerTempo << "  roundedSamples: " << roundedSamples << endl);

		      if (roundTempoDone) {
			      // force sync for slaves... not sure if this is good
			      pfSyncOutput[lSampleIndex] = 2.0f;
		      }
	      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
	      // A deferred take end sends the pulse the immediate end sends
	      // (for loops synced to this one), now that it ends.
	      // bEdgeStopDeferred stays set until after the transition below.
	      if (pLS->bEdgeStopDeferred && pLS->fEdgeStopPulse != 0.0f)
		      pfSyncOutput[lSampleIndex] = pLS->fEdgeStopPulse;
#endif
	      //------ albEnd ----------------------------------

		      //------ albBegin ----------------------------------
		      // STOCK: if (fSyncMode == 2.0f) {
		      // CHANGE: not for a deferred take end, which ends as its
		      // immediate branch does (TRIGGER_LATENCY_ALL_EDGES).
		      if (fSyncMode == 2.0f && !edgeDeferred) {
		      //------ albEnd ----------------------------------
			      //cerr << "ending recstop sync2d: " << lCurrPos << endl;
			      pLS->recSyncEnded = true;
		      }
		      else {
			      //cerr << "ending recstop sync1: " << lCurrPos << endl;
		      }

		      // update current info before transition
		      loop->lLoopLength = (unsigned long) lCurrPos;
		      loop->lCycleLength = loop->lLoopLength;
		      loop->lCycles = 1;
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // Increment (c): a take ended at once with trigger_latency >
		      // input_latency ends lEdgeTrim back, at the press; the samples
		      // past it fade into the loop's start, and the position stays
		      // here, lEdgeTrim past the new end (see the restarts and
		      // Reverse below).
		      if (pLS->lEdgeTrim > 0 && (unsigned long) lCurrPos > pLS->lEdgeTrim) {
			      pLS->fLoopFadeAtten = endTakeTrimmed(pLS, loop, pLS->lEdgeTrim, xfadeSamples);
		      }
		      else {
			      pLS->lEdgeTrim = 0;
		      }
#endif
#if REL_SYNC_LATE_HIT
		      // The take ends at its end point less the early start: drop
		      // the samples past that (the early start, plus the overrun
		      // after a late command) into the loop's start.  The position
		      // stays here, so playback (below) carries on that many
		      // samples into the loop, as the plain Sync late stop does.
		      // Before transitionToNext(), so a following overdub copies
		      // the faded start.  Not for a deferred take end
		      // (TRIGGER_LATENCY_ALL_EDGES), which ends as its immediate
		      // branch does.
		      if (fSyncMode == 2.0f && !edgeDeferred) {
			      const unsigned long lateSamples = loop->lStartEarly
				      + (relSyncLate ? lCurrPos - loop->lRelSyncLastPoint : 0);
			      if (lateSamples > 0 && (unsigned long) lCurrPos > lateSamples) {
				      pLS->fLoopFadeAtten = endTakeTrimmed(pLS, loop, lateSamples, xfadeSamples);
			      }
		      }
#endif
#if LATE_STOP_ANY_COMMAND || SYNC_PREROLL
		      // Plain Sync: a late command ends just before the pulse T + 1
		      // samples back, as the Record late stop trims
		      // (LATE_STOP_ANY_COMMAND); a pre-rolled take ends P before
		      // that (SYNC_PREROLL).  Not for a deferred take end
		      // (TRIGGER_LATENCY_ALL_EDGES), which ends as its immediate
		      // branch does.
		      if (fSyncMode == 1.0f && !edgeDeferred) {
			      unsigned long lateSamples = 0;
#if LATE_STOP_ANY_COMMAND
			      if (plainSyncLate) lateSamples += lGlobalSyncAtButtonPress + 1;
#endif
#if SYNC_PREROLL
			      lateSamples += loop->lStartEarly;
#endif
			      if (lateSamples > 0 && (unsigned long) lCurrPos > lateSamples) {
				      pLS->fLoopFadeAtten = endTakeTrimmed(pLS, loop, lateSamples, xfadeSamples);
			      }
		      }
#endif
		      //------ albEnd ----------------------------------

		      DBG(fprintf(stderr,"%u:%u  transitioning to %d  at %g with length: %lu\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->nextState, loop->dCurrPos, loop->lLoopLength));
		      

		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // The state a deferred take end goes to, and Scratch's reverse
		      // rate, which its immediate branch sets before placing it.
		      const int edgeNext = pLS->nextState;
		      // How long ago the press was, past where the take now ends:
		      // lEdgeTrim, less the take's free pre-roll (FREE_PREROLL),
		      // which moved the end but not the press; negative when the
		      // end waited, for the pre-roll, less than input_latency -
		      // trigger_latency.  lEdgeTrim without one.
		      const double edgePressAgo = (double) pLS->lEdgeTrim - (double) takePreroll(loop);
		      if (edgeDeferred && pLS->bEdgeStopScratch)
			      fRate = pLS->fCurrRate = -1.0f;
#endif
		      //------ albEnd ----------------------------------
		      loop = transitionToNext (pLS, loop, pLS->nextState);
		      pLS->waitingForSync = 0;
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // A deferred Trigger, Oneshot or Scratch: placed as its
		      // immediate branch places it (the slack window's count, read
		      // now, with Sync on; not for Scratch, which ignores it).
		      if (edgeDeferred && loop
			  && (edgeNext == STATE_TRIGGER_PLAY || edgeNext == STATE_ONESHOT))
			      loop->dCurrPos = restartPosition(pLS, loop, edgeNext,
					fSyncMode >= 1.0f && !pLS->bEdgeStopScratch,
					fSyncOffsetSamples);
#if ONESHOT_ONE_PASS_FIX
		      // One-pass fix: the pass, less the count restartPosition()
		      // just added.
		      if (edgeDeferred && loop && edgeNext == STATE_ONESHOT)
			      startOneshotPass(pLS, loop,
					(fSyncMode >= 1.0f && !pLS->bEdgeStopScratch)
					? (double) pLS->lSamplesSinceSync + 1.0 : 0.0);
#endif
		      // A deferred take end by Reverse: reverse now, so the IL + OL
		      // add below subtracts, as Reverse's immediate branch does.
		      if (pLS->bEdgeStopDeferred && pLS->bEdgeStopReverse) {
			      pLS->bEdgeStopReverse = false;
			      fRate = pLS->fCurrRate *= -1.0f;
			      // Trimmed (increment (c)): the loop has run backwards from
			      // the new end since the press, lEdgeTrim ago: there, not
			      // lEdgeTrim past the end.
			      if (edgePressAgo != 0.0 && loop)
				      loop->dCurrPos -= 2.0 * edgePressAgo;
		      }
		      // Trimmed (increment (c)): a restart (Trigger, Oneshot, the
		      // reversed oneshot of Scratch) happened at the press, lEdgeTrim
		      // ago: that much further in, in its direction; a oneshot's pass
		      // that much shorter.
		      if (edgePressAgo != 0.0 && edgeDeferred && loop
			  && (edgeNext == STATE_TRIGGER_PLAY || edgeNext == STATE_ONESHOT)) {
			      loop->dCurrPos += edgePressAgo * fRate;
#if ONESHOT_ONE_PASS_FIX
			      if (edgeNext == STATE_ONESHOT)
				      pLS->dOneshotLeft -= edgePressAgo;
#endif
		      }
		      pLS->lEdgeTrim = 0;
#endif
		      //------ albEnd ----------------------------------
		      //------ albBegin ----------------------------------
		      // TRIG_STOP playback-start offset
		      // Advance dCurrPos so that rCurrPos starts at 0 on the first
		      // STATE_PLAY sample.
		      //
		      // During playback:
		      //   rCurrPos = dCurrPos - (lOutputLatency + lInputLatency)  (mod L)
		      //
		      // transitionToNext() is called above with dCurrPos == lLoopLength
		      // (the sync pulse fires at exactly that position and finalises the
		      // loop length).  Adding (lOutputLatency + lInputLatency) gives:
		      //   rCurrPos = lLoopLength + lOutputLatency + lInputLatency
		      //              - lOutputLatency - lInputLatency  (mod L)
		      //            = lLoopLength  (mod L)  =  0
		      //
		      // This is correct for both on-time and late-start recordings.
		      // In the late-start path the backfill writes exactly what pfInput
		      // would have captured from the sync boundary onward (see the
		      // MULTI_RECORD / default case for the derivation), so loop[0]
		      // contains the same boundary audio as a normal on-time recording
		      // and the same offset formula applies without modification.
		      // lOutputLatency and lInputLatency appear symmetrically in the
		      // record and playback paths, so this formula is valid for any
		      // hardware latency combination.
		      loop->dCurrPos = loop->dCurrPos + ( lOutputLatency + lInputLatency ) * fRate;
		      pLS->lFramesUntilFilled = lOutputLatency + lInputLatency;
		      //------ albEnd ----------------------------------
		      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		      // A deferred take end: what the immediate branch does that
		      // the transition above does not.
		      if (pLS->bEdgeStopDeferred) {
			      pLS->bEdgeStopDeferred = false;
			      pLS->fEdgeStopPulse = 0.0f;
			      // As every immediate take end (beginInsert does not).
			      pLS->fFeedSrcFadeDelta = 1.0f / xfadeSamples;
			      // Pause: the position after the IL + OL, as the
			      // immediate branch has it.
			      if (pLS->state == STATE_PAUSED && loop)
				      pLS->dPausedPos = loop->dCurrPos;
			      // Oneshot and Scratch set the feedback fade, which
			      // transitionToNext(STATE_ONESHOT) does not; Scratch
			      // wraps after the IL + OL.
			      if (edgeNext == STATE_ONESHOT)
				      pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
			      if (pLS->bEdgeStopScratch) {
				      pLS->bEdgeStopScratch = false;
				      if (loop)
					      loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
			      }
		      }
#endif
		      //------ albEnd ----------------------------------

		      pLS->fLoopSrcFadeDelta = -1.0f / xfadeSamples;
		      
		      break;
	      }

	      if (fSyncMode != 0.0f) {
		      pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
	      }

	      if (fSyncMode >= 1.0f) {
		      pLS->lSamplesSinceSync++;

		      if (pfSyncInput[lSampleIndex] > 1.5f) {
			      DBG(cerr <<  pLS->lLoopIndex << ":" << pLS->lChannelIndex << " TS reseting sync at pos: " << loop->dCurrPos << "  had: " << pLS->lSamplesSinceSync << " sync offset: " << loop->lSyncOffset << endl);
			      pLS->lSamplesSinceSync = 0;

		      }
	      }
	      
	      
	      *(pLoopSample) = pLS->fLoopFadeAtten * fInputSample;
	      //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
	      // Count down a deferred stop, for samples that stay (and were
	      // just recorded).
	      if (pLS->lEdgeDelay > 0) pLS->lEdgeDelay--;
#endif
	      //------ albEnd ----------------------------------
	      
	      // increment according to current rate
	      loop->dCurrPos = loop->dCurrPos + fRate;


// 	      if ((char *)(loop->pLoopStart + (unsigned int)loop->dCurrPos)
// 		  > (pLS->pSampleBuf + pLS->lBufferSize)) {
// 		 // out of space! give up for now!
// 		 // undo!
// 		 pLS->state = STATE_PLAY;
// 		 //undoLoop(pLS);
// 		 DBG(fprintf(stderr,"Record Stopped early! Out of memory!\n"));
// 		 loop->dCurrPos = 0.0f;
// 		 break;
// 	      }

	      
	      pfOutput[lSampleIndex] = fDry * fInputSample;

	      
	   }

	   // update loop values (in case we get stopped by an event)
	   //------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT || LATE_STOP_ANY_COMMAND || SYNC_PREROLL
	   // Only while still waiting.  After an end above the values are set
	   // already, possibly trimmed (REL_SYNC_LATE_HIT,
	   // LATE_STOP_ANY_COMMAND), and `loop` may be a new overdub chunk;
	   // this would put the untrimmed length back.
	   if (loop && pLS->state == STATE_TRIG_STOP) {
#else
	   if (loop) {
#endif
	   //------ albEnd ----------------------------------
		   loop->lLoopLength = (unsigned long) lCurrPos;
		   loop->lCycleLength = loop->lLoopLength;
		   loop->lCycles = 1;
	   }
	

   
	} break;


	
	case STATE_OVERDUB:
	case STATE_REPLACE:
	case STATE_SUBSTITUTE:
	{
	   if (loop && loop->srcloop && loop->lLoopLength)
	   {
	      srcloop = loop->srcloop;
		   
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
		 //------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
		 // A deferred end: once the countdown runs out, do what the
		 // command's immediate branch does, here, and let the outer loop
		 // run the rest of the block in the new state.
		 if (pLS->bEdgeEndDeferred) {
			 if (pLS->lEdgeDelay <= 0) {
				 pLS->bEdgeEndDeferred = false;
				 if (pLS->lEdgeEndNext != STATE_PLAY) {
					 // Another command (2b-ii, 2b-iii): its transition,
					 // plus what its immediate branch does that
					 // transitionToNext() does not.
					 const int next = pLS->lEdgeEndNext;
					 loop = transitionToNext (pLS, loop, next);
					 // A restart: placed as its immediate branch places
					 // it (the slack window's count, read now, with
					 // Sync on), then IL + OL (RESTART_POSITION_FIX).
					 if (loop && (next == STATE_TRIGGER_PLAY || next == STATE_ONESHOT)) {
						 loop->dCurrPos = restartPosition(pLS, loop, next,
							 fSyncMode >= 1.0f, fSyncOffsetSamples);
#if RESTART_POSITION_FIX
						 loop->dCurrPos += ((long) *pLS->pfInputLatency
							    + (long) *pLS->pfOutputLatency) * pLS->fCurrRate;
#endif
#if ONESHOT_ONE_PASS_FIX
						 // One-pass fix: the pass, less the count
						 // restartPosition() just added.
						 if (next == STATE_ONESHOT)
							 startOneshotPass(pLS, loop, fSyncMode >= 1.0f
								 ? (double) pLS->lSamplesSinceSync + 1.0 : 0.0);
#endif
					 }
					 if (next == STATE_ONESHOT)
						 pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
					 if (pLS->fEdgeEndPulse != 0.0f)
						 pfSyncOutput[lSampleIndex] = pLS->fEdgeEndPulse;
					 break;
				 }
				 // The same command again (MULTI_OVERDUB / REPLACE /
				 // SUBSTITUTE, 2a).
				 pLS->state = STATE_PLAY;
				 pLS->wasMuted = false;
				 DBG(fprintf(stderr,"%u:%u  Deferred layer end: entering PLAY state\n", pLS->lLoopIndex, pLS->lChannelIndex));
				 pLS->fLoopFadeDelta = -1.0f / xfadeSamples;
				 pLS->fPlayFadeDelta = 1.0f / xfadeSamples;
				 pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
				 // then send out a sync here for any slaves
				 pfSyncOutput[lSampleIndex] = 1.0f;
				 break;
			 }
			 pLS->lEdgeDelay--;
		 }
#endif
		 //------ albEnd ----------------------------------
	         fWet += wetDelta;
                 fDry += dryDelta;
	         fFeedback += feedbackDelta;
	         fScratchPos += scratchDelta;

		 pLS->fPlayFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fPlayFadeAtten + pLS->fPlayFadeDelta);
		 pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);
		 

		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
		 //rCurrPos = fmod (loop->dCurrPos - lOutputLatency), loop->lLoopLength);
		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 // lLayerLag further back: where the input read lLayerLag back
		 // was heard (TL_OVER_IL_LAYER_FIX).
		 rCurrPos = fmod (loop->dCurrPos - (fRate * (lOutputLatency + lInputLatency + pLS->lLayerLag)), loop->lLoopLength);
#else
		 rCurrPos = fmod (loop->dCurrPos - (fRate * (lOutputLatency + lInputLatency)), loop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rCurrPos < 0) {
		    rCurrPos += loop->lLoopLength;
		 }
		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 rpCurrPos = fmod (loop->dCurrPos - (fRate * (lOutputLatency + lInputLatency + pLS->lLayerLag)), srcloop->lLoopLength);
#else
		 rpCurrPos = fmod (loop->dCurrPos - (fRate * (lOutputLatency + lInputLatency)), srcloop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rpCurrPos < 0) {
		    rpCurrPos += srcloop->lLoopLength;
		 }
		 
		 pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];

		 if (pLS->lFramesUntilInput <= 0) {
			 rLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + (unsigned int) rCurrPos) & pLS->lBufferSizeMask];
			 rpLoopSample = & pLS->pSampleBuf[(srcloop->lLoopStart + (unsigned int) rpCurrPos) & pLS->lBufferSizeMask];
			 pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
			 pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopSrcFadeAtten + pLS->fLoopSrcFadeDelta);
			 pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
			 lInputReadPos = - pLS->lFramesUntilInput; // negate it
			 lInputReadPos = (lInputReadPos <= pLS->lInputBufWritePos)
				 ? (pLS->lInputBufWritePos - lInputReadPos)
				 : (pLS->lInputBufSize - (lInputReadPos - pLS->lInputBufWritePos)) ;

		 }
		 else { // jlc over
			 DBG(fprintf(stderr, "%u:%u  overdub frames until input: %ld\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->lFramesUntilInput));
			 rLoopSample = 0;
			 rpLoopSample = 0;
			 pLS->lFramesUntilInput--;
			 lInputReadPos = pLS->lInputBufWritePos;
		 }
		 
		 if (fSyncMode != 0) {
			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];

			 pLS->lSamplesSinceSync++;
			 
			 if (pfSyncInput[lSampleIndex] > 1.5f) {
				 // cerr << "od sync reset" << endl;
				 DBG(fprintf(stderr, "%u:%u  TS reset sync in overdub\n", pLS->lLoopIndex, pLS->lChannelIndex));
				 pLS->lSamplesSinceSync = 0;
			 }

			 if (fSyncMode == 2.0f) {
				 
				 if (pLS->recSyncEnded) {
					 // we just synced, need to noitfy slave... this could be problem
					 pfSyncOutput[lSampleIndex] = 2.0f;
					 DBG(fprintf(stderr, "%u:%u  sync out on relsync end on overdub/repl/sub\n", pLS->lLoopIndex, pLS->lChannelIndex));
					 pLS->recSyncEnded = false;
				 }
			 }
			 
		 }
		 else {
			 if (fQuantizeMode == QUANT_OFF 
			     //------ albBegin ----------------------------------
#if SIGNED_SYNC_POS_FIX
			     || (fQuantizeMode == QUANT_CYCLE && onSyncGrid((int) rCurrPos, loop->lSyncPos, loop->lCycleLength))
			     || (fQuantizeMode == QUANT_LOOP && onSyncGrid((int) rCurrPos, loop->lSyncPos, loop->lLoopLength))
#else
			     || (fQuantizeMode == QUANT_CYCLE && (((int)rCurrPos + loop->lSyncPos) % loop->lCycleLength) == 0)
			     || (fQuantizeMode == QUANT_LOOP && (((int) rCurrPos +loop->lSyncPos) % loop->lLoopLength )== 0) 
#endif
			     //------ albEnd ----------------------------------
			     || (fQuantizeMode == QUANT_8TH && (((int) rCurrPos + loop->lSyncPos) % eighthSamples) == 0))
			 {
				 pfSyncOutput[lSampleIndex] = 2.0f;
			 }
		 }
		 
		 if (pLS->waitingForSync && ((fSyncMode == 0.0f && fQuantizeMode == QUANT_OFF) || pfSyncOutput[lSampleIndex] != 0.0f))
		 {
			 DBG(fprintf(stderr,"%u:%u  Finishing synced overdub/replace/subs\n", pLS->lLoopIndex, pLS->lChannelIndex));
			 loop = transitionToNext (pLS, loop, pLS->nextState);
			 pLS->nextState = -1;
			 pLS->waitingForSync = 0;
			 break;
		 }

		 
		 
		 //fInputSample = pfInput[lSampleIndex];
		 fInputSample = pfInputLatencyBuf[(lInputReadPos + lSampleIndex) & pLS->lInputBufMask];

		 //  xfade input into source loop (for cases immediately after record)
		 if (rpLoopSample) {
			 *(rpLoopSample) = ((*rpLoopSample) * pLS->fFeedSrcFadeAtten) +  pLS->fLoopSrcFadeAtten * fInputSample;
		 }

		 if (pLS->lFramesUntilFilled > 0) {
			 // fill from the record position * and for the play pos !!?
			 //DBG(fprintf(stderr, "filling rcurrpos=%u  pppos %d\n", (unsigned int) rCurrPos, lCurrPos));
			 fillLoops(pLS, loop, (unsigned int) rCurrPos, true);
			 pLS->lFramesUntilFilled--;
		 }

		 fillLoops(pLS, loop, lCurrPos, false);

		 
		 switch(pLS->state)
		 {
		 case STATE_OVERDUB:
			 // use our self as the source (we have been filled by the call above)
			 fOutputSample = fWet  *  *(pLoopSample)
				 + fDry * fInputSample;

			 if (rLoopSample) {
				 *(rLoopSample) =  
					 ((pLS->fLoopFadeAtten * fInputSample) + (fSafetyFeedback * pLS->fFeedFadeAtten * fFeedback *  *(rLoopSample)));
			 }
			 break;
		 case STATE_REPLACE:
			 // state REPLACE use only the new input
			 // use our self as the source (we have been filled by the call above)
			 fOutputSample = pLS->fPlayFadeAtten * fWet  *  *(pLoopSample)
				 + fDry * fInputSample;
			 
			 if (rLoopSample) {
				 *(rLoopSample) = fInputSample * pLS->fLoopFadeAtten +  (pLS->fFeedFadeAtten * fFeedback *  *(rLoopSample));
			 }
			 break;
		 case STATE_SUBSTITUTE:
		 default:
			 // use our self as the source (we have been filled by the call above)
			 // hear the loop
			 fOutputSample = fWet  *  *(pLoopSample)
				 + fDry * fInputSample;

			 // but not feed it back (xfade it really)
			 if (rLoopSample) {
				 *(rLoopSample) = fInputSample * pLS->fLoopFadeAtten + (pLS->fFeedFadeAtten * fFeedback *  *(rLoopSample));
			 }
			 break;
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;
		 

		 // increment and wrap at the proper loop end
		 loop->dCurrPos = loop->dCurrPos + fRate;

		 //if (fSyncMode != 0.0 && pLS->fNextCurrRate != 0 && pfSyncInput[lSampleIndex] != 0.0) {
		 if (pLS->fNextCurrRate != 0 && pfSyncOutput[lSampleIndex] > 1.5f) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0f;
		       DBG(fprintf(stderr, "%u:%u  Starting quantized rate change ovr at %d\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos));
		 }

		 
		 if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;

		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0f;
		       DBG(fprintf(stderr, "%u:%u  Starting quantized rate change\n", pLS->lLoopIndex, pLS->lChannelIndex));
		    }

		 }
		 else if (loop->dCurrPos >= loop->lLoopLength) {
		    // wrap around length
		    loop->dCurrPos = fmod(loop->dCurrPos, loop->lLoopLength);
		    if (pLS->fNextCurrRate != 0) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0f;
		       DBG(fprintf(stderr, "%u:%u  Starting quantized rate change\n", pLS->lLoopIndex, pLS->lChannelIndex));
		    }
		 }
		 
	      }


	      
	   }
	   else {
	      goto passthrough;

	   }
	   
	   
	} break;


	case STATE_MULTIPLY:
	{
	   if (loop && loop->lLoopLength && loop->srcloop)
	   {
	      srcloop = loop->srcloop;
	      firsttime = loop->firsttime;

	      if (pLS->nextState == STATE_MUTE) {
		 // no loop output
		 fWet = 0.0f;
		 wetDelta = 0.0f;
	      }
	      
	      loop = ensureLoopSpace (pLS, loop, SampleCount - lSampleIndex, NULL);

	      if (!loop || (loop->srcloop && !loop->srcloop->valid)) {
		      // out of space! give up for now!
		      // undo!
		      DBG(fprintf(stderr,"Multiply fail done! Out of memory or lost source!\n"));
		      if (loop) {
			      loop = endMultiply(pLS, loop, STATE_PLAY);
		      }
		      else {
			      pLS->rounding = false;
			      pLS->state = STATE_PLAY;
			      pLS->wasMuted = false;
			      undoLoop(pLS, false);
			      goto passthrough;
		      }
			      
		      break;
	      }
	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
	         fWet += wetDelta;
                 fDry += dryDelta;
	         fFeedback += feedbackDelta;
	         fScratchPos += scratchDelta;



		 
// 		 if (slCurrPos > 0 && (unsigned)(loop->pLoopStart + slCurrPos)
// 		     > (unsigned)(pLS->pSampleBuf + pLS->lBufferSize)) {
// 		    // out of space! give up for now!
// 		    // undo!
// 		    pLS->state = STATE_PLAY;
// 		    undoLoop(pLS);
// 		    DBG(fprintf(stderr,"Multiply Undone! Out of memory!\n"));
// 		    break;
// 		 }

		 lpCurrPos =(unsigned int) fmod(loop->dCurrPos + loop->lStartAdj, srcloop->lLoopLength);
		 slCurrPos =(long) loop->dCurrPos;

		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 // lLayerLag further back (TL_OVER_IL_LAYER_FIX).
		 rCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency - pLS->lLayerLag, loop->lLoopLength);
#else
		 rCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency, loop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rCurrPos < 0) {
		    rCurrPos += loop->lLoopLength;
		 }
		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 rpCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency - pLS->lLayerLag, srcloop->lLoopLength);
#else
		 rpCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency, srcloop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rpCurrPos < 0) {
		    rpCurrPos += srcloop->lLoopLength;
		 }
		 
		 spLoopSample = & pLS->pSampleBuf[(srcloop->lLoopStart + lpCurrPos) & pLS->lBufferSizeMask];
		 pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + slCurrPos) & pLS->lBufferSizeMask];

		 if (pLS->lFramesUntilInput <= 0) {
			 rLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + (unsigned int) rCurrPos) & pLS->lBufferSizeMask];
			 rpLoopSample = & pLS->pSampleBuf[(srcloop->lLoopStart + (unsigned int) rpCurrPos) & pLS->lBufferSizeMask];
			 pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
			 pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopSrcFadeAtten + pLS->fLoopSrcFadeDelta);
			 pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);
			 pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
			 lInputReadPos = - pLS->lFramesUntilInput; // negate it
			 lInputReadPos = (lInputReadPos <= pLS->lInputBufWritePos)
				 ? (pLS->lInputBufWritePos - lInputReadPos)
				 : (pLS->lInputBufSize - (lInputReadPos - pLS->lInputBufWritePos)) ;

		 }
		 else {
			 rLoopSample = 0;
			 rpLoopSample = 0;
			 pLS->lFramesUntilInput--;
			 lInputReadPos = pLS->lInputBufWritePos;
		 }
		 
		 //pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
		 //pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);


		 
		 if (fSyncMode != 0) {
			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];

			 if (fSyncMode == 2.0f) {
				 pLS->lSamplesSinceSync++;
				 
				 if (pfSyncInput[lSampleIndex] > 1.5f) {
					 // cerr << "mt sync reset" << endl;
					 DBG(fprintf(stderr, "%u:%u  TS reset sync in multiply\n", pLS->lLoopIndex, pLS->lChannelIndex));
					 pLS->lSamplesSinceSync = 0;
				 }
				 else if (pLS->recSyncEnded) {
					 // we just synced, need to noitfy slave... this could be problem
					 pfSyncOutput[lSampleIndex] = 2.0f;
					 DBG(fprintf(stderr, "%u:%u  sync out on relsync end on multiply\n", pLS->lLoopIndex, pLS->lChannelIndex));

					 //cerr << "mt notified: " <<  endl;
					 pLS->recSyncEnded = false;
				 }
			 }
			 
		 }
		 else {
			 if (fQuantizeMode == QUANT_OFF 
			     //------ albBegin ----------------------------------
#if SIGNED_SYNC_POS_FIX
			     || (fQuantizeMode == QUANT_CYCLE && onSyncGrid((int) rCurrPos, loop->lSyncPos, loop->lCycleLength)))
#else
			     || (fQuantizeMode == QUANT_CYCLE && (((int) rCurrPos + loop->lSyncPos) % loop->lCycleLength) == 0))
#endif
			     //------ albEnd ----------------------------------
			 {
				 pfSyncOutput[lSampleIndex] = 2.0f;
			 }
		 }
		 
		 
		 if (pLS->waitingForSync && (fSyncMode == 0.0f || pfSyncOutput[lSampleIndex] != 0.0f || slCurrPos  >= (long)(loop->lLoopLength)))
		 {
			 DBG(fprintf(stderr,"%u:%u  Finishing synced multiply\n", pLS->lLoopIndex, pLS->lChannelIndex));
			 loop = endMultiply (pLS, loop, pLS->nextState);

			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
			 
			 pLS->waitingForSync = 0;
			 break;
		 }

		 
		 
		 fInputSample = pfInputLatencyBuf[(lInputReadPos + lSampleIndex) & pLS->lInputBufMask];
		 //fInputSample = pfInput[lSampleIndex];



		 //  xfade input into source loop (for cases immediately after record)
		 if (rpLoopSample) {
			 *(rpLoopSample) = ((*rpLoopSample) * pLS->fFeedSrcFadeAtten) +  pLS->fLoopSrcFadeAtten * fInputSample;
		 }

		 if (pLS->lFramesUntilFilled > 0) {
			 // fill source from the record position
			 fillLoops(pLS, loop, (unsigned int) rpCurrPos, true);
			 pLS->lFramesUntilFilled--;
		 }
		 
		 //fillLoops(pLS, loop, lpCurrPos, false);
		 fillLoops(pLS, loop, slCurrPos, false);
		 
		 
		 
		 // always use the source loop as the source
		 
		 fOutputSample = (fWet *  (*spLoopSample)
				  + fDry * fInputSample);


		 if (slCurrPos < 0) {
		    // this is part of the loop that we need to ignore
		    // fprintf(stderr, "Ignoring at %ul\n", lCurrPos);
		 }
		 else if ((loop->lCycles <=1 && fQuantizeMode != 0)) {
			 // do not include the new input
			 if (rLoopSample) {
				 *(rLoopSample)
					 = pLS->fFeedFadeAtten * fFeedback *  (*rpLoopSample);
			 }
			 //*(pLoopSample)
			 //	 = pLS->fFeedFadeAtten * fFeedback *  (*spLoopSample);

		 }
		 if ((slCurrPos > (long) loop->lMarkEndL &&  *pLS->pfRoundMode == 0)) {
			 // do not include the new input (at end) when not rounding
			 pLS->fLoopFadeDelta = -1.0f / xfadeSamples;

			 if (rLoopSample) {
				 *(rLoopSample) =  
					 ((pLS->fLoopFadeAtten * fInputSample) + (pLS->fFeedFadeAtten * fFeedback *  *(rpLoopSample)));
			 }
			 
			 //*(pLoopSample)
			 //	 = (pLS->fFeedFadeAtten * fFeedback *  (*spLoopSample)) +  (pLS->fLoopFadeAtten * fInputSample);
			 // fprintf(stderr, "Not including input at %ul\n", lCurrPos);
		 }
		 else {
			 pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
	      
			 if (rLoopSample) {
				 *(rLoopSample) =  
					 ((pLS->fLoopFadeAtten * fInputSample) + (pLS->fFeedFadeAtten * fSafetyFeedback * fFeedback *  *(rpLoopSample)));
			 }
			 //*(pLoopSample)
			 //	 = ( (pLS->fLoopFadeAtten * fInputSample) + (pLS->fFeedFadeAtten * fSafetyFeedback *  fFeedback * (*spLoopSample)));
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;

		 
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;
	      

		 // ASSUMPTION: our rate is +1 only		 
		 if ((unsigned long)loop->dCurrPos  >= (loop->lLoopLength)) {


			 if (loop->mult_out == (int) loop->lCycles
			     || (unsigned long)loop->dCurrPos >= loop->lMarkEndH) {
				 //------ albBegin ----------------------------------
				 // STOCK (sooperlooper 1.7.9) finished here, when the play
				 // position reached the end:
				 //   loop->lMarkEndH = LONG_MAX;
				 //   backfill = loop->backfill = 0;
				 //   loop->dCurrPos = 0.0f;
				 //   loop->lLoopLength = loop->lCycles * loop->lCycleLength;
				 //   pLS->rounding = false;
				 //   loop = transitionToNext(pLS, loop, pLS->nextState);
				 //   break;
				 // MULTIPLY_END_FIX: keep multiplying, without adding a cycle,
				 // until the record position (IL + OL behind) reaches the end
				 // too, so the loop's last IL + OL samples get written; then
				 // finish with the play position carried on, not reset.
				 // This sample is done (its position advanced past it), so
				 // the next state starts on the next one, not on it again.
#if MULTIPLY_END_FIX
#if TL_OVER_IL_LAYER_FIX
				 // TL_OVER_IL_LAYER_FIX: the record position is lLayerLag
				 // further back.
				 if (loop->dCurrPos < (double) loop->lLoopLength + (double) (lInputLatency + lOutputLatency + pLS->lLayerLag)) {
					 continue;
				 }
#else
				 if (loop->dCurrPos < (double) loop->lLoopLength + (double) (lInputLatency + lOutputLatency)) {
					 continue;
				 }
#endif
				 backfill = 0;
				 loop = finishRoundedMultiply(pLS, loop);
				 ++lSampleIndex;
				 break;
#else
				 // we be done this only happens in round mode
				 // adjust curr position
				 loop->lMarkEndH = LONG_MAX;
				 backfill = loop->backfill = 0;
				 // do adjust it for our new length
				 loop->dCurrPos = 0.0f;
				 
				 loop->lLoopLength = loop->lCycles * loop->lCycleLength;
				 pLS->rounding = false;
				 // fprintf(stderr, "mult is over with %d cyc\n", loop->lCycles);
				 loop = transitionToNext(pLS, loop, pLS->nextState);
				 break;
#endif
				 //------ albEnd ----------------------------------
			 }

			 
			 // increment cycle and looplength
			 loop->lCycles += 1;
			 loop->lLoopLength += loop->lCycleLength;
			 //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
			 // this signifies the end of the original cycle
			 loop->firsttime = 0;
			 DBG(fprintf(stderr,"%u:%u  Multiply added cycle %lu  at %g\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lCycles, loop->dCurrPos));
			 
			 // now we set this to rise in case we were quantized
			 pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
			 pLS->fFeedFadeDelta = 1.0f / xfadeSamples;
			 
			 loop = ensureLoopSpace (pLS, loop, SampleCount - lSampleIndex, NULL);
			 if (!loop) {
				 // out of space! give up for now!
				 // undo!
				 pLS->state = STATE_PLAY;
				 pLS->wasMuted = false;
				 undoLoop(pLS, false);
				 DBG(fprintf(stderr,"dfMultiply Undone! Out of memory!\n"));
				 break;
			 }
			 
		 }
	      }
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;

	case STATE_INSERT:
	{
	   if (loop && loop->lLoopLength && loop->srcloop)
	   {
	      srcloop = loop->srcloop;
	      firsttime = loop->firsttime;

	      if (pLS->nextState == STATE_MUTE) {
		 // no loop output
		 fWet = 0.0f;
		 wetDelta = 0.0f;
	      }
	      

	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
	         fWet += wetDelta;
                 fDry += dryDelta;
	         fFeedback += feedbackDelta;
	         fScratchPos += scratchDelta;

		 pLS->fPlayFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fPlayFadeAtten + pLS->fPlayFadeDelta);
		 //pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
		 //pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);
		 
		 lpCurrPos =(unsigned int) fmod(loop->dCurrPos, srcloop->lLoopLength);
		 lCurrPos =(unsigned int) loop->dCurrPos;

		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 // lLayerLag further back (TL_OVER_IL_LAYER_FIX).
		 rCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency - pLS->lLayerLag, loop->lLoopLength);
#else
		 rCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency, loop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rCurrPos < 0) {
		    rCurrPos += loop->lLoopLength;
		 }
		 //------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
		 rpCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency - pLS->lLayerLag, srcloop->lLoopLength);
#else
		 rpCurrPos = fmod (loop->dCurrPos - lOutputLatency - lInputLatency, srcloop->lLoopLength);
#endif
		 //------ albEnd ----------------------------------
		 if (rpCurrPos < 0) {
		    rpCurrPos += srcloop->lLoopLength;
		 }

		 spLoopSample = & pLS->pSampleBuf[(srcloop->lLoopStart + lpCurrPos) & pLS->lBufferSizeMask];
		 pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];

		 if (pLS->lFramesUntilInput <= 0) {
			 rLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + (unsigned int) rCurrPos) & pLS->lBufferSizeMask];
			 rpLoopSample = & pLS->pSampleBuf[(srcloop->lLoopStart + (unsigned int) rpCurrPos) & pLS->lBufferSizeMask];
			 pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
			 pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopSrcFadeAtten + pLS->fLoopSrcFadeDelta);
			 pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);
			 pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
			 lInputReadPos = - pLS->lFramesUntilInput; // negate it
			 lInputReadPos = (lInputReadPos <= pLS->lInputBufWritePos)
				 ? (pLS->lInputBufWritePos - lInputReadPos)
				 : (pLS->lInputBufSize - (lInputReadPos - pLS->lInputBufWritePos)) ;

		 }
		 else {
			 rLoopSample = 0;
			 rpLoopSample = 0;
			 pLS->lFramesUntilInput--;
			 lInputReadPos = pLS->lInputBufWritePos;
		 }

		 
		 //fInputSample = pfInput[lSampleIndex];
		 fInputSample = pfInputLatencyBuf[(lInputReadPos + lSampleIndex) & pLS->lInputBufMask];

		 // xfade input into source loop (for cases immediately after record)
		 if (rpLoopSample) {
			 *(rpLoopSample) = ((*rpLoopSample) * pLS->fFeedSrcFadeAtten) +  pLS->fLoopSrcFadeAtten * fInputSample;
		 }

		 // fill from the record position
		 if (pLS->lFramesUntilFilled > 0) {
			 fillLoops(pLS, loop, (unsigned int) rCurrPos, true);
			 pLS->lFramesUntilFilled--;
		 }
		 
		 fillLoops(pLS, loop, lCurrPos, false);
		 

		 
		 if (firsttime && *pLS->pfQuantMode != 0 )
		 {
		    // just the source and input
			 fOutputSample = (pLS->fPlayFadeAtten * fWet *  (*spLoopSample))
				 + fDry * fInputSample;
		    
		    // do not include the new input
		    //*(loop->pLoopStart + lCurrPos)
		    //  = fFeedback *  *(srcloop->pLoopStart + lpCurrPos);
		    //*(pLoopSample) = (pLS->fFeedFadeAtten * fFeedback *  (*pLoopSample));
		 }
		 else if (lCurrPos > loop->lMarkEndL && *pLS->pfRoundMode == 0)
		 {
		    // insert zeros, we finishing an insert with nothingness
		    pLS->fLoopFadeDelta = -1.0f / xfadeSamples;

		    fOutputSample = fDry * fInputSample;

		    if (rLoopSample) {
			    *(rLoopSample) = fInputSample * pLS->fLoopFadeAtten;
		    }

		 }
		 else {
		    // just the input we are now inserting
		    pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
		    pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
		    pLS->fPlayFadeDelta = -1.0f / xfadeSamples;

		    fOutputSample = fDry * fInputSample  + (pLS->fPlayFadeAtten * fWet *  (*spLoopSample));

		    if (rLoopSample) {
			    *(rLoopSample) = (fInputSample * pLS->fLoopFadeAtten) + (pLS->fFeedFadeAtten * fFeedback *  (*rLoopSample));
		    }

		 }
		 
		 
		 pfOutput[lSampleIndex] = fOutputSample;

		 if (fSyncMode != 0) {
			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];

			 pLS->lSamplesSinceSync++;
			 
			 if (pfSyncInput[lSampleIndex] > 1.5f) {
				 //cerr << "ins sync reset" << endl;
				 pLS->lSamplesSinceSync = 0;
			 }

			 if (fSyncMode == 2.0f) {
				 if (pLS->recSyncEnded) {
					 // we just synced, need to noitfy slave... this could be problem
					 pfSyncOutput[lSampleIndex] = 2.0f;
					 DBG(fprintf(stderr, "%u:%u  sync out on relsync end on insert\n", pLS->lLoopIndex, pLS->lChannelIndex));

					 //cerr << "ins notified" << endl;
					 pLS->recSyncEnded = false;
				 }
			 }
		 }
		 else if (fQuantizeMode == QUANT_OFF 
			  //------ albBegin ----------------------------------
#if SIGNED_SYNC_POS_FIX
			  || (fQuantizeMode == QUANT_CYCLE && onSyncGrid((int) rCurrPos, loop->lSyncPos, loop->lCycleLength))
			  || (fQuantizeMode == QUANT_LOOP && onSyncGrid((int) rCurrPos, loop->lSyncPos, loop->lLoopLength))
#else
			  || (fQuantizeMode == QUANT_CYCLE && (((int) rCurrPos + loop->lSyncPos) % loop->lCycleLength) == 0)
			  || (fQuantizeMode == QUANT_LOOP && (((int)rCurrPos + loop->lSyncPos) % loop->lLoopLength)== 0) 
#endif
			  //------ albEnd ----------------------------------
			  || (fQuantizeMode == QUANT_8TH && (((int)rCurrPos + loop->lSyncPos) % eighthSamples) == 0)) {
			 pfSyncOutput[lSampleIndex] = 2.0f;
		 }
		 
		 
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;
	      

		 
		 if ((unsigned long)loop->dCurrPos >= loop->lMarkEndH) {
		    // we be done.. this only happens in round mode
		    // adjust curr position to 0

		    
		    loop->lMarkEndL = (unsigned long) loop->dCurrPos;
		    loop->lMarkEndH = loop->lLoopLength - 1;
		    backfill = loop->backfill = 1;
		    pLS->rounding = false;
		    loop->lLoopLength = loop->lCycles * loop->lCycleLength;
		    
		    DBG(fprintf(stderr, "%u:%u  Looplength = %lu   cycles=%lu\n", pLS->lLoopIndex, pLS->lChannelIndex, loop->lLoopLength, loop->lCycles));
		    
		    loop = transitionToNext(pLS, loop, pLS->nextState);
		    DBG(fprintf(stderr,"%u:%u  Entering state %d from insert\n", pLS->lLoopIndex, pLS->lChannelIndex, pLS->state));
		    break;
		 }

		 // ASSUMPTION: our rate is +1 only		 
		 if (firsttime && ((unsigned long)rCurrPos % loop->lCycleLength) == 0)
		 {
		    firsttime = loop->firsttime = 0;
		    DBG(fprintf(stderr, "first time done\n"));
		    // now we set this to rise in case we were quantized
		    pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
		    pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
		 }
		 
		 if ((lCurrPos % loop->lCycleLength) == ((loop->lInsPos-1) % loop->lCycleLength)) {

			 if ((unsigned)(loop->lLoopLength + loop->lCycleLength)
			     > (unsigned)(pLS->lBufferSize))
				 
			 {
				 // out of space! give up for now!
				 pLS->rounding = false;
				 pLS->state = STATE_PLAY;
				 pLS->wasMuted = false;
				 //undoLoop(pLS);
				 DBG(fprintf(stderr,"Insert finish early! Out of memory!\n"));
				 break;
			 }
			 else {
				 // increment cycle and looplength
				 loop->lCycles += 1;
				 loop->lLoopLength += loop->lCycleLength;
				 //loop->lLoopStop = loop->lLoopStart + loop->lLoopLength;
				 // this signifies the end of the original cycle
				 DBG(fprintf(stderr,"insert added cycle. Total=%lu\n", loop->lCycles));
				 // now we set this to rise in case we were quantized
				 pLS->fLoopFadeDelta = 1.0f / xfadeSamples;
				 pLS->fFeedFadeDelta = -1.0f / xfadeSamples;
			 }
		 }
	      }
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;
	
	case STATE_UNDO_ALL:
	case STATE_REDO_ALL:
	case STATE_UNDO:
	case STATE_REDO:
	case STATE_PLAY:
	case STATE_ONESHOT:
	case STATE_SCRATCH:
	case STATE_MUTE:
	case STATE_PAUSED:
	{
	   //fprintf(stderr,"in play begin\n");	   
	   // play  the input out mixed with the recorded loop.
	   if (loop && loop->lLoopLength)
	   {
	      tmpWet = fWet;
	      
	      if(pLS->state == STATE_SCRATCH)
	      {
	      
		 // calculate new rate if rateSwitch is on
		 fPosRatio = (loop->dCurrPos / loop->lLoopLength);
	      
		 if (pLS->fLastScratchVal != fScratchPos
		     && pLS->lScratchSamples > 0) {
		    // we have a change in scratching pos. Find new rate

		    if (pLS->lScratchSamples < 14000) {
		       pLS->fCurrScratchRate = (fScratchPos - fPosRatio) * loop->lLoopLength
			  / pLS->lScratchSamples;

		    }
		    else if (pLS->bRateCtrlActive && pLS->pfRate) {
		       fRate = *pLS->pfRate;
		    }
		    else {
		       fRate = 0.0f;
		    }
		    
		    pLS->lScratchSamples = 0;
		    pLS->fLastScratchVal = fScratchPos;


		    
		    //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  \n", fScratchPos,
		    //   pLS->fCurrScratchRate);
		 
		 }
		 else if (fabs(pLS->fCurrScratchRate) < 0.2f
			  || ( pLS->lScratchSamples > 14000)
			  || ( pLS->fCurrScratchRate > 0.0f && (fPosRatio >= pLS->fLastScratchVal ))
			  || ( pLS->fCurrScratchRate < 0.0f && (fPosRatio <= pLS->fLastScratchVal )))
		 {
		    // we have reached the destination, no more scratching
		    pLS->fCurrScratchRate = 0.0f;

		    if (pLS->bRateCtrlActive && pLS->pfRate) {
		       fRate = *pLS->pfRate;
		    }
		    else {
		       // pure scratching
		       fRate = 0.0f;
		    }
		    //fprintf(stderr, "fScratchPos: %f   fCurrScratchRate: %f  ******\n", fScratchPos,
		    //	   pLS->fCurrScratchRate);
		 
		 }
		 else {
		    fRate = pLS->fCurrScratchRate;
		 }

	      }


	      srcloop = loop->srcloop;

	      bool recenter = true;

	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {

		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
		 //fprintf(stderr, "curr = %u\n", lCurrPos);
		 pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];
			  
                 xLoopSample = 0; // init to nil
                          if (pLS->state == STATE_UNDO){
				  prevloop = pLS->headLoopChunk->prev;
				  if (prevloop) {
                                          xCurrPos = (unsigned int) fmod(loop->dCurrPos, prevloop->lLoopLength);
                                          xLoopSample = & pLS->pSampleBuf[(prevloop->lLoopStart + xCurrPos) & pLS->lBufferSizeMask];
                                  }
			  }
			  if (pLS->state == STATE_REDO) {
				  nextloop = pLS->headLoopChunk->next;
                                  if (nextloop) {
                                          xCurrPos = (unsigned int) fmod(loop->dCurrPos, nextloop->lLoopLength);
                                          xLoopSample = & pLS->pSampleBuf[(nextloop->lLoopStart + xCurrPos) & pLS->lBufferSizeMask];
                                  }
			  }
			  if (pLS->state == STATE_REDO_ALL) {
				  nextloop = pLS->headLoopChunk;
				  while (nextloop->next) {
					  nextloop = nextloop->next;
				  }
                                  if (nextloop) {
                                          xCurrPos = (unsigned int) fmod(loop->dCurrPos, nextloop->lLoopLength);
                                          xLoopSample = & pLS->pSampleBuf[(nextloop->lLoopStart + xCurrPos) & pLS->lBufferSizeMask];
                                  }
			  }

		 rCurrPos = fmod (loop->dCurrPos - (fRate * (lOutputLatency + lInputLatency)), loop->lLoopLength);
		 if (rCurrPos < 0) {
			 rCurrPos += loop->lLoopLength;
		 }

		 rLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + (unsigned int) rCurrPos) & pLS->lBufferSizeMask];
		 lInputReadPos = pLS->lInputBufWritePos;

		 if (rCurrPos == loop->lLoopLength-1) {
			 //DBG(cerr << "In play rcurrpos == : " << rCurrPos << "  val: " << *pLoopSample << endl;);
		 }

		 
		 
		 if (fSyncMode != 0.0f) {
			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];

			 pLS->lSamplesSinceSync++;
			 
			 if (pfSyncInput[lSampleIndex] > 1.5f) {
				 //DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex << " TS sync reset at: " << loop->dCurrPos << "  with since: " << pLS->lSamplesSinceSync << endl);
				 pLS->lSamplesSinceSync = 0;
			 }

			 if (fSyncMode == 2.0f) {
				 if (pLS->recSyncEnded) {
					 // we just synced, need to noitfy slave... this could be problem
					 pfSyncOutput[lSampleIndex] = 1.0f;
					 DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex << "  notified rel synced at " << loop->dCurrPos << endl);
					 pLS->recSyncEnded = false;
				 }
			 }
		 }
		 else if (fQuantizeMode == QUANT_OFF 
			  //------ albBegin ----------------------------------
#if SIGNED_SYNC_POS_FIX
			  || (fQuantizeMode == QUANT_CYCLE && onSyncGrid(lCurrPos, loop->lSyncPos, loop->lCycleLength))
			  || (fQuantizeMode == QUANT_LOOP && onSyncGrid(lCurrPos, loop->lSyncPos, loop->lLoopLength))
#else
			  || (fQuantizeMode == QUANT_CYCLE && ((lCurrPos + loop->lSyncPos) % loop->lCycleLength) == 0)
			  || (fQuantizeMode == QUANT_LOOP && (((lCurrPos + loop->lSyncPos) % loop->lLoopLength) == 0)) 
#endif
			  //------ albEnd ----------------------------------
			  || (fQuantizeMode == QUANT_8TH && ((lCurrPos + loop->lSyncPos) % eighthSamples) == 0)) {
			 pfSyncOutput[lSampleIndex] = 2.0f;
		 }


 		 if (pLS->fNextCurrRate != 0 && pfSyncOutput[lSampleIndex] > 1.5f && fTempo > 0.0f) {
 		       // commit the new rate at boundary (quantized)
 		       pLS->fCurrRate = pLS->fNextCurrRate;
 		       pLS->fNextCurrRate = 0.0f;
 		       DBG(fprintf(stderr, "%u%u  Starting quantized rate change at %d  %g\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos, pfSyncOutput[lSampleIndex]));
 		 }
		 
		 // test playback sync

		 if (syncSamples && fPlaybackSyncMode != 0.0f && fQuantizeMode != QUANT_OFF && !pLS->donePlaySync
		     && ( pfSyncInput[lSampleIndex] > 1.5f)
		     && (labs((long)(loop->lLoopLength - loop->lSyncPos) - lCurrPos) < syncSamples
		     	 || (lCurrPos + loop->lSyncPos) < syncSamples))
		 {
			 DBG(cerr << pLS->lLoopIndex << ":" << pLS->lChannelIndex  << "PLAYBACK SYNC " <<  "  hit at " << lCurrPos << endl);
			 //pLS->waitingForSync = 1;
			 pLS->donePlaySync = true;

			 if (pLS->fCurrRate > 0)
				 loop->dCurrPos = (double) (loop->lLoopLength - loop->lSyncPos) + fSyncOffsetSamples;
			 else
				 loop->dCurrPos = (loop->lLoopLength - loop->lSyncPos) - 1 - fSyncOffsetSamples;

			 pfSyncOutput[lSampleIndex] = 2.0f;
		 }
		 
		 
		 if (pLS->waitingForSync && 
		     ((fSyncMode == 0.0f && pfSyncOutput[lSampleIndex] != 0.0f) 
		      || pfSyncInput[lSampleIndex] != 0.0f
		      ||  (pLS->nextState == STATE_TRIGGER_PLAY && fSyncMode >= 1.0f && pLS->lSamplesSinceSync < eighthSamples))) // some slack
		 {
			 loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy);
				 
			 DBG(fprintf(stderr, "%u:%u  transition to next at: %lu: %u  %g  : %lu\n", pLS->lLoopIndex, pLS->lChannelIndex, lSampleIndex, lCurrPos, loop->dCurrPos, loop->lLoopLength));
			 loop = transitionToNext (pLS, loop, pLS->nextState);
			 if (loop)  srcloop = loop->srcloop;
			 else srcloop = NULL;
			 pLS->waitingForSync = 0;
			 recenter = false;
			 break;
		 }

		 fWet += wetDelta;
                 fDry += dryDelta;
     	         fFeedback += feedbackDelta;
	         fScratchPos += scratchDelta;
		 pLS->fLoopFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopFadeAtten + pLS->fLoopFadeDelta);
		 pLS->fLoopSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fLoopSrcFadeAtten + pLS->fLoopSrcFadeDelta);
		 pLS->fFeedSrcFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedSrcFadeAtten + pLS->fFeedSrcFadeDelta);
		 pLS->fPlayFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fPlayFadeAtten + pLS->fPlayFadeDelta);
		 pLS->fFeedFadeAtten = LIMIT_BETWEEN_0_AND_1 (pLS->fFeedFadeAtten + pLS->fFeedFadeDelta);

		 tmpWet = fWet;
		 
		 //if (pLS->fPlayFadeAtten != 0.0f && pLS->fPlayFadeAtten != 1.0f) {
			 //cerr << "play fade: " << pLS->fPlayFadeAtten << endl;
		 //}
  
		      
		 tmpWet *= pLS->fPlayFadeAtten;

// 		 // modify fWet if we are in a ramp up/down
// 		 if (pLS->lRampSamples > 0) {
// 		    if (pLS->state == STATE_MUTE) {
// 		       //negative linear ramp
// 		       tmpWet = fWet * (pLS->lRampSamples * 1.0f) / xfadeSamples;
// 		    }
// 		    else {
// 		       // positive linear ramp
// 		       tmpWet = fWet * (xfadeSamples - pLS->lRampSamples)
// 			  * 1.0f / xfadeSamples;
// 		    }

// 		    pLS->lRampSamples -= 1;
// 		 }

		 
		 //fInputSample = pfInput[lSampleIndex];
		 fInputSample = pfInputLatencyBuf[(lInputReadPos + lSampleIndex) & pLS->lInputBufMask];

		 // fill from the record position ??
		 if (pLS->lFramesUntilFilled > 0) {
			 fillLoops(pLS, loop, (unsigned int) rCurrPos, true);
			 pLS->lFramesUntilFilled--;
		 }
		 
		 fillLoops(pLS, loop, lCurrPos, false);

	      
		 
		 fOutputSample =   tmpWet *  (*pLoopSample)
		    + fDry * fInputSample;
		 if (xLoopSample && (pLS->state == STATE_UNDO || pLS->state == STATE_REDO || pLS->state == STATE_REDO_ALL)) {
			 //fprintf(stderr, "fading.. :%g\n", tmpWet);
			 fOutputSample =   tmpWet *  (*xLoopSample)
			 + fDry * fInputSample + (fWet-tmpWet) * (*pLoopSample);
		 }

		 // jlc play
		 // we might add a bit from the input still during xfadeout
		 *(rLoopSample) = ((*rLoopSample) * pLS->fFeedFadeAtten) +  pLS->fLoopFadeAtten * fInputSample;
		 // if (pLS->fLoopFadeAtten > 0.9 && pLS->fLoopFadeAtten < 1) fprintf(stderr, "fLoopFadeAtten: %g, SampleIndex: %d\n", pLS->fLoopFadeAtten, lCurrPos);

		 // optionally support feedback during playback (use rLoopSample??)
		 if (useFeedbackPlay) {
			 *(pLoopSample) *= fFeedback * pLS->fFeedFadeAtten;
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;
			  
			  

		 if (pLS->state == STATE_PAUSED && pLS->fPlayFadeAtten == 0.0f) {
			 // do not increment time
		 }
		 else {
			 // increment and wrap at the proper loop end
			 loop->dCurrPos = loop->dCurrPos + fRate;
			 //------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
			 // The oneshot ends after one pass from its start, not at
			 // the buffer's end (the tests below are off).
			 if (pLS->state == STATE_ONESHOT) {
				 pLS->dOneshotLeft -= fabs(fRate);
				 if (pLS->dOneshotLeft <= 0.0) {
					 DBG(fprintf(stderr, "%u:%u  finished ONESHOT pass\n", pLS->lLoopIndex, pLS->lChannelIndex));
					 pLS->state = STATE_MUTE;
					 pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
				 }
			 }
#endif
			 //------ albEnd ----------------------------------
		 }

 		 if (pLS->fNextCurrRate != 0 && pfSyncOutput[lSampleIndex] > 1.5f && fTempo > 0.0f) {
 		       // commit the new rate at boundary (quantized)
 		       pLS->fCurrRate = pLS->fNextCurrRate;
 		       pLS->fNextCurrRate = 0.0f;
 		       DBG(fprintf(stderr, "%u:%u   Starting quantized rate change at %d\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos));
 		 }
		 
		 if (loop->dCurrPos >= loop->lLoopLength) {
		    //------ albBegin ----------------------------------
#if !ONESHOT_ONE_PASS_FIX
		    //------ albEnd ----------------------------------
		    if (pLS->state == STATE_ONESHOT) {
		       // done with one shot
			    DBG(fprintf(stderr, "%u:%u  finished ONESHOT  lcurrPos=%d\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos));
		       pLS->state = STATE_MUTE;
		       pLS->fPlayFadeDelta = -1.0f / xfadeSamples;

		       //pLS->lRampSamples = xfadeSamples;
		       //fWet = 0.0;
		    }
		    //------ albBegin ----------------------------------
#endif
		    //------ albEnd ----------------------------------

		    if (pLS->fNextCurrRate != 0 && pfSyncOutput[lSampleIndex] > 1.5f) {
			    // commit the new rate at boundary (quantized)
			    pLS->fCurrRate = pLS->fNextCurrRate;
			    pLS->fNextCurrRate = 0.0f;
			    DBG(fprintf(stderr, "%u:%u (2) Starting quantized rate change at %d\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos));
		    }

		    pLS->donePlaySync = false;
		 }
		 else if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;
		    //------ albBegin ----------------------------------
#if !ONESHOT_ONE_PASS_FIX
		    //------ albEnd ----------------------------------
		    if (pLS->state == STATE_ONESHOT) {
		       // done with one shot
		       DBG(fprintf(stderr, "%u:%u  finished ONESHOT neg\n", pLS->lLoopIndex, pLS->lChannelIndex));
		       pLS->state = STATE_MUTE;
		       //fWet = 0.0;
		       pLS->fPlayFadeDelta = -1.0f / xfadeSamples;
		       //pLS->lRampSamples = xfadeSamples;
		    }
		    //------ albBegin ----------------------------------
#endif
		    //------ albEnd ----------------------------------

		    if (pLS->fNextCurrRate != 0 && pfSyncOutput[lSampleIndex] > 1.5f) {
		       // commit the new rate at boundary (quantized)
		       pLS->fCurrRate = pLS->fNextCurrRate;
		       pLS->fNextCurrRate = 0.0f;
		       DBG(fprintf(stderr, "%u:%u (3) Starting quantized rate change at %d\n", pLS->lLoopIndex, pLS->lChannelIndex, lCurrPos));
		    }

		    pLS->donePlaySync = false;

		 }


	      }
	      
		   if (pLS->state == STATE_UNDO && pLS->fPlayFadeAtten == 1.0f) {
			   // play some of the old loop first and switch later
			   undoLoop(pLS, false);
			   DBG(fprintf(stderr, "finished UNDO...\n"));
			   pLS->state = pLS->nextState;
		   }
		   if (pLS->state == STATE_REDO && pLS->fPlayFadeAtten == 1.0f) {
			   // play some of the old loop first and switch later
			   redoLoop(pLS);
			   DBG(fprintf(stderr, "finished REDO...\n"));
			   pLS->state = pLS->nextState;
		   }
		   if (pLS->state == STATE_UNDO_ALL && pLS->fPlayFadeAtten == 0.0f) {
			   // fade out the old loop and goto state_off
			   clearLoopChunks(pLS);
			   DBG(fprintf(stderr, "finished UNDO ALL...\n"));
				 if (pLS->wasMuted)
			     pLS->state = STATE_OFF_MUTE;
				 else
			     pLS->state = STATE_OFF;
		   }
		   if (pLS->state == STATE_REDO_ALL && pLS->fPlayFadeAtten == 1.0f) {
			   // play some of the old loop first and switch later
			   lastloop = pLS->headLoopChunk;
			   redoLoop(pLS);
			   while (pLS->headLoopChunk != lastloop) {
				   lastloop = pLS->headLoopChunk;
				   redoLoop(pLS);
			   }
			   DBG(fprintf(stderr, "finished REDO ALL...\n"));
			   pLS->state = pLS->nextState;
		   }
		   
		   
	      // recenter around the mod
	      if (loop) {
		      lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));
		      
		      if (recenter) {
			      loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy);
		      }
	      }
	   }
	   else {
	      goto passthrough;
	   }
	   
	} break;


	case STATE_DELAY:
	{
	   if (loop && loop->lLoopLength)
	   {
	      // the loop length is our delay time.
	      backfill = loop->backfill;
	      
	      for (;lSampleIndex < SampleCount;
		   lSampleIndex++)
	      {
	         fWet += wetDelta;
                 fDry += dryDelta;
     	         fFeedback += feedbackDelta;
	         fScratchPos += scratchDelta;
		      
		 // wrap properly
		 lCurrPos =(unsigned int) fmod(loop->dCurrPos, loop->lLoopLength);
		 pLoopSample = & pLS->pSampleBuf[(loop->lLoopStart + lCurrPos) & pLS->lBufferSizeMask];

		 fInputSample = pfInput[lSampleIndex];

		 if (backfill && lCurrPos >= loop->lMarkEndL && lCurrPos <= loop->lMarkEndH) {
		    // our delay buffer is invalid here, clear it
		    *(pLoopSample) = 0.0f;

		    if (fRate > 0) {
		       loop->lMarkEndL = lCurrPos;
		    }
		    else {
		       loop->lMarkEndH = lCurrPos;
		    }
		 }


		 fOutputSample =   fWet *  *(pLoopSample)
		    + fDry * fInputSample;


		 if (!pLS->bHoldMode) {
		    // now fill in from input if we are not holding the delay
		    *(pLoopSample) = 
		      (fInputSample +  fFeedback *  *(pLoopSample));
		 }
		 
		 pfOutput[lSampleIndex] = fOutputSample;

		 if (fSyncMode != 0 || fQuantizeMode == QUANT_OFF) {
			 pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
		 }
		 //------ albBegin ----------------------------------
#if SIGNED_SYNC_POS_FIX
		 else if (onSyncGrid(lCurrPos, loop->lSyncPos, loop->lCycleLength)) {
#else
		 else if (((lCurrPos + loop->lSyncPos) % loop->lCycleLength) == 0) {
#endif
		 //------ albEnd ----------------------------------
			 pfSyncOutput[lSampleIndex] = 2.0f;
		 }

		 
		 // increment 
		 loop->dCurrPos = loop->dCurrPos + fRate;

		 if (backfill && loop->lMarkEndL == loop->lMarkEndH) {
		    // no need to clear the buf first now
		    backfill = loop->backfill = 0;
		 }

		 else if (loop->dCurrPos < 0)
		 {
		    // our rate must be negative
		    // adjust around to the back
		    loop->dCurrPos += loop->lLoopLength;
		 }

		 
	      }


	      // recenter around the mod
	      lCurrPos = (unsigned int) fabs(fmod(loop->dCurrPos, loop->lLoopLength));
	      
	      loop->dCurrPos = lCurrPos + modf(loop->dCurrPos, &dDummy);

	      
	   }
	   else {
	      goto passthrough;
	   }
	} break;
	
	default:
	{
	   goto passthrough;

	}  break;
	
     }

     //goto loopend;
     continue;
     
    passthrough:

     // simply play the input out directly
     // no loop has been created yet
     for (;lSampleIndex < SampleCount;
	  lSampleIndex++)
     {
        fWet += wetDelta;
        fDry += dryDelta;
        fFeedback += feedbackDelta;
	fScratchPos += scratchDelta;
	     
	pfOutput[lSampleIndex] = fDry * pfInput[lSampleIndex];

	if (fSyncMode != 0 || fQuantizeMode == QUANT_OFF) {
		pfSyncOutput[lSampleIndex] = pfSyncInput[lSampleIndex];
	}
	
	if (fSyncMode >= 1.0f) {
		pLS->lSamplesSinceSync++;
		if (pfSyncInput[lSampleIndex] > 1.5f) {
			//cerr << "passthru resetting sync: " << pLS << endl;
			pLS->lSamplesSinceSync = 0;
		}
	}
	
     }
     
  }
  
  // keep track of time between triggers to ignore settling issues
  // pLS->lRecTrigSamples += SampleCount;
  pLS->lScratchSamples += SampleCount;  
  pLS->lTapTrigSamples += SampleCount;

  // printf ("wet is %g    targ was %g\n", fWet, *(pLS->pfWet));
  pLS->fWetCurr = wetTarget;
  pLS->fWetTarget = wetTarget;

  pLS->fDryCurr = dryTarget;
  pLS->fDryTarget = dryTarget;
  
  pLS->fFeedbackCurr = feedbackTarget;
  pLS->fFeedbackTarget = feedbackTarget;


  pLS->fScratchPosCurr = scratchTarget;
  pLS->fScratchPosTarget = scratchTarget;
  
  // update output ports
  *pLS->pfStateOut = (LADSPA_Data) pLS->state;

  *pLS->pfNextStateOut = (LADSPA_Data) pLS->nextState;
  
  // either waiting for sync, waiting for a reverse, or rounding a mult
  *pLS->pfWaiting = (pLS->waitingForSync || pLS->state == STATE_TRIG_START || pLS->state == STATE_TRIG_STOP
		     || pLS->fNextCurrRate != 0.0f || pLS->rounding)  ? 1.0f: 0.0f;

  *pLS->pfRateOutput = (LADSPA_Data) pLS->fCurrRate *  (*pLS->pfRate);


  pLS->lInputBufWritePos = (pLS->lInputBufWritePos + SampleCount) & pLS->lInputBufMask;

  
  if (pLS->pfSecsFree) {
	  *pLS->pfSecsFree = pLS->lBufferSize / pLS->fSampleRate;
//      *pLS->pfSecsFree = (pLS->fTotalSecs) -
// 	(pLS->headLoopChunk ?
// 	 ((((unsigned)pLS->headLoopChunk->pLoopStop - (unsigned)pLS->pSampleBuf)
// 	  / sizeof(LADSPA_Data)) / pLS->fSampleRate)   :
// 	 0);
  }
  
  if (loop) {
     if (pLS->pfLoopPos)
	*pLS->pfLoopPos = (LADSPA_Data) (loop->dCurrPos / pLS->fSampleRate);

     if (pLS->pfLoopLength)
	*pLS->pfLoopLength = ((LADSPA_Data) loop->lLoopLength) / pLS->fSampleRate;

     if (pLS->pfCycleLength)
	*pLS->pfCycleLength = ((LADSPA_Data) loop->lCycleLength) / pLS->fSampleRate;


     
  }
  else {
     if (pLS->pfLoopPos)
	*pLS->pfLoopPos = 0.0f;
     if (pLS->pfLoopLength)
	*pLS->pfLoopLength = 0.0f;     
     if (pLS->pfCycleLength)
	*pLS->pfCycleLength = 0.0f;

     if (pLS->pfStateOut && pLS->state != STATE_OFF_MUTE  && pLS->state != STATE_MUTE && pLS->state != STATE_TRIG_START)
	*pLS->pfStateOut = (LADSPA_Data) STATE_OFF;

  }
  
  
}


/*****************************************************************************/

LADSPA_Descriptor * g_psDescriptor = NULL;

/*****************************************************************************/

/* sl_init() is called automatically when the plugin library is first
   loaded. */

LADSPA_Descriptor * create_sl_descriptor()
{
  char ** pcPortNames;
  LADSPA_PortDescriptor * piPortDescriptors;
  LADSPA_PortRangeHint * psPortRangeHints;
  LADSPA_Descriptor * psDescriptor;
    
  psDescriptor
    = (LADSPA_Descriptor *)malloc(sizeof(LADSPA_Descriptor));
  if (psDescriptor) {
    psDescriptor->UniqueID
      = 1601;
    psDescriptor->Label
      = strdup("SooperLooper");
    psDescriptor->Properties
      = LADSPA_PROPERTY_HARD_RT_CAPABLE;
    psDescriptor->Name 
      = strdup("SooperLooper");
    psDescriptor->Maker
      = strdup("Jesse Chappell");
    psDescriptor->Copyright
      = strdup("2002, Jesse Chappell");

    psDescriptor->PortCount 
      = PORT_COUNT;
    piPortDescriptors
      = (LADSPA_PortDescriptor *)calloc(PORT_COUNT, sizeof(LADSPA_PortDescriptor));
    psDescriptor->PortDescriptors 
      = (const LADSPA_PortDescriptor *)piPortDescriptors;
    piPortDescriptors[WetLevel]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[DryLevel]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;

    piPortDescriptors[Feedback]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[TriggerThreshold]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[Rate]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[ScratchPosition]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;

    piPortDescriptors[Multi]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;

    piPortDescriptors[TapDelayTrigger]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;

    piPortDescriptors[UseFeedbackPlay]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[Quantize]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[Round]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[Sync]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[PlaybackSync]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[UseRate]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[FadeSamples]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[TempoInput]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    
    piPortDescriptors[RedoTap]
      = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
    
    piPortDescriptors[AudioInputPort]
      = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
    piPortDescriptors[AudioOutputPort]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
    piPortDescriptors[SyncInputPort]
      = LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
    piPortDescriptors[SyncOutputPort]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;

    
    piPortDescriptors[State]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[LoopPosition]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[LoopLength]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[CycleLength]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[LoopMemory]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[LoopFreeMemory]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[Waiting]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[TrueRate]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
    piPortDescriptors[NextState]
      = LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;


    
    pcPortNames
      = (char **)calloc(PORT_COUNT, sizeof(char *));
    psDescriptor->PortNames
      = (const char **)pcPortNames;
    pcPortNames[DryLevel] 
      = strdup("Dry Level (dB)");
    pcPortNames[WetLevel] 
      = strdup("Wet Level (dB)");

    pcPortNames[Feedback] 
      = strdup("Feedback");
    pcPortNames[TriggerThreshold] 
      = strdup("Trigger Threshold");
    pcPortNames[Rate] 
      = strdup("Rate");
    pcPortNames[ScratchPosition] 
      = strdup("Scratch Destination");

    pcPortNames[Multi] 
      = strdup("Multi Control");

    pcPortNames[TapDelayTrigger] 
      = strdup("Tap Delay Trigger");

    pcPortNames[UseFeedbackPlay] 
      = strdup("Use Feedback Play");
    pcPortNames[Quantize] 
      = strdup("Quantize Mode");
    pcPortNames[Round] 
      = strdup("Round Mode");
    pcPortNames[RedoTap] 
      = strdup("Redo Tap Mode");
    pcPortNames[Sync] 
      = strdup("Sync Mode");
    pcPortNames[PlaybackSync] 
      = strdup("Playback Sync Mode");
    pcPortNames[UseRate] 
      = strdup("Use Rate Ctrl");
    pcPortNames[FadeSamples] 
      = strdup("Fade samples");
    pcPortNames[TempoInput] 
      = strdup("Tempo");

    pcPortNames[InputLatency] 
      = strdup("InputLatency");
    pcPortNames[OutputLatency] 
      = strdup("OutputLatency");
    pcPortNames[TriggerLatency] 
      = strdup("TriggerLatency");
    pcPortNames[MuteQuantized] 
      = strdup("MuteQuantized");
    pcPortNames[OverdubQuantized] 
      = strdup("OverdubQuantized");
    
    pcPortNames[AudioInputPort] 
      = strdup("Input");
    pcPortNames[AudioOutputPort]
      = strdup("Output");

    pcPortNames[SyncInputPort] 
      = strdup("Sync Input");
    pcPortNames[SyncOutputPort]
      = strdup("Sync Output");
    
    pcPortNames[State] 
      = strdup("State Output");
    pcPortNames[LoopLength]
      = strdup("Loop Length Out (s)");
    pcPortNames[LoopPosition]
      = strdup("Loop Position Out (s)");
    pcPortNames[CycleLength]
      = strdup("Cycle Length Out (s)");

    pcPortNames[LoopMemory]
      = strdup("Total Sample Mem (s)");
    pcPortNames[LoopFreeMemory]
      = strdup("Free Sample Mem (s)");
    pcPortNames[Waiting]
      = strdup("Waiting");
    pcPortNames[TrueRate]
      = strdup("True Rate");
    pcPortNames[NextState] 
      = strdup("Next State Output");

    
    psPortRangeHints = ((LADSPA_PortRangeHint *)
			calloc(PORT_COUNT, sizeof(LADSPA_PortRangeHint)));
    psDescriptor->PortRangeHints
      = (const LADSPA_PortRangeHint *)psPortRangeHints;

    psPortRangeHints[DryLevel].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[DryLevel].LowerBound 
      = -90.0f;
    psPortRangeHints[DryLevel].UpperBound
      = 0.0;

    psPortRangeHints[WetLevel].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[WetLevel].LowerBound 
      = -90.0f;
    psPortRangeHints[WetLevel].UpperBound
      = 0.0;
    
    psPortRangeHints[Feedback].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[Feedback].LowerBound 
      = 0.0;
    psPortRangeHints[Feedback].UpperBound
      = 1.0;

    psPortRangeHints[TriggerThreshold].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[TriggerThreshold].LowerBound 
      = 0.0;
    psPortRangeHints[TriggerThreshold].UpperBound
      = 1.0;

    psPortRangeHints[Rate].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[Rate].LowerBound 
      = -4.0;
    psPortRangeHints[Rate].UpperBound
      = 4.0;

    psPortRangeHints[TrueRate].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[TrueRate].LowerBound 
      = -4.0;
    psPortRangeHints[TrueRate].UpperBound
      = 4.0;

    psPortRangeHints[ScratchPosition].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
    psPortRangeHints[ScratchPosition].LowerBound 
      = 0.0;
    psPortRangeHints[ScratchPosition].UpperBound
      = 1.0;

    psPortRangeHints[Multi].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[Multi].LowerBound 
      = 0.0;
    psPortRangeHints[Multi].UpperBound
      = 127.0;

    psPortRangeHints[TapDelayTrigger].HintDescriptor
      = LADSPA_HINT_TOGGLED;

    psPortRangeHints[UseFeedbackPlay].HintDescriptor
	    = LADSPA_HINT_TOGGLED;

    psPortRangeHints[Quantize].HintDescriptor
      = LADSPA_HINT_BOUNDED_ABOVE|LADSPA_HINT_BOUNDED_BELOW|LADSPA_HINT_INTEGER;
    psPortRangeHints[Quantize].LowerBound 
      = 0.0f;
    psPortRangeHints[Quantize].UpperBound
      = 3.0f;

    psPortRangeHints[FadeSamples].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[FadeSamples].LowerBound 
      = 0.0f;
    psPortRangeHints[FadeSamples].UpperBound
      = 8192.0f;

    psPortRangeHints[TempoInput].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[TempoInput].LowerBound 
      = 0.0f;
    psPortRangeHints[TempoInput].UpperBound
      = 1000.0f;

    psPortRangeHints[InputLatency].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[InputLatency].LowerBound 
      = 0.0f;
    psPortRangeHints[InputLatency].UpperBound
      = 1000000.0f;
    psPortRangeHints[OutputLatency].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[OutputLatency].LowerBound 
      = 0.0f;
    psPortRangeHints[OutputLatency].UpperBound
      = 1000000.0f;
    psPortRangeHints[TriggerLatency].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_INTEGER;
    psPortRangeHints[TriggerLatency].LowerBound 
      = 0.0f;
    psPortRangeHints[TriggerLatency].UpperBound
      = 1000000.0f;

    psPortRangeHints[MuteQuantized].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_TOGGLED;
    psPortRangeHints[MuteQuantized].LowerBound 
      = 0.0f;
    psPortRangeHints[MuteQuantized].UpperBound
      = 1.0f; 
    psPortRangeHints[OverdubQuantized].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_TOGGLED;
    psPortRangeHints[OverdubQuantized].LowerBound 
      = 0.0f;
    psPortRangeHints[OverdubQuantized].UpperBound
      = 1.0f;
   
    
    psPortRangeHints[Round].HintDescriptor
      = LADSPA_HINT_TOGGLED;
    psPortRangeHints[RedoTap].HintDescriptor
      = LADSPA_HINT_TOGGLED;
    psPortRangeHints[Sync].HintDescriptor
      = LADSPA_HINT_TOGGLED;
    psPortRangeHints[PlaybackSync].HintDescriptor
      = LADSPA_HINT_TOGGLED;
    psPortRangeHints[UseRate].HintDescriptor
      = LADSPA_HINT_TOGGLED;
    
    psPortRangeHints[AudioInputPort].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[AudioInputPort].LowerBound 
      = 0.0;
    psPortRangeHints[AudioOutputPort].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[AudioOutputPort].LowerBound 
      = 0.0;

    psPortRangeHints[State].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[LoopPosition].LowerBound 
      = 0.0;

    psPortRangeHints[LoopPosition].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[LoopPosition].LowerBound 
      = 0.0;
    
    psPortRangeHints[LoopLength].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[LoopLength].LowerBound 
      = 0.0;

    psPortRangeHints[CycleLength].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[CycleLength].LowerBound 
      = 0.0;

    psPortRangeHints[LoopMemory].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[LoopMemory].LowerBound 
      = 0.0;
    psPortRangeHints[LoopFreeMemory].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[LoopFreeMemory].LowerBound 
      = 0.0;

    psPortRangeHints[Waiting].HintDescriptor
      = LADSPA_HINT_TOGGLED;

    psPortRangeHints[NextState].HintDescriptor
      = LADSPA_HINT_BOUNDED_BELOW;
    psPortRangeHints[NextState].LowerBound 
      = 0.0;

    
    psDescriptor->instantiate
      = instantiateSooperLooper;
    psDescriptor->connect_port 
      = connectPortToSooperLooper;
    psDescriptor->activate
      = activateSooperLooper;
    psDescriptor->run 
      = runSooperLooper;
    psDescriptor->run_adding
      = NULL;
    psDescriptor->set_run_adding_gain
      = NULL;
    psDescriptor->deactivate
      = NULL;
    psDescriptor->cleanup
      = cleanupSooperLooper;
  }
  
  return psDescriptor;
}

/*****************************************************************************/


void cleanup_sl_descriptor(LADSPA_Descriptor * psDescriptor)
{
  unsigned long lIndex;
  if (psDescriptor) {
    free((char *)psDescriptor->Label);
    free((char *)psDescriptor->Name);
    free((char *)psDescriptor->Maker);
    free((char *)psDescriptor->Copyright);
    free((LADSPA_PortDescriptor *)psDescriptor->PortDescriptors);
    for (lIndex = 0; lIndex < psDescriptor->PortCount; lIndex++)
      free((char *)(psDescriptor->PortNames[lIndex]));
    free((char **)psDescriptor->PortNames);
    free((LADSPA_PortRangeHint *)psDescriptor->PortRangeHints);
    free(psDescriptor);
	psDescriptor = 0;
  }
}


void sl_init()
{
	if (!g_psDescriptor) {
		g_psDescriptor = create_sl_descriptor();
	}
}

/* _fini() is called automatically when the library is unloaded. */
void 
sl_fini() {
	
	cleanup_sl_descriptor(g_psDescriptor);
}


/*****************************************************************************/

/* Return a descriptor of the requested plugin type. Only one plugin
   type is available in this library. */
const LADSPA_Descriptor * 
ladspa_descriptor(unsigned long Index) {
  if (Index == 0)
    return g_psDescriptor;
  else
    return NULL;
}

/*****************************************************************************/

/* EOF */
