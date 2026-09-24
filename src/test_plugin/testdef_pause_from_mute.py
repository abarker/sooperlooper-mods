#! /usr/bin/env python3
"""
Regression tests for PAUSE_FROM_MUTE_FIX (ROADMAP entry 7).

Stock SooperLooper 1.7.9 bug (verified present verbatim in
original_plugin.cc, not introduced by any patch in this fork): with
mute-quantize on and sync active, pressing Pause while Muted has to wait
for the next sync boundary. The wait-for-sync resolution only
special-cased the reverse direction (Paused+Mute -> Mute); it had no
case for Mute+Pause -> Paused, so it fell through to the generic
"resolve to Playing" branch instead -- the performer presses Pause and
gets audible playback.

The fix has two parts, both gated by PAUSE_FROM_MUTE_FIX (plugin.cc):
  1. The missing case in the wait-for-sync branch.
  2. transitionToNext()'s STATE_PAUSED case: wasMuted must be derived
     from pLS->state (true only when actually coming from STATE_MUTE),
     not unconditionally false -- otherwise an Undo fired while paused
     would resume to Playing instead of Mute.

Same buffer-precise timing technique as testdef_mute_late_hit.py
(process_count, not time.sleep()).
"""

import time
import unittest
import test_engine
import engine_wrapper


