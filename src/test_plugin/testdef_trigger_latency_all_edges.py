#! /usr/bin/env python3
"""
Tests for TRIGGER_LATENCY_ALL_EDGES (plugin.hpp): trigger_latency is taken
to mean exactly the command latency, and these edges wait input_latency -
trigger_latency (s) after the command, so they land on the presses:
the Record command's own start and stop with Sync off (increment 1), and
every end of a take or a layer that acts when its command arrives (2a,
2b): the same command again, Record -> Overdub, a take ended by another
command (Sync off, and with Sync on where the command acts at once), and
a layer ended by another command, including ones that start a new chunk
(Record, Multiply, Insert, another layer).  A take keeps the press
interval as its length and the stock latency compensation in playback;
a Pause, Reverse or Scratch ending a take resumes or plays where the
immediate end's would; a restart in the slack window keeps the loop's
phase.  At s <= 0 nothing changes (the "Equal" classes, TL = IL, run the
stock immediate branches; several expectations are measured there).

Two cases need ROADMAP entry 43's fixes (TRIGGER_FEED_FADE_FIX,
ONESHOT_ONE_PASS_FIX), which are bugs on the stock path too.

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
    sample the layer took: s - 1 after the ending press (-1 at s = 0).
    s is signed: at s < 0 (increment (c), entry 42) the take starts -s
    early and the layer ends -s before its command."""

    def s(self):
        return self.IL - self.TL

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
    port: Insert's new chunk is two takes long, its cycle one.  s is
    signed: at s < 0 (increment (c)) the take ends -s before the command,
    trimmed, and the loop runs on from the press."""

    def s(self):
        return self.IL - self.TL

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

    def testReverse_PlacedAsTheImmediateEnd(self):
        """Reverse ending a take plays it backwards from loop position
        L - (IL + OL) at the take's end (measured: the s = 0 class runs the
        immediate branch); the deferred end, s after the press, the same."""
        s0, s1 = self.end_take_with("REVERSE", "Playing")
        L = s1 - s0
        end = s1 + self.s()
        base = self.loop_frame(0)
        checked = 0
        for _ in range(12):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            val = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue                  # raced the callback
            if fr - s1 < 4 * self.buf:
                continue                  # the fade
            idx = (frame_of(val) - base) % RAMP
            if not (64 < idx < L - 64):
                continue                  # the seam
            self.assertEqual((idx + fr - end) % L, (L - self.IL - self.OL) % L,
                             f"{fr - end} samples after the end: loop position {idx}")
            checked += 1
        self.assertGreater(checked, 4)

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
            # At s < 0 the pause came -s after the take's end (the press).
            pos = (fr - r + self.IL + self.OL + max(0, -self.s())) % L
            if fr - r < 128 or pos < 64 or pos > L - 64:
                continue                  # the fade-in, and the seam
            self.assertEqual(frame_of(val), self.loop_frame(pos),
                             f"{fr - r} samples after the resume: not loop position {pos}")
            checked += 1
        self.assertGreater(checked, 4)


class LayerEndsByOtherMixin:
    """Increment 2b-ii: a Replace layer ended by a different command ends
    on the press: the newest input frame in the loop is s - 1 after it
    (-1 at s = 0), as LayerEndsMixin checks for the same command."""

    def s(self):
        return max(0, self.IL - self.TL)

    def end_layer_with(self, command, state, sync_on=False):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.wait_buffers(4)
        r0 = self.press_at("REPLACE", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Replacing")
        if sync_on:
            # The rig when synced: Sync on, the mute not quantized.
            self.looper.set_port(test_engine.MuteQuantized, 0.0)
            self.looper.set_port(test_engine.Sync, 1.0)
            self.wait_buffers(2)
            self.assertEqual(self.state(), "Replacing")
        r1 = self.press_at(command, r0 + int(0.15 * self.cycle))
        self.wait_state(state)
        self.wait_buffers(4)
        delta = self.looper.input_minus_sync
        self.assertEqual(LayerEndsMixin.newest_offset(self, r1 + delta, self.cycle),
                         self.s() - 1,
                         f"a Replace ended by {command}: its last sample is not s - 1"
                         " after the press")

    def testMute(self):
        self.end_layer_with("MUTE", "Muted")

    def testMuteOn(self):
        self.end_layer_with("MUTE_ON", "Muted")

    def testMuteTrigger(self):
        self.end_layer_with("MUTE_TRIGGER", "Muted")

    def testMute_SyncOn(self):
        self.end_layer_with("MUTE", "Muted", sync_on=True)

    def testPause(self):
        self.end_layer_with("PAUSE", "Paused")

    def testTrigger(self):
        self.end_layer_with("TRIGGER", "Playing")

    def testOneshot(self):
        # Same deferral as Trigger, to STATE_ONESHOT.
        self.end_layer_with("ONESHOT", "OneShot")


class NewChunkLayerEndsMixin:
    """2b-iii: a Replace (or Substitute) ended by a command that starts a
    new chunk (Record, Multiply, Insert, another layer): the ended layer
    is now headLoopChunk->prev, and its newest input frame is s - 1 after
    the press (-1 at s = 0)."""

    def s(self):
        return max(0, self.IL - self.TL)

    def prev_offsets(self, ref, L, span):
        """Signed offsets from input frame `ref` of the input frames in
        the previous chunk, within `span`."""
        out = []
        for pos in range(L):
            d = (frame_of(self.looper.get_prev_loop_sample(0, pos)) - ref) % RAMP
            if d > RAMP // 2:
                d -= RAMP
            if abs(d) <= span:
                out.append(d)
        return out

    def end_layer_new_chunk(self, layer, layer_state, command, state):
        s0, s1 = self.take(int(0.6 * self.cycle))
        L = s1 - s0
        self.wait_buffers(4)
        r0 = self.press_at(layer, self.looper.sync_clock + 2 * self.buf)
        self.wait_state(layer_state)
        r1 = self.press_at(command, r0 + int(0.15 * self.cycle))
        self.wait_state(state)
        self.wait_buffers(4)
        ref = r1 + self.looper.input_minus_sync
        prev = self.prev_offsets(ref, L, self.cycle)
        self.assertTrue(prev, "no input frames near the press in the ended layer")
        self.assertEqual(max(prev), self.s() - 1,
                         f"a {layer} ended by {command}: its last sample is not s - 1"
                         " after the press")
        return ref, L, max(prev)

    def testRecord(self):
        self.end_layer_new_chunk("REPLACE", "Replacing", "RECORD", "Recording")

    def testMultiply(self):
        self.end_layer_new_chunk("REPLACE", "Replacing", "MULTIPLY", "Multiplying")

    def testInsert(self):
        self.end_layer_new_chunk("REPLACE", "Replacing", "INSERT", "Inserting")

    def testOverdub(self):
        self.end_layer_new_chunk("REPLACE", "Replacing", "OVERDUB", "Overdubbing")

    def testSubstitute(self):
        self.end_layer_new_chunk("REPLACE", "Replacing", "SUBSTITUTE", "Substitute")

    def testReplaceFromSubstitute(self):
        self.end_layer_new_chunk("SUBSTITUTE", "Substitute", "REPLACE", "Replacing")


class SyncOnTakeEndsMixin:
    """2b-iii: a take ended at once with Sync on (Mute with mute_quantized
    off, Replace with replace_quantized off, Reverse, Scratch) ends on the
    press: the take's last input frame is s - 1 after it.  The take starts
    with Sync off; Sync goes on while recording."""

    def s(self):
        return max(0, self.IL - self.TL)

    def sync_on_take_end(self, command, state, prev=False):
        self.looper.set_port(test_engine.MuteQuantized, 0.0)
        for ch in range(4):
            self.looper.set_replace_quantized(ch, False)
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(2)
        self.assertEqual(self.state(), "Recording")
        r1 = self.press_at(command, s0 + int(0.6 * self.cycle))
        self.wait_state(state)
        self.wait_buffers(2)
        L = int(round(self.looper.get_control_value(test_engine.CycleLength) * self.sr))
        read = ((lambda pos: frame_of(self.looper.get_prev_loop_sample(0, pos)))
                if prev else self.loop_frame)
        last = (read(L - 1) - (r1 + self.looper.input_minus_sync)) % RAMP
        if last > RAMP // 2:
            last -= RAMP
        self.assertEqual(last, self.s() - 1,
                         f"a take ended by {command} with Sync on: its last sample"
                         " is not s - 1 after the press")

    def testMute(self):
        self.sync_on_take_end("MUTE", "Muted")

    def testMuteOn(self):
        self.sync_on_take_end("MUTE_ON", "Muted")

    def testReplace(self):
        self.sync_on_take_end("REPLACE", "Replacing", prev=True)

    def testReverse(self):
        self.sync_on_take_end("REVERSE", "Playing")

    def testScratch(self):
        self.sync_on_take_end("SCRATCH", "OneShot")


class TestTriggerLatencyLayerEnds(LayerEndsMixin, TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyNewChunkLayerEnds(NewChunkLayerEndsMixin,
                                          TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyNewChunkLayerEndsEqual(NewChunkLayerEndsMixin,
                                               TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencySyncOnTakeEnds(SyncOnTakeEndsMixin,
                                       TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencySyncOnTakeEndsEqual(SyncOnTakeEndsMixin,
                                            TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencyLayerEndsByOther(LayerEndsByOtherMixin,
                                         TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyLayerEndsByOtherEqual(LayerEndsByOtherMixin,
                                              TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencyTakeEnds(TakeEndsMixin, TriggerLatencyAllEdgesTestCase):
    pass


class TestTriggerLatencyTakeEndsEqual(TakeEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencyTakeEndsAbove(TakeEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 900            # s = -300 (increment (c)): trimmed at once


class TestTriggerLatencyLayerEndsAbove(LayerEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 900            # s = -300: the take (increment (c)) and the layers
                        # (entry 42's TL_OVER_IL_LAYER_FIX)


class TestTriggerLatencyLayerEndsEqual(LayerEndsMixin, TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing


class TestTriggerLatencyEqualToInput(TriggerLatencyAllEdgesTestCase):

    TL = 600            # s = 0: stock timing

    def testStartsAtThePress(self):
        s0, s1 = self.take(int(0.6 * self.cycle))
        self.assertStartsAt(s0, 0)
        self.assertEqual(self.length(), s1 - s0)


class TestTriggerLatencyAboveInput(TestTriggerLatencyAllEdges):

    TL = 900            # s = -300 (increment (c)): the take starts 300
                        # samples before the command, from the input
                        # history, and ends as much before the second


from testdef_rel_sync_pos import RelSyncPosTestCase, CYCLE, QUARTERS

SLACK_LO, SLACK_HI = 100, 500   # a press this far after a pulse is in the slack window


class SyncRestartMixin:
    """2b-iii: Trigger or Oneshot pressed in the slack window (Sync on,
    acting at once) to end an overdub keeps the loop's phase: deferred, it
    is placed from the count when it fires, which is where the immediate
    restart would be by then.  The overdub's input is silent, so the loop
    and its playback phase stay readable."""

    IL = 600
    OL = 0
    TL = 200            # s = 400

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.InputLatency, float(self.IL))
        self.looper.set_port(test_engine.OutputLatency, float(self.OL))
        self.looper.set_port(test_engine.TriggerLatency, float(self.TL))
        self.wait_buffers(4)

    def tearDown(self):
        self.looper.set_port(test_engine.InputLatency, 0.0)
        self.looper.set_port(test_engine.OutputLatency, 0.0)
        self.looper.set_port(test_engine.TriggerLatency, 0.0)
        self.looper.input_inject_value = 0.0
        super().tearDown()

    def phases(self, bufs=30, skip_silent=False):
        """As playback_offsets(), over `bufs` buffers; with skip_silent,
        only while it plays (a oneshot ends Muted)."""
        L = self.length()
        start = (self.loop_frame(64) - 64) % RAMP
        offs = set()
        for _ in range(bufs):
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            v = self.looper.out_first
            if self.looper.out_first_frame != fr:
                continue
            if skip_silent and v == 0.0:
                continue
            if (fr - start) % L < 64:
                continue
            offs.add(((fr - frame_of(v)) % RAMP) % CYCLE)
        return offs

    def restart_from_overdub(self, command, state):
        self.record_at_phase(*QUARTERS[1])
        before = self.phases()
        self.assertEqual(len(before), 1, "not playing at a steady phase before")
        self.looper.input_ramp = False
        self.looper.input_inject_value = 0.0
        self.engine.request("OVERDUB")
        self.wait_state("Overdubbing")
        self.wait_buffers(2)
        self.press_at(command, self.slack_frame())
        self.wait_state(state)
        return before

    def slack_frame(self):
        """A block start SLACK_LO..SLACK_HI after a pulse at least two
        cycles on.  A press acts at the first block starting at or after
        its frame, so "pulse + 300" can land up to a block later, past
        the eighth (1001)."""
        k = self.looper.sync_clock // CYCLE + 2
        while True:
            f = -(-(k * CYCLE + SLACK_LO) // self.buf) * self.buf
            if f - k * CYCLE <= SLACK_HI:
                return f
            k += 1

    def testTrigger_KeepsPhase(self):
        before = self.restart_from_overdub("TRIGGER", "Playing")
        self.assertEqual(self.phases(), before, "the restart moved the loop's phase")

    def testOneshot_KeepsPhase(self):
        before = self.restart_from_overdub("ONESHOT", "OneShot")
        after = self.phases(bufs=12, skip_silent=True)
        self.assertTrue(after, "the oneshot did not play")
        self.assertEqual(after, before, "the oneshot moved the loop's phase")


@unittest.skipUnless(ENABLED, "TRIGGER_LATENCY_ALL_EDGES is 0")
class TestSyncRestart(SyncRestartMixin, RelSyncPosTestCase):
    pass


@unittest.skipUnless(ENABLED, "TRIGGER_LATENCY_ALL_EDGES is 0")
class TestSyncRestartEqual(SyncRestartMixin, RelSyncPosTestCase):

    TL = 600            # s = 0: stock timing


if __name__ == "__main__":
    unittest.main()
