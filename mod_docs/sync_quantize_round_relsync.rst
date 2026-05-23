Sync, Quantize, Round, Rel Sync, Multiply, and Eighths/Cycle
=============================================================

These settings work together to control how SooperLooper aligns loops to a
rhythmic grid and how it builds up longer loops from shorter ones. There are
two separate "sync" concepts that are easily confused, so we describe them
first before covering the other settings.

The running examples assume a JACK-connected sync source (such as Hydrogen)
at 120 BPM with Eighths/Cycle set to 8 (one standard 4/4 bar), unless noted.

----

Two separate "sync" concepts
-----------------------------

SooperLooper has two independent controls that both use the word "sync":

**1. "Sync To" (sync source selector)**
This drop-down selects where the timing reference comes from. The options are:

- *None / Internal Tempo* — SooperLooper uses its own internal tempo clock.
  The tap button lets you set the tempo by feel.
- *MIDI Clock* — tempo and beat pulses come from incoming MIDI clock messages.
- *JACK Transport* — tempo is provided by the JACK time master (a DAW,
  Hydrogen, or any other transport master).
- *Loop N* — any existing loop can be the sync source. The tempo is derived
  from that loop's length divided by Eighths/Cycle, so Eighths/Cycle must be
  set correctly for this to work.

The sync source must be active for any of the sync-dependent features below
to do anything.

**2. Per-loop "Sync" checkbox**
Each loop has its own Sync checkbox. When checked, most operations on that
loop — Record, Multiply, Replace, Substitute, Insert, Reverse, Trigger, and
Once — wait for the next pulse from the sync source before taking effect.
With Sync unchecked, operations take effect at the exact sample the command
arrives, regardless of the sync source.

These are independent: you can have a sync source configured without the Sync
checkbox enabled on any loop, and vice versa. Neither does anything useful
without the other.

----

Eighths/Cycle
-------------

**What it is**

Eighths/Cycle tells SooperLooper the phrase length you are working with,
expressed as a count of eighth notes per cycle. For common time (4/4), one
bar contains eight eighth notes: use 8. For 3/4 waltz time, use 6. For a
two-bar phrase, use 16.

**What it affects**

*Sync calculations.* When using Rel Sync, Quantize 8th, or Quantize Cycle, the
engine converts Eighths/Cycle and the current tempo into a ``syncSamples``
value: the half-period used for boundary detection, Play Sync, and Rel Sync's
phase-offset correction. The formula is:
``syncSamples = (sampleRate * 30 / tempo) * eighthsPerCycle / 2``.

*Loop-as-sync-source.* When another loop is the sync source, there is no
external BPM reference. The tempo is derived from the loop's length divided
by Eighths/Cycle. Setting this correctly is essential in that mode.

**What it does NOT affect**

The Multiply operation counts cycles in units of the originally recorded loop
length (``lCycleLength``), not in units of Eighths/Cycle. If you record a
two-bar loop and Multiply it, each pass adds one two-bar unit regardless of
this setting.

----

Quantize
--------

**What it does**

The Quantize button selects the snap resolution. There are four modes:

- **Off** — no snap-grid subdivisions for internal sync or Rel Sync.
  With Sync also off, presses take effect immediately. With Sync on and an
  external pulse source active, the late-hit correction window still fires —
  the snap point is the external pulse itself, which is independent of
  Quantize mode. Quantize Off does disable Play Sync and the Rel Sync
  phase-offset half-period correction.

- **Cycle** — the snap boundary is one full cycle, where a cycle is one
  repetition of the original loop length (``lCycleLength``). For a freshly
  recorded loop, cycle and loop length are identical. After Multiply, the
  cycle is the original pre-multiply length. ``syncSamples`` is set to half
  the cycle period in samples.

- **8th** — the snap boundary is one eighth note. ``syncSamples`` is set to
  half an eighth-note period in samples. **Note:** the physical Quantize
  button in the GUI only toggles between Off and Cycle. The 8th mode is
  accessible only via OSC or MIDI.

- **Loop** — operations snap to the end of the complete loop, including all
  cycles after a Multiply. Where Cycle snaps at the end of each one-bar unit
  within a four-bar loop, Loop only snaps at the end of all four bars.
  ``syncSamples`` is calculated identically to Cycle mode. Also only
  accessible via OSC or MIDI.

**What Quantize does and does not control**

It is easy to confuse the roles of Sync and Quantize:

