#! /usr/bin/env python3
"""
A multiply after a Record late stop keeps the loop's content.

Found on the rig with rig_probes/multiply_live_probe.py: a take ended by a
Record press just after a pulse (the late stop) and then multiplied went
silent about a beat after the multiply ended, for good.  The same take
ended by a press that waited for its pulse kept its content; the start
(waiting or late hit) and the pre-roll made no difference.

Each take is one cycle with plain Sync: Record from a waiting or late-hit
start, ended by a waiting or late Record stop.  The input is then silent,
and a Multiply starts before the next pulse and ends two cycles on.  The
multiplied loop is two cycles of the take repeated, so the output over
two whole passes is the ramp: never silent.
"""

import unittest
import test_engine
from testdef_rel_sync_late_hit import MARGIN
from testdef_sync_preroll import SyncPrerollTestCase


class MultiplyAfterStopTestCase(SyncPrerollTestCase):

    PREROLL_MS = 0.0

    def take(self, late_start, late_stop):
        pulse = self.start_late() if late_start else self.start_waiting()
        if late_stop:
            self.end_late(pulse, cycles=1)
        else:
            self.end_early(pulse, cycles=1)
        self.assertEqual(self.length(), self.cycle)
        self.looper.input_ramp = False            # silent from here on
        self.looper.input_inject_value = 0.0
        p2 = self.next_pulse()
        self.press_at("MULTIPLY", p2 - 6 * self.buf)
        self.wait_state("Multiplying")
        return p2 + 2 * self.cycle

    def end_multiply(self, end, late_end):
        if late_end:
            self.press_at("MULTIPLY", end + MARGIN)
        else:
            self.press_at("MULTIPLY", end - max(6 * self.buf, self.P + 4 * self.buf + MARGIN))
        self.wait_state("Playing", units=3)
        self.assertEqual(self.length(), 2 * self.cycle)

    def assertOutputHasContent(self, what):
        L = self.length()
        silent = total = 0
        for _ in range(L // self.buf + 4):
            self.wait_buffers(1)
            total += 1
            if self.looper.out_first == 0.0:
                silent += 1
        self.assertLessEqual(silent, 1,
                             f"{what}: the output was silent at the start of {silent} of {total}"
                             f" buffers over a pass of the {L}-sample loop")

    def check(self, late_start, late_stop, late_end=False):
        what = "%s start, %s stop, %s multiply end" % (
            "late-hit" if late_start else "waiting", "late" if late_stop else "waiting",
            "late" if late_end else "early")
        end = self.take(late_start, late_stop)
        self.end_multiply(end, late_end)
        # (Not the loop memory: after a multiply the new loop is filled in
        # as it plays, from the take before it.)
        self.assertOutputHasContent(what)
        self.assertOutputHasContent(what + ", second pass")


class TestMultiplyAfterStop(MultiplyAfterStopTestCase):

    def testWaitingStop_WaitingStart(self):
        self.check(late_start=False, late_stop=False)

    def testWaitingStop_LateHitStart(self):
        self.check(late_start=True, late_stop=False)

    def testLateStop_WaitingStart(self):
        self.check(late_start=False, late_stop=True)

    def testLateStop_LateHitStart(self):
        self.check(late_start=True, late_stop=True)

    def testLateStop_LateMultiplyEnd(self):
        self.check(late_start=False, late_stop=True, late_end=True)


class TestMultiplyAfterStopPreroll(TestMultiplyAfterStop):
    """The same with a pre-roll, as on the rig (40 ms there)."""

    PREROLL_MS = 10.0


if __name__ == "__main__":
    unittest.main()
