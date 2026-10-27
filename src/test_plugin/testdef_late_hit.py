#! /usr/bin/env python3
"""
Regression tests for the late-hit recording correction patch.

Timing is controlled via process_count (a counter incremented every JACK
callback) rather than time.sleep(), making tests independent of JACK period
size.  The correction window is set explicitly in samples via
g_late_hit_window_ms before each test, so "within window" and "outside
window" are exact rather than probabilistic.

Sync buffer injection values:
  0.0  — no sync signal; STATE_TRIG_START/STOP waits indefinitely
  2.0  — sync pulse (> 1.5 threshold); sets bSyncPulseReceived and
         resets lGlobalSamplesSinceSync
"""

import time
import unittest
import test_engine
import engine_wrapper

_DEFAULT_WINDOW_MS = 100.0   # restore in tearDown


class LateHitTestCase(unittest.TestCase):
    """Base class providing buffer-precise timing helpers."""

    # Number of buffer-periods used for each phase.  The window is set to
    # WINDOW_BUFS buffer-periods so "within" and "outside" are exact.
    PULSE_BUFS   = 3    # how long to hold the pulse high
    WITHIN_BUFS  = 2    # wait this many no-pulse buffers → T < window
    OUTSIDE_BUFS = 6    # wait this many no-pulse buffers → T > window
    RECORD_BUFS  = 10   # minimum recording duration before a late stop

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_latehit")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())

        # Set window = WITHIN_BUFS+1 buffer-periods so WITHIN_BUFS is inside
        # and OUTSIDE_BUFS is outside.  Use WITHIN_BUFS+1 as the exact ceiling.
        self._set_window(self.WITHIN_BUFS + 1)

        # Arm sync injection BEFORE raising fSyncMode to 1.  BLOCK 8 in plugin.cc
        # runs on every JACK callback whenever fSyncMode >= 1.0, and reads
        # pfSyncInput (= _our_syncin_buf) immediately.  _our_syncin_buf is
        # allocated with `new` and contains uninitialized memory until the fill
        # loop runs for the first time.  If set_port(Sync, 1.0) were called first,
        # a JACK callback could fire between that call and the two lines below,
        # reading garbage from the uninitialised buffer.  Any sample > 1.5 would
        # set bSyncPulseReceived = true on instance 0, invalidating the guard that
        # testNoCorrection_Stop_NoPriorPulse relies on.
        #
        # Correct order: enable injection and set value to 0.0 first, so the buffer
        # is filled with 0.0 before BLOCK 8 can see it.
        self.looper.sync_inject_value   = 0.0   # no signal
        self.looper.sync_inject_enabled = True   # fill buffer before fSyncMode=1
        self.looper.set_port(test_engine.Sync, 1.0)

    def tearDown(self):
        test_engine.cvar.g_late_hit_window_ms = _DEFAULT_WINDOW_MS
        del self.engine

    # ------------------------------------------------------------------
    # helpers

    def _set_window(self, n_bufs):
        """Set the correction window to exactly n_bufs JACK buffer-periods."""
        test_engine.cvar.g_late_hit_window_ms = (n_bufs * self.buf / self.sr) * 1000.0

    def wait_buffers(self, n, timeout=5.0):
        """Busy-wait until exactly n more JACK callbacks have completed."""
        target   = self.looper.process_count + n
        # `timeout` is a floor; slow periods get 3x the real time n buffers take.
        period = (float(self.looper.get_buffer_size())
                  / float(self.engine.get_sample_rate()))
        deadline = time.time() + max(timeout, 3.0 * n * period)
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    f"Timed out waiting for {n} JACK callbacks "
                    f"(process_count stuck at {self.looper.process_count})")

    def prime_pulse(self):
        """Inject a sync pulse for PULSE_BUFS callbacks, then clear.
        On return: bSyncPulseReceived=True, lGlobalSamplesSinceSync=0."""
        self.looper.sync_inject_value = 2.0
        self.wait_buffers(self.PULSE_BUFS)
        self.looper.sync_inject_value = 0.0

    def assertState(self, state, msg=None):
        actual = self.engine.getState()
        self.assertEqual(
            actual,
            engine_wrapper.states[state],
            (engine_wrapper.states[actual] + " != " + state)
            if msg is None else msg
        )

    @property
    def t_within(self):
        """T in samples after WITHIN_BUFS no-pulse buffers."""
        return self.WITHIN_BUFS * self.buf

    @property
    def t_outside(self):
        """T in samples after OUTSIDE_BUFS no-pulse buffers."""
        return self.OUTSIDE_BUFS * self.buf

    @property
    def window_samples(self):
        """Current window ceiling in samples."""
        return (self.WITHIN_BUFS + 1) * self.buf


