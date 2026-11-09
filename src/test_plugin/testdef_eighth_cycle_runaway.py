"""
Characterization tests for the ``_eighth_cycle`` update rule in
``Engine::set_tempo`` (../engine.cpp).

Unlike the other testdef_* files these need neither JACK nor the compiled
``_test_engine.so``: the rule under test lives in engine.cpp, which the SWIG
harness does not wrap (it wraps plugin.cc).  Rather than reimplement the rule
and prove nothing, the structural tests below assert the shape of the real
source text, so they fail if engine.cpp is edited.  The simulation tests then
show what that shape implies over a realistic pedal ramp.

Background
----------
``set_tempo(tempo, rt)`` adjusts eighths-per-cycle when the tempo leaves the
60..240 BPM band::

    if (_tempo > 240.0) {
        _eighth_cycle *= 0.5;
        _eighth_cycle = max(1.0f, _eighth_cycle);   // clamped
    }
    else if (_tempo < 60.0) {
        _eighth_cycle *= 2;
        _eighth_cycle = min(128.0f, _eighth_cycle); // clamped -- FIXED 2026-08-28
    }

Before the fix, the two branches were asymmetric: the halving path had a
floor of 1.0, the doubling path had no ceiling.  ``set_tempo(info.bpm, true)``
is called from the JACK process callback (engine.cpp, JackSync branch of
generate_sync) every time transport BPM changes while rolling, so a downward
tempo ramp that crosses 60 BPM doubled ``_eighth_cycle`` once per step, with
nothing bringing it back down except tempo going *above 240* -- an inflated
value latched for the life of the process.  ``min(128.0f, _eighth_cycle)``
now bounds that growth, mirroring the existing floor clamp's style. 128 is an
arbitrary but generous ceiling (8x the 16.0f default); no existing max was
defined for this port to mirror exactly.

**Confirmed present in stock SooperLooper 1.7.9** (``src/original_engine.cpp``,
byte-for-byte identical guard/branch shape before this fix) -- this is not an
ALB-mod-introduced defect, and stock also defaults ``_smart_eighths = true``.
The ceiling clamp is the only change made here; the deeper issue that the
guard tests *being* under 60 BPM rather than the *direction* of travel (so
both ramp directions inflate it -- see ``test_ramping_back_up_...`` below) is
left alone, per the same stock-code reasoning: not an ALB regression, so out
of scope unless it starts mattering.

Reachability on the live rig (checked 2026-08-28)
-------------------------------------------------
**Currently unreachable, and known not to be the cause of any observed
failure.**  ``SL_session_save_256.slsess`` sets ``smart_eighths="0"``, and
that flag is the first term of the guard, so the whole block is skipped.

Confirmed empirically rather than by reading: with the rig running, the tempo
was ramped 173 -> 20 -> 190 BPM (crossing 60 in both directions) while polling
the ``eighth_per_cycle`` global over OSC.  It stayed at 2 throughout.

These tests are therefore a guard on a defect that is *latent* on this rig
(smart eighths is off): they document what would happen if it were ever
enabled, and now that the ceiling clamp is in place, that the growth stays
bounded. They are not evidence about the 2026-08-28 02:26 xrun storm, which
remains unexplained.
"""

import os
import re

import pytest

ENGINE_CPP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "engine.cpp")

# Defaults taken from engine.cpp: _eighth_cycle = 16.0f, _smart_eighths = true.
INITIAL_EIGHTH_CYCLE = 16.0

# Matches the min(128.0f, ...) clamp added to the doubling branch.
EIGHTH_CYCLE_CEILING = 128.0

# Tempo bounds from a live rig's Big Tempo config.
MIN_BPM = 20.0
STEP_BPM = 5.0


# ---------------------------------------------------------------------------
# Source-structure tests -- these assert against the real engine.cpp.
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def smart_eighths_block():
    """The body of the `_smart_eighths` conditional inside Engine::set_tempo."""
    with open(ENGINE_CPP, "r") as f:
        src = f.read()

    start = src.index("Engine::set_tempo")
    guard = src.index("_smart_eighths &&", start)
    # Far enough to cover both branches (including the albBegin/albEnd
    # comment around the ceiling clamp) plus a little of what follows.
    return src[guard:guard + 1400]


