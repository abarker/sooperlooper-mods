#! /usr/bin/env python3
"""
Regression test for a stale lGlobalSamplesSinceSync counter surviving a
plain Sync-port toggle (0 -> 1) with no sync-SOURCE change in between.

Background
----------
plugin.cc's global sync counter (lGlobalSamplesSinceSync, used to compute
T = lGlobalSyncAtButtonPress for the late-hit/on-time decision at the
Record, Insert, and Mute/Pause call sites) is incremented and reset
entirely inside a block gated on `fSyncMode >= 1.0f`:

    if (fSyncMode >= 1.0f) {
        for (...) {
            pLS->lGlobalSamplesSinceSync++;
            if (pfSyncInput[n] > 1.5f) { ...; lGlobalSamplesSinceSync = 0; ... }
        }
    }

When fSyncMode drops to 0.0 (this rig's big_tempo tool sends OSC
`/sl/-1/set sync 0.0` on every JACK transport stop), the counter FREEZES
at whatever value it had -- the loop body above does not run at all.
When fSyncMode returns to >= 1.0 (transport restart, OSC `sync 1.0`),
the counter resumes incrementing from that stale value -- nothing
resets it at the 0->1 transition itself, only a real sync pulse does.

There IS an existing fix for a related scenario -- Looper::reset_late_hit_state()
(plugin.cc/looper.cpp, "C6 fix") -- but it is only reached via
Engine::update_sync_source(), which fires on a sync-SOURCE change
(Event::SyncTo, the "sync_source" OSC param) or on add_loop(). It is
NOT called when the per-loop "sync" PORT itself (Event::SyncMode,
fSyncMode) is toggled off and on -- exactly what big_tempo's OSC
sync/mute_quantized toggle does on every JACK transport edge.

testdef_late_hit.py's existing testAfterSyncOff_NoCorrectionOnNextRecord
exercises a plain Sync-port toggle, but primes the counter to exactly 0
immediately beforehand, so a frozen-at-0 counter is indistinguishable
from a correctly-reset one there -- its assertion is deliberately
permissive (assertIn(state, [WaitStart, Recording])) and does not pin
down which one actually happens. This file closes that gap by building
the counter up to a known, substantial nonzero value BEFORE disabling
sync, so a frozen-stale value and a freshly-reset value make opposite,
unambiguous predictions.

See mod_docs/ROADMAP.rst entry 8 (eighth-note off-beat sync
investigation) for the live-incident context this test was written to
check.

Sync buffer injection values (same convention as testdef_late_hit.py):
  0.0  — no sync signal
  2.0  — sync pulse (> 1.5 threshold); sets bSyncPulseReceived and
         resets lGlobalSamplesSinceSync (while fSyncMode >= 1.0f)
"""

import time
import unittest
import test_engine
import engine_wrapper

_DEFAULT_WINDOW_MS = 100.0  # restore in tearDown


class StaleSyncCounterTestCase(unittest.TestCase):
    """Base class: buffer-precise timing helpers, mirroring
    testdef_late_hit.py's LateHitTestCase / testdef_mute_late_hit.py's
    MuteLateHitTestCase."""

    PULSE_BUFS   = 3   # how long to hold a sync pulse high
    WITHIN_BUFS  = 2   # no-pulse buffers -> T within the late-hit window
    BUILDUP_BUFS = 6   # no-pulse buffers run BEFORE disabling sync, so the
                        # frozen value ends up well outside the window
    STOPPED_BUFS = 8   # no-pulse buffers held with sync disabled, standing
                        # in for a real "transport stopped" gap

    def setUp(self):
        self.engine = engine_wrapper.Engine("SL_test_stalesync")
        self.looper = self.engine.looper
        self.buf    = int(self.looper.get_buffer_size())
        self.sr     = int(self.engine.get_sample_rate())

        # Window = WITHIN_BUFS+1 buffer-periods: WITHIN_BUFS is inside,
        # BUILDUP_BUFS is outside. Same ceiling technique as
        # testdef_late_hit.py / testdef_mute_late_hit.py.
        self._set_window(self.WITHIN_BUFS + 1)

        self.looper.set_port(test_engine.MuteQuantized, 1.0)

        # Same ordering caution as testdef_late_hit.py's setUp: arm sync
        # injection before raising fSyncMode, to avoid reading
        # uninitialised sync-input-buffer memory.
        self.looper.sync_inject_value   = 0.0
        self.looper.sync_inject_enabled = True
        self.looper.set_port(test_engine.Sync, 1.0)

    def tearDown(self):
        test_engine.cvar.g_late_hit_window_ms = _DEFAULT_WINDOW_MS
        test_engine.cvar.g_mute_late_hit_ms   = _DEFAULT_WINDOW_MS
        del self.engine

    # ------------------------------------------------------------------
    # helpers

    def _set_window(self, n_bufs):
        """Set both late-hit windows to exactly n_bufs JACK buffer-periods."""
        ms = (n_bufs * self.buf / self.sr) * 1000.0
        test_engine.cvar.g_late_hit_window_ms = ms
        test_engine.cvar.g_mute_late_hit_ms   = ms

    def wait_buffers(self, n, timeout=5.0):
        """Busy-wait until exactly n more JACK callbacks have completed."""
        target   = self.looper.process_count + n
        deadline = time.time() + timeout
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    f"Timed out waiting for {n} JACK callbacks "
                    f"(process_count stuck at {self.looper.process_count})")

    def prime_pulse(self):
        """Inject a sync pulse for PULSE_BUFS callbacks, then clear.
        On return (while fSyncMode >= 1.0): bSyncPulseReceived=True,
        lGlobalSamplesSinceSync=0."""
        self.looper.sync_inject_value = 2.0
        self.wait_buffers(self.PULSE_BUFS)
        self.looper.sync_inject_value = 0.0

    def assertState(self, state, msg=None):
        actual = self.engine.getState()
        self.assertEqual(
            actual,
            engine_wrapper.states[state],
            (engine_wrapper.states[actual] + " != " + state)
            if msg is None else msg
        )

    @property
    def window_samples(self):
        return (self.WITHIN_BUFS + 1) * self.buf