# ======================================================================
# Late-start tests
# ======================================================================

class TestLateStart(LateHitTestCase):

    def testLateStart_DirectlyRecording(self):
        """Core case: RECORD WITHIN_BUFS after a pulse → Recording directly.
        T = WITHIN_BUFS × buf_size < window = (WITHIN_BUFS+1) × buf_size."""
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)   # T = WITHIN_BUFS × buf exactly
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Recording",
                         f"T={self.t_within} ≤ window={self.window_samples}: "
                         "late-hit should enter Recording directly")

    def testNoCorrection_SyncOff(self):
        """Regression: sync OFF → Recording immediately (stock path unchanged)."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Recording", "Sync off: stock path must still reach Recording")

    def testNoCorrection_NoPulse(self):
        """bSyncPulseReceived guard: no pulse ever → WaitStart.
        Background 0.0 keeps TRIG_START waiting indefinitely."""
        self.wait_buffers(self.WITHIN_BUFS)   # T in range but no pulse seen
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("WaitStart",
                         "No pulse: correction must not fire without bSyncPulseReceived")

    def testNoCorrection_TooLate(self):
        """Window boundary: T = OUTSIDE_BUFS × buf > window → WaitStart.
        Background 0.0 keeps TRIG_START waiting."""
        self.prime_pulse()
        self.wait_buffers(self.OUTSIDE_BUFS)  # T > window
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("WaitStart",
                         f"T={self.t_outside} > window={self.window_samples}: "
                         "no correction should fire")

    def testNoCorrection_OnTime(self):
        """On-time press (T=0): pulse and RECORD in the same callbacks.
        Must not crash; acceptable results are Recording or WaitStart."""
        self.looper.sync_inject_value = 2.0   # pulse held on
        self.wait_buffers(self.PULSE_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        state = self.engine.getState()
        self.assertIn(state,
                      [engine_wrapper.states["Recording"],
                       engine_wrapper.states["WaitStart"]],
                      "On-time press must not produce an unexpected state")


# ======================================================================
# Late-stop tests
# ======================================================================

class TestLateStop(LateHitTestCase):

    def _record_a_loop(self):
        """Enter Recording via the late-hit path, record for RECORD_BUFS."""
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Recording", "_record_a_loop: could not enter Recording")
        self.wait_buffers(self.RECORD_BUFS)

    def testLateStop_DirectlyPlaying(self):
        """Core case: RECORD stop WITHIN_BUFS after a pulse → Playing directly."""
        self._record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Playing",
                         "Late-hit stop should enter Playing directly, not WaitStop")

    def testNoCorrection_Stop_TooLate(self):
        """Window boundary on stop: T > window → WaitStop."""
        self._record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.OUTSIDE_BUFS)  # T > window
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("WaitStop",
                         f"T={self.t_outside} > window={self.window_samples}: "
                         "stop correction must not fire")

    def testNoCorrection_Stop_NoPriorPulse(self):
        """bSyncPulseReceived guard: recording started without sync, so no
        pulse has been seen.  Stop correction must not fire."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Recording", "Could not enter Recording with sync off")
        self.looper.set_port(test_engine.Sync, 1.0)
        # bSyncPulseReceived still False — no pulse has been injected
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("WaitStop",
                         "No pulse seen: stop correction must not fire")

    def testLateStop_LoopLengthTrimmed(self):
        """After a late stop the loop must be shorter than the raw recording.
        T = WITHIN_BUFS × buf, so trim = T / sr seconds.
        raw_recording ≈ (PULSE_BUFS + WITHIN_BUFS + RECORD_BUFS + PULSE_BUFS
                          + WITHIN_BUFS) × buf / sr.
        trimmed ≈ raw - T."""
        self._record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Playing")

        loop_len = self.engine.looper.get_control_value(test_engine.LoopLength)

        # Total recording duration (known exactly in buffers):
        #   _record_a_loop: PULSE_BUFS + WITHIN_BUFS + 1(settle) + RECORD_BUFS
        #   stop phase:     PULSE_BUFS + WITHIN_BUFS
        total_recording_bufs = (2 * self.PULSE_BUFS + 2 * self.WITHIN_BUFS
                                 + 1 + self.RECORD_BUFS)
        raw_secs = total_recording_bufs * self.buf / self.sr
        # T = WITHIN_BUFS buffer-periods
        t_secs   = self.WITHIN_BUFS * self.buf / self.sr

        self.assertGreater(loop_len, 0.0,
                           "Loop length must be positive")
        # Upper bound: loop must be shorter than total recording time
        # (trimmed by T; add a few buffers of slack for scheduling jitter)
        max_secs = raw_secs + 0.030
        self.assertLess(loop_len, max_secs,
                        f"Loop ({loop_len:.4f}s) must be < raw recording "
                        f"({raw_secs:.4f}s + 30ms slack)")
        # The trim T should be visible
        self.assertLess(loop_len, raw_secs,
                        f"Loop ({loop_len:.4f}s) should be shorter than "
                        f"raw recording ({raw_secs:.4f}s) by T ({t_secs:.4f}s)")