def test_set_tempo_is_reachable_from_the_rt_callback():
    """The RT caller that makes this rule fire on every transport BPM change."""
    with open(ENGINE_CPP, "r") as f:
        src = f.read()
    assert "set_tempo(info.bpm, true)" in src, (
        "expected the JackSync branch of generate_sync() to call set_tempo() "
        "with rt=true; the runaway below is only reachable through it"
    )


def test_halving_branch_is_clamped(smart_eighths_block):
    """The >240 BPM branch has a floor, so it cannot run away downward."""
    assert "_eighth_cycle *= 0.5;" in smart_eighths_block
    assert re.search(r"_eighth_cycle\s*=\s*max\s*\(\s*1\.0f\s*,\s*_eighth_cycle\s*\)",
                     smart_eighths_block), (
        "the halving branch is expected to clamp with max(1.0f, _eighth_cycle)"
    )


def test_doubling_branch_is_now_clamped(smart_eighths_block):
    """The <60 BPM branch now has a ceiling, mirroring the halving branch's floor.

    Fixed 2026-08-28 (was previously unclamped -- see git history / mod_docs
    for the prior version of this test).  If this assertion starts failing,
    the clamp has been removed or changed; update this module and reopen the
    runaway defect it exists to guard against.
    """
    # Note: "_tempo < 60.0" also appears in the guard expression above, so
    # anchor on the branch itself rather than the first occurrence.
    doubling = smart_eighths_block[smart_eighths_block.index("else if (_tempo < 60.0)"):]
    assert "_eighth_cycle *= 2;" in doubling

    branch_body = doubling[:doubling.index("}")]
    assert re.search(r"_eighth_cycle\s*=\s*min\s*\(\s*128\.0f\s*,\s*_eighth_cycle\s*\)",
                     branch_body), (
        "expected the doubling branch to clamp with min(128.0f, _eighth_cycle):\n"
        + branch_body
    )


def test_only_tempo_above_240_can_shrink_eighth_cycle(smart_eighths_block):
    """There is no recovery path in the normal 60..240 BPM range.

    The ceiling clamp bounds growth but is not a recovery path: it never
    lowers _eighth_cycle, it only stops it climbing past 128.
    """
    guard = smart_eighths_block[:smart_eighths_block.index("{")]
    assert "_tempo > 240.0" in guard and "_tempo < 60.0" in guard, (
        "the whole block is skipped inside 60..240 BPM, so an inflated "
        "_eighth_cycle cannot recover by returning to a normal tempo"
    )


# ---------------------------------------------------------------------------
# Behavioural model -- what the structure above implies.
# ---------------------------------------------------------------------------

def apply_set_tempo(eighth_cycle, tempo, smart_eighths=True):
    """Model of the _eighth_cycle update in Engine::set_tempo.

    Kept deliberately literal so it can be diffed against the quoted source
    in this module's docstring.
    """
    if smart_eighths and tempo > 1.0 and (tempo > 240.0 or tempo < 60.0):
        if tempo > 240.0:
            eighth_cycle *= 0.5
            eighth_cycle = max(1.0, eighth_cycle)
        elif tempo < 60.0:
            eighth_cycle *= 2
            eighth_cycle = min(EIGHTH_CYCLE_CEILING, eighth_cycle)
    return eighth_cycle


def ramp_down(start_bpm, eighth_cycle=INITIAL_EIGHTH_CYCLE):
    """Walk BPM down in STEP_BPM decrements to MIN_BPM, as the pedal does."""
    history = []
    bpm = start_bpm
    while bpm > MIN_BPM:
        bpm = max(MIN_BPM, bpm - STEP_BPM)
        eighth_cycle = apply_set_tempo(eighth_cycle, bpm)
        history.append((bpm, eighth_cycle))
    return eighth_cycle, history


def test_ramp_above_60_is_harmless():
    """The observed part of the 2026-08-28 log: 121 -> 91 BPM changes nothing."""
    eighth_cycle = INITIAL_EIGHTH_CYCLE
    for bpm in (116.0, 111.0, 106.0, 101.0, 96.0, 91.0):
        eighth_cycle = apply_set_tempo(eighth_cycle, bpm)
    assert eighth_cycle == INITIAL_EIGHTH_CYCLE