# ======================================================================
# Direct, mechanical proof of the freeze
# ======================================================================

class TestCounterFreezesWhileSyncDisabled(StaleSyncCounterTestCase):
    """Reads lGlobalSamplesSinceSync straight off the SWIG-exposed
    property -- independent of any button-press behaviour."""

    def testCounterFreezesAndStaleValueSurvivesReenable(self):
        self.prime_pulse()
        self.wait_buffers(self.BUILDUP_BUFS)
        stale_value = self.looper.get_global_samples_since_sync()
        self.assertGreater(
            stale_value, self.window_samples,
            "test setup error: buildup did not exceed the window -- "
            "increase BUILDUP_BUFS")

        # Disable sync -- the increment/reset loop is skipped entirely.
        # One buffer may already be "in flight" using the pre-disable
        # port snapshot (Python's set_port() has no synchronisation with
        # the JACK thread's callback boundary) -- absorb that expected
        # one-buffer race before taking the frozen baseline, so the
        # freeze check itself isn't flaky.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(1)
        frozen_value = self.looper.get_global_samples_since_sync()
        self.wait_buffers(self.STOPPED_BUFS - 1)
        self.assertEqual(
            self.looper.get_global_samples_since_sync(), frozen_value,
            "lGlobalSamplesSinceSync changed while fSyncMode==0 -- "
            "expected it to freeze")
        stale_value = frozen_value

        # Re-enable sync. No pulse fed yet. Same one-buffer race as the
        # disable step above -- absorb it before reading, otherwise this
        # can flakily observe the pre-toggle buffer still in flight.
        self.looper.set_port(test_engine.Sync, 1.0)
        self.wait_buffers(2)
        value_after_reenable = self.looper.get_global_samples_since_sync()

        # The actual hypothesis test: a correctly-reset implementation
        # would show a small value here (0 plus a buffer or two of real
        # elapsed time since re-enable); the current code is expected to
        # instead show the stale pre-disable value (plus a sample or
        # two), because nothing resets the counter at the fSyncMode
        # 0->1 transition itself -- only a real pulse does.
        self.assertLess(
            value_after_reenable, self.window_samples,
            f"BUG CONFIRMED: lGlobalSamplesSinceSync == {value_after_reenable} "
            f"immediately after re-enabling sync (stale value was "
            f"{stale_value}, window is {self.window_samples}) -- the stale "
            "pre-disable count survived the fSyncMode 0->1 transition "
            "instead of being reset.")


# ======================================================================
# Behavioural consequence: does the stale T change what Record does?
# ======================================================================

