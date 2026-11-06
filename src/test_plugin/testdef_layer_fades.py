#! /usr/bin/env python3
"""
Tests for the fades at a layer's edges and at a Record over a playing
loop: TL_OVER_IL_LAYER_FIX's tail (plugin.hpp; ROADMAP entry 49),
TL_UNDER_IL_LAYER_FIX (plugin.cc), and RECORD_OVER_PLAY_FADE (plugin.hpp;
entry 48).

The crossfade here is FADE samples (most tests run at 1).  The take is a
constant (BASE) and the layer's input another (LEVEL), so a loop with its
fades right holds only levels, joined by straight ramps of FADE samples:
no two neighbouring samples differ by more than a ramp's step.  A cut, a
layer's fade-out k after its end with the old loop between (entry 49), a
Replace's step where the old loop's fade ran ahead of the input's, or
silence left unfilled before a Multiply or Insert (both TL < IL) each make
a bigger one.  Every layer type, at TL above, equal to, and below IL.

An Insert's start is left out: the old loop stops there with no fade
(stock; the inserted input fades in from silence).

Record over a playing loop: the output, read back sample by sample
(TestLooper.get_output_at), plays the loop on until the take starts (the
wait of IL - TL with TRIGGER_LATENCY_ALL_EDGES, or none) and then fades
it out over FADE samples.
"""

import unittest
import test_engine
from testdef_layer_placement import LayerPlacementTestCase, BASE, IL, OL

LAYER_FIX = bool(getattr(test_engine, "TL_OVER_IL_LAYER_FIX", 0))
ALL_EDGES = bool(getattr(test_engine, "TRIGGER_LATENCY_ALL_EDGES", 0))
RECORD_FADE = bool(getattr(test_engine, "RECORD_OVER_PLAY_FADE", 0))
FADE = 64
LEVEL = 0.5
STEP = 1.25 * LEVEL / FADE      # the steepest ramp (BASE to BASE + LEVEL), with a margin
TAKE = 16000


