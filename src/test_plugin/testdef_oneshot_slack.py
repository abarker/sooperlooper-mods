#! /usr/bin/env python3
"""
Tests for ONESHOT_SLACK_FIX (plugin.cc): with Sync on and a tempo set, a
Once (one-shot) pressed less than an eighth after a pulse acts at once
(the slack window).  Stock started the loop from the top there, so the
whole pass played as far behind the beat as the press was late.  Now it
starts that far in, as Trigger does, and plays in phase; a Once that waits
for the pulse is unchanged, and the pass still ends in Mute.

Uses testdef_sync_count_off_by_one.py's setup (exact tempo, ramp input).
"""

import unittest
from testdef_sync_count_off_by_one import SyncCountTestCase, CYCLE, EIGHTH, SLACK, WAIT
from testdef_rel_sync_late_hit import frame_of, RAMP


class OneShotTestCase(SyncCountTestCase):

    def once(self, after):
        """Mute, press Once `after` samples after a pulse; return the
        playback offsets (mod the cycle) during the pass."""
        self.record()
        L = self.length()
        start = (self.loop_frame(64) - 64) % RAMP
        self.engine.request("MUTE")
        self.wait_state("Muted")
        pulse = (self.looper.sync_clock // CYCLE + 2) * CYCLE
        self.press_at("ONESHOT", pulse + after)
        self.assertEqual(self.state() == "OneShot", after < EIGHTH,
                         "in the slack window it should act at once, else wait")
        self.wait_state("OneShot", units=2)
        offs = set()
        for _ in range(8):                # well inside the 2-cycle pass
            self.wait_buffers(1)
            fr = self.looper.out_first_frame
            v = self.looper.out_first
            if self.looper.out_first_frame != fr or v == 0.0:
                continue
            if (fr - start) % L < 64:
                continue                  # the seam
            offs.add(((fr - frame_of(v)) % RAMP) % CYCLE)
        self.assertGreater(len(offs), 0)
        self.assertEqual(offs, {0}, f"Once {after} samples after the pulse: out of phase")
        self.wait_state("Muted", units=3)


class TestPlainSync(OneShotTestCase):

    SYNC = 1.0

    def testSlack_InPhase(self):
        self.once(SLACK)

    def testWaits_InPhase(self):
        self.once(WAIT)


class TestRelSync(OneShotTestCase):

    SYNC = 2.0

    def testSlack_InPhase(self):
        self.once(SLACK)

    def testWaits_InPhase(self):
        self.once(WAIT)


if __name__ == "__main__":
    unittest.main()