# ======================================================================
# Rel Sync: late-hit must not apply (fSyncMode == 2)
# ======================================================================

class TestRelSyncNoLateHit(LateHitTestCase):
    """Under Rel Sync a record starts where it is pressed and its end waits
    for the same offset from the pulse, so the late-hit start and stop
    corrections must stay out of it.  Before the fix the stop was trimmed
    to the pulse, giving a loop that was not a whole number of cycles.

    These use the harness's single-sample pulse generator, as a real
    source gives, rather than prime_pulse(): Rel Sync tracks the pulse
    spacing (REL_SYNC_STALE_PHASE_FIX), and a pulse held high for whole
    buffers is a spacing of one sample."""

    CYCLE_BUFS = 16

    def setUp(self):
        LateHitTestCase.setUp(self)
        self.cycle = self.CYCLE_BUFS * self.buf
        self.looper.sync_clock = 0
        self.looper.sync_inject_period = self.cycle
        self.looper.set_port(test_engine.Sync, 2.0)    # Rel Sync
        self.wait_buffers(2 * self.CYCLE_BUFS)

    def tearDown(self):
        self.looper.sync_inject_period = 0
        LateHitTestCase.tearDown(self)

    def wait_bufs_after_pulse(self, n):
        """Wait until the next block starts n buffers after a pulse."""
        while self.looper.sync_clock % self.cycle != n * self.buf:
            self.wait_buffers(1)

    def testRelSync_LateStopNotApplied(self):
        self.wait_bufs_after_pulse(self.OUTSIDE_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState("Recording", "Rel Sync record should start at once")
        self.wait_buffers(self.RECORD_BUFS)
        # inside the window after a pulse, but before the take's end point
        self.wait_bufs_after_pulse(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState("WaitStop",
                         "Rel Sync stop must wait for its offset, not be "
                         "late-hit corrected to the pulse")

    def testRelSync_LateStartNotBackfilled(self):
        # A late-hit start would backfill T samples, putting the position
        # about T ahead of a plain start.
        self.wait_bufs_after_pulse(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState("Recording")
        pos = self.looper.get_control_value(test_engine.LoopPosition) * self.sr
        self.assertLess(pos, self.t_within + self.buf // 2,
                        "position %.0f samples suggests the start was "
                        "backfilled to the pulse (T=%d)" % (pos, self.t_within))


# ======================================================================
# Window constant tests
# ======================================================================

class TestWindowControl(LateHitTestCase):
    """Verify that g_late_hit_window_ms actually controls the boundary."""

    def testWindowBoundaryExact(self):
        """At T = window-1 buf correction fires; at T = window+1 buf it does not.
        This pins the window arithmetic directly."""
        # --- within: window set to WITHIN_BUFS+1, T = WITHIN_BUFS → fires
        self._set_window(self.WITHIN_BUFS + 1)
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("Recording",
                         f"T={self.t_within} inside window={self.window_samples}: "
                         "correction should fire")

        # tear down and rebuild for the outside case
        del self.engine
        self.engine = engine_wrapper.Engine("SL_test_latehit")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())
        self.looper.set_port(test_engine.Sync, 1.0)
        self.looper.sync_inject_enabled = True
        self.looper.sync_inject_value   = 0.0

        # --- outside: window set to WITHIN_BUFS-1, T = WITHIN_BUFS → does not fire
        self._set_window(self.WITHIN_BUFS - 1)
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)     # now T > window
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        self.assertState("WaitStart",
                         f"T={self.t_within} outside window="
                         f"{(self.WITHIN_BUFS-1)*self.buf}: "
                         "correction must not fire")


# ======================================================================
# Sync-source reset regression (C6)
# ======================================================================

class TestSyncSourceReset(LateHitTestCase):

    def testAfterSyncOff_NoCorrectionOnNextRecord(self):
        """Toggle sync off/on without a new pulse: first press should not
        fire the correction (bSyncPulseReceived cleared on mode change)."""
        self.prime_pulse()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(self.PULSE_BUFS)
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)  # let JACK callback settle
        state = self.engine.getState()
        self.assertIn(state,
                      [engine_wrapper.states["WaitStart"],
                       engine_wrapper.states["Recording"]],
                      "After sync toggle, state must be WaitStart or Recording")


