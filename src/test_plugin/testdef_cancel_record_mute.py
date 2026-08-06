"""Regression tests for CANCEL_RECORD_MUTE ("cancel_record_mute").

Cancels an in-progress or pending record and keeps the prior take, muting
only if audio was actually captured.  No-op in every other state -- which
is what makes it safe as the long-press partner of an eager-single record
button.  Design and rationale: mod_docs/ROADMAP.rst entry 1.

Scope: these run with sync and quantize off (the harness default), so they
cover the STATE_RECORD branch and the no-op branches.  The STATE_TRIG_START
(pending record) and STATE_TRIG_STOP (pending record end) branches need
sync injection and belong with the machinery in testdef_late_hit.py.
"""
import time
import unittest

import testdef_simple_state
import engine_wrapper
import test_engine


CMD = "CANCEL_RECORD_MUTE"

# Distinct constants for the audio-content tests below.
TAKE_LEVEL  =  0.5    # the take we want back
NOISE_LEVEL = -0.9    # what the input is doing while muted


class commandOrdinalContract(unittest.TestCase):
    """The number sent to request_cmd() is the Event::command_t ordinal.

    plugin.cc separately hardcodes MULTI_UNDO..MULTI_PAUSE_OFF as integer
    literals 0-23 mirroring the first 24 enum values.  Insert an enum in
    that range and the literals silently start naming different commands,
    with no compiler diagnostic.  This asserts the Python side of that
    contract; plugin.cc carries matching static_asserts for the C++ side.
    """

    def testPositionalTupleMatchesEnum(self):
        for i, name in enumerate(engine_wrapper.commands):
            self.assertEqual(
                engine_wrapper.command_ordinals[name], i,
                "%s is ordinal %d in event.hpp but index %d in "
                "engine_wrapper.commands -- the positional command tuple "
                "has drifted from the enum"
                % (name, engine_wrapper.command_ordinals[name], i))

    def testCommandIsRegistered(self):
        self.assertIn(CMD, engine_wrapper.command_ordinals)


class cancelRecordMute(testdef_simple_state.simpleStateTest):

    def testNoOpFromOff(self):
        # Nothing recording, nothing pending: must do nothing whatsoever.
        self.engine.request(CMD)
        time.sleep(0.001)
        self.assertState("Off")

    def testCancelsFirstRecord(self):
        # First loop, so there is no previous take and the undo falls
        # through to the UNDO_ALL path.  Forcing wasMuted makes it land
        # muted -- compare testdef_simple_state_off.testUndo, where a plain
        # UNDO from the same point gives "Off".
        self.engine.request("RECORD")
        self.engine.request(CMD)
        time.sleep(0.001)
        self.assertState("OffMuted")

    def testNoOpWhilePlaying(self):
        # RECORD twice completes a take and leaves STATE_PLAY.  This is the
        # "pressed to end the record, then lingered" accident: the take is
        # being kept and must not be touched.
        self.engine.request("RECORD")
        self.engine.request("RECORD")
        time.sleep(0.001)
        self.assertState("Playing")
        self.engine.request(CMD)
        time.sleep(0.001)
        self.assertState("Playing")

    def testCancelsRecordOverExistingTake(self):
        # A take exists, a second record is started over it, then cancelled:
        # the prior take is restored and left muted.
        self.engine.request("RECORD")
        self.engine.request("RECORD")
        self.engine.request("RECORD")
        time.sleep(0.001)
        self.assertState("Recording")
        self.engine.request(CMD)
        time.sleep(0.001)
        self.assertState("Muted")


class _AudioHelpers(object):
    """Shared helpers for tests that assert on audio content, not just state.

    Plain object, not a TestCase, so pytest does not collect it.
    """

    def initAudio(self):
        self.looper = self.engine.looper
        self.looper.set_port(test_engine.DryLevel, 0.0)   # output = pure loop
        self.looper.set_port(test_engine.WetLevel, 1.0)

    def waitBuffers(self, n, timeout=5.0):
        """Busy-wait until n more JACK callbacks have completed."""
        target = self.looper.process_count + n
        deadline = time.time() + timeout
        while self.looper.process_count < target:
            if time.time() > deadline:
                raise RuntimeError(
                    "Timed out waiting for %d JACK callbacks "
                    "(process_count stuck at %d)"
                    % (n, self.looper.process_count))
            time.sleep(0.001)

    def outputRange(self, blocks=120):
        """min/max of the output over `blocks` callbacks.

        Accumulated over many blocks because one block covers only a
        fraction of a loop period, so a single block can easily miss a
        contaminated region.
        """
        self.looper.reset_output_stats()
        self.waitBuffers(blocks)
        return self.looper.output_min, self.looper.output_max

    def recordTake(self, level):
        """Record a complete take of a constant `level`, leaving it playing."""
        self.looper.input_inject_value = level
        self.engine.request("RECORD")
        self.waitBuffers(30)
        self.engine.request("RECORD")
        self.waitBuffers(10)
        self.assertState("Playing")


