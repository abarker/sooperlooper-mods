#! /usr/bin/env python3
"""
Tests for TL_OVER_IL_LAYER_FIX (plugin.hpp) and
OVERDUB_WRAP_FILL_FIX (plugin.cc), both stock bugs.

TL_OVER_IL_LAYER_FIX: with trigger_latency larger than input_latency
(k = TL - IL), an Overdub, Replace or Substitute started at once by its
command reads the input k samples back but wrote it at the usual place,
so the whole layer landed k samples late.  Now it is written where it
was heard, and its first and last samples are the ones played at the two
presses (the input k samples before each command).

OVERDUB_WRAP_FILL_FIX: a layer started less than OL + IL (+ k) into the
loop writes its first samples near the loop's end, and the copy of the
previous layer into the new chunk overwrote them there: the layer lost
its first OL + IL (+ k) - start samples.  Now none are lost, and the old
layer around them is intact.

The take is a constant (BASE) and the layer's input a ramp, so every loop
sample says whether it is the old layer (BASE) or the new one, and which
input frame it holds (Overdub adds the input to BASE: feedback 1, safety
feedback off; Replace and Substitute write the input alone).  A layer
sample is in time when it sits where its input frame was heard: the
take's input frame g0 + p is at loop position p, so input frame g belongs
at (g - g0) mod L.  Sync and Quantize off.

Presses land on block starts and the take is whole blocks long, so the
play position at a press is OL + IL + a whole number of blocks (mod L):
the start positions used are the reachable one nearest the loop's start
(the written stretch wraps by most of OL + IL), the last one before the
stretch stops wrapping (exactly OL + IL when s = 0: it wraps by k only,
or starts at 0 when k is 0), and one well past both.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import RelSyncLateHitTestCase, frame_of, RAMP

LAYER_FIX = bool(getattr(test_engine, "TL_OVER_IL_LAYER_FIX", 0))
ALL_EDGES = bool(getattr(test_engine, "TRIGGER_LATENCY_ALL_EDGES", 0))
BASE = 0.25
IL, OL = 1300, 900


class LayerPlacementTestCase(RelSyncLateHitTestCase):

    IL, OL = IL, OL
    TL = 1700                  # k = 400

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.set_port(test_engine.Quantize, 0.0)
        self.looper.set_port(test_engine.UseSafetyFeedback, 0.0)
        self.looper.set_port(test_engine.Feedback, 1.0)
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

    def k(self):
        return max(0, self.TL - self.IL)

    def base_take(self, length):
        """A take of BASE, `length` frames between the presses; returns
        (s0, s1), the sync frames where it started and ended."""
        self.looper.input_ramp = False
        self.looper.input_inject_value = BASE
        s0 = self.press_at("RECORD", self.looper.sync_clock + 2 * self.buf)
        self.wait_state("Recording")
        s1 = self.press_at("RECORD", s0 + length)
        self.wait_state("Playing")
        self.assertEqual(self.length(), s1 - s0)
        self.wait_buffers(4)
        self.looper.input_ramp = True
        # With TRIGGER_LATENCY_ALL_EDGES the take starts and ends s =
        # IL - TL after each press (before, from the history and trimmed,
        # when s < 0; same length): use the frames where it really did.
        shift = (self.IL - self.TL) if ALL_EDGES else 0
        return s0 + shift, s1 + shift

    def reachable(self, s1, desired, below=False):
        """The play position at a block start nearest `desired`, at or
        above it (at or below with `below`): block starts give OL + IL +
        whole blocks after the take's end s1."""
        rr = (self.IL + self.OL - s1) % self.buf
        if below:
            return desired - ((desired - rr) % self.buf)
        return desired + ((rr - desired) % self.buf)

    def layer(self, s0, s1, command, state, start_pos, frames):
        """`command` pressed at the next block start where the play
        position is start_pos (None: anywhere at least half the loop from
        the loop's start; "near": nearest the loop's start; "latency": the
        last before the written stretch stops wrapping), ended by the same
        command `frames` later.  Returns (p0, p1, the play position at
        p0)."""
        L = s1 - s0
        s = max(0, self.IL - self.TL)
        if start_pos == "near":
            start_pos = self.reachable(s1, 0)
        elif start_pos == "latency":
            # The layer writes from s after the press, OL + IL behind.
            start_pos = self.reachable(s1, self.OL + self.IL - s, below=True)
        f = -(-(self.looper.sync_clock + 2 * self.buf) // self.buf) * self.buf
        for _ in range(4 * L // self.buf + 8):
            # Play position at sync frame f: playback after the take starts
            # OL + IL into the loop at s1.
            pos = (f - s1 + self.IL + self.OL) % L
            if (pos == start_pos) if start_pos is not None else (L // 2 <= pos < L // 2 + self.buf):
                break
            f += self.buf
        else:
            self.fail(f"no block start at play position {start_pos}")
        p0 = self.press_at(command, f)
        self.assertEqual(p0, f)
        self.assertEqual(self.state(), state)
        p1 = self.press_at(command, p0 + frames)
        self.wait_state("Playing")
        # The new chunk is filled from the old one as the play position
        # passes: read it a whole loop later.
        self.wait_buffers(L // self.buf + 4)
        return p0, p1, pos

    def read_loop(self, s0, s1, command):
        """(layer, base): {loop position: input frame (sync frames)} for
        the new layer, and the positions still holding BASE."""
        L = s1 - s0
        delta = self.looper.input_minus_sync
        sub = BASE if command == "OVERDUB" else 0.0
        layer, base = {}, []
        for pos in range(L):
            v = self.looper.get_loop_sample(0, pos)
            if v == BASE:
                base.append(pos)
            else:
                layer[pos] = (frame_of(v - sub) - delta) % RAMP
        return layer, base

    def check_layer(self, command, state, start_pos=None, frames=None):
        s0, s1 = self.base_take(16000)
        L = s1 - s0
        self.assertGreater(L, 4 * (self.OL + self.IL + self.k()))
        p0, p1, d = self.layer(s0, s1, command, state, start_pos, frames or L // 4)
        layer, base = self.read_loop(s0, s1, command)
        k, s = self.k(), max(0, self.IL - self.TL)
        # Placement: each layer sample where its input frame was heard.
        wrong = [(pos, g) for pos, g in layer.items() if (g - s0) % L != pos]
        self.assertEqual(wrong[:5], [], f"{len(wrong)} of {len(layer)} layer samples misplaced"
                         f" (position, input frame); off by {[(g - s0 - pos) % L for pos, g in wrong[:3]]}")
        # Edges: from the input played at the first press to that played
        # at the second (k before each command), none lost.  A layer that
        # waits s (TL < IL) starts s after the command; it ends at the
        # second command, or s after it with TRIGGER_LATENCY_ALL_EDGES.
        end_s = s if ALL_EDGES else 0
        got = sorted(layer.values())
        self.assertEqual(got[0], p0 - k + s, "first layer sample")
        self.assertEqual(got[-1], p1 - k + end_s - 1, "last layer sample")
        self.assertEqual(len(layer), p1 - p0 - s + end_s, "layer samples lost")
        self.assertEqual(len(base), L - len(layer), "old layer damaged")
        # Where the layer starts in the loop: OL + IL + k behind the play
        # position (less s), wrapped if that is before the loop's start.
        self.assertEqual((got[0] - s0) % L, (d - self.OL - self.IL - k + s) % L)
        return d


class LayerPlacementTests:
    """The cases, run by each TL class below."""

    def testOverdub(self):
        self.check_layer("OVERDUB", "Overdubbing")

    def testReplace(self):
        self.check_layer("REPLACE", "Replacing")

    def testSubstitute(self):
        self.check_layer("SUBSTITUTE", "Substitute")

    def testOverdub_Short(self):
        # Shorter than OL + IL + k: the layer ends before its write
        # position has crossed the stretch behind the play position.
        self.check_layer("OVERDUB", "Overdubbing", frames=self.buf)

    def testOverdub_StartsNearLoopStart(self):
        # The written stretch wraps by most of OL + IL (+ k).
        self.check_layer("OVERDUB", "Overdubbing", start_pos="near")

    def testReplace_StartsNearLoopStart(self):
        self.check_layer("REPLACE", "Replacing", start_pos="near")

    def testOverdub_StartsAtLatency(self):
        # Exactly OL + IL in: wraps by k only (or, at k = 0, starts at the
        # loop's start, where the clamped front-fill mark copied over
        # sample 0).
        self.check_layer("OVERDUB", "Overdubbing", start_pos="latency")

    def testReplace_StartsAtLatency(self):
        self.check_layer("REPLACE", "Replacing", start_pos="latency")


@unittest.skipUnless(LAYER_FIX, "TL_OVER_IL_LAYER_FIX is 0")
class TestLayerPlacementTLOverIL(LayerPlacementTests, LayerPlacementTestCase):
    TL = 1700                  # k = 400


class TestLayerPlacementTLEqualIL(LayerPlacementTests, LayerPlacementTestCase):
    TL = IL                    # k = 0: the stock path; OVERDUB_WRAP_FILL_FIX only


class TestLayerPlacementTLUnderIL(LayerPlacementTests, LayerPlacementTestCase):
    TL = 1000                  # waits IL - TL = 300; the stock path



if __name__ == "__main__":
    unittest.main()
