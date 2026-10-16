"""Regression tests for the record failsafe (auto CANCEL_RECORD_MUTE).

If a record is left running in STATE_RECORD longer than
g_record_failsafe_seconds, the engine treats it exactly as if the
long-press CANCEL_RECORD_MUTE command had just arrived -- same guard
table, same fade-residue fix, just a second way to reach that one branch.
See "New Feature: record failsafe" in mod_docs/late_hit_patch_summary.rst
(and "New Command: cancel_record_mute" for the branch it reuses).

Specialized 2026-09-01: the failsafe only ever arms when the RECORD press
that started this take interrupted an actively playing, unmuted loop
(state was Playing or, structurally, OneShot -- see
bRecordEnteredFromActivePlay in plugin.hpp/plugin.cc). A first/fresh
recording from an empty loop, or a record started from Muted or Paused,
can run indefinitely with no auto-cancel, same as before this feature
existed. Motivation: the risky case is an eager-single RECORD that fired
while the performer actually meant to hold for a long-press
cancel_record_mute (or simply got distracted mid-overdub) -- there, an
existing audible take is what's silently at stake. A fresh/empty
recording has no such ambiguity with a "meant to mute" gesture and no
prior take to protect, so it's left alone.

g_record_failsafe_seconds is set to a small value for these tests via the
SWIG-exposed global (test_engine.cvar), the same mechanism
testdef_cancel_record_mute.py uses for g_late_hit_window_ms, so the timeout
fires in around a second of real JACK callbacks. The compiled-in default is
now 0.0 (off, see plugin.cc) since this feature can discard a take with no
press at all; tests still exercise a representative non-zero value (45.0,
this file's prior default) rather than 0 in setUp/tearDown so the "fires"
tests remain meaningful.

Timing budget (buffer period is real wall-clock time -- this harness runs
against a live JACK server, not a simulated clock):
  _TEST_SECONDS = 1.5s must clear recordTake()'s own 30+10-buffer settle
  wait (~0.85s at 1024 frames/48 kHz, ~0.21s at 256) with margin, so an
  ordinary recordTake() never fires it by accident.  That holds for any
  period up to 1024 frames at 48 kHz.  The "let it fire" and "safe leg"
  waits are specified in seconds and converted to buffers at the
  server's actual period (buffersFor()), so they hold at any period.
"""
import math
import time
import unittest

import testdef_simple_state
import test_engine


# Distinct constants for the audio-content tests, matching
# testdef_cancel_record_mute.py's convention.
TAKE_LEVEL  =  0.5    # the take we want back
NOISE_LEVEL = -0.9    # what the input is doing while the failsafe is armed

# Small timeout so the failsafe fires within a couple of seconds of real
# JACK callbacks.  Must clear recordTake()'s own settle wait (~0.85s at
# 1024 frames) with margin.  Restored in tearDown.
_TEST_SECONDS = 1.5
# Not the compiled-in default (that's 0.0, off) -- just a representative
# non-zero value these tests restore between cases so an accidental leak of
# _TEST_SECONDS into another test fails loudly instead of silently disabling
# the feature for it.
_DEFAULT_SECONDS = 45.0

# Time to wait for the failsafe to have clearly fired at _TEST_SECONDS,
# and time for a single record leg that must clearly NOT fire it.
# Converted to buffers by buffersFor() at the server's actual period.
_FIRE_WAIT_SECONDS = 2.3     # comfortably over 1.5s
_SAFE_LEG_SECONDS  = 0.32    # comfortably under 1.5s


class _AudioHelpers(object):
    """Shared helpers, same shape as testdef_cancel_record_mute.py's."""

    def initAudio(self):
        self.looper = self.engine.looper
        self.looper.set_port(test_engine.DryLevel, 0.0)   # output = pure loop
        self.looper.set_port(test_engine.WetLevel, 1.0)

    def buffersFor(self, seconds):
        """Whole JACK buffers covering `seconds` at the server's period."""
        period = (float(self.looper.get_buffer_size())
                  / float(self.engine.get_sample_rate()))
        return int(math.ceil(seconds / period))

    def waitBuffers(self, n, timeout=5.0):
        """Busy-wait until n more JACK callbacks have completed."""
        target = self.looper.process_count + n
        # `timeout` is a floor; slow periods get 3x the real time n buffers take.
        period = (float(self.looper.get_buffer_size())
                  / float(self.engine.get_sample_rate()))
        deadline = time.time() + max(timeout, 3.0 * n * period)
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    "Timed out waiting for %d JACK callbacks "
                    "(process_count stuck at %d)"
                    % (n, self.looper.process_count))
            time.sleep(0.001)

    def outputRange(self, blocks=120):
        """min/max of the output over `blocks` callbacks."""
        self.looper.reset_output_stats()
        self.waitBuffers(blocks)
        return self.looper.output_min, self.looper.output_max

    def recordTake(self, level):
        """Record a complete take of a constant `level`, leaving it playing."""
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.waitBuffers(30)
        self.engine.request("RECORD")
        self.waitBuffers(10)
        self.assertState("Playing")


