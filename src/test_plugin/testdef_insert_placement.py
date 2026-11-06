#! /usr/bin/env python3
"""
Tests for TL_OVER_IL_LAYER_FIX's Insert half (plugin.hpp; ROADMAP entry
42, increment (b)): with trigger_latency larger than input_latency
(k = TL - IL), an Insert started at once by its command reads the input
k samples back but wrote it at the usual place, k samples late.  Now the
inserted stretch starts where the press was heard, OL + IL + k behind
the play position, with the input played at the press, and (unrounded)
ends with the input played at the second press.

As testdef_multiply_placement.py: a take of a constant (BASE), the
inserted input a ramp; input frame g belongs at P(p0) + g - IL - OL - p0.
Only the inserted stretch and the old loop before it are checked: what
follows is stock behaviour not covered here (the old loop resumes at the
play position, not the heard one, and a rounded Insert's last OL + IL
(+ k) samples are silent; ROADMAP entry 47).  Sync and Quantize off,
IL = OL; started mid-loop.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import frame_of, RAMP
from testdef_layer_placement import LayerPlacementTestCase, BASE

LAYER_FIX = bool(getattr(test_engine, "TL_OVER_IL_LAYER_FIX", 0))


class InsertPlacementTestCase(LayerPlacementTestCase):

    IL = OL = 1100
    TL = 1500                  # k = 400

    def insert(self, rounded, frames):
        self.looper.set_port(test_engine.Round, 1.0 if rounded else 0.0)
        s0, s1 = self.base_take(16000)
        L = s1 - s0
        f = -(-(self.looper.sync_clock + 2 * self.buf) // self.buf) * self.buf
        while not (L // 3 <= (f - s1 + self.IL + self.OL) % L < L // 3 + self.buf):
            f += self.buf
        p0 = self.press_at("INSERT", f)
        self.assertEqual(p0, f)
        self.assertEqual(self.state(), "Inserting")
        d = (p0 - s1 + self.IL + self.OL) % L
        p1 = self.press_at("INSERT", p0 + frames)
        self.wait_state("Playing", units=8)
        newL = self.length()
        self.wait_buffers(newL // self.buf + 4)
        return p0, p1, d, newL

    def check(self, rounded, frames):
        p0, p1, d, newL = self.insert(rounded, frames)
        k = self.k()
        delta = self.looper.input_minus_sync
        first = d - self.IL - self.OL - k
        before = [pos for pos in range(first) if self.looper.get_loop_sample(0, pos) != BASE]
        self.assertEqual(before[:5], [], "old loop damaged before the insert")
        # The inserted run: every sample the input frame heard there.
        n = (p1 - p0) if not rounded else 4 * self.buf
        wrong = []
        for pos in range(first, first + n):
            v = self.looper.get_loop_sample(0, pos)
            g = (frame_of(v) - delta) % RAMP
            if v == BASE or pos != d + g - self.IL - self.OL - p0:
                wrong.append((pos, g))
        self.assertEqual(wrong[:5], [], f"{len(wrong)} of {n} inserted samples misplaced")
        self.assertEqual((frame_of(self.looper.get_loop_sample(0, first)) - delta) % RAMP,
                         p0 - k, "first inserted sample")
        if not rounded:
            last = first + (p1 - p0) - 1
            self.assertEqual((frame_of(self.looper.get_loop_sample(0, last)) - delta) % RAMP,
                             p1 - k - 1, "last inserted sample")
            self.assertEqual(self.looper.get_loop_sample(0, last + 1), 0.0,
                             "inserted past the second press")


class InsertPlacementTests:
    """The cases, run by each TL class below."""

    def testUnrounded(self):
        self.check(False, 6000)

    def testRounded(self):
        self.check(True, 6000)


@unittest.skipUnless(LAYER_FIX, "TL_OVER_IL_LAYER_FIX is 0")
class TestInsertPlacementTLOverIL(InsertPlacementTests, InsertPlacementTestCase):
    TL = 1500                  # k = 400


class TestInsertPlacementTLEqualIL(InsertPlacementTests, InsertPlacementTestCase):
    TL = 1100                  # k = 0: the stock path


if __name__ == "__main__":
    unittest.main()