class cancelRecordMuteAudio(_AudioHelpers, testdef_simple_state.simpleStateTest):
    """Audio-content tests with sync off (the unquantized branches)."""

    def setUp(self):
        testdef_simple_state.simpleStateTest.setUp(self)
        self.initAudio()

    def testInjectionAndReadback(self):
        # Sanity check on the harness support itself: a take recorded from a
        # constant input must play back as that constant.
        self.recordTake(TAKE_LEVEL)
        mn, mx = self.outputRange()
        self.assertAlmostEqual(mn, TAKE_LEVEL, places=3)
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)

    def testResidueAfterCancelOverExistingTake(self):
        # Regression test for the loop/feed fade residue.
        #
        # MULTI_UNDO's STATE_MUTE branch only sets fPlayFadeDelta; it assumes
        # the caller was already muted with the loop write gain at zero.
        # Arriving there straight out of STATE_RECORD leaves fLoopFadeAtten at
        # 1.0, and plugin.cc:5262 -- which runs in the play block whose case
        # group includes STATE_MUTE -- then evaluates
        #     loop = loop * fFeedFadeAtten + fLoopFadeAtten * input
        # every sample.  Both attenuations at 1.0 makes that loop += input: an
        # accumulator with no decay, so the restored take runs away into gross
        # clipping while muted and is only discovered on unmute.  Measured at
        # -10.3 with an input of -0.9 before the fix.
        #
        # The contaminating input while muted is what exposes it; with a
        # silent input the bug is invisible.
        self.recordTake(TAKE_LEVEL)

        self.engine.request("RECORD")            # second take over the first
        self.waitBuffers(10)
        self.assertState("Recording")

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request(CMD)
        self.waitBuffers(5)
        self.assertState("Muted")

        # Muted output must be silent...
        mn, mx = self.outputRange(40)
        self.assertAlmostEqual(mn, 0.0, places=3)
        self.assertAlmostEqual(mx, 0.0, places=3)

        # ...and the loop must not be absorbing the input behind that silence.
        self.waitBuffers(200)
        self.engine.request("MUTE_OFF")
        self.waitBuffers(10)

        mn, mx = self.outputRange()
        self.assertAlmostEqual(
            mn, TAKE_LEVEL, places=3,
            msg="loop content drifted to %.4f..%.4f; expected the take at "
                "%.2f. Input was %.2f while muted, so the loop absorbed it."
                % (mn, mx, TAKE_LEVEL, NOISE_LEVEL))
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)

    def testCancelUndoesOnlyOneLevel(self):
        # The cancel must discard *only* the recording it cancelled -- it is
        # a single undo, not an undo_all.  Build two levels (a take, then an
        # overdub on top), start a third record, cancel it, and check the
        # overdub is what comes back.
        #
        # Note: when the cancelled record is the *first* loop, with nothing
        # before it, MULTI_UNDO does route through STATE_UNDO_ALL -- but only
        # one chunk exists then, so it still cannot reach past one level.
        # undo_all in mechanism, never in effect.
        self.recordTake(TAKE_LEVEL)                     # level 1
        mn, mx = self.outputRange(60)
        self.assertAlmostEqual(mn, TAKE_LEVEL, places=3)

        overdub = 0.2
        self.looper.input_inject_value = overdub        # level 2, sums on top
        self.engine.request("OVERDUB")
        self.waitBuffers(60)
        self.engine.request("OVERDUB")
        self.waitBuffers(10)
        self.assertState("Playing")
        od_min, od_max = self.outputRange(60)
        self.assertGreater(od_min, TAKE_LEVEL + 0.05,
                           "overdub should have raised the level above the "
                           "bare take, got %.3f" % od_min)

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request("RECORD")                   # third record
        self.waitBuffers(10)
        self.assertState("Recording")
        self.engine.request(CMD)
        self.waitBuffers(5)
        self.assertState("Muted")
        self.engine.request("MUTE_OFF")
        self.waitBuffers(10)

        mn, mx = self.outputRange()
        self.assertAlmostEqual(
            mn, od_min, places=2,
            msg="expected the overdubbed loop (%.3f..%.3f) back; got "
                "%.3f..%.3f. Lower than the bare take (%.2f) means it undid "
                "too far." % (od_min, od_max, mn, mx, TAKE_LEVEL))
        self.assertAlmostEqual(mx, od_max, places=2)

    def testPlainUndoStillUnmutedAndIntact(self):
        # Side-effect guard.  CANCEL_RECORD_MUTE forces wasMuted and sets the
        # loop/feed fade deltas; all of that lives in its own remap branch and
        # must never leak into an ordinary UNDO.  If it did, this plain undo
        # would land Muted instead of Playing.
        self.recordTake(TAKE_LEVEL)
        self.engine.request("RECORD")            # second take over the first
        self.waitBuffers(10)
        self.assertState("Recording")
        self.engine.request("UNDO")
        self.waitBuffers(10)
        self.assertState("Playing")              # NOT Muted
        mn, mx = self.outputRange()
        self.assertAlmostEqual(mn, TAKE_LEVEL, places=3)
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)


