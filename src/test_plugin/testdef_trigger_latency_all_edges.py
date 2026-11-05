#! /usr/bin/env python3
"""
Tests for TRIGGER_LATENCY_ALL_EDGES (plugin.hpp), increment 1:
trigger_latency is taken to mean exactly the command latency, and the
Record command's own start and stop, with Sync off, wait input_latency -
trigger_latency (s) after the command, so the take starts and ends on the
presses.  The length stays the press interval and playback keeps the stock
latency compensation.  At s <= 0 nothing changes.

Skipped when the switch is off.  Same exact-frame presses and ramp input
as testdef_rel_sync_late_hit.py.  The harness has no real latency, so the
latency settings here only move where the engine starts and stops taking
input and where playback starts; the expected values follow from them.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase, RAMP, frame_of

ENABLED = bool(getattr(test_engine, "TRIGGER_LATENCY_ALL_EDGES", 0))


@unittest.skipUnless(ENABLED, "TRIGGER_LATENCY_ALL_EDGES is 0")
class TriggerLatencyAllEdgesTestCase(RelSyncLateHitTestCase):

    IL = 600
    OL = 0
    TL = 200            # s = IL - TL = 400

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.Quantize, 0.0)
        self.looper.set_port(test_engine.InputLatency, float(self.IL))
        self.looper.set_port(test_engine.OutputLatency, float(self.OL))
        self.looper.set_port(test_engine.TriggerLatency, float(self.TL))
        self.wait_buffers(4)

    def tearDown(self):
        self.looper.set_port(test_engine.InputLatency, 0.0)
        self.looper.set_port(test_engine.OutputLatency, 0.0)
        self.looper.set_port(test_engine.TriggerLatency, 0.0)
        super().tearDown()

    def take(self, length):
        """Record from a press to a press `length` frames later; return the
        two presses' frames."""
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")          # deferred s samples
        s1 = self.press_at("RECORD", s0 + length)
        self.wait_state("Playing")
        return s0, s1

    def assertPlaysWithOffset(self, length, offset):
        """Each sample played at frame F was recorded at F - offset - k *
        length: playback keeps the take's phase, shifted by the latency
        compensation `offset` (IL + OL, which the harness does not have)."""
        start = (self.loop_frame(64) - 64) % RAMP
        checked = 0
        for _ in range(40):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            val = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            if (fr + offset - start) % length < 64:
                continue                  # the seam
            diff = (fr - frame_of(val)) % RAMP
            self.assertEqual((diff + offset) % length, 0,
                             f"frame {fr} plays input from {diff} frames earlier,"
                             f" not a whole number of loops less {offset}")
            checked += 1
        self.assertGreater(checked, 10)


class TestTriggerLatencyAllEdges(TriggerLatencyAllEdgesTestCase):

    def testStartAndStopOnThePresses(self):
        s = self.IL - self.TL
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.assertStartsAt(s0, -s)          # input s after the press
        self.assertEqual(self.length(), s1 - s0)
        self.assertContiguous(self.length())

    def testPlaybackKeepsCompensation(self):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.assertPlaysWithOffset(s1 - s0, self.IL + self.OL)


class TestTriggerLatencyEqualToInput(TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing

    def testStartsAtThePress(self):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.assertStartsAt(s0, 0)
        self.assertEqual(self.length(), s1 - s0)


class TestTriggerLatencyAboveInput(TriggerLatencyAllEdgesTestCase):

    TL = 900            # s < 0: left at the stock timing for now

    def testStartsAtThePress(self):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.assertStartsAt(s0, 0)
        self.assertEqual(self.length(), s1 - s0)


if __name__ == "__main__":
    unittest.main()