class TestStaleCounterAffectsRecordDecision(StaleSyncCounterTestCase):
    """NOTE: building T up PAST the window before disabling sync does
    NOT actually distinguish buggy from correct behaviour -- a stale,
    already-outside-window value and a genuinely-fresh, no-real-pulse-
    yet value both correctly predict "not within window" (WaitStart).
    This class documents that (safe) outcome as a regression baseline;
    see TestStaleSmallCounterCausesFalsePositiveLateHit below for the
    scenario that actually separates the two cases (a SMALL stale T
    surviving the toggle, which can falsely read as "within window")."""

    def testRecordImmediatelyAfterReenable_NoInterveningPulse(self):
        self.prime_pulse()
        self.wait_buffers(self.BUILDUP_BUFS)   # T built up past the window
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(self.STOPPED_BUFS)
        self.looper.set_port(test_engine.Sync, 1.0)
        # No pulse fed here. A large stale T (still past the window) and
        # a large fresh T would both correctly land here, so this does
        # NOT prove correctness -- it only documents that this
        # particular direction isn't the dangerous one.
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState(
            "WaitStart",
            "Unexpected: RECORD immediately after re-enable (large stale "
            "T, no pulse) entered something other than WaitStart -- "
            "investigate before drawing any conclusion from this test.")

    def testControl_RecordAfterFreshPulseFollowingReenable_Works(self):
        """Contrast case: feed one real pulse after re-enabling sync,
        before the button press. This must reset the counter via the
        normal pfSyncInput > 1.5 path, so Recording should fire
        immediately -- isolating the vulnerable window from an unrelated
        harness/timing quirk."""
        self.prime_pulse()
        self.wait_buffers(self.BUILDUP_BUFS)
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(self.STOPPED_BUFS)
        self.looper.set_port(test_engine.Sync, 1.0)
        self.prime_pulse()  # real pulse after re-enable -> proper reset
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState(
            "Recording",
            "Control failed: even with a fresh pulse after re-enabling "
            "sync, RECORD did not enter Recording within the window -- "
            "unrelated harness/timing issue, not the stale-counter bug.")


# ======================================================================
# The dangerous direction: a SMALL stale T causes a FALSE-POSITIVE
# late-hit misfire, not just an overly-cautious wait.
# ======================================================================

class TestStaleSmallCounterCausesFalsePositiveLateHit(StaleSyncCounterTestCase):
    """The class above builds T up PAST the window before disabling sync,
    so a stale value and a correctly-reset value both land outside the
    window -- "not within window" is the right answer either way, so a
    WaitStart result there doesn't actually distinguish buggy from
    correct behaviour.

    This class instead disables sync while T is still SMALL (inside the
    window) and holds it there through a long simulated stop. On
    re-enable with no real pulse, a correctly-behaving implementation
    has no way to know a real boundary is nearby -- no real pulse has
    fired since the stop began -- so it should NOT apply the late-hit
    correction (should go to WaitStart, deferring to the next real
    boundary). The current code instead sees a stale-but-small T,
    concludes (wrongly) "you're within the window", and fires the
    correction anyway -- a false positive that makes a content-affecting
    timing decision (where the loop's boundary "really" was) using a
    number with no relationship to real elapsed time. This is the
    direction that actually matches the live symptom (a loop plays
    immediately but lands on the wrong phase), not the merely-deferred
    WaitStart case above.
    """

    def testSmallStaleTMisfiresLateHitOnReenable(self):
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)   # T stays inside the window
        small_stale_value = self.looper.get_global_samples_since_sync()
        self.assertLess(
            small_stale_value, self.window_samples,
            "test setup error: T is not actually within the window -- "
            "decrease WITHIN_BUFS")

        # Absorb the expected one-buffer race (Python's set_port() has no
        # synchronisation with the JACK thread's callback boundary, so
        # one already-in-flight buffer may still see the pre-disable
        # value) before taking the frozen baseline -- see the identical
        # comment in TestCounterFreezesWhileSyncDisabled above.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(1)
        frozen_value = self.looper.get_global_samples_since_sync()
        self.wait_buffers(self.STOPPED_BUFS - 1)   # long simulated stop
        self.assertEqual(
            self.looper.get_global_samples_since_sync(), frozen_value,
            "lGlobalSamplesSinceSync changed while fSyncMode==0 -- "
            "expected it to freeze")

        self.looper.set_port(test_engine.Sync, 1.0)
        # No pulse fed here -- no real boundary has occurred since the
        # stop began, so nothing legitimately supports a late-hit fire.
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState(
            "WaitStart",
            "BUG CONFIRMED: RECORD pressed immediately after re-enabling "
            "sync entered Recording via a false-positive late-hit fire -- "
            "the small stale T carried over from before the (long) sync "
            "disable period was indistinguishable from a genuinely fresh, "
            "on-time T, even though no real sync pulse has occurred since "
            "sync was re-enabled. This makes a content-affecting timing "
            "decision using a number unrelated to real elapsed time.")

    def testControl_SameSmallWindow_NoToggle_CorrectionFiresNormally(self):
        """Contrast case: same small, within-window T, but sync is never
        disabled at all (no toggle). The late-hit correction firing here
        is correct and expected -- isolates the bug to the toggle itself,
        not to "small T + RECORD" in general."""
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("RECORD")
        self.wait_buffers(1)
        self.assertState(
            "Recording",
            "Control failed: with no sync toggle at all, a genuinely "
            "fresh within-window T did not fire the late-hit correction -- "
            "unrelated harness/timing issue, not the stale-counter bug.")


