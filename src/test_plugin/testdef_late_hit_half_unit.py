#! /usr/bin/env python3
"""
Tests for the late-hit window's limits against the Quantize unit (the
spacing of the boundaries the Quantize setting gives: one eighth with
Quantize 8th, Eighths/Cycle eighths with Quantize Cycle).

* The window is at most half the unit, whatever the settings: the nearer
  boundary wins.  A press a little before a boundary starts on it, even
  with a window longer than half the unit; a wider window took such a
  press as late for the boundary before and gave a take one unit too long.
  Mute and unmute share the cap.
* late_hit_max_quant_unit_fraction is a fraction of the Quantize unit, not
  of the cycle: under Quantize 8th it was applied to the whole cycle.

Tempo 120 BPM at 48 kHz: an eighth is 12000 samples (250 ms), the cycle 8
eighths.  Pulses are injected one Quantize unit apart.  Same exact-frame
presses and ramp input as testdef_rel_sync_late_hit.py.
"""

import unittest
import test_engine
from testdef_late_stop_any_command import LateStopAnyCommandTestCase
from testdef_rel_sync_late_hit import RAMP, MARGIN

EIGHTH = 12000
EIGHTHS_PER_CYCLE = 8


class HalfUnitTestCase(LateStopAnyCommandTestCase):

    QUANTIZE = 2.0          # QUANT_8TH
    UNIT = EIGHTH
    WINDOW_MS = 300.0       # 14400 samples, more than half an eighth
    FRACTION = 100.0        # the default: no fraction cap of its own
    HISTORY = 4.0           # --input-history seconds (the default)

    def setUp(self):
        # The input history is sized when the loop is made (in super().setUp()).
        test_engine.cvar.g_input_history_seconds = self.HISTORY
        super().setUp()
        self.looper.set_port(test_engine.Quantize, self.QUANTIZE)
        self.looper.set_port(test_engine.EighthPerCycleLoop, float(EIGHTHS_PER_CYCLE))
        self.looper.set_port(test_engine.TempoInput, self.sr * 30.0 / EIGHTH)
        self.looper.sync_inject_period = 0
        self.cycle = self.UNIT                                  # pulse spacing
        self.looper.sync_clock = 0
        self.looper.sync_inject_period = self.UNIT
        test_engine.cvar.g_late_hit_window_ms = self.WINDOW_MS
        test_engine.cvar.g_mute_late_hit_ms = self.WINDOW_MS
        test_engine.cvar.g_late_hit_max_quant_unit_fraction = self.FRACTION
        self.wait_cycles(3)

    def tearDown(self):
        test_engine.cvar.g_late_hit_max_quant_unit_fraction = 100.0
        test_engine.cvar.g_mute_late_hit_ms = 100.0
        test_engine.cvar.g_input_history_seconds = 4.0
        super().tearDown()

    def pulse_ahead(self, inside_buffer=True):
        """A pulse a few units ahead; inside_buffer: one that falls well
        inside a buffer, not at its start."""
        k = self.looper.sync_clock // self.UNIT + 3
        while inside_buffer and not (self.buf // 4 < (k * self.UNIT) % self.buf < 3 * self.buf // 4):
            k += 1
        return k * self.UNIT

    def take_from(self, press_frame, pulse):
        """Record from a press at press_frame, end just after the pulse
        four units after `pulse`; return (where the loop starts relative to
        `pulse`, the loop length)."""
        self.press_at("RECORD", press_frame)
        self.press_at("RECORD", pulse + 4 * self.UNIT + MARGIN)
        self.wait_state("Playing")
        start = (self.loop_frame(64) - 64 - self.looper.input_minus_sync) % RAMP
        return ((start - pulse + RAMP // 2) % RAMP) - RAMP // 2, self.length()

    def early_bufs(self):
        """Buffers before a pulse for an early press: up to 4, within the
        last third of the unit.  At least 3, so the state can still be read
        before the pulse after press_at()'s 2-buffer wait (skip if the
        buffers are too long for that)."""
        n = min(4, self.UNIT // 3 // self.buf)
        if n < 3:
            self.skipTest(f"{self.buf}-frame buffers are too long for an early press here")
        return n

    def assertStartsOnPulse(self, press_frame, pulse, what):
        off, length = self.take_from(press_frame, pulse)
        self.assertEqual(off, 0, f"{what}: the loop starts {off} samples from the pulse")
        self.assertEqual(length, 4 * self.UNIT, f"{what}: the loop is not 4 units long")


class TestHalfUnitCap(HalfUnitTestCase):

    def testEarlyPressInPulseBuffer_StartsOnPulse(self):
        p = self.pulse_ahead()
        self.assertStartsOnPulse((p // self.buf) * self.buf, p,
                                 "a press early in the pulse's own buffer")

    def testEarlyPressBuffersBefore_StartsOnPulse(self):
        p = self.pulse_ahead()
        self.assertStartsOnPulse((p // self.buf - self.early_bufs()) * self.buf, p,
                                 "a press a few buffers early")

    def testLatePressUnderHalf_Corrected(self):
        # Up to 0.4 of a unit late (100 ms), inside both the 300 ms window
        # and the half-unit cap: still a late hit on the pulse.  (A press
        # lands on the first buffer start at or after its target.)
        p = self.pulse_ahead()
        self.assertStartsOnPulse(p + int(0.4 * self.UNIT) - (self.buf - 1), p,
                                 "a press up to 0.4 unit late")

    def testLatePressOverHalf_WaitsForNext(self):
        # 0.6 of a unit late: nearer the next pulse, so it waits for it.
        p = self.pulse_ahead()
        self.assertStartsOnPulse(p + int(0.6 * self.UNIT), p + self.UNIT,
                                 "a press 0.6 unit late")

    def testEarlyMute_WaitsForPulse(self):
        p = self.pulse_ahead()
        self.take_from(p, p)
        q = self.pulse_ahead()
        self.press_at("MUTE", (q // self.buf - self.early_bufs()) * self.buf)
        self.assertEqual(self.state(), "Playing",
                         "a Mute a few buffers early acted at once instead of waiting")
        self.wait_state("Muted")


class TestHalfUnitCapCycle(HalfUnitTestCase):
    """Quantize Cycle: the unit is the cycle; the cap is half of it."""

    QUANTIZE = 1.0          # QUANT_CYCLE
    UNIT = EIGHTH * EIGHTHS_PER_CYCLE
    WINDOW_MS = 600.0       # 28800 samples, under half the 96000-sample cycle

    def testLatePress_CorrectedWithWindowUnderHalf(self):
        # 500 ms late: corrected, the window being under half the cycle.
        p = self.pulse_ahead(inside_buffer=False)
        self.assertStartsOnPulse(p + 24000, p, "a press 500 ms late")


class TestFractionOfQuantizeUnit(HalfUnitTestCase):
    """Under Quantize 8th the fraction is of the eighth, not of the cycle:
    0.3 of an eighth is 3600 samples (0.3 of the cycle would be 28800,
    more than the 300 ms window)."""

    FRACTION = 0.3

    def testPressPastFractionOfEighth_NotCorrected(self):
        # 3900 samples late or a little more: past 0.3 of the eighth, inside
        # the window and (for buffers up to 2048) under the half-unit cap.
        p = self.pulse_ahead()
        self.assertStartsOnPulse(p + 3900, p + self.UNIT, "a press 3900 samples late")

    def testPressInsideFractionOfEighth_Corrected(self):
        if MARGIN + self.buf > 3600:
            self.skipTest(f"{self.buf}-frame buffers are too long for this press")
        p = self.pulse_ahead()
        self.assertStartsOnPulse(p + MARGIN, p, "a press just after the pulse")


class InputHistoryTestCase(HalfUnitTestCase):
    """Quantize Cycle and a 900 ms window (under half the 2 s cycle); a
    press 800 ms late needs more than the shortest input history, 32768
    samples (683 ms)."""

    QUANTIZE = 1.0          # QUANT_CYCLE
    UNIT = EIGHTH * EIGHTHS_PER_CYCLE
    WINDOW_MS = 900.0
    LATE = 38400            # 800 ms


class TestInputHistoryShortest(InputHistoryTestCase):

    HISTORY = 0.0           # rounds up to the 32768-sample minimum

    def testLatePressPastHistory_NotCorrectedAndCounted(self):
        self.assertEqual(self.looper.get_channel_history_size(0), 32768)
        before = self.looper.get_channel_history_short_count(0)
        p = self.pulse_ahead(inside_buffer=False)
        self.assertStartsOnPulse(p + self.LATE, p + self.UNIT, "a press 800 ms late")
        self.assertGreater(self.looper.get_channel_history_short_count(0), before,
                           "the refused late start was not counted")


class TestInputHistoryLonger(InputHistoryTestCase):
    """--input-history 2: the history holds it, so the press is corrected
    and nothing is counted."""

    HISTORY = 2.0

    def testLatePressWithinHistory_Corrected(self):
        self.assertEqual(self.looper.get_channel_history_size(0), 131072)   # 2.7 s
        before = self.looper.get_channel_history_short_count(0)
        p = self.pulse_ahead(inside_buffer=False)
        self.assertStartsOnPulse(p + self.LATE, p, "a press 800 ms late")
        self.assertEqual(self.looper.get_channel_history_short_count(0), before)


if __name__ == "__main__":
    unittest.main()
