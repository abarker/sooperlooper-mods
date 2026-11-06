#! /usr/bin/env python3
"""
Tests for TL_OVER_IL_LAYER_FIX's Multiply half (plugin.hpp; ROADMAP
entry 42, increment (b)): with trigger_latency larger than input_latency
(k = TL - IL), a Multiply started at once by its command reads the input
k samples back but wrote it at the usual place, k samples late.  Now it
is written where it was heard; its first sample is the one played at the
press; an unrounded end leaves the old loop intact after the second
press (the k samples the end's leftover fill does not cover are filled
at the end); a rounded end records up to the new loop's end
(MULTIPLY_END_FIX's wait is k longer).

As testdef_layer_placement.py: a take of a constant (BASE), the Multiply's
input a ramp, feedback 1, safety feedback off, so each loop sample is the
old loop (BASE) or BASE plus the input frame it holds.  The new loop's
play position runs on from the old one's at the press, so input frame g,
heard at frame g - IL - OL, belongs at position P(p0) + g - IL - OL - p0.
Sync and Quantize off; started mid-loop.  The loop is read a whole loop
after the Multiply ends.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import frame_of, RAMP
from testdef_layer_placement import LayerPlacementTestCase, BASE

LAYER_FIX = bool(getattr(test_engine, "TL_OVER_IL_LAYER_FIX", 0))


class MultiplyPlacementTestCase(LayerPlacementTestCase):

    # IL = OL: stock beginMultiply() leaves an IL - OL gap otherwise
    # (ROADMAP entry 45), not this test's subject.
    IL = OL = 1100
    TL = 1500                  # k = 400

    def multiply(self, rounded, frames):
        self.looper.set_port(test_engine.Round, 1.0 if rounded else 0.0)
        s0, s1 = self.base_take(16000)
        L = s1 - s0
        # A block start where the play position is mid-loop.
        f = -(-(self.looper.sync_clock + 2 * self.buf) // self.buf) * self.buf
        while not (L // 3 <= (f - s1 + self.IL + self.OL) % L < L // 3 + self.buf):
            f += self.buf
        p0 = self.press_at("MULTIPLY", f)
        self.assertEqual(p0, f)
        self.assertEqual(self.state(), "Multiplying")
        d = (p0 - s1 + self.IL + self.OL) % L
        p1 = self.press_at("MULTIPLY", p0 + frames)
        self.wait_state("Playing", units=8)
        newL = self.length()
        self.wait_buffers(newL // self.buf + 4)
        delta = self.looper.input_minus_sync
        layer, base = {}, []
        for pos in range(newL):
            v = self.looper.get_loop_sample(0, pos)
            if v == BASE:
                base.append(pos)
            else:
                layer[pos] = (frame_of(v - BASE) - delta) % RAMP
        return s0, s1, L, p0, p1, d, newL, layer, base

    def check(self, rounded, frames):
        s0, s1, L, p0, p1, d, newL, layer, base = self.multiply(rounded, frames)
        k = self.k()
        self.assertEqual(newL % L, 0, "not whole cycles")
        wrong = [(pos, g) for pos, g in layer.items()
                 if pos != d + g - self.IL - self.OL - p0]
        self.assertEqual(wrong[:5], [], f"{len(wrong)} of {len(layer)} multiply samples misplaced"
                         f"; off by {[pos - (d + g - self.IL - self.OL - p0) for pos, g in wrong[:3]]}")
        got = sorted(layer.values())
        self.assertEqual(got[0], p0 - k, "first multiply sample")
        first = d - self.IL - self.OL - k
        if rounded:
            # Recorded up to the new loop's end.
            self.assertEqual(max(layer), newL - 1, "rounded: not recorded to the end")
            self.assertEqual(len(layer), newL - first, "multiply samples lost")
        else:
            self.assertEqual(got[-1], p1 - k - 1, "last multiply sample")
            self.assertEqual(len(layer), p1 - p0, "multiply samples lost")
        self.assertEqual(len(base), newL - len(layer), "old loop damaged")


class MultiplyPlacementTests:
    """The cases, run by each TL class below."""

    def testUnrounded(self):
        self.check(False, 16000 + 16000 // 2)

    def testUnrounded_Short(self):
        self.check(False, 16000 // 2)

    def testRounded_OneCycle(self):
        # Ends inside the first cycle: records on to the loop's end, k
        # more samples with the lag (MULTIPLY_END_FIX's wait).
        self.check(True, 16000 // 3)

    @unittest.skip("Rounded Multiply past its first cycle: the end's leftover fill "
                   "copies the source over OL + IL samples one cycle back "
                   "(stock): ROADMAP entry 46")
    def testRounded(self):
        self.check(True, 16000 + 16000 // 2)


@unittest.skipUnless(LAYER_FIX, "TL_OVER_IL_LAYER_FIX is 0")
class TestMultiplyPlacementTLOverIL(MultiplyPlacementTests, MultiplyPlacementTestCase):
    TL = 1500                  # k = 400


class TestMultiplyPlacementTLEqualIL(MultiplyPlacementTests, MultiplyPlacementTestCase):
    TL = 1100                  # k = 0: the stock path



if __name__ == "__main__":
    unittest.main()