class LayerFadeTestCase(LayerPlacementTestCase):

    def setUp(self):
        super().setUp()
        self.looper.set_port(test_engine.FadeSamples, float(FADE))
        self.looper.set_port(test_engine.DryLevel, 0.0)
        self.wait_buffers(2)

    def tearDown(self):
        self.looper.set_port(test_engine.FadeSamples, 1.0)
        self.looper.set_port(test_engine.Round, 0.0)
        super().tearDown()

    def take(self):
        s0, s1 = self.base_take(TAKE)
        self.looper.input_ramp = False
        self.looper.input_inject_value = LEVEL
        self.wait_buffers(2)
        return s0, s1

    def press_mid_loop(self, s1, L, command, state, frac=0.5):
        """`command` at a block start where the play position is about
        frac of the loop in; returns its frame."""
        f = -(-(self.looper.sync_clock + 2 * self.buf) // self.buf) * self.buf
        while not (int(frac * L) <= (f - s1 + self.IL + self.OL) % L < int(frac * L) + self.buf):
            f += self.buf
        p0 = self.press_at(command, f)
        self.assertEqual(self.state(), state)
        return p0

    def loop_values(self):
        L = self.length()
        return [self.looper.get_loop_sample(0, pos) for pos in range(L)]

    def assertNoSteps(self, vals, skip=(), what="loop"):
        """No two neighbouring samples (around the loop) differ by more
        than a ramp's step, except within `skip` (positions)."""
        L = len(vals)
        bad = [(p, vals[p - 1], vals[p]) for p in range(L)
               if abs(vals[p] - vals[p - 1]) > STEP and p not in skip]
        self.assertEqual(bad[:4], [], f"{len(bad)} steps in the {what} (position, before, after);"
                         f" a ramp's step is at most {STEP:.4f}")

    def assertHolds(self, vals, level, at_least):
        n = sum(1 for v in vals if abs(v - level) < 1e-6)
        self.assertGreaterEqual(n, at_least, f"only {n} samples at {level}")

    # -- layers --------------------------------------------------------
    def check_layer(self, command, state, level, frames=TAKE // 4):
        s0, s1 = self.take()
        L = s1 - s0
        p0 = self.press_mid_loop(s1, L, command, state)
        p1 = self.press_at(command, p0 + frames)
        self.wait_state("Playing")
        self.wait_buffers(L // self.buf + 4)
        vals = self.loop_values()
        # (Presses land on block starts: the real interval.)
        self.assertHolds(vals, level, p1 - p0 - 2 * FADE - max(0, self.IL - self.TL))
        self.assertHolds(vals, BASE, L - (p1 - p0) - 2 * FADE)
        self.assertNoSteps(vals)

    def check_multiply(self, rounded, frames):
        self.looper.set_port(test_engine.Round, 1.0 if rounded else 0.0)
        s0, s1 = self.take()
        L = s1 - s0
        p0 = self.press_mid_loop(s1, L, "MULTIPLY", "Multiplying", frac=0.3)
        self.press_at("MULTIPLY", p0 + frames)
        self.wait_state("Playing", units=8)
        newL = self.length()
        self.wait_buffers(newL // self.buf + 4)
        vals = self.loop_values()
        self.assertHolds(vals, BASE + LEVEL, frames // 2)
        self.assertNoSteps(vals)

    def check_insert(self, rounded, frames):
        self.looper.set_port(test_engine.Round, 1.0 if rounded else 0.0)
        s0, s1 = self.take()
        L = s1 - s0
        p0 = self.press_mid_loop(s1, L, "INSERT", "Inserting", frac=0.4)
        self.press_at("INSERT", p0 + frames)
        self.wait_state("Playing", units=8)
        newL = self.length()
        self.wait_buffers(newL // self.buf + 4)
        vals = self.loop_values()
        self.assertHolds(vals, LEVEL, frames // 2)
        # The insert's start: the one step allowed, where BASE drops to
        # the inserted input's fade-in from silence (stock).
        starts = [p for p in range(newL) if vals[p - 1] == BASE and vals[p] < STEP]
        self.assertEqual(len(starts), 1, f"the insert's start: {starts[:4]}")
        self.assertGreater(vals[starts[0]], 0.0, "silence before the inserted input")
        self.assertNoSteps(vals, skip={starts[0]})


class LayerFadeTests:
    """The cases, run by each TL class below."""

    def testOverdub(self):
        self.check_layer("OVERDUB", "Overdubbing", BASE + LEVEL)

    def testReplace(self):
        self.check_layer("REPLACE", "Replacing", LEVEL)

    def testSubstitute(self):
        self.check_layer("SUBSTITUTE", "Substitute", LEVEL)

    def testOverdub_Short(self):
        # Ends before the fade-in is through.
        self.check_layer("OVERDUB", "Overdubbing", BASE + LEVEL, frames=self.buf)

    def testMultiply_Unrounded(self):
        self.check_multiply(False, TAKE + TAKE // 2)

    def testMultiply_Rounded(self):
        self.check_multiply(True, TAKE + TAKE // 2)

    def testInsert_Unrounded(self):
        self.check_insert(False, TAKE // 4)

    def testInsert_Rounded(self):
        self.check_insert(True, TAKE // 4)


@unittest.skipUnless(LAYER_FIX, "TL_OVER_IL_LAYER_FIX is 0")
class TestLayerFadesTLOverIL(LayerFadeTests, LayerFadeTestCase):
    TL = IL + 400


class TestLayerFadesTLEqualIL(LayerFadeTests, LayerFadeTestCase):
    TL = IL


class TestLayerFadesTLUnderIL(LayerFadeTests, LayerFadeTestCase):
    TL = IL - 300


# -- Record over a playing loop -------------------------------------------
class RecordOverPlayTests:

    def testRecordOverPlay_FadesOut(self):
        s0, s1 = self.take()
        L = s1 - s0
        self.looper.input_inject_value = 0.0
        # (press_at waits two blocks: past any wait here.)
        p = self.press_mid_loop(s1, L, "RECORD", "Recording")
        self.wait_state("Recording")
        self.wait_buffers(4 + FADE // self.buf)
        frame = lambda f: f + self.looper.input_minus_sync    # sync -> input_clock frame
        out = [self.looper.get_output_at(frame(f)) for f in range(p - self.buf, p + self.wait() + FADE + 2 * self.buf)]
        start = self.buf + self.wait()           # index of the take's first sample
        self.assertTrue(all(abs(v - BASE) < 1e-6 for v in out[:start]),
                        f"the loop stopped before the take started: {[round(v, 4) for v in out[:start] if abs(v - BASE) >= 1e-6][:4]}")
        self.assertTrue(all(v == 0.0 for v in out[start + FADE + 1:]), "not faded out")
        steps = [abs(out[i] - out[i - 1]) for i in range(1, len(out))]
        self.assertLessEqual(max(steps), 1.25 * BASE / FADE, "the loop was cut, not faded out")

    def wait(self):
        """Samples from the press to the take's start: IL - TL with
        TRIGGER_LATENCY_ALL_EDGES (when positive), else none."""
        return max(0, self.IL - self.TL) if ALL_EDGES else 0


@unittest.skipUnless(RECORD_FADE, "RECORD_OVER_PLAY_FADE is 0")
class TestRecordOverPlayTLOverIL(RecordOverPlayTests, LayerFadeTestCase):
    TL = IL + 400


@unittest.skipUnless(RECORD_FADE, "RECORD_OVER_PLAY_FADE is 0")
class TestRecordOverPlayTLEqualIL(RecordOverPlayTests, LayerFadeTestCase):
    TL = IL


@unittest.skipUnless(RECORD_FADE, "RECORD_OVER_PLAY_FADE is 0")
class TestRecordOverPlayTLUnderIL(RecordOverPlayTests, LayerFadeTestCase):
    TL = IL - 300


if __name__ == "__main__":
    unittest.main()