def test_ramp_below_60_doubles_then_hits_ceiling():
    """Each 5 BPM step under 60 doubles _eighth_cycle, until the ceiling caps it."""
    eighth_cycle = INITIAL_EIGHTH_CYCLE
    seen = []
    for bpm in (56.0, 51.0, 46.0, 41.0):
        eighth_cycle = apply_set_tempo(eighth_cycle, bpm)
        seen.append(eighth_cycle)
    # Pre-fix this was [32.0, 64.0, 128.0, 256.0]; the last step now clamps.
    assert seen == [32.0, 64.0, 128.0, 128.0]


def test_full_pedal_ramp_to_min_bpm_is_bounded():
    """Holding the 'down' pedal from a normal tempo to min_bpm.

    Eight steps land under 60 BPM (55, 50, 45, 40, 35, 30, 25, 20).  Pre-fix
    this ran 16 -> 4096 (256x inflation from one pedal hold); the ceiling
    clamp now stops it at 128 after the third step under 60.
    """
    final, history = ramp_down(start_bpm=120.0)

    below_60 = [(bpm, ec) for bpm, ec in history if bpm < 60.0]
    assert len(below_60) == 8, [b for b, _ in history]
    assert final == EIGHTH_CYCLE_CEILING


def test_ramping_back_up_does_not_exceed_the_ceiling():
    """The rule is still direction-independent -- being under 60 still re-fires
    the doubling branch on the way back up, not just on the way down (see the
    module docstring: this part of the defect is left alone, as stock code).
    But the ceiling clamp now holds regardless: once at 128, further doubling
    attempts (128 * 2 = 256) are clamped straight back down to 128, so
    reversing the pedal can no longer make things worse than the ceiling.
    """
    inflated, _ = ramp_down(start_bpm=120.0)
    assert inflated == EIGHTH_CYCLE_CEILING

    eighth_cycle = inflated
    bpm = MIN_BPM
    steps_below_60 = 0
    while bpm < 240.0:
        bpm = min(240.0, bpm + STEP_BPM)
        if bpm < 60.0:
            steps_below_60 += 1
        eighth_cycle = apply_set_tempo(eighth_cycle, bpm)

    assert steps_below_60 == 7
    assert eighth_cycle == EIGHTH_CYCLE_CEILING


def test_nothing_in_the_normal_range_restores_eighth_cycle():
    """Inside 60..240 BPM the block is skipped entirely, so a clamped-high
    value latches at the ceiling rather than recovering."""
    eighth_cycle = EIGHTH_CYCLE_CEILING
    for bpm in (60.0, 75.0, 90.0, 120.0, 150.0, 180.0, 210.0, 240.0):
        eighth_cycle = apply_set_tempo(eighth_cycle, bpm)
    assert eighth_cycle == EIGHTH_CYCLE_CEILING, (
        "only tempo strictly above 240 BPM lowers _eighth_cycle, so a "
        "ceiling-clamped value survives any normal playing tempo"
    )


def test_repeated_ramps_no_longer_compound():
    """Pre-fix, the defect was cumulative across pedal gestures within one
    session (16 -> 2**24 over three ramps).  The ceiling clamp now holds
    across repeated gestures too: once at 128, further ramps cannot push it
    any higher.
    """
    eighth_cycle = INITIAL_EIGHTH_CYCLE
    for _ in range(3):
        eighth_cycle, _ = ramp_down(start_bpm=120.0, eighth_cycle=eighth_cycle)
    assert eighth_cycle == EIGHTH_CYCLE_CEILING


def test_eighth_cycle_can_no_longer_overflow_float():
    """_eighth_cycle is a float (engine.hpp).  Pre-fix, enough ramps overflowed
    it to infinity within 16 pedal holds:

        Downstream, calculate_tempo_frames() computes
        ``_tempo_frames = samplerate * (1/_tempo) * 30.0 * _eighth_cycle``
        under QUANT_CYCLE, and calculate_midi_tick() computes
        ``_midi_loop_tick = (unsigned int)(_eighth_cycle * 12)`` -- a cast
        that is undefined once the value exceeds UINT_MAX.

    With the ceiling clamp in place this can no longer happen: many, many
    more ramps than any real session would apply still leave it exactly at
    the ceiling.
    """
    import struct

    def to_float32(x):
        return struct.unpack("f", struct.pack("f", x))[0]

    eighth_cycle = INITIAL_EIGHTH_CYCLE
    for _ in range(1000):
        eighth_cycle, _ = ramp_down(start_bpm=120.0, eighth_cycle=eighth_cycle)
        eighth_cycle = to_float32(eighth_cycle)

    assert eighth_cycle == EIGHTH_CYCLE_CEILING
