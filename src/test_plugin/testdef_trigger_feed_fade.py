#! /usr/bin/env python3
"""
Tests for TRIGGER_FEED_FADE_FIX (plugin.cc; stock bug):
a Replace or Substitute ended by Trigger, or by a Oneshot that waits for
the pulse, keeps the loop.  Replace and Substitute fade the loop's
feedback to 0; stock never faded it back in on those two transitions, so
Play wrote the loop back times 0 and erased it one pass after the
restart: heard once, then silence, the buffer all zeros.

A take of 0.6 cycle, the layer 0.15 cycle long, then the ending command:
at once with Sync off, or at the pulse with Sync on (pressed half a
cycle past a pulse, out of any slack window).  Three loop lengths
later, the loop must still hold input everywhere (the input is a ramp, so
no recorded sample is 0 short of one frame in 2^20) and still play.
Overdub, which does not fade the feedback, is the control.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase


class TriggerFeedFadeTestCase(RelSyncLateHitTestCase):

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.Quantize, 0.0)
        self.looper.set_port(test_engine.MuteQuantized, 0.0)
        self.wait_buffers(4)

    def end_layer(self, layer, layer_state, command, state, sync_on=False):
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")
        self.press_at("RECORD", s0 + int(0.6 * self.cycle))
        self.wait_state("Playing")
        L = self.length()
        self.wait_buffers(4)
        r0 = self.press_at(layer, self.looper.sync_clock + 2 * self.buf)
        self.wait_state(layer_state)
        if sync_on:
            self.looper.set_port(test_engine.Sync, 1.0)
            self.wait_buffers(2)
            self.assertEqual(self.state(), layer_state)
        if sync_on:
            # Half a cycle past a pulse: out of any slack window.
            self.press_at(command, (r0 // self.cycle + 1) * self.cycle + self.cycle // 2)
        else:
            self.press_at(command, r0 + int(0.15 * self.cycle))
        if sync_on:
            self.assertEqual(self.state(), layer_state, "it should wait for the pulse")
        self.wait_state(state, units=2)
        self.looper.set_port(test_engine.Sync, 0.0)
        return L

    def assertLoopKept(self, L, command="TRIGGER"):
        if self.state() != "Playing":
            self.engine.request(command)
            self.wait_state("Playing")
        self.wait_buffers(int(3 * L / self.buf) + 2)
        zeros = [pos for pos in range(0, L, 97) if self.looper.get_loop_sample(0, pos) == 0.0]
        self.assertLessEqual(len(zeros), 1, f"loop positions now silent: {zeros[:10]}")
        self.looper.reset_output_stats()
        self.wait_buffers(int(L / self.buf) + 2)
        self.assertGreater(self.looper.output_max, 0.0, "the loop no longer plays")


class TestTriggerFeedFade(TriggerFeedFadeTestCase):

    def testReplace_Trigger(self):
        self.assertLoopKept(self.end_layer("REPLACE", "Replacing", "TRIGGER", "Playing"))

    def testSubstitute_Trigger(self):
        self.assertLoopKept(self.end_layer("SUBSTITUTE", "Substitute", "TRIGGER", "Playing"))

    def testReplace_Trigger_SyncOn(self):
        self.assertLoopKept(self.end_layer("REPLACE", "Replacing", "TRIGGER", "Playing",
                                           sync_on=True))

    def testReplace_Oneshot_SyncOn(self):
        # A oneshot that waited for the pulse: transitionToNext(STATE_ONESHOT).
        L = self.end_layer("REPLACE", "Replacing", "ONESHOT", "OneShot", sync_on=True)
        self.wait_state("Muted", units=3)
        self.assertLoopKept(L)

    def testReplace_Oneshot(self):
        # At once: MULTI_ONESHOT's own branch (stock was right here).
        L = self.end_layer("REPLACE", "Replacing", "ONESHOT", "OneShot")
        self.wait_state("Muted", units=3)
        self.assertLoopKept(L)

    def testOverdub_Trigger(self):
        # Control: Overdub does not fade the feedback.
        self.assertLoopKept(self.end_layer("OVERDUB", "Overdubbing", "TRIGGER", "Playing"))


if __name__ == "__main__":
    unittest.main()
