#! /usr/bin/env python3
"""
Regression tests for the mute/unmute late-hit correction patch.

Covers both directions of ROADMAP entry 5: a MUTE press arriving just
after a sync boundary is treated as if it arrived at that boundary,
instead of waiting for the NEXT one -- both entering mute (from Playing)
and leaving mute (unmute, from Muted). Unlike Record's late-hit
correction, neither direction reconstructs buffer content: dCurrPos keeps
advancing while muted, so this is a pure branch-selection decision. See
g_mute_late_hit_ms's comment block in plugin.cc and
mod_docs/ROADMAP.rst entry 5.

Same buffer-precise timing technique as testdef_late_hit.py: timing is
controlled via process_count rather than time.sleep(), and the window is
set explicitly in samples via g_mute_late_hit_ms before each test, so
"within window" and "outside window" are exact rather than probabilistic.

Sync buffer injection values:
  0.0  — no sync signal; a quantized mute/unmute waits indefinitely
  2.0  — sync pulse (> 1.5 threshold); sets bSyncPulseReceived and
         resets lGlobalSamplesSinceSync
"""

import time
import unittest
import test_engine
import engine_wrapper

_DEFAULT_WINDOW_MS = 100.0  # compiled-in default (on, matches g_late_hit_window_ms); restore in tearDown


