#! /usr/bin/env python3
"""
Tests for LATE_STOP_ANY_COMMAND (plugin.cc): under plain Sync, a command
other than Record that ends a take -- Overdub, a quantized Mute or
mute_trigger -- pressed at most the late-hit window after a pulse ends the
take on that pulse at once, as the Record late stop does, instead of
recording another whole cycle.  (Record's own late stop is in
testdef_late_hit.py.)

Same exact-frame presses and ramp input as testdef_rel_sync_late_hit.py.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase, RAMP, MARGIN


class LateStopAnyCommandTestCase(RelSyncLateHitTestCase):

    CHANNELS = 1

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.Sync, 1.0)            # plain Sync
        self.looper.set_port(test_engine.MuteQuantized, 1.0)
        self.wait_cycles(1)

    def take(self, command, after_pulse, cycles=2, window_ms_after_start=None):
        """Record from a pulse (a late-hit start just after it), then end
        with `command` `after_pulse` samples after the pulse `cycles` on.
        Returns (start pulse frame, samples past that pulse the command
        landed)."""
        c = self.cycle
        k = self.looper.sync_clock // c + 2
        self.press_at("RECORD", k * c + MARGIN)
        self.assertEqual(self.state(), "Recording")
        if window_ms_after_start is not None:
            test_engine.cvar.g_late_hit_window_ms = window_ms_after_start
        s1 = self.press_at(command, (k + cycles) * c + after_pulse)
        return k * c, s1 - (k + cycles) * c

    def assertLoopFromPulse(self, pulse, length, ch=0):
        delta = self.looper.input_minus_sync
        start = (self.loop_frame(64, ch) - 64) % RAMP
        self.assertEqual(start, (pulse + delta) % RAMP,
                         "the loop should start on the pulse")
        self.assertContiguous(length, ch)


class TestLateStopAnyCommand(LateStopAnyCommandTestCase):

    def testLateMuteTrigger_EndsOnPulse(self):
        pulse, over = self.take("MUTE_TRIGGER", MARGIN)
        self.assertLessEqual(over, self.window - MARGIN)
        self.assertEqual(self.state(), "Muted", "a late mute_trigger should end the take at once")
        self.assertEqual(self.length(), 2 * self.cycle)
        self.assertLoopFromPulse(pulse, 2 * self.cycle)

    def testLateQuantizedMute_EndsOnPulse(self):
        pulse, over = self.take("MUTE", MARGIN)
        self.assertEqual(self.state(), "Muted")
        self.assertEqual(self.length(), 2 * self.cycle)
        self.assertLoopFromPulse(pulse, 2 * self.cycle)

    def testLateOverdub_EndsOnPulse(self):
        pulse, over = self.take("OVERDUB", MARGIN)
        self.assertEqual(self.state(), "Overdubbing")
        self.assertEqual(self.length(), 2 * self.cycle)

    def testLateMuteTrigger_ThenUnmute_PlaysInPhase(self):
        # The muted loop keeps its place; unmuted later it plays in phase.
        pulse, over = self.take("MUTE_TRIGGER", MARGIN)
        self.assertEqual(self.state(), "Muted")
        self.wait_cycles(1.3)
        self.engine.request("MUTE")
        self.wait_state("Playing")
        self.assertPlaysInPhase(2 * self.cycle)

    def testMuteTriggerOutsideWindow_WaitsAnotherCycle(self):
        pulse, over = self.take("MUTE_TRIGGER", self.window + MARGIN)
        self.assertGreater(over, self.window)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Muted")
        self.assertEqual(self.length(), 3 * self.cycle)

    def testEarlyMuteTrigger_WaitsForPulse(self):
        pulse, over = self.take("MUTE_TRIGGER", -6 * self.buf)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Muted")
        self.assertEqual(self.length(), 2 * self.cycle)

    def testWindowZero_WaitsAnotherCycle(self):
        pulse, over = self.take("MUTE_TRIGGER", MARGIN, window_ms_after_start=0.0)
        self.assertEqual(self.state(), "WaitStop")
        self.wait_state("Muted")
        self.assertEqual(self.length(), 3 * self.cycle)

    def testRecordLateStopUnchanged(self):
        pulse, over = self.take("RECORD", MARGIN)
        self.assertEqual(self.state(), "Playing")
        self.assertEqual(self.length(), 2 * self.cycle)
        self.assertLoopFromPulse(pulse, 2 * self.cycle)


class TestLateStopAnyCommandStereo(LateStopAnyCommandTestCase):

    CHANNELS = 2

    def testLateMuteTrigger_ChannelsAlike(self):
        pulse, over = self.take("MUTE_TRIGGER", MARGIN)
        self.assertEqual(self.looper.get_channel_state(1),
                         self.looper.get_channel_state(0))
        self.assertEqual(self.state(), "Muted")
        self.assertEqual(self.looper.get_channel_loop_length(0), 2 * self.cycle)
        self.assertEqual(self.looper.get_channel_loop_length(1), 2 * self.cycle)
        self.assertLoopFromPulse(pulse, 2 * self.cycle, ch=1)


if __name__ == "__main__":
    unittest.main()