- The per-loop Sync checkbox determines *whether* operations snap at all —
  by making them wait for the next sync pulse.
- Quantize determines the *subdivision* used within a sync period: Cycle,
  8th-note, or full Loop boundaries. This matters for Rel Sync (see below)
  and Play Sync, where the engine needs to know the expected pulse interval
  to compute phase offsets correctly.

For absolute sync (Sync checkbox on, Rel Sync off), the snap always fires
on the next incoming pulse from the sync source — Quantize mode does not
change *when* that snap occurs. The snap interval is entirely defined by the
rate at which your sync source fires. Quantize matters here for one thing:
setting ``syncSamples`` for Play Sync and slave loop boundary output. The
late-hit correction window fires based solely on ``fSyncMode > 0`` and the
elapsed time since the last pulse — Quantize Off does not disable it.

**The late-hit correction window (this patch)**

When an external sync source is active (``fSyncMode > 0``) and the Sync
checkbox is on, pressing Record or Stop within a configurable window
(default 100ms, set via ``late_hit_window_ms`` in the session file) *after*
a sync pulse is automatically corrected back to that
pulse. Without this correction, any press arriving after the pulse is treated
as belonging to the next cycle — one full pulse interval later — regardless
of how slightly late it was.

The boundary being corrected back to is the last sync pulse, tracked by the
``lGlobalSamplesSinceSync`` counter which counts samples elapsed since the
most recent pulse in all states. The counter increments in all engine
states and resets on every incoming pulse, so it always reflects the true
elapsed time regardless of Quantize mode.

**Musical meaning**

With Sync on, on-time presses snap to the external pulse identically
whether Quantize is on or off. The late-hit correction window is also
active in both cases when an external source is present — an 80ms-late
press is corrected back to the barline regardless of Quantize setting.
What Quantize Off does disable is Play Sync drift correction and the Rel
Sync half-period phase offset correction, both of which require a non-zero
``syncSamples`` value.

----

Sync checkbox + Quantize — the full picture
--------------------------------------------

These two settings combine as follows:

.. list-table::
   :header-rows: 1

   * - Sync checkbox
     - Quantize
     - On-time press behaviour
     - Late press behaviour
   * - Off
     - Off
     - Takes effect immediately
     - Takes effect immediately
   * - Off
     - Cycle/8th/Loop
     - Takes effect immediately (Quantize has no effect without Sync)
     - Takes effect immediately
   * - On
     - Off
     - Snaps to next sync pulse
     - Corrected back if within window (external pulse is the snap point)
   * - On
     - Cycle/8th/Loop
     - Snaps to next sync pulse
     - Corrected back to current pulse if within window

----

Multiply
--------

**What it does — the basic operation**

Multiply is the primary tool for building up longer loops from a shorter one.
The sequence is:

1. You have a loop already playing.

2. You press **Multiply once.** The engine enters Multiply mode. It continues
   playing your existing loop as before. Simultaneously it begins recording
   new audio into an extended buffer. The length of the original loop is
   locked in as the unit for this operation (``lCycleLength``).

3. As the original loop repeats, the buffer extends automatically. After one
   full pass the loop doubles; after two passes it triples. At each cycle
   boundary the engine increments its cycle counter (``lCycles``) and appends
   another ``lCycleLength`` block.

4. You press **Multiply again** to end the operation. What happens next
   depends on Round mode (see below).

**What the performer hears during Multiply**

The output is the source loop — your original recording — playing back with
your live input mixed over it. You hear the original loop underneath what you
are now playing; the engine records your new material simultaneously.

**A worked musical example**

You have a two-bar bass riff looping. You press Multiply and play a chord
progression that takes four bars. While you play you hear the bass riff
repeating underneath — once, twice. After your four bars you press Multiply
again.

You now have a four-bar loop. Bars 1–2 contain the bass riff plus your first
pass. Bars 3–4 contain the bass riff repeating plus your second pass. Every
subsequent cycle plays all four bars in sync. This is the central workflow for
building up live layered accompaniment without stopping playback.

**Pressing Multiply additional times during the operation**

If you press Multiply a third time while already in Multiply mode, the engine
notes the current cycle count as a checkpoint and schedules the end of the
operation at that checkpoint. Pressing multiple times accumulates additional
cycles before committing. This is a way to end a multiply pass on a future
cycle boundary without timing the final press precisely.

**Ending Multiply with a Record press**

