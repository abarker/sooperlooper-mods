#! /usr/bin/env python3
"""
Tests for REL_SYNC_LATE_HIT (plugin.cc): a Rel Sync record that starts at
the press starts trigger_latency + rel_sync_early_ms earlier, from the
input history, and ends as much earlier; a command ending the take
(Record, Overdub, a quantized Mute or mute_trigger) at most the late-hit
window past the take's end point ends it there at once instead of
recording another Quantize unit.

Presses go in at exact frames (TestLooper.request_at_clock), and each
press's real frame is read back (last_request_clock), so every expected
length comes from where the presses actually landed.  The input is a ramp
(TestLooper.input_ramp): each input sample says which frame it came from,
so the tests check exactly which input landed in the loop and which loop
sample plays when.  The harness's latencies are 0 and its crossfade 1
sample, so no fade touches the samples checked here.
"""

import unittest
import test_engine
import engine_wrapper
from testdef_rel_sync_known_phase import RelSyncKnownPhaseTestCase

RAMP = 1 << 20          # the ramp wraps every 2^20 frames
WINDOW_MS = 40.0        # late-hit window for these tests (1920 samples)
MARGIN = 300            # keep presses this far from a window edge


def frame_of(sample):
    return int(round(sample * RAMP))


class RelSyncLateHitTestCase(RelSyncKnownPhaseTestCase):

    TRIG_LATENCY = 0     # samples
    EARLY_MS = 0.0

    def setUp(self):
        super().setUp()
        test_engine.cvar.g_late_hit_window_ms = WINDOW_MS
        test_engine.cvar.g_rel_sync_early_ms = self.EARLY_MS
        self.looper.set_port(test_engine.TriggerLatency, float(self.TRIG_LATENCY))
        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.looper.input_ramp = True
        self.early = self.TRIG_LATENCY + int(round(self.EARLY_MS * 0.001 * self.sr))
        self.window = int(WINDOW_MS * 0.001 * self.sr)
        self.wait_cycles(0.5)

    def tearDown(self):
        test_engine.cvar.g_rel_sync_early_ms = 0.0
        self.looper.input_ramp = False
        self.looper.request_at_clock = -1
        super().tearDown()

    # ------------------------------------------------------------------

    def press_at(self, command, clock):
        """Press `command` in the first block starting at or after sync
        frame `clock`; return the frame of the block it landed in."""
        if command in engine_wrapper.commands:
            num = engine_wrapper.commands.index(command)
        else:
            num = engine_wrapper.command_ordinals[command]
        self.looper.request_at_cmd = num
        self.looper.request_at_clock = clock
        while self.looper.request_at_clock >= 0 or self.looper.request_pending:
            self.wait_buffers(1)
        self.wait_buffers(2)
        return self.looper.last_request_clock

    def loop_frame(self, pos, ch=0):
        return frame_of(self.looper.get_loop_sample(ch, pos))

    def start_take(self):
        """Press Record mid-cycle; return the sync frame of the press."""
        now = self.looper.sync_clock
        target = now + 2 * self.buf
        target += (int(0.35 * self.cycle) - target) % self.cycle
        s0 = self.press_at("RECORD", target)
        self.assertEqual(self.state(), "Recording", "did not start at the press")
        return s0

    def stop(self, s0, at, command="RECORD"):
        """Press `command` `at` samples after the start (at or just after:
        presses land on block starts).  Returns (frames after the start,
        samples past the last end point, the length expected if it ends
        at once, the length expected if it waits for the next point)."""
        s1 = self.press_at(command, s0 + at)
        since = s1 - s0
        whole = since // self.cycle
        return since, since - whole * self.cycle, whole * self.cycle, (whole + 1) * self.cycle

    def length(self):
        return int(round(self.looper.get_control_value(test_engine.LoopLength)
                         * self.sr))

    def wait_state(self, name, units=1.5):
        for _ in range(int(units * self.cycle / self.buf) + 4):
            if self.state() == name:
                return
            self.wait_buffers(1)
        self.assertEqual(self.state(), name)

    def assertContiguous(self, length, ch=0):
        """The loop is one unbroken stretch of input."""
        f = self.loop_frame(64, ch)
        for pos in (65, length // 2, length - 1):
            self.assertEqual((self.loop_frame(pos, ch) - f) % RAMP, pos - 64,
                             f"loop position {pos} is not the input following position 64")

    def assertStartsAt(self, s0, early):
        """Loop position 0 is the input `early` samples before the press."""
        delta = self.looper.input_minus_sync
        start = (self.loop_frame(64) - 64) % RAMP
        self.assertEqual(start, (s0 + delta - early) % RAMP,
                         f"the loop starts {(s0 + delta - start) % RAMP} samples"
                         f" before the press, not {early}")

    def assertPlaysInPhase(self, length):
        """Each sample played at frame F was recorded at F - k * length:
        playback keeps the recorded phase."""
        start = (self.loop_frame(64) - 64) % RAMP
        checked = 0
        for _ in range(40):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            val = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            if (fr - start) % length < 64:
                continue                  # the seam
            diff = (fr - frame_of(val)) % RAMP
            self.assertEqual(diff % length, 0,
                             f"frame {fr} plays input from {diff} frames earlier,"
                             f" not a whole number of loops ({length})")
            checked += 1
        self.assertGreater(checked, 10)


class TestRelSyncEarlyStart(RelSyncLateHitTestCase):

    TRIG_LATENCY = 500
    EARLY_MS = 10.0      # 480 samples at 48 kHz

    def testStartsEarlyFromInputHistory(self):
        s0 = self.start_take()
        self.assertStartsAt(s0, self.early)
        self.assertContiguous(self.early + self.buf)

    def testEndsEarlyWholeCycles(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle - 6 * self.buf)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Playing")
        self.assertEqual(self.length(), next_len)
        self.assertStartsAt(s0, self.early)
        self.assertContiguous(next_len)

    def testPlaybackKeepsPhase(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle - 6 * self.buf)
        self.wait_state("Playing")
        self.assertPlaysInPhase(next_len)

    def testWaitingRecordHasNoEarlyStart(self):
        # Count unknown (pulses stopped, then resumed): the record waits
        # for the pulse and starts there, not early.
        self.looper.sync_inject_period = 0
        self.wait_cycles(3)
        self.restart_pulses(int(0.6 * self.cycle))
        self.wait_buffers(1)
        self.press_record()
        self.assertEqual(self.state(), "WaitStart")
        self.wait_state("Recording")
        self.wait_buffers(2)
        delta = self.looper.input_minus_sync
        start = (self.loop_frame(64) - 64) % RAMP
        self.assertEqual((start - delta) % self.cycle, 1,
                         "the waiting record should start one sample after the pulse"
                         " (as before), with no early start")


class TestRelSyncLateStop(RelSyncLateHitTestCase):

    def testLateStopInWindow_EndsAtPoint(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertLessEqual(over, self.window - MARGIN)
        self.assertEqual(self.state(), "Playing", "a stop in the window should end at once")
        self.assertEqual(self.length(), now_len)
        self.assertEqual(now_len, 2 * self.cycle)
        self.assertStartsAt(s0, 0)
        self.assertContiguous(now_len)

    def testLateStopInWindow_PlaybackKeepsPhase(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertEqual(self.state(), "Playing")
        self.assertPlaysInPhase(now_len)

    def testLateStopOutsideWindow_WaitsAnotherCycle(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + self.window + MARGIN)
        self.assertGreater(over, self.window)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Playing")
        self.assertEqual(self.length(), next_len)
        self.assertEqual(next_len, 3 * self.cycle)

    def testEarlyStop_WaitsForPoint(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle - 6 * self.buf)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Playing")
        self.assertEqual(self.length(), next_len)
        self.assertEqual(next_len, 2 * self.cycle)

    def testStopJustAfterStart_WaitsWholeCycle(self):
        # The start is an end point too, but a stop in the window after it
        # must not end a loop of no cycles.
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, MARGIN)
        self.assertEqual(now_len, 0)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Playing")
        self.assertEqual(self.length(), self.cycle)

    def testWindowZero_WaitsAnotherCycle(self):
        test_engine.cvar.g_late_hit_window_ms = 0.0
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Playing")
        self.assertEqual(self.length(), next_len)


class TestRelSyncLateStopWithEarlyStart(RelSyncLateHitTestCase):

    TRIG_LATENCY = 500
    EARLY_MS = 10.0

    def testLateStopInWindow_EndsAtPointLessEarly(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertEqual(self.state(), "Playing")
        self.assertEqual(self.length(), now_len)
        self.assertStartsAt(s0, self.early)
        self.assertContiguous(now_len)
        self.assertPlaysInPhase(now_len)

    def testRecordIntoOverdub_EndsAtPointLessEarly(self):
        # Another command ending the take before the end point: it waits
        # for the point and gets the early-start trim there.
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle - 6 * self.buf,
                                                   command="OVERDUB")
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Overdubbing")
        self.assertEqual(self.length(), next_len)
        # (no content check: the overdub is adding to the loop)

    def testLateMuteTrigger_EndsAtPointLessEarly(self):
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN,
                                                   command="MUTE_TRIGGER")
        self.assertEqual(self.state(), "Muted")
        self.assertEqual(self.length(), now_len)
        self.assertStartsAt(s0, self.early)
        self.assertContiguous(now_len)


class TestRelSyncLateStopOtherCommands(RelSyncLateHitTestCase):
    """Every command that ends a Rel Sync take waits in STATE_TRIG_STOP,
    so each gets the late stop."""

    def lateEnd(self, command, state):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN,
                                                   command=command)
        self.assertLessEqual(over, self.window - MARGIN)
        self.assertEqual(self.state(), state, f"a late {command} should end the take at once")
        self.assertEqual(self.length(), now_len)
        self.assertEqual(now_len, 2 * self.cycle)
        if state != "Overdubbing":        # an overdub is adding to the loop
            self.assertContiguous(now_len)

    def testLateOverdub_EndsAtPoint(self):
        self.lateEnd("OVERDUB", "Overdubbing")

    def testLateQuantizedMute_EndsAtPoint(self):
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        self.lateEnd("MUTE", "Muted")

    def testLateQuantizedMuteTrigger_EndsAtPoint(self):
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        self.lateEnd("MUTE_TRIGGER", "Muted")

    def testLateMuteTrigger_OutsideWindow_WaitsAnotherCycle(self):
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + self.window + MARGIN,
                                                   command="MUTE_TRIGGER")
        self.assertGreater(over, self.window)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Muted")
        self.assertEqual(self.length(), next_len)

    def testEarlyMuteTrigger_WaitsForPoint(self):
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle - 6 * self.buf,
                                                   command="MUTE_TRIGGER")
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Muted")
        self.assertEqual(self.length(), next_len)
        self.assertEqual(next_len, 2 * self.cycle)

    def testLateOverdub_UnmutedPlaybackAfterRecordStop(self):
        # Record (stop) late, then the loop plays in phase: as the Record
        # tests, through the same STATE_TRIG_STOP path.
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertEqual(self.state(), "Playing")
        self.assertPlaysInPhase(now_len)