# ======================================================================
# Exact lengths and fades
# ======================================================================

class TestLateHitExact(LateHitTestCase):
    """A late-hit take must equal an on-time one: the same length and the
    same fades.  Pulses here are single samples at buffer starts
    (sync_inject_single), so a loop from pulse to pulse is a whole number
    of buffers, however the presses fall.  The input is a constant level,
    so a loop with its fades in the right places plays back flat: a fade
    left inside the loop dips toward 0, an overlay rises toward 2x."""

    IL, OL = 300, 500
    LEVEL = 0.5

    def setUp(self):
        super().setUp()
        self._set_window(self.WITHIN_BUFS + 3)   # margin for request timing
        self.looper.sync_inject_single = True
        self.looper.input_inject_value = self.LEVEL
        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.set_port(test_engine.WetLevel, 1.0)
        self.looper.set_port(test_engine.InputLatency, float(self.IL))
        self.looper.set_port(test_engine.OutputLatency, float(self.OL))
        self.looper.set_port(test_engine.FadeSamples, 64.0)

    def rec_bufs(self):
        """A loop several times longer than IL + OL."""
        return max(self.RECORD_BUFS, 4 * (self.IL + self.OL) // self.buf + 1)

    def loop_len(self):
        return int(round(self.looper.get_control_value(test_engine.LoopLength) * self.sr))

    def start(self, late, extra_bufs=0):
        if late:
            self.prime_pulse()
            self.wait_buffers(self.WITHIN_BUFS + extra_bufs)
            self.engine.request("RECORD")
        else:
            self.engine.request("RECORD")
            self.wait_buffers(1)
            self.assertState("WaitStart")
            self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Recording")
        self.wait_buffers(self.rec_bufs())

    def stop(self, late, extra_bufs=0):
        if late:
            self.prime_pulse()
            self.wait_buffers(self.WITHIN_BUFS + extra_bufs)
            self.engine.request("RECORD")
        else:
            self.engine.request("RECORD")
            self.wait_buffers(1)
            self.assertState("WaitStop")
            self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Playing")

    def assertTake(self, what):
        L = self.loop_len()
        self.assertEqual(L % self.buf, 0,
                         f"{what}: loop length {L} is not a whole number of "
                         f"{self.buf}-sample buffers (pulse to pulse); one off is "
                         "the late-hit off-by-one")
        fade = int(self.looper.get_control_value(test_engine.FadeSamples))
        self.wait_buffers(3 + fade // self.buf)   # past the play fade-in
        self.looper.reset_output_stats()
        self.wait_buffers(L // self.buf + 3)      # one full pass and more
        lo, hi = self.looper.output_min, self.looper.output_max
        self.assertTrue(0.9 * self.LEVEL <= lo and hi <= 1.1 * self.LEVEL,
                        f"{what}: playback ranges {lo:.3f}..{hi:.3f}, expected a flat "
                        f"{self.LEVEL} (a misplaced record fade dips or overlays)")

    def testOnTime_Reference(self):
        """Control: on-time start and stop give a whole number of buffers, flat."""
        self.start(late=False)
        self.stop(late=False)
        self.assertTake("on-time start and stop")

    def testLateStart_SameAsOnTime(self):
        """Late start: backfill from the pulse sample itself, fade at index 0."""
        self.start(late=True)
        self.stop(late=False)
        self.assertTake("late start")

    def testLateStop_SameAsOnTime(self):
        """Late stop: trim back to just before the pulse sample; the seam
        crossfade at index 0, not where playback took over."""
        self.start(late=False)
        self.stop(late=True)
        self.assertTake("late stop")

    def testLateStartAndStop_FadeLongerThanLateness(self):
        """Both late, with a fade longer than the lateness, so both fades
        are split between the late-hit code and the normal state code.  The
        stop is a buffer later than the start: equally late, the old code's
        misplaced fade-in and fade-out landed on the same index and
        cancelled, as did its two length errors."""
        self.looper.set_port(test_engine.FadeSamples,
                             float((self.WITHIN_BUFS + 4) * self.buf))
        self.start(late=True)
        self.stop(late=True, extra_bufs=1)
        self.assertTake("late start and stop, long fade")


if __name__ == "__main__":
    unittest.main()