If you press **Record** (rather than Multiply) to end a Multiply operation,
the loop stops immediately at the exact sample of the press. Additionally,
the cycle length is redefined to the full new loop length (``lCycleLength =
lLoopLength``, ``lCycles = 1``). Subsequent Multiply passes treat the longer
loop as a single indivisible cycle. Use this to promote a multiplied loop into
a new base unit.

**Can Multiply reduce a loop?**

Yes. If you use Multiply on a loop that has already been multiplied, you can
end the operation before reaching the original cycle count, producing a shorter
result. The loop must be at least one cycle long.

**Effect on subsequent operations**

After Multiply, ``lLoopLength`` is longer but ``lCycleLength`` stays the same
(the original loop length). Subsequent Multiply passes add one ``lCycleLength``
unit per pass. Overdub and Replace span the full new length.

----

Round
-----

**What it does**

Round applies **only** to Multiply and Insert operations — it has no effect
on Record or Stop. When you press Multiply to end the operation, you are
almost never at an exact cycle boundary. Round determines how the partial
cycle at the end is handled:

- **Round off** — the engine fills the portion from your button press to the
  end of the current cycle from the original source loop (``srcloop``). Your
  playing up to the press is kept; the tail is patched silently from the
  original.

- **Round on** — the engine keeps recording your live input all the way
  through to the next complete cycle boundary. Nothing is filled from the
  source; every sample up to the boundary is your new playing.

**Musical consequence**

With Round off: press Multiply at any point. The tail is covered by the
original loop material — comfortable in live performance because you can ease
off before the boundary and the loop closes itself. The trade-off: the final
beats revert to the original content.

With Round on: you must keep playing until SooperLooper closes the loop. After
you press Multiply there is a brief wait — the engine holds the recording open
until the next cycle boundary. Every note you play right up to that moment is
captured. Choose Round on when you want full control over every sample, or
when the original material would sound wrong patched into the tail.

**Practical example**

You have a 2-bar loop. You start Multiply and play for about 3.5 bars, then
press Multiply to end.

- Round off: bars 1–3.5 are your new playing; bars 3.5–4 are filled from the
  original 2-bar loop. Final result: a clean 4-bar loop.
- Round on: you continue playing through bar 4. The loop closes at the
  boundary. Final result: a 4-bar loop entirely of your new performance.

**Without an external sync source**

Round still works, using the internal cycle length (``lCycleLength``) to
determine where boundaries fall. The separate Round Integer Tempo parameter
(``pfRoundIntegerTempo``, not the Round button) applies a different mechanism
in the Record/Stop path: it finds the nearest integer-BPM interpretation of
the current loop length and extends the recording to that exact length.


Insert
------

**What it does**

Insert is a splice operation. Where Multiply extends the loop at the end by
recording additional cycles during playback, Insert cuts the loop at the
*current playback position* and splices in a new cycle's worth of material,
shifting the original content that follows the cut point to after the new
material.

Concretely: suppose your loop contains three sections ``[A][B][C]`` and you
press Insert while the loop is partway through ``[B]``. The engine mutes the
loop output and you record a new cycle. When Insert ends, the loop becomes
``[A][new material][B][C]``. The original content from the cut point onward
is preserved, pushed to after the inserted cycle.

**The sequence**

1. Press **Insert** while the loop is playing. The engine records the current
   position as the cut point (``lInsPos``). With Round on, the cut point is
   rounded to the next cycle boundary; with Round off, the cut point is
   wherever you pressed.

2. During Insert, the output is muted and you record a new cycle. The engine
   plays back the source loop up to the cut point, then replaces output with
   your live input for one cycle length, then resumes the original content from
   the cut point onward (the ``lRemLen`` remainder).

3. Press **Insert again** (or another command) to end. Insert end is
   *not* sync-gated — unlike Multiply end, it takes effect at the exact sample
   you press, since the cycle length is already fixed by the operation itself.

**Musical meaning**

Insert is for adding a section that was not in the original loop — a
transition, a fill, a bridge. Unlike Overdub (which layers on top of what is
already there), Insert genuinely lengthens the loop. Unlike Multiply (which
repeats cycles), Insert adds novel content in the middle.

A practical example: you have a two-bar loop of sustained chords. While
playing, you press Insert at the start of bar 2 to splice in a one-bar drum
fill. The result is a three-bar loop: bar 1 (chords) + bar 2 (fill) +
original bar 2 (chords continuing). The loop is now one cycle longer.

