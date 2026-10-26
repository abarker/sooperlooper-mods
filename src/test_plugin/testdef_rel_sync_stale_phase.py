#! /usr/bin/env python3
"""
Regression tests for REL_SYNC_STALE_PHASE_FIX (plugin.cc): a Rel Sync
record pressed before the first cycle pulse after Sync comes back on.

Background
----------
lSamplesSinceSync, the samples since the last cycle pulse, counts only
while the loop's Sync is on.  Under Rel Sync a record takes its value at
the press as the loop's offset, and the stop waits for the counter to
equal that offset again.  After Sync is turned off and on (the slgui
checkbox, a sync settings change, big_tempo at every JACK transport stop
and start) the counter is stale until the next pulse, so stock gives a
loop of the wrong length -- or, when the stale value is over a cycle, a
recording that never stops.

With the fix, entering Rel Sync marks the counter unknown; a record
pressed then waits for the next cycle pulse and starts there, and once a
pulse has passed, records start at once as before.

Pulses come from the harness itself (sync_inject_period): one sample of
2.0 every CYCLE samples, so they never race Python.  CYCLE is not a
multiple of the buffer size, so pulses land mid-buffer.
"""

import time
import unittest
import test_engine
import engine_wrapper

_DEFAULT_WINDOW_MS = 100.0   # restore in tearDown

CYCLE_BUFS = 8      # cycle length in buffers, plus EXTRA samples
EXTRA = 300
RECORD_CYCLES = 2   # cycles between the record press and the stop press


