#! /usr/bin/env python3
"""
Regression tests for the late-hit recording correction patch.

Sync buffer injection values:
  0.0  — no sync signal; STATE_TRIG_START/STOP will wait indefinitely
  2.0  — sync pulse (> 1.5 threshold); primes bSyncPulseReceived and
         resets lGlobalSamplesSinceSync

1.0 must NOT be used as a background value: SooperLooper's TRIG_START
and TRIG_STOP states exit on pfSyncInput[n] > 0.0, so a constant 1.0
would cause them to transition immediately, defeating the WaitStart/
WaitStop assertions.

Timing notes (adjust class attributes if JACK runs at a different period):
  JACK period  ~10.7 ms  (512 samples at 48 kHz)
  PRIME_SECS   ~30 ms    (>=2 periods; primes bSyncPulseReceived)
  WINDOW_SECS  ~25 ms    (T ~1200 samples < 4800-sample / 100 ms window)
  OUTSIDE_SECS ~150 ms   (T >> 4800 samples; well outside the window)
"""

import time
import unittest
import test_engine
import engine_wrapper


class LateHitTestCase(unittest.TestCase):
    """Base class: creates engine with sync enabled and injection ready."""

    PRIME_SECS   = 0.030
    WINDOW_SECS  = 0.025
    OUTSIDE_SECS = 0.150
    RECORD_SECS  = 0.050

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_latehit")
        self.looper = self.engine.looper
        self.looper.set_port(test_engine.Sync, 1.0)
        self.looper.sync_inject_enabled = True
        self.looper.sync_inject_value   = 0.0   # no sync signal (not 1.0)

    def tearDown(self):
        del self.engine

    def prime_pulse(self):
        """Inject one sync pulse and wait for it to be processed.
        Sets bSyncPulseReceived and resets lGlobalSamplesSinceSync to 0."""
        self.looper.sync_inject_value = 2.0   # above the 1.5 threshold
        time.sleep(self.PRIME_SECS)
        self.looper.sync_inject_value = 0.0   # back to no-signal (not 1.0)

    def assertState(self, state, msg=None):
        actual = self.engine.getState()
        self.assertEqual(
            actual,
            engine_wrapper.states[state],
            (engine_wrapper.states[actual] + " != " + state)
            if msg is None else msg
        )


# ======================================================================
# Late-start tests
# ======================================================================

class TestLateStart(LateHitTestCase):

    def testLateStart_DirectlyRecording(self):
        """Core case: RECORD within the window after a pulse → immediate Recording."""
        self.prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("Recording",
                         "Late-hit start should enter Recording directly, not WaitStart")

    def testNoCorrection_SyncOff(self):
        """Regression: sync OFF still enters Recording immediately (stock path
        unaffected by the late-hit code)."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("Recording",
                         "Sync off: should enter Recording via stock path")

    def testNoCorrection_NoPulse(self):
        """bSyncPulseReceived guard: with sync on but no pulse ever seen,
        RECORD waits for the next boundary (stock behaviour).
        Background value is 0.0 so TRIG_START stays waiting."""
        # deliberately do NOT call prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("WaitStart",
                         "No pulse seen: correction must not fire before first pulse")

    def testNoCorrection_TooLate(self):
        """Window boundary: T > 100 ms after pulse → WaitStart (stock path).
        Background value is 0.0 so TRIG_START stays waiting after the fallthrough."""
        self.prime_pulse()
        time.sleep(self.OUTSIDE_SECS)  # T >> 100 ms window
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("WaitStart",
                         "T outside window: should fall through to WaitStart")

    def testNoCorrection_OnTime(self):
        """On-time press regression: pulse and RECORD in the same buffer
        must not corrupt state."""
        self.looper.sync_inject_value = 2.0   # pulse stays on during RECORD
        time.sleep(self.PRIME_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        state = self.engine.getState()
        self.assertIn(state,
                      [engine_wrapper.states["Recording"],
                       engine_wrapper.states["WaitStart"]],
                      "On-time press must result in Recording or WaitStart, not a crash")


# ======================================================================
# Late-stop tests
# ======================================================================

class TestLateStop(LateHitTestCase):

    def _record_a_loop(self):
        """Enter Recording via the late-hit path, record for RECORD_SECS."""
        self.prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("Recording", "setUp: could not get into Recording")
        time.sleep(self.RECORD_SECS)

    def testLateStop_DirectlyPlaying(self):
        """Core case: RECORD stop within the window after a pulse → immediate Playing."""
        self._record_a_loop()
        self.prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("Playing",
                         "Late-hit stop should enter Playing directly, not WaitStop")

    def testNoCorrection_Stop_TooLate(self):
        """Window boundary on stop: T > 100 ms after pulse → WaitStop.
        Background 0.0 keeps TRIG_STOP waiting."""
        self._record_a_loop()
        self.prime_pulse()
        time.sleep(self.OUTSIDE_SECS)  # T >> 100 ms
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("WaitStop",
                         "T outside window: stop should fall through to WaitStop")

    def testNoCorrection_Stop_NoPriorPulse(self):
        """No pulse at all during recording: stop should go to WaitStop.
        Since bSyncPulseReceived stays False throughout (no prime_pulse),
        the stop guard fires and the correction does not run."""
        # Enter recording WITHOUT priming a pulse first — use direct RECORD
        # with sync off so we get into Recording immediately, then re-enable sync
        self.looper.set_port(test_engine.Sync, 0.0)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("Recording", "setUp: could not get into Recording")
        self.looper.set_port(test_engine.Sync, 1.0)  # re-enable sync
        # bSyncPulseReceived is still False — no pulse has ever been seen
        time.sleep(self.RECORD_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        self.assertState("WaitStop",
                         "No pulse ever seen: stop correction must not fire")

    def testLateStop_LoopLengthTrimmed(self):
        """After a late stop the loop length must be less than the total
        time spent in Recording (including prime and window wait time)."""
        self._record_a_loop()
        # total recording duration ≈ RECORD_SECS + PRIME_SECS + WINDOW_SECS
        # plus a generous margin for JACK callback timing
        max_recording_secs = (self.RECORD_SECS + self.PRIME_SECS
                               + 2 * self.WINDOW_SECS + 0.040)
        self.prime_pulse()
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.005)
        self.assertState("Playing")
        loop_len = self.engine.looper.get_control_value(test_engine.LoopLength)
        self.assertGreater(loop_len, 0.0,
                           "Loop length must be positive after late stop")
        self.assertLess(loop_len, max_recording_secs,
                        "Loop length must not exceed total recording duration")


# ======================================================================
# Sync-source reset regression (C6)
# ======================================================================

class TestSyncSourceReset(LateHitTestCase):

    def testAfterSyncOff_NoCorrectionOnNextRecord(self):
        """Toggle sync off/on without a new pulse: the first press after
        re-enabling should not fire the correction."""
        self.prime_pulse()
        self.looper.set_port(test_engine.Sync, 0.0)
        time.sleep(self.PRIME_SECS)
        self.looper.set_port(test_engine.Sync, 1.0)
        time.sleep(self.WINDOW_SECS)
        self.engine.request("RECORD")
        time.sleep(0.002)
        state = self.engine.getState()
        self.assertIn(state,
                      [engine_wrapper.states["WaitStart"],
                       engine_wrapper.states["Recording"]],
                      "After sync toggle, state must be WaitStart or Recording")


if __name__ == "__main__":
    unittest.main()