**How Round affects Insert**

With Round off: the cut point is wherever you pressed. The new cycle starts
at that exact position, and the original remainder follows immediately after.
The resulting loop length may not be a clean multiple of the cycle length.

With Round on: the cut point is rounded to the next cycle boundary, so the
insert always begins at a musically clean point and the resulting loop remains
a multiple of the original cycle length.

**Relation to Substitute**

Substitute is a simplified Insert-like operation. It overwrites one cycle
starting at the current position with new material (no muting, no shifting),
then returns to play. The ``bReplaceQuantized`` (Replace Quant) flag controls
whether Substitute start is sync-gated — the same flag as Replace, not a
separate control.

----

Play Sync
---------

**What it does**

The Play Sync checkbox causes the loop's playback position to be retriggered
when a sync pulse arrives near a loop boundary. Specifically: if a sync pulse
occurs while the loop position is within ``syncSamples`` of the loop's start
or end, the playback position is snapped to the sync-aligned position.

Play Sync requires Quantize to be set to something other than Off, because
``syncSamples`` must be non-zero for the near-boundary test to have any range.

**Musical meaning**

Play Sync is useful when your sync source (particularly MIDI clock) may drift
slightly over time. Without it, a loop recorded to MIDI clock will gradually
drift out of phase as the two run independently. With Play Sync on, the loop
is nudged back into phase alignment on each sync pulse, as long as it is close
to a boundary. It does not do any time-stretching — it is a position
correction only, so it is effective only for small accumulated drift.

----

Rel Sync (Relative Sync)
------------------------

**What it does**

Rel Sync is a fundamentally different sync mode, not an add-on to the Sync
checkbox. When Rel Sync is active:

- Pressing Record starts immediately — no waiting for a sync pulse. The engine
  records the exact phase offset between the last sync pulse and the moment you
  pressed Record (stored as ``lSyncOffset``).

- Pressing Stop waits for the moment in the next sync period where the same
  phase offset recurs (``lSamplesSinceSync == lSyncOffset``). The loop length
  is therefore an exact multiple of the sync period even though you did not
  start on a boundary.

- During playback, every time a sync pulse arrives, the engine re-cues the
  loop position to the stored offset (``lSyncPos``), correcting accumulated
  drift.

**Musical meaning**

Normal Sync forces your loop to start and end on beat boundaries. Rel Sync
lets the loop start wherever your phrase starts — even mid-beat — while
keeping it phase-locked to the tempo on every repeat.

Suppose you want to capture a phrase that begins on the "and of 4" — a pickup
before the bar. With normal Sync you would have to wait until beat 1, losing
the pickup. With Rel Sync you press Record on the "and of 4," the engine notes
the offset, and when you press Stop the loop closes at the next "and of 4."
Every subsequent repeat re-anchors on each incoming sync pulse.

**Rel Sync and Quantize**

Rel Sync works with Quantize off, but is designed to be used with Quantize set
to Cycle or 8th. When ``syncSamples`` is non-zero (Quantize ≠ Off), the engine
applies a half-period correction to the stored ``lSyncPos``: if you pressed
Record in the second half of a sync interval, ``lSyncPos`` is adjusted so that
the playback re-cue aligns to the nearest boundary. Without this correction
(Quantize off), presses in the second half of a period store an uncorrected
offset that can cause slight misalignment on subsequent pulses.

**When Rel Sync is a no-op**

Rel Sync requires a live sync signal. Without a sync source, the stop
condition never fires and the playback re-cue never runs.


Overdub Quant, Replace Quant, and Mute Quant
--------------------------------------------

These three checkboxes control whether specific operations snap to a sync
boundary when Sync is enabled. They are independent of the main Quantize
setting and of each other.

**Overdub Quant** (``fOverdubQuantized``, default: off)

When off (the default), Overdub start and stop take effect immediately
regardless of sync settings. When on, both the start and stop of an Overdub
operation wait for the next sync pulse before taking effect.

Use Overdub Quant when you want overdub layers to always begin and end on
beat boundaries — for example, when adding a sustained pad that should start
exactly on the downbeat rather than wherever your finger landed.

**Replace Quant** (``bReplaceQuantized``, default: on)

When on, both Replace start and Replace stop wait for the next sync pulse.
When off, Replace takes effect immediately.