class PauseFromMuteTestCase(unittest.TestCase):

    PULSE_BUFS = 3   # how long to hold a sync pulse high

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_pausefrommute")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())

        # Mute-quantize on, so a plain PAUSE_ON/MUTE_ON takes the "wait
        # for sync" branch by default -- otherwise there is nothing for
        # this fix to correct.
        self.looper.set_port(test_engine.MuteQuantized, 1.0)

        # Arm sync injection before raising fSyncMode to 1 -- same
        # ordering testdef_late_hit.py/testdef_mute_late_hit.py use, to
        # avoid a race reading uninitialised sync-buffer memory.
        self.looper.sync_inject_value   = 0.0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 1.0)

        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.set_port(test_engine.WetLevel, 1.0)

    def tearDown(self):
        del self.engine

    def wait_buffers(self, n, timeout=5.0):
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

    def record_and_mute(self, level=0.5):
        """Record a short take (sync off, so Record's own late-hit
        correction can't interfere), then mute it immediately (quantize
        off during the mute press for a deterministic, instant mute)."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.wait_buffers(15)
        self.engine.request("RECORD")
        self.wait_buffers(5)
        self.assertState("Playing", "record_and_mute: could not enter Playing")

        self.looper.set_port(test_engine.MuteQuantized, 0.0)   # instant mute
        self.engine.request("MUTE_ON")
        self.wait_buffers(2)
        self.assertState("Muted", "record_and_mute: could not enter Muted")
        self.looper.set_port(test_engine.MuteQuantized, 1.0)   # restore for the test
        self.looper.set_port(test_engine.Sync, 1.0)


class TestPauseFromMuteFix(PauseFromMuteTestCase):

    def testPauseFromMuteQuantized_ResolvesToPaused(self):
        """The core fix: Muted, mute-quantize + sync on, PAUSE_ON has to
        wait for a boundary -> must resolve to Paused, not Playing."""
        self.record_and_mute()
        self.engine.request("PAUSE_ON")
        self.wait_buffers(2)
        self.assertState("Muted", "PAUSE_ON must wait for sync, not act immediately")

        self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Paused",
                         "PAUSE_FROM_MUTE_FIX: quantized Pause from Muted "
                         "must resolve to Paused, not Playing")

    def testPauseFromMuteQuantized_PositionFrozen(self):
        """dPausedPos correctness: once Paused, the loop position must
        stay frozen (not silently resume advancing/playing audibly)."""
        self.record_and_mute()
        self.engine.request("PAUSE_ON")
        self.wait_buffers(2)
        self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Paused")

        pos1 = self.looper.get_control_value(test_engine.LoopPosition)
        self.wait_buffers(20)
        pos2 = self.looper.get_control_value(test_engine.LoopPosition)
        self.assertAlmostEqual(pos1, pos2, places=4,
                               msg="Loop position must stay frozen while Paused")

        # And silent -- confirms this is really Paused, not a disguised
        # Playing (which would also eventually "look frozen" only by
        # coincidence if output happened to be constant; check audio too).
        self.looper.reset_output_stats()
        self.wait_buffers(20)
        self.assertAlmostEqual(self.looper.output_min, 0.0, places=3)
        self.assertAlmostEqual(self.looper.output_max, 0.0, places=3)

    def testPauseFromMuteQuantized_UndoAllGoesOffMuted(self):
        """wasMuted correctness -- the fix's second, non-obvious part --
        for the single-take case. With no prior loop chunk (loop->prev
        is NULL), Undo takes the STATE_UNDO_ALL path
        (plugin.cc:3812-3843), not the simpler
        `state = wasMuted ? MUTE : PLAY` branch -- but STATE_UNDO_ALL's
        own resolution (plugin.cc:5649-5657) *also* reads wasMuted, to
        choose OffMuted vs. plain Off. Without the fix, wasMuted would
        be false here and this would land on plain Off instead."""
        self.record_and_mute()
        self.engine.request("PAUSE_ON")
        self.wait_buffers(2)
        self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Paused", "setup: must reach Paused via the fixed path")

        self.engine.request("UNDO")
        self.wait_buffers(10)
        self.assertState("OffMuted",
                         "wasMuted must be true after resolving Mute->Pause via "
                         "the wait-for-sync path, so undoing the only take "
                         "lands on OffMuted, not plain Off -- getting Off here "
                         "means the wasMuted half of PAUSE_FROM_MUTE_FIX "
                         "regressed")

    def testPauseFromMuteQuantized_UndoWithPriorTakeResumesMute(self):
        """Same wasMuted correctness, exercising the *other* fork of the
        undo switch: with a real prior loop chunk (loop->prev set), Undo
        takes the direct `state = wasMuted ? MUTE : PLAY` branch
        (plugin.cc:3816) instead of STATE_UNDO_ALL. Without the fix,
        this would resume to Playing instead of Muted."""
        self.record_and_mute()

        # Record a second take over the first (unmute, interrupt
        # playback with a fresh RECORD -- creates loop->prev), then
        # re-mute so the setup matches record_and_mute's end state.
        # Sync off for the recording itself -- record_and_mute() leaves
        # Sync on, and a quantized/synced record start would itself wait
        # rather than recording immediately.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.MuteQuantized, 0.0)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(2)
        self.assertState("Playing", "could not unmute to record a second take")
        self.looper.input_inject_value = 0.7
        self.engine.request("RECORD")
        self.wait_buffers(15)
        self.engine.request("RECORD")
        self.wait_buffers(5)
        self.assertState("Playing", "could not complete the second take")
        self.engine.request("MUTE_ON")
        self.wait_buffers(2)
        self.assertState("Muted", "could not re-mute after the second take")
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        self.looper.set_port(test_engine.Sync, 1.0)

        self.engine.request("PAUSE_ON")
        self.wait_buffers(2)
        self.prime_pulse()
        self.wait_buffers(1)
        self.assertState("Paused", "setup: must reach Paused via the fixed path")

        self.engine.request("UNDO")
        self.wait_buffers(2)
        self.assertState("Muted",
                         "with a prior take, Undo must resume to Muted, not "
                         "Playing -- this is the direct wasMuted ? MUTE : PLAY "
                         "branch the fix's Part 2 targets")

    def testPauseFromMuteImmediate_Unaffected(self):
        """Regression guard: the pre-existing immediate path (quantize
        off) is untouched by this fix -- Mute+Pause still goes straight
        to Paused with no wait."""
        self.record_and_mute()
        self.looper.set_port(test_engine.MuteQuantized, 0.0)
        self.engine.request("PAUSE_ON")
        self.wait_buffers(2)
        self.assertState("Paused",
                         "Immediate (unquantized) Mute->Pause must be unaffected")


if __name__ == "__main__":
    unittest.main()