class TestRelSyncLateStopEighth(RelSyncLateHitTestCase):

    def setUp(self):
        super().setUp()
        self.looper.sync_inject_period = 0
        self.looper.set_port(test_engine.Quantize, 3.0)          # 8th
        self.cycle //= 2                                         # the unit
        self.looper.sync_clock = 0
        self.looper.sync_inject_period = self.cycle
        self.wait_cycles(4)

    def testLateStopInWindow_EndsAtEighthPoint(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 3 * self.cycle + MARGIN)
        self.assertLessEqual(over, self.window - MARGIN)
        self.assertEqual(self.state(), "Playing")
        self.assertEqual(self.length(), now_len)
        self.assertEqual(now_len, 3 * self.cycle)


class TestRelSyncLateStopStereo(RelSyncLateHitTestCase):

    CHANNELS = 2
    TRIG_LATENCY = 500

    def testLateStopInWindow_ChannelsAlike(self):
        s0 = self.start_take()
        since, over, now_len, next_len = self.stop(s0, 2 * self.cycle + MARGIN)
        self.assertEqual(self.looper.get_channel_state(1),
                         self.looper.get_channel_state(0))
        self.assertEqual(self.state(), "Playing")
        self.assertEqual(self.looper.get_channel_loop_length(0), now_len)
        self.assertEqual(self.looper.get_channel_loop_length(1), now_len)
        self.assertContiguous(now_len, ch=1)
        self.assertEqual(self.loop_frame(64, 0), self.loop_frame(64, 1))


if __name__ == "__main__":
    unittest.main()