Notably, **Replace Quant also controls Substitute**. The code reads the same
flag (``bReplaceQuantized``) in both the Replace and Substitute dispatch
paths. There is no separate Substitute quantize control.

Replace Quant defaults to on, unlike the other two, because Replace
destructively overwrites loop content. Snapping to a boundary by default
reduces the risk of accidentally replacing audio at the wrong position.

**Mute Quant** (``fMuteQuantized``, default: off)

When on, *returning from mute to play* waits for the next sync pulse. Muting
itself (pressing Mute while playing) is always immediate — only the return
to play is gated. This means you can mute at any moment, but the loop will
re-enter audibly exactly on the next beat.

Pause behaviour is similar: leaving the Paused state to return to Play is
gated; entering Pause is immediate.

**Are these subject to late-hit correction?**

No, and by design. These operations snap to the *next* sync boundary forward
in time, not *back* to a recent one. They are used to control texture
mid-performance (layering, muting, replacing a section) rather than to define
loop boundaries. The user is not trying to hit a precise boundary — they are
performing within an existing loop and the sync wait is a forward-looking
cleanup, not a correction of a missed entry point. Late-hit correction only
applies to Record, Multiply end, and Insert start/end because those are the
operations where the user is explicitly trying to land on a specific boundary.

----

A note on Overdub
-----------------

Overdub is not quantized in normal operation — it starts and stops immediately
when the button is pressed, regardless of Sync or Quantize settings. An
internal ``OverdubQuantized`` parameter exists in the engine (``pfOverdubQuantized``)
but defaults to off and is not exposed in the standard GUI.

----

Summary table
-------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Affects
     - Requires
     - No-op when
   * - **Sync To** (source selector)
     - Provides the timing reference pulse
     - JACK connection, MIDI clock, or an existing loop
     - Nothing connected
   * - **Sync** (per-loop checkbox)
     - Operations wait for the next sync pulse before acting
     - Sync source active
     - No sync source
   * - **Quantize** Off/Cycle/8th/Loop
     - Subdivision for Rel Sync and Play Sync; enables late-hit window
     - Sync checkbox on (for late-hit window), or Rel Sync on
     - Sync and Rel Sync both off
   * - **Round**
     - Multiply/Insert end on a complete cycle boundary
     - An active Multiply or Insert operation
     - Using only Record/Stop
   * - **Play Sync**
     - Retriggering near boundaries to correct drift
     - Sync source active; Quantize ≠ Off
     - No sync source, or Quantize off
   * - **Rel Sync**
     - Records phase offset; re-cues playback on each pulse
     - Sync source active; Quantize ≠ Off recommended
     - No sync source
   * - **Eighths/Cycle**
     - Cycle period for sync calculations; defines tempo if loop is source
     - Rel Sync, Quantize Cycle/8th/Loop, or loop-as-sync-source
     - Absolute external-pulse sync with no Rel Sync

----

Big-picture control flow (pseudocode)
--------------------------------------

The following pseudocode shows the main decision paths in the engine
(``plugin.cc``) when a Record or Multiply button arrives. It omits per-sample
audio detail to focus on the control logic.