class cancelRecordMuteQuantized(_AudioHelpers,
                                testdef_simple_state.simpleStateTest):
    """The quantized branches: STATE_TRIG_START and STATE_TRIG_STOP.

    With sync on and no pulse injected, a record request parks in WaitStart
    (or WaitStop) indefinitely, so the pending states can be held and probed
    deterministically rather than raced against a boundary.

    Sync injection must be armed *before* raising the Sync port -- see the
    ordering note in testdef_late_hit.py; the sync buffer is uninitialised
    until the fill loop first runs.
    """

    _DEFAULT_WINDOW_MS = 100.0

    def setUp(self):
        testdef_simple_state.simpleStateTest.setUp(self)
        self.initAudio()

    def tearDown(self):
        test_engine.cvar.g_late_hit_window_ms = self._DEFAULT_WINDOW_MS
        testdef_simple_state.simpleStateTest.tearDown(self)

    def syncOn(self):
        self.looper.sync_inject_value   = 0.0    # no signal yet
        self.looper.sync_inject_enabled = True   # fill buffer before Sync=1
        self.looper.set_port(test_engine.Sync, 1.0)
        self.waitBuffers(3)

    def pulse(self, n=3):
        self.looper.sync_inject_value = 2.0      # > 1.5 threshold
        self.waitBuffers(n)
        self.looper.sync_inject_value = 0.0

    def testPendingCancelFromOff(self):
        # No prior take: cancelling a pending record just returns to Off.
        self.syncOn()
        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("WaitStart")
        self.engine.request(CMD)
        self.waitBuffers(3)
        self.assertState("Off")

    def testPendingCancelKeepsExistingTake(self):
        # Decision 1(b), and the case that matters most on the live rig,
        # where sync is always on: a take is playing, a second record is
        # requested and parks in WaitStart, and the long press lands before
        # the boundary.  Nothing was captured, so the take must simply keep
        # playing -- unmuted and unmodified.
        #
        # Regression: routing this to MULTI_UNDO (as the first
        # implementation did) destroyed the take.  No chunk is pushed on the
        # pending path -- pushNewLoopChunk lives in the late-hit branch
        # (~2697), while the TRIG_START fallback (~2808) only sets state --
        # so headLoopChunk is the existing take with prev == NULL, and the
        # undo popped it straight through to UNDO_ALL.  Measured: Off, with
        # an empty loop.
        self.recordTake(TAKE_LEVEL)
        self.syncOn()

        self.engine.request("RECORD")
        self.waitBuffers(2)
        self.assertState("WaitStart")

        self.looper.input_inject_value = NOISE_LEVEL
        self.engine.request(CMD)
        self.waitBuffers(5)
        self.assertState("Playing")              # not Off, not Muted

        self.waitBuffers(150)
        mn, mx = self.outputRange()
        self.assertAlmostEqual(
            mn, TAKE_LEVEL, places=3,
            msg="take should have been untouched, got %.4f..%.4f" % (mn, mx))
        self.assertAlmostEqual(mx, TAKE_LEVEL, places=3)

    def testPendingStopIsNotCancelled(self):
        # The accident the design must tolerate: pressed to *end* a record,
        # then lingered.  With sync on the end parks in WaitStop, which must
        # be a no-op -- the take is being kept.  Late-hit is disabled so the
        # stop takes the plain TRIG_STOP path rather than completing early.
        test_engine.cvar.g_late_hit_window_ms = 0.0
        self.syncOn()

        self.pulse()
        self.engine.request("RECORD")
        self.pulse()                              # boundary: recording starts
        self.waitBuffers(10)
        self.assertState("Recording")

        self.engine.request("RECORD")             # ask to end
        self.waitBuffers(2)
        self.assertState("WaitStop")

        self.engine.request(CMD)                  # must do nothing at all
        self.waitBuffers(3)
        self.assertState("WaitStop")

        self.pulse()                              # boundary: take completes
        self.waitBuffers(10)
        self.assertState("Playing")