# ======================================================================
# Same false-positive misfire, via the Mute/Unmute late-hit call site
# (plugin.cc:3587, withinMuteWindow) instead of Record.
# ======================================================================

class TestStaleSmallCounterCausesFalsePositiveLateHit_Unmute(StaleSyncCounterTestCase):
    """Mirrors TestStaleSmallCounterCausesFalsePositiveLateHit above, but
    through the mute/unmute late-hit consumer of the same
    lGlobalSamplesSinceSync counter (added later, 0d8787c, on top of the
    already-vulnerable counter from 692fb98) rather than Record's. Setup
    borrowed from testdef_mute_late_hit.py's MuteLateHitTestCase
    (record_a_loop / _mute_it) since reaching Muted is a prerequisite
    that TestLooper's raw state machine doesn't reach from STATE_OFF."""

    def record_a_loop(self, level=0.5):
        """Record a short take with sync off (so the record itself isn't
        subject to any late-hit correction), leaving it Playing."""
        self.looper.set_port(test_engine.Sync, 0.0)
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.wait_buffers(15)
        self.engine.request("RECORD")
        self.wait_buffers(5)
        self.assertState("Playing", "record_a_loop: could not enter Playing")
        self.looper.set_port(test_engine.Sync, 1.0)

    def _mute_it(self):
        self.record_a_loop()
        self.looper.set_port(test_engine.Sync, 0.0)
        self.engine.request("MUTE_ON")
        self.wait_buffers(5)
        self.assertState("Muted", "_mute_it: could not enter Muted")
        self.looper.set_port(test_engine.Sync, 1.0)

    def testSmallStaleTMisfiresMuteLateHitOnReenable(self):
        self._mute_it()

        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)   # T stays inside the window
        small_stale_value = self.looper.get_global_samples_since_sync()
        self.assertLess(
            small_stale_value, self.window_samples,
            "test setup error: T is not actually within the window -- "
            "decrease WITHIN_BUFS")

        # Same one-buffer-race absorption as the Record version above.
        self.looper.set_port(test_engine.Sync, 0.0)
        self.wait_buffers(1)
        frozen_value = self.looper.get_global_samples_since_sync()
        self.wait_buffers(self.STOPPED_BUFS - 1)   # long simulated stop
        self.assertEqual(
            self.looper.get_global_samples_since_sync(), frozen_value,
            "lGlobalSamplesSinceSync changed while fSyncMode==0 -- "
            "expected it to freeze")

        self.looper.set_port(test_engine.Sync, 1.0)
        # No pulse fed here -- no real boundary has occurred since the
        # stop began, so nothing legitimately supports a late-hit fire.
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState(
            "Muted",
            "BUG CONFIRMED (mute path): MUTE_OFF pressed immediately "
            "after re-enabling sync entered Playing via a false-positive "
            "late-hit fire -- the small stale T carried over from before "
            "the (long) sync disable period was indistinguishable from a "
            "genuinely fresh, on-time T, even though no real sync pulse "
            "has occurred since sync was re-enabled.")

    def testControl_SameSmallWindow_NoToggle_MuteCorrectionFiresNormally(self):
        """Contrast case: same small, within-window T, but sync is never
        disabled. Correct and expected for the correction to fire here --
        isolates the bug to the toggle itself. (This is effectively
        testdef_mute_late_hit.py's own testUnmuteWithinWindow_IsImmediate,
        repeated here so this file's mute-path evidence is self-contained.)"""
        self._mute_it()
        self.prime_pulse()
        self.wait_buffers(self.WITHIN_BUFS)
        self.engine.request("MUTE_OFF")
        self.wait_buffers(1)
        self.assertState(
            "Playing",
            "Control failed: with no sync toggle at all, a genuinely "
            "fresh within-window T did not fire the mute late-hit "
            "correction -- unrelated harness/timing issue, not the "
            "stale-counter bug.")


if __name__ == "__main__":
    unittest.main()