class MuteLateHitTestCase(unittest.TestCase):
    """Base class providing buffer-precise timing helpers, mirroring
    testdef_late_hit.py's LateHitTestCase, plus mute-quantize setup and
    helpers to reach Playing/Muted."""

    PULSE_BUFS   = 3    # how long to hold the pulse high
    WITHIN_BUFS  = 2    # wait this many no-pulse buffers → T < window
    OUTSIDE_BUFS = 6    # wait this many no-pulse buffers → T > window

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_mutelatehit")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())

        # Window = WITHIN_BUFS+1 buffer-periods, so WITHIN_BUFS is inside
        # and OUTSIDE_BUFS is outside -- same ceiling technique as
        # testdef_late_hit.py.
        self._set_window(self.WITHIN_BUFS + 1)

        # Mute-quantize on, so a plain MUTE/MUTE_ON/MUTE_OFF takes the
        # "wait for sync" branch by default -- otherwise there is nothing
        # for the late-hit correction to correct.
        self.looper.set_port(test_engine.MuteQuantized, 1.0)

        # Arm sync injection BEFORE raising fSyncMode to 1 -- see
        # testdef_late_hit.py's setUp for why this ordering matters
        # (avoids a race reading uninitialised sync-buffer memory).
        self.looper.sync_inject_value   = 0.0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 1.0)

        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.set_port(test_engine.WetLevel, 1.0)

    def tearDown(self):
        test_engine.cvar.g_mute_late_hit_ms = _DEFAULT_WINDOW_MS
        del self.engine

    # ------------------------------------------------------------------
    # helpers

    def _set_window(self, n_bufs):
        """Set the correction window to exactly n_bufs JACK buffer-periods."""
        test_engine.cvar.g_mute_late_hit_ms = (n_bufs * self.buf / self.sr) * 1000.0

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

    def record_a_loop(self, level=0.5):
        """Record a short take with sync off (so the record itself is not
        subject to any late-hit correction), leaving it Playing."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.wait_buffers(15)
        self.engine.request("RECORD")
        self.wait_buffers(5)
        self.assertState("Playing", "record_a_loop: could not enter Playing")
        self.looper.set_port(test_engine.Sync, 1.0)

    @property
    def t_within(self):
        return self.WITHIN_BUFS * self.buf

    @property
    def t_outside(self):
        return self.OUTSIDE_BUFS * self.buf

    @property
    def window_samples(self):
        return (self.WITHIN_BUFS + 1) * self.buf


# ======================================================================
# Unmute tests
# ======================================================================

class TestUnmuteLateHit(MuteLateHitTestCase):

    def _mute_it(self):
        self.record_a_loop()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.engine.request("MUTE_ON")
        self.wait_buffers(5)
        self.assertState("Muted", "_mute_it: could not enter Muted")
        self.looper.set_port(test_engine.Sync, 1.0)

    def testUnmuteWithinWindow_IsImmediate(self):
        """Core case: MUTE_OFF WITHIN_BUFS after a pulse → Playing directly,
        not left waiting for the next boundary."""
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState("Playing",
                         f"T={self.t_within} <= window={self.window_samples}: "
                         "late-hit unmute should enter Playing directly")

    def testUnmuteWithinWindow_ContentCorrect(self):
        """Same as above, confirmed via actual audio content -- the
        unmuted loop must be the correctly-phased take, not silence or a
        discontinuity (dCurrPos never stopped advancing while muted, so
        this should just work, but verify rather than assume)."""
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState("Playing")
        self.looper.reset_output_stats()
        self.wait_buffers(20)
        self.assertAlmostEqual(self.looper.output_min, 0.5, places=3)
        self.assertAlmostEqual(self.looper.output_max, 0.5, places=3)

    def testUnmuteOutsideWindow_StillWaits(self):
        """Regression guard: a press well past the window still waits for
        the real next pulse -- stock behaviour for genuinely late presses
        preserved."""
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.OUTSIDE_BUFS)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState("Muted",
                         f"T={self.t_outside} > window={self.window_samples}: "
                         "correction must not fire")

    def testUnmuteZeroDisablesCorrection(self):
        """0.0 must disable the feature entirely -- same "0 disables"
        convention as the other late-hit/failsafe globals. (The
        compiled-in default is 100.0, on -- see _DEFAULT_WINDOW_MS.)"""
        test_engine.cvar.g_mute_late_hit_ms = 0.0
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState("Muted",
                         "g_mute_late_hit_ms=0: correction must not fire")

    def testPauseOnUnaffectedByMuteLateHit(self):
        """Scoping guard: unmuteLateHit requires lMultiCtrl == MULTI_MUTE,
        so it can never fire for a MULTI_PAUSE request (PAUSE_ON) sharing
        this same STATE_MUTE/STATE_ONESHOT/STATE_PAUSED case, no matter
        the timing. Same T, same window, same quantize/sync setup as
        testUnmuteWithinWindow_IsImmediate -- only the command differs --
        so PAUSE_ON must still wait for the next real boundary rather
        than transitioning immediately (pre-existing, unaffected
        behaviour: mute-quantize gates PAUSE_ON exactly like MUTE_OFF,
        just never through the new disjunct)."""
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)   # same T as the unmute case
        self.engine.request("PAUSE_ON")
        self.wait_buffers(1)
        self.assertState("Muted",
                         "PAUSE_ON must still wait for sync -- "
                         "unmuteLateHit must not leak into MULTI_PAUSE")
        # Confirm the normal wait-for-sync path still resolves once a real
        # pulse arrives -- no regression in ordinary PAUSE_ON use. The
        # resolved state is Paused, not Playing -- was a pre-existing
        # stock quirk (untouched by this file's own patch, entry 5) but
        # now fixed by PAUSE_FROM_MUTE_FIX (ROADMAP entry 7, see
        # testdef_pause_from_mute.py for the dedicated regression tests).
        # This assertion tracks entry 7's fix, not entry 5's own scope --
        # kept here because this exact scenario is what surfaced the bug
        # in the first place.
        self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Paused",
                         "PAUSE_FROM_MUTE_FIX: PAUSE_ON from Muted must "
                         "resolve to Paused once fixed")


# ======================================================================
# Entering-mute tests
# ======================================================================

class TestMuteLateHit(MuteLateHitTestCase):

    def testMuteWithinWindow_IsImmediate(self):
        """Core case: MUTE_ON WITHIN_BUFS after a pulse → Muted directly,
        not left audibly playing until the next boundary."""
        self.record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_ON")
        self.wait_buffers(1)
        self.assertState("Muted",
                         f"T={self.t_within} <= window={self.window_samples}: "
                         "late-hit mute should enter Muted directly")

    def testMuteWithinWindow_IsSilent(self):
        """Confirms the loop is actually silenced, not just state-labelled
        Muted with audio still leaking through."""
        self.record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_ON")
        self.wait_buffers(1)
        self.assertState("Muted")
        self.looper.reset_output_stats()
        self.wait_buffers(20)
        self.assertAlmostEqual(self.looper.output_min, 0.0, places=3)
        self.assertAlmostEqual(self.looper.output_max, 0.0, places=3)

    def testMuteOutsideWindow_StillWaits(self):
        """Regression guard: a press well past the window still waits for
        the real next pulse."""
        self.record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.OUTSIDE_BUFS)
        self.engine.request("MUTE_ON")
        self.wait_buffers(1)
        self.assertState("Playing",
                         f"T={self.t_outside} > window={self.window_samples}: "
                         "correction must not fire")

    def testMuteZeroDisablesCorrection(self):
        """0.0 must disable the feature entirely (compiled-in default is
        100.0, on -- see _DEFAULT_WINDOW_MS)."""
        test_engine.cvar.g_mute_late_hit_ms = 0.0
        self.record_a_loop()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_ON")
        self.wait_buffers(1)
        self.assertState("Playing",
                         "g_mute_late_hit_ms=0: correction must not fire")

    def testRecordEntryUnaffected(self):
        """Scoping guard: muteLateHit requires pLS->state == STATE_PLAY,
        so it must not interact with the STATE_RECORD/STATE_TRIG_STOP
        output-latency-compensation branch sharing the same default:
        case. Record a loop (sync off, so no interference from Record's
        own late-hit correction) and confirm it reaches Playing normally
        -- the entering-mute default: case is never even reached here,
        but this is the most direct behavioural check available without
        instrumenting the RT thread."""
        self.record_a_loop()
        self.assertState("Playing", "Plain record/stop must be unaffected")


if __name__ == "__main__":
    unittest.main()
