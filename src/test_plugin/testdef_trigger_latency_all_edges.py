#! /usr/bin/env python3
"""
Tests for TRIGGER_LATENCY_ALL_EDGES (plugin.hpp): trigger_latency is taken
to mean exactly the command latency, and these edges wait input_latency -
trigger_latency (s) after the command, so they land on the presses:
the Record command's own start and stop with Sync off (increment 1), and
Record -> Overdub and a Replace or Substitute ended by the same command
(increment 2a), and a take ended by Multiply, Insert, Replace,
Substitute, Mute, Pause, Oneshot or Trigger with Sync off (2b-i; after
Pause, a resume starts where the immediate end's does).  The
length stays the press interval and playback keeps
the stock latency compensation.  At s <= 0 nothing changes.

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


class LayerEndsMixin:
    """Increment 2a: Record -> Overdub, and a Replace or Substitute ended
    by the same command.  Replace and Substitute write the input itself
    (no sum), so the newest input frame anywhere in the loop is the last
    sample the layer took: s - 1 after the ending press (-1 at s = 0)."""

    def s(self):
        return max(0, self.IL - self.TL)

    def newest_offset(self, ref, span):
        """The newest input frame in the loop, as a signed offset from
        input frame `ref`; frames more than `span` away are ignored."""
        L = self.length()
        best = None
        for pos in range(L):
            d = (self.loop_frame(pos) - ref) % RAMP
            if d > RAMP // 2:
                d -= RAMP
            if abs(d) <= span and (best is None or d > best):
                best = d
        return best

    def layer_end(self, command, state):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.wait_buffers(4)
        r0 = self.press_at(command, self.looper.sync_clock + 2 * self.buf)
        self.wait_state(state)
        r1 = self.press_at(command, r0 + int(0.15 * self.cycle))
        self.wait_state("Playing")
        self.wait_buffers(4)
        delta = self.looper.input_minus_sync
        return self.newest_offset(r1 + delta, self.cycle)

    def testRecordIntoOverdub_TakeOnThePresses(self):
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")
        self.wait_buffers(2)
        self.assertStartsAt(s0, -self.s())     # read before the overdub writes
        s1 = self.press_at("OVERDUB", s0 + int(0.6 * self.cycle))
        self.wait_state("Overdubbing")
        self.assertEqual(self.length(), s1 - s0)

    def testReplaceEnd_OnThePress(self):
        self.assertEqual(self.layer_end("REPLACE", "Replacing"), self.s() - 1)

    def testSubstituteEnd_OnThePress(self):
        self.assertEqual(self.layer_end("SUBSTITUTE", "Substitute"), self.s() - 1)


class TakeEndsMixin:
    """Increment 2b-i: a take ended by another command, Sync off, ends on
    the press, so it is the press interval long.  Read on the CycleLength
    port: Insert's new chunk is two takes long, its cycle one."""

    def cycle_length(self):
        return int(round(self.looper.get_control_value(test_engine.CycleLength)
                         * self.sr))

    def end_take_with(self, command, state):
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")
        s1 = self.press_at(command, s0 + int(0.6 * self.cycle))
        self.wait_state(state)
        self.assertEqual(self.cycle_length(), s1 - s0,
                         f"a take ended by {command}: not the press interval")
        return s0, s1

    def testMultiply(self):
        self.end_take_with("MULTIPLY", "Multiplying")

    def testInsert(self):
        self.end_take_with("INSERT", "Inserting")

    def testReplace(self):
        self.end_take_with("REPLACE", "Replacing")

    def testSubstitute(self):
        self.end_take_with("SUBSTITUTE", "Substitute")

    def testMute(self):
        self.end_take_with("MUTE", "Muted")

    def testMuteOn(self):
        # Becomes Mute before the dispatch (not muted yet).
        self.end_take_with("MUTE_ON", "Muted")

    def testMuteTrigger(self):
        # Likewise (not muted yet).
        self.end_take_with("MUTE_TRIGGER", "Muted")

    def testOneshot(self):
        self.end_take_with("ONESHOT", "OneShot")

    def testTrigger(self):
        s0, s1 = self.end_take_with("TRIGGER", "Playing")
        self.assertPlaysWithOffset(s1 - s0, self.IL + self.OL)

    def testPause_ResumesWhereTheImmediateEndDoes(self):
        """A resume after Pause ended a take plays from loop position
        IL + OL: stock's immediate end keeps the position after adding
        IL + OL (measured: the s = 0 class runs that branch).  The deferred
        end must match; set before the add, it would resume IL + OL
        earlier."""
        s0, s1 = self.end_take_with("PAUSE", "Paused")
        L = s1 - s0
        self.wait_buffers(4)
        r = self.press_at("PAUSE", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Playing")
        checked = 0
        for _ in range(12):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            val = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            pos = (fr - r + self.IL + self.OL) % L
            if fr - r < 128 or pos < 64 or pos > L - 64:
                continue                  # the fade-in, and the seam
            self.assertEqual(frame_of(val), self.loop_frame(pos),
                             f"{fr - r} samples after the resume: not loop position {pos}")
            checked += 1
        self.assertGreater(checked, 4)


class TestTriggerLatencyLayerEnds(LayerEndsMixin, TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyTakeEnds(TakeEndsMixin, TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyTakeEndsEqual(TakeEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencyLayerEndsEqual(LayerEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


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