::

    # ── On each JACK buffer callback ─────────────────────────────────────────

    # 1. Snapshot global sync phase BEFORE scanning this buffer
    T = lGlobalSamplesSinceSync          # samples since last sync pulse

    # 2. Scan this buffer for sync pulses; update the global counter
    for each sample in buffer:
        lGlobalSamplesSinceSync += 1
        if pfSyncInput[sample] > 1.5:
            lGlobalSamplesSinceSync = 0  # pulse arrived; reset counter

    # 3. Process any control event that arrived this buffer
    switch command:

        case RECORD_BUTTON:
            switch current_state:

                case STATE_RECORD:          # second press = stop recording
                    if fSyncMode == 0 and not bRoundIntegerTempo:
                        → STATE_PLAY immediately
                    elif late-stop window:
                        # (fSyncMode > 0 and 0 < T <= shiftSamples)
                        trim lLoopLength by T
                        set dCurrPos = lLoopLength + latency + T
                        → STATE_PLAY immediately
                    else:
                        → STATE_TRIG_STOP  # wait for next sync pulse

                default:                    # first press = start recording
                    shiftSamples = g_late_hit_window_ms * sampleRate / 1000
                    if fSyncMode > 0
                       and 0 < T <= shiftSamples:
                        # late-hit correction
                        allocate loop buffer
                        backfill T samples from input ring buffer
                          at readPos = inputWritePos - T - inputLatency
                        dCurrPos = T
                        → STATE_RECORD immediately
                    elif fSyncMode > 0:
                        → STATE_TRIG_START  # wait for next sync pulse
                    else:
                        → STATE_RECORD immediately

        case MULTIPLY_BUTTON:
            switch current_state:

                case STATE_PLAY:            # first press = begin multiply
                    lock lCycleLength = current lLoopLength
                    → STATE_MULTIPLY

                case STATE_MULTIPLY:        # second press = end multiply
                    if Sync and not yet at boundary:
                        → STATE_TRIG_STOP  # wait for next pulse, then endMultiply
                    else:
                        endMultiply():
                            if Round off:
                                lLoopLength = lCycles * lCycleLength
                                fill tail from srcloop
                            if Round on:
                                extend recording to next cycle boundary
                                (live input fills to boundary)
                        → STATE_PLAY

    # ── STATE_TRIG_START: per-sample, waiting for sync pulse ─────────────────

    for each sample:
        if fSyncMode == 0 and input > threshold:   → STATE_RECORD
        elif fSyncMode == 2 (Rel Sync):            → STATE_RECORD immediately
        elif fSyncMode > 0 and syncInput != 0:     → STATE_RECORD

    # ── STATE_TRIG_STOP: per-sample, waiting for sync pulse ──────────────────

    for each sample:
        roundTempoDone = bRoundIntegerTempo and dCurrPos == roundedSamples
        if fSyncMode == 0 and (not bRoundIntegerTempo or roundTempoDone):
            → nextState (STATE_PLAY or STATE_OVERDUB)
        elif fSyncMode == 1 and syncInput != 0:
            → nextState
        elif fSyncMode == 2 and lSamplesSinceSync == lSyncOffset:
            → nextState

    # ── STATE_PLAY: per-sample ────────────────────────────────────────────────

    for each sample:
        output = loop[dCurrPos % lLoopLength]
        if PlaySync on and Quantize != OFF:
            if syncInput != 0 and dCurrPos near loop boundary:
                dCurrPos = lLoopLength - lSyncPos  # retrigger to stored offset
        dCurrPos += fRate
        # natural wrap:
        dCurrPos = fmod(dCurrPos, lLoopLength)  # wraps at each loop end

----

Common setups for live performance
-----------------------------------

**Free looping, no sync**
Sync To: None. Sync checkbox off, Quantize off. Record and Stop take effect
immediately. Good for ambient or non-metered work, or when you want to
determine loop length by feel.

**Metered looping, tight timing**
Sync To: JACK Transport or MIDI Clock. Sync checkbox on, Quantize Cycle. The
source provides the beat; SooperLooper snaps every loop to exact bar multiples.
The late-hit window means you can press by feel without watching a metronome.

**Building up layered parts live**
Sync on, Quantize Cycle, Round on. Record a short seed loop (one or two bars),
then Multiply through it while adding parts. Round on means each Multiply pass
closes cleanly at the bar line — you do not need to release the button at a
precise moment. Typical session: record bass riff → Multiply, play chords for
4 bars, press Multiply → press Multiply again, play melody for 8 bars, press
Multiply. Each press closes at the next clean bar.

**Redefining the cycle mid-performance**
After building a 4-bar loop via Multiply, end a further Multiply with a Record
press instead. This immediately makes the 4-bar loop a single new cycle.
Subsequent Multiply passes add 4-bar units rather than 1-bar units.

**Phrases that begin off the beat**
Rel Sync on, Quantize Cycle, sync source active. Press Record at the natural
start of your phrase. SooperLooper captures the phase offset and closes the
loop at the matching offset in the next cycle. Every repeat re-anchors to the
sync pulse. Use this for pickups or grooves whose downbeat does not land on
beat 1.

**Loop-as-sync-source with a slave loop**
Record a master loop. Set Sync To: Loop 1. Set Eighths/Cycle to match its
phrase length. On a second looper channel, enable the Sync checkbox. The
second loop snaps to cycle boundaries of the first, keeping both locked
together.

**Drift correction for MIDI clock**
Sync To: MIDI Clock. Sync on, Quantize Cycle, Play Sync on. The loop snaps to
bar lines on record; Play Sync nudges playback back into phase on each incoming
clock pulse if the MIDI source drifts slightly.