class recordFailsafe(_AudioHelpers, testdef_simple_state.simpleStateTest):

    def setUp(self):
        testdef_simple_state.simpleStateTest.setUp(self)
        self.initAudio()
        test_engine.cvar.g_record_failsafe_seconds = _TEST_SECONDS

    def tearDown(self):
        test_engine.cvar.g_record_failsafe_seconds = _DEFAULT_SECONDS
        testdef_simple_state.simpleStateTest.tearDown(self)

    def testDoesNotFireFromFirstRecord(self):
        # No prior take -- entered STATE_RECORD from STATE_OFF, not an
        # actively playing loop -- so bRecordEnteredFromActivePlay is
        # false and the failsafe must never arm, no matter how long this
        # runs.  Before the 2026-09-01 specialization this used to fire
        # and land OffMuted.  Since 2026-10-03 the manual cancel ignores a
        # first record too (compare
        # testdef_cancel_record_mute.testIgnoredDuringFirstRecord).
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.waitBuffers(self.buffersFor(_FIRE_WAIT_SECONDS))
        self.assertState("Recording")   # still recording -- never fired

    def testFiresOverExistingTakeAndRestoresIt(self):
        # The canonical case the specialization keeps: a take is actively
        # playing (unmuted) when a second record interrupts it and is then
        # left running past the timeout.  Must be discarded and the prior
        # take restored, muted -- with no residual audio leaking through
        # (the same fade-residue class of bug CANCEL_RECORD_MUTE itself
        # had to fix; this exercises the identical code path, just reached
        # automatically).
        self.recordTake(TAKE_LEVEL)

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request("RECORD")            # second take, never stopped
        self.waitBuffers(10)
        self.assertState("Recording")

        self.waitBuffers(self.buffersFor(_FIRE_WAIT_SECONDS))  # let it fire
        self.assertState("Muted")

        mn, mx = self.outputRange(40)
        self.assertAlmostEqual(mn, 0.0, places=3)
        self.assertAlmostEqual(mx, 0.0, places=3)

        self.engine.request("MUTE_OFF")
        self.waitBuffers(10)
        mn, mx = self.outputRange()
        self.assertAlmostEqual(
            mn, TAKE_LEVEL, places=3,
            msg="expected the prior take (%.2f) back; got %.4f..%.4f"
                % (TAKE_LEVEL, mn, mx))
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)

    def testDoesNotFireBeforeTimeout(self):
        # A record that completes well within the timeout must be totally
        # unaffected -- this is the "never fires during ordinary use" half
        # of the contract, checked with a generous timeout so an ordinary
        # take is never at risk.
        test_engine.cvar.g_record_failsafe_seconds = 30.0
        self.recordTake(TAKE_LEVEL)
        self.waitBuffers(20)
        self.assertState("Playing")
        mn, mx = self.outputRange()
        self.assertAlmostEqual(mn, TAKE_LEVEL, places=3)
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)

    def testCounterDoesNotLeakAcrossSeparateTakes(self):
        # Record and stop several times in a row, each leg well inside the
        # timeout -- but enough legs that the *cumulative* time would clear
        # 1.5s if lRecordFailsafeSamples ever failed to reset between takes
        # (6 legs * ~0.32s = ~1.9s > 1.5s), while each individual leg stays
        # safely under it on its own (~0.32s vs 1.5s).
        for _ in range(6):
            self.looper.input_inject_value = TAKE_LEVEL
            self.engine.request("RECORD")
            self.waitBuffers(self.buffersFor(_SAFE_LEG_SECONDS))
            self.engine.request("RECORD")
            self.waitBuffers(2)
            self.assertState("Playing")

    def testZeroDisablesFailsafe(self):
        # <= 0.0f must disable the feature entirely, matching the late-hit
        # window's "0 disables" convention.  Records over an existing take
        # (bRecordEnteredFromActivePlay true) specifically so this isolates
        # the g_record_failsafe_seconds guard -- a first/fresh recording
        # would never fire regardless of this value (see
        # testDoesNotFireFromFirstRecord), which would make this test pass
        # for the wrong reason.
        self.recordTake(TAKE_LEVEL)
        test_engine.cvar.g_record_failsafe_seconds = 0.0
        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.waitBuffers(self.buffersFor(_FIRE_WAIT_SECONDS))
        self.assertState("Recording")   # still recording -- never fired

    def testDoesNotFireWhenEnteredFromMute(self):
        # The other half of "active, unmuted": a take exists but is muted,
        # not playing, when the second record starts.
        # bRecordEnteredFromActivePlay must be false here even though a
        # prior take exists to protect -- entering from STATE_MUTE is
        # deliberately excluded (see the module docstring). Must behave
        # like testDoesNotFireFromFirstRecord: runs past the timeout with
        # no auto-cancel.
        self.recordTake(TAKE_LEVEL)
        self.engine.request("MUTE_ON")
        self.waitBuffers(5)
        self.assertState("Muted")

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.waitBuffers(self.buffersFor(_FIRE_WAIT_SECONDS))
        self.assertState("Recording")   # still recording -- never fired

    def testManualCancelStillWorksWithFailsafeArmed(self):
        # Side-effect guard: the failsafe is an alternate trigger into the
        # same branch, not a replacement for the manual long-press path.
        # Recorded over a playing take: that is the only case the manual
        # cancel acts on.
        # (This test used a first take, which the cancel now ignores.)
        self.recordTake(0.5)
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.engine.request("CANCEL_RECORD_MUTE")
        self.waitBuffers(5)
        self.assertState("Muted")
