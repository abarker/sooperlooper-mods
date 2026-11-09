#! /usr/bin/env python3
"""
Tests for the Rel Sync known-phase design:
the loop keeps track of where it is in the cycle while its Sync is off, so
a Rel Sync record can start at the press as soon as Rel Sync comes on.

Background
----------
REL_SYNC_STALE_PHASE_FIX (see testdef_rel_sync_stale_phase.py) marks the
count unknown whenever Rel Sync comes on, so the first record after that
waits for the next cycle pulse.  But the pulses reach channel 0 of every
loop whatever its Sync setting; the loop only stopped counting them.  With
a count kept all the time:

* Rel Sync coming on while pulses flow: the record starts at the press.
* The count is unknown -- and the record waits for the next pulse, as now
  -- only when it really is: no pulse since the loop started, a Quantize
  change (the pulse spacing changes), a sync source change (real engine
  only; Looper::reset_late_hit_state()), or no pulse for well over a cycle
  (the transport stopped), so a count from before a stop is never used.
* A stereo loop's other channels read channel 0's sync OUTPUT, which
  carries channel 0's own loop boundaries while Sync is off, so they get
  channel 0's count copied across.

The tests marked NEW failed before this design was built (they waited,
or started on a stale count); the others passed before and after.
"""

import unittest
import test_engine
from testdef_rel_sync_stale_phase import RelSyncStalePhaseTestCase


class RelSyncKnownPhaseTestCase(RelSyncStalePhaseTestCase):

    def assertStartsAtPress(self):
        self.assertEqual(self.state(), "Recording",
                         "the record waited although the cycle position was known")

    def finish_recording_unit(self, unit, cycles=2):
        """As finish_recording, for a pulse spacing of `unit` samples."""
        self.wait_buffers(int(cycles * unit / self.buf) + 1)
        self.press_record()
        for _ in range(int(3 * unit / self.buf) + 6):
            if self.state() == "Playing":
                break
            self.wait_buffers(1)
        self.assertEqual(self.state(), "Playing", "the Rel Sync stop never came")
        return int(round(self.looper.get_control_value(test_engine.LoopLength)
                         * self.sr))

    def assertWholeUnits(self, length, unit):
        off = length % unit
        self.assertEqual(off, 0, f"loop is {length} samples = {length / unit:.4f}"
                         f" units of {unit} ({off} samples over a whole number)")

    def restart_pulses(self, next_in):
        """Pulses resume, the next one `next_in` samples from now (the
        generator is off while the clock is set)."""
        self.looper.sync_inject_period = 0
        self.looper.sync_clock = self.cycle - next_in
        self.looper.sync_inject_period = self.cycle


class TestRelSyncKnownPhase(RelSyncKnownPhaseTestCase):

    # -- NEW: start at the press when the position was tracked ----------

    def testSyncOffAndOn_PulsesFlowing_StartsAtPress(self):
        # NEW.  Pulses went by while Sync was off; the count kept up.
        self.toggle_sync()
        self.press_record()
        self.assertStartsAtPress()
        self.assertWholeCycles(self.finish_recording())

    def testFromPlainSync_StartsAtPress(self):
        # NEW.  Plain Sync was counting already.
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_cycles(1.3)
        while not (0.05 * self.cycle < self.phase() < 0.15 * self.cycle):
            self.wait_buffers(1)
        self.looper.set_port(test_engine.Sync, 2.0)
        self.wait_buffers(2)
        self.press_record()
        self.assertStartsAtPress()
        self.assertWholeCycles(self.finish_recording())

    # -- NEW: unknown when it really is ----------------------------------

    def testQuantizeChange_WaitsForFirstNewPulse(self):
        # NEW.  Cycle -> 8th with Rel Sync on: the pulses come twice as
        # often from now on, at a new phase; the old count is stale.
        half = self.cycle // 2
        self.wait_cycles(0.5)
        self.looper.sync_inject_period = 0
        self.looper.set_port(test_engine.Quantize, 2.0)          # 8th
        self.looper.sync_clock = half * 7 + 300                  # next in half - 300
        self.looper.sync_inject_period = half
        self.wait_buffers(1)
        self.press_record()
        self.assertEqual(self.state(), "WaitStart",
                         "Rel Sync record started on a count from the old Quantize")
        self.assertWholeUnits(self.finish_recording_unit(half, cycles=3), half)

    def testPulsesStoppedThenResumed_WaitsForFirstPulse(self):
        # NEW.  The transport stops (no pulses for 3 cycles) and starts
        # again at another phase; the count from before the stop is stale.
        self.looper.sync_inject_period = 0
        self.wait_cycles(3)
        self.restart_pulses(int(0.8 * self.cycle))
        self.wait_buffers(1)
        self.press_record()
        self.assertEqual(self.state(), "WaitStart",
                         "Rel Sync record started on a count from before the stop")
        self.wait_cycles(1)
        self.assertWholeCycles(self.finish_recording())

    # -- unchanged behaviour (pass now) -----------------------------------

    def testNoPulseEver_WaitsForFirstPulse(self):
        # A loop that has never seen a pulse: unknown, so wait.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.sync_inject_period = 0
        self.undo_all()
        del self.engine
        self.engine = __import__("engine_wrapper").Engine("SL_test_relsync", self.CHANNELS)
        self.looper = self.engine.looper
        self.looper.sync_inject_enabled = True
        self.looper.sync_inject_period = 0
        self.looper.set_port(test_engine.Quantize, 1.0)
        self.looper.set_port(test_engine.Sync, 2.0)
        self.wait_cycles(1)
        self.restart_pulses(int(0.8 * self.cycle))
        self.wait_buffers(1)
        self.press_record()
        self.assertEqual(self.state(), "WaitStart")
        self.wait_cycles(1)
        self.assertWholeCycles(self.finish_recording())

    def testPulsesFlowingThroughout_StartsAtPress(self):
        self.wait_cycles(0.4)
        self.press_record()
        self.assertStartsAtPress()
        self.assertWholeCycles(self.finish_recording())


class TestRelSyncKnownPhaseStereo(RelSyncKnownPhaseTestCase):

    CHANNELS = 2

    def testSyncOffAndOn_PulsesFlowing_ChannelsStartTogetherAtPress(self):
        # NEW.  Channel 1 cannot count the source while Sync is off (its
        # sync input is channel 0's output); it needs channel 0's count.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.press_record()                     # a free take, so channel 0
        self.wait_cycles(0.7)                   # writes its own boundaries
        self.press_record()
        self.wait_cycles(1.2)
        # Sync on just after a pulse it missed, so none passes before the
        # press.
        while not (0.05 * self.cycle < self.phase() < 0.15 * self.cycle):
            self.wait_buffers(1)
        self.looper.set_port(test_engine.Sync, 2.0)
        self.wait_buffers(2)
        self.press_record()                     # record over it, Rel Sync
        for ch in (0, 1):
            self.assertEqual(self.looper.get_channel_state(ch),
                             self.looper.get_channel_state(0))
        self.assertStartsAtPress()
        length = self.finish_recording()
        self.assertEqual(self.looper.get_channel_loop_length(1),
                         self.looper.get_channel_loop_length(0),
                         "channel lengths differ")
        self.assertWholeCycles(length)


if __name__ == "__main__":
    unittest.main()
