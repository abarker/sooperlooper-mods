"""Regression tests for the record failsafe (auto CANCEL_RECORD_MUTE).

If a record is left running in STATE_RECORD longer than
g_record_failsafe_seconds, the engine treats it exactly as if the
long-press CANCEL_RECORD_MUTE command had just arrived -- same guard
table, same fade-residue fix, just a second way to reach that one branch.
Design and rationale: mod_docs/ROADMAP.rst entry 4 (and entry 1 for the
CANCEL_RECORD_MUTE branch itself, reused verbatim here).

g_record_failsafe_seconds is set to a small value for these tests via the
SWIG-exposed global (test_engine.cvar), the same mechanism
testdef_cancel_record_mute.py uses for g_late_hit_window_ms, so the timeout
fires in around a second of real JACK callbacks instead of the 45-second
production default.

Timing budget (buffer period is real wall-clock time -- this harness runs
against a live JACK server, not a simulated clock):
  _TEST_SECONDS = 1.5s must clear recordTake()'s own 30+10-buffer settle
  wait (~0.85s at the 1024/48000 dummy-backend rate used for these tests,
  ~21.3ms/buffer) with margin, so an ordinary recordTake() never fires it
  by accident; "let it fire" waits then use ~110 buffers (~2.3s) to clear
  1.5s with margin the other way.  If the dummy backend's rate/period ever
  changes, these buffer counts need re-tuning together.
"""
import time
import unittest

import testdef_simple_state
import test_engine


# Distinct constants for the audio-content tests, matching
# testdef_cancel_record_mute.py's convention.
TAKE_LEVEL  =  0.5    # the take we want back
NOISE_LEVEL = -0.9    # what the input is doing while the failsafe is armed

# Small timeout so the failsafe fires within a couple of seconds of real
# JACK callbacks instead of the 45-second production default.  Must clear
# recordTake()'s own ~0.85s settle wait with margin.  Restored in tearDown.
_TEST_SECONDS = 1.5
_DEFAULT_SECONDS = 45.0

# Buffers to wait for the failsafe to have clearly fired at _TEST_SECONDS,
# and buffers to wait for a single record leg that must clearly NOT fire it.
_FIRE_WAIT_BUFFERS = 110    # ~2.3s at ~21.3ms/buffer -- comfortably over 1.5s
_SAFE_LEG_BUFFERS  = 15     # ~0.32s -- comfortably under 1.5s


class _AudioHelpers(object):
    """Shared helpers, same shape as testdef_cancel_record_mute.py's."""

    def initAudio(self):
        self.looper = self.engine.looper
        self.looper.set_port(test_engine.DryLevel, 0.0)   # output = pure loop
        self.looper.set_port(test_engine.WetLevel, 1.0)

    def waitBuffers(self, n, timeout=5.0):
        """Busy-wait until n more JACK callbacks have completed."""
        target = self.looper.process_count + n
        deadline = time.time() + timeout
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

    def testFiresFromFirstRecord(self):
        # No prior take: left recording past the timeout, same landing spot
        # as a manual cancel from the same state -- OffMuted (compare
        # testdef_cancel_record_mute.testCancelsFirstRecord).
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.waitBuffers(_FIRE_WAIT_BUFFERS)
        self.assertState("OffMuted")

    def testFiresOverExistingTakeAndRestoresIt(self):
        # A take exists; a second record left running past the timeout must
        # be discarded and the prior take restored, muted -- with no
        # residual audio leaking through (the same fade-residue class of
        # bug CANCEL_RECORD_MUTE itself had to fix; this exercises the
        # identical code path, just reached automatically).
        self.recordTake(TAKE_LEVEL)

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request("RECORD")            # second take, never stopped
        self.waitBuffers(10)
        self.assertState("Recording")

        self.waitBuffers(_FIRE_WAIT_BUFFERS)      # let the failsafe fire
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
            self.waitBuffers(_SAFE_LEG_BUFFERS)
            self.engine.request("RECORD")
            self.waitBuffers(2)
            self.assertState("Playing")

    def testZeroDisablesFailsafe(self):
        # <= 0.0f must disable the feature entirely, matching the late-hit
        # window's "0 disables" convention.
        test_engine.cvar.g_record_failsafe_seconds = 0.0
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.waitBuffers(_FIRE_WAIT_BUFFERS)
        self.assertState("Recording")   # still recording -- never fired

    def testManualCancelStillWorksWithFailsafeArmed(self):
        # Side-effect guard: the failsafe is an alternate trigger into the
        # same branch, not a replacement for the manual long-press path.
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("Recording")
        self.engine.request("CANCEL_RECORD_MUTE")
        self.waitBuffers(5)
        self.assertState("OffMuted")