class RelSyncStalePhaseTestCase(unittest.TestCase):

    CHANNELS = 1

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_relsync", self.CHANNELS)
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())
        self.cycle  = CYCLE_BUFS * self.buf + EXTRA
        # Late-hit is off under Rel Sync anyway; zero it so nothing else
        # can move a start or stop.
        test_engine.cvar.g_late_hit_window_ms = 0.0
        self.looper.sync_inject_period  = self.cycle
        self.looper.sync_clock          = 0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Quantize, 1.0)   # Cycle
        self.looper.set_port(test_engine.Sync, 2.0)       # Rel Sync
        self.wait_cycles(2)

    def tearDown(self):
        test_engine.cvar.g_late_hit_window_ms = _DEFAULT_WINDOW_MS
        del self.engine

    # ------------------------------------------------------------------
    # helpers

    def wait_buffers(self, n, timeout=5.0):
        """Busy-wait until exactly n more JACK callbacks have completed."""
        target = self.looper.process_count + n
        period = float(self.buf) / float(self.sr)
        deadline = time.time() + max(timeout, 3.0 * n * period)
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    f"Timed out waiting for {n} JACK callbacks "
                    f"(process_count stuck at {self.looper.process_count})")

    def wait_cycles(self, c):
        self.wait_buffers(int(c * self.cycle / self.buf) + 1)

    def phase(self):
        """Samples since the last injected pulse, at a buffer boundary."""
        return self.looper.sync_clock % self.cycle

    def state(self):
        return engine_wrapper.states[self.engine.getState()]

    def toggle_sync(self, off_cycles=1.6, to=2.0):
        """Sync off for about off_cycles (pulses go by unseen), then on to
        `to` just after a pulse, so the next one is most of a cycle away."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_cycles(off_cycles)
        while not (0.05 * self.cycle < self.phase() < 0.15 * self.cycle):
            self.wait_buffers(1)
        self.looper.set_port(test_engine.Sync, to)
        self.wait_buffers(2)

    def press_record(self):
        self.engine.request("RECORD")
        self.wait_buffers(1)

    def finish_recording(self):
        """Record for RECORD_CYCLES, press again, return the loop length
        once playing (fails if the stop never comes)."""
        self.wait_cycles(RECORD_CYCLES)
        self.press_record()
        for _ in range(int(2 * self.cycle / self.buf) + 4):
            if self.state() == "Playing":
                break
            self.wait_buffers(1)
        self.assertEqual(self.state(), "Playing",
                         "the Rel Sync stop never came")
        return int(round(self.looper.get_control_value(test_engine.LoopLength)
                         * self.sr))

    def assertWholeCycles(self, length):
        off = length % self.cycle
        self.assertEqual(
            off, 0,
            f"loop is {length} samples = {length / self.cycle:.4f} cycles "
            f"({off} samples over a whole number)")

    def undo_all(self):
        self.engine.request("UNDO_ALL")
        self.wait_buffers(2)


class TestRelSyncStalePhase(RelSyncStalePhaseTestCase):

    def testSyncOnThroughout_WholeCycles(self):
        # Control: no toggle, so the counter is right; mid-cycle press.
        self.wait_cycles(0.4)
        self.press_record()
        self.assertEqual(self.state(), "Recording")
        self.assertWholeCycles(self.finish_recording())

    def testPressRightAfterSyncOn_WaitsForPulse(self):
        self.toggle_sync()
        self.press_record()
        self.assertEqual(self.state(), "WaitStart",
                         "Rel Sync record started on an unknown offset")
        self.wait_cycles(1)
        self.assertEqual(self.state(), "Recording",
                         "the record did not start at the next pulse")
        self.assertWholeCycles(self.finish_recording())

    def testPressRightAfterSyncOn_WholeCycles(self):
        # The bug's own symptom, whatever the start does: stock gives a
        # wrong length here, or a stop that never comes.
        self.toggle_sync()
        self.press_record()
        self.wait_cycles(1)
        self.assertWholeCycles(self.finish_recording())

    def testRecordingAfterThat_StartsAtOnce(self):
        self.toggle_sync()
        self.press_record()
        self.wait_cycles(1)
        self.finish_recording()
        self.undo_all()
        self.wait_cycles(0.3)
        self.press_record()
        self.assertEqual(self.state(), "Recording")
        self.assertWholeCycles(self.finish_recording())

    def testPressAfterOnePulse_StartsAtOnce(self):
        self.toggle_sync()
        self.wait_cycles(1.3)
        self.press_record()
        self.assertEqual(self.state(), "Recording")
        self.assertWholeCycles(self.finish_recording())

    def testFromPlainSync_WaitsForPulse(self):
        # Turning Rel Sync on over plain Sync: the counter was counting,
        # but entering Rel Sync is treated alike (one press waits).
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_cycles(1.3)
        while not (0.05 * self.cycle < self.phase() < 0.15 * self.cycle):
            self.wait_buffers(1)
        self.looper.set_port(test_engine.Sync, 2.0)
        self.wait_buffers(2)
        self.press_record()
        self.assertEqual(self.state(), "WaitStart")
        self.wait_cycles(1)
        self.assertWholeCycles(self.finish_recording())


class TestRelSyncStalePhaseStereo(RelSyncStalePhaseTestCase):
    """Channel 1 takes its sync from channel 0's sync output, as in a
    stereo loop; both must start on the same sample and come out the same
    length.  Channel 0 must pass the pulse on while it waits."""

    CHANNELS = 2

    def assertChannelsAgree(self):
        s0 = self.looper.get_channel_state(0)
        s1 = self.looper.get_channel_state(1)
        self.assertEqual(s1, s0, "channel states differ: %s / %s"
                         % (engine_wrapper.states[s0], engine_wrapper.states[s1]))

    def testPressRightAfterSyncOn_ChannelsTogether(self):
        self.toggle_sync()
        self.press_record()
        self.assertChannelsAgree()
        self.wait_cycles(1)
        self.assertEqual(self.state(), "Recording")
        self.assertChannelsAgree()
        length = self.finish_recording()
        self.assertChannelsAgree()
        self.assertEqual(self.looper.get_channel_loop_length(1),
                         self.looper.get_channel_loop_length(0),
                         "channel lengths differ")
        self.assertWholeCycles(length)

    def testPressAfterOnePulse_ChannelsTogether(self):
        self.toggle_sync()
        self.wait_cycles(1.3)
        self.press_record()
        self.assertChannelsAgree()
        length = self.finish_recording()
        self.assertEqual(self.looper.get_channel_loop_length(1),
                         self.looper.get_channel_loop_length(0),
                         "channel lengths differ")
        self.assertWholeCycles(length)


if __name__ == "__main__":
    unittest.main()
