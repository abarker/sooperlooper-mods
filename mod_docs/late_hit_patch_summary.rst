SooperLooper Late-Hit Recording Modification Summary
====================================================

This fork of SooperLooper 1.7.9 makes looping to a beat more forgiving and
more precise:

- a Record start or stop, Mute or Unmute, or Multiply-end press that lands a
  moment after the beat counts as on time, instead of waiting a whole cycle;
- when synced to JACK transport, loops stay on the transport master's beat
  through tempo changes;
- SooperLooper can take commands, and send MIDI clock, over JACK MIDI
  directly;
- two new commands for a switch's long or double press: one cancels a
  recording just started, the other undoes the last overdub even though
  the press has already started a new one; and an optional timeout cancels
  a recording left running over a playing loop;
- a separate large-text display, ``slbigstate``, shows loop state from
  across the room;
- a latency tool, ``sllatency``, measures JACK's and SooperLooper's latency
  with a click and sets SooperLooper's to match.

This document describes each change and how to configure it.  How each is
implemented, and where the changes are in the source, is in the appendix
and glossaries at the end.

.. contents:: Table of Contents
   :local:
   :depth: 3

.. tip:: **Compiling on Ubuntu 24.04**

   The patched source is the ``master`` branch of
   https://github.com/abarker/sooperlooper-mods, so a plain clone gets it;
   stock SooperLooper 1.7.9 is the tag ``stock-1.7.9``.  A convenience
   script ``ubuntu_compile.bash`` at the top level of the repository automates
   the build.

   For a first build, set all four flags near the top of the script to
   ``true`` and run it::

       bash ubuntu_compile.bash

   On subsequent recompiles only ``compile_modified_fork=true`` is needed;
   the other three flags (``install_deps``, ``run_autogen``, ``run_config``)
   can remain ``false``.  The script prompts before running
   ``sudo make install``.

.. tip:: **Platform**

   Built and tested only on Ubuntu 24.04, using the script above.  It
   should compile wherever stock SooperLooper 1.7.9 does, since this
   patch leaves the Windows and macOS code paths untouched.

   Stock's Windows build notes say JACK MIDI input does not work there.
   That has not been rechecked; if it still holds, this patch's JACK MIDI
   feature would not work there either.  The late-hit corrections go
   through OSC and should be unaffected.

   Stock's standalone macOS build has one reported startup hang on
   Catalina, unrelated to this patch, with a workaround (enabling system
   sharing): https://github.com/essej/sooperlooper/issues/32.  Other
   users report it working, and stock has had several Catalina-specific
   point releases.

.. tip:: **Quick Start**

   The late-hit correction works as soon as the fork is built, with no
   session file changes.  Without the late-hit keys in the session file it
   uses the built-in defaults: a 100 ms window, with the cycle-fraction cap
   off (100.0).  Setting ``late_hit_max_quant_unit_fraction`` to, say, 0.5
   also caps the window at half a quantize cycle, which matters at fast
   tempos, where 100 ms can be more than half a cycle.

   To change the values:

   1. **Run SooperLooper** with your usual setup and save a session file via
      *File → Save Session*, then exit.

   2. **Edit the session file** in a text editor. Find the ``<Globals>`` element
      and add the two late-hit keys at the end of its attribute list, before the
      closing ``/>``::

          late_hit_max_quant_unit_fraction="0.5" late_hit_window_ms="100"

      A complete ``<Globals>`` element from a real session file looks like
      this (your other values will differ).  It also has
      ``record_failsafe_seconds`` and ``mute_late_hit_ms``, two further
      settings described later, which you do not need to add here::

          <Globals tempo="170" eighth_per_cycle="2" common_dry="0" common_wet="1"
           input_gain="1" sync_source="-1" auto_disable_latency="1"
           jack_timebase_master="0" output_midi_clock="0" use_midi_start="0"
           use_midi_stop="0" send_midi_start_on_trigger="0" smart_eighths="0"
           late_hit_window_ms="200" late_hit_max_quant_unit_fraction="0.5"
           record_failsafe_seconds="120.0" mute_late_hit_ms="100"/>

   3. **Restart SooperLooper** with the session file::

          sooperlooper --load-session yourfile.slsess

   All four settings can also be read and changed while SooperLooper runs,
   over OSC, instead of editing the session file -- see `OSC Control`_.


What This Patch Adds at a Glance
---------------------------------

Everything this fork adds to SooperLooper 1.7.9, grouped by area.  Each
item links to its full description.

.. rubric:: Late-Hit Timing Corrections

- **Late Record start.** A Record press that lands just after a sync
  boundary starts the loop at that boundary, as if pressed on time, instead
  of waiting a whole cycle.  The audio played in the moment between the
  boundary and the press is kept.  See `Late Record Start`_.

- **Late Record stop.** A press to end a recording that lands just after a
  boundary ends the loop at that boundary, so the loop is not a cycle too
  long.  See `Late Record Stop`_.

- **Late Multiply end.** The same correction for ending a Multiply.  It can
  be switched off at build time with the ``LATE_HIT_MULTIPLY_CORRECTION``
  flag, if it ever causes problems.

- **Late Mute and Unmute.** With mute-quantize on, a Mute or Unmute press
  just after the boundary acts at that boundary instead of the next one.
  On by default, with its own 100 ms window.  See `New Feature: mute/unmute
  late-hit correction`_.

- **Pause from Mute fix.** With mute-quantize on, pressing Pause while Muted
  now ends up Paused, as it already does without quantize; stock 1.7.9 ends
  up Playing.  Controlled by the ``PAUSE_FROM_MUTE_FIX`` build flag (on).
  See `Quantized Pause from Muted Now Resolves to Paused`_ in the appendix.

- **Adjustable window.** How late a press can be and still count as on
  time is set in milliseconds, optionally also capped at a fraction of the
  quantize cycle, and stored in the session file.  See `Record Late-Hit
  Window Size`_.

- **Robust in stereo and across sync-source changes.** The channels of a
  multi-channel loop stay in step when a correction fires, and changing the
  sync source cannot leave stale timing from the old source behind.  See
  `Late-Hit Correction Robustness`_ in the appendix.

.. rubric:: New Commands and Safety Features

- **New command** ``cancel_record_mute``.  Cancels a recording that has
  started, or one still waiting for the beat, and brings back the previous
  take; does nothing otherwise.  It is meant for a switch that fires
  ``record`` the moment it is pressed and sends this command when held or
  double-tapped, so one switch can record, play, and -- on a hold -- stop,
  leaving the loop muted, with no added latency on the record itself;
  doing nothing in other states is what makes that safe.  See `New Command:
  cancel_record_mute`_.

- **New command** ``cancel_overdub_undo``.  A general undo button bound
  on top of an overdub switch: its hold or double tap undoes as if the
  ``overdub`` the press already fired had not happened -- usually taking
  back the last overdub, but undoing whatever was done last.  Never
  removes a loop's first take.  See `New Command: cancel_overdub_undo`_.

- **Record failsafe.**  If a recording that cut off a playing loop is left
  running longer than ``record_failsafe_seconds``, it is cancelled
  automatically and the previous take comes back, muted.  It catches a
  record pressed to stop a loop without the long press that would cancel
  the new recording, and any other accidental record trigger over a
  playing loop.  Off by default.  It never touches a first recording into
  an empty loop, or one started from Mute or Pause.  See
  `New Feature: record failsafe`_.

- **Four settings, one place.** ``late_hit_window_ms``,
  ``late_hit_max_quant_unit_fraction``, ``record_failsafe_seconds`` and
  ``mute_late_hit_ms`` are all saved in the session file's ``<Globals>``
  line, and can be read and set live over OSC.  See `Record Late-Hit
  Window Size`_ and `OSC Control`_.

.. rubric:: JACK MIDI and JACK Sync

- **JACK MIDI input port** (``sooperlooper:midi_in``).  SooperLooper can
  take MIDI commands directly from JACK, with no ALSA bridge or
  ``a2jmidid``, acting on each at its exact sample within the audio
  buffer.  OSC and ALSA MIDI work as before.  See `JACK MIDI Ports`_.

- **JACK MIDI output port** (``sooperlooper:midi_out``).  Sends 24-ppqn MIDI
  clock, and MIDI Start/Stop on JACK transport start and stop, for driving
  external hardware.  See `JACK MIDI Ports`_.

- **Sync that follows tempo changes** (``ALB_JACK_BEAT_TRACKING``, on by
  default).  With the sync source set to JACK, sync stays on the transport
  master's beat -- Hydrogen's, a DAW's, or any other master's -- when the
  tempo changes while the transport is rolling; stock drifts off by up to
  half a cycle until the next relocate.  See `JackSync Beat Tracking`_.

- **Eighths/Cycle changes that keep to the bar** (``ALB_CYCLE_CHANGE_REANCHOR``,
  **on** by default, not yet tried live).  When Eighths/Cycle changes
  while JACK or MIDI clock sync is running, the new cycles start at the
  old cycles' nearest boundary, instead of being counted from transport
  position 0 or the last MIDI Start.  See `Eighths/Cycle Changes While
  Running`_.

- **MIDI clock sync fixes.**  MIDI clock arriving on ``midi_in`` now sets
  the tempo; before this fix those events carried no usable timestamp and
  the tempo stayed at 0 (a bug this fork introduced).  And a stock bug is
  fixed (``ALB_MIDI_SYNC_CLEAR_FIX``, on by default): with MIDI clock sync,
  a boundary mark could linger in the sync signal for many buffers, so a
  synced recording sometimes started at once instead of waiting for the
  next boundary.  Clock on ``midi_in``, with its perfectly regular timing,
  hit this reliably; clock through ALSA only rarely.

.. rubric:: Realtime-Safety Fixes

- **Fewer xruns on transport locates** (``ALB_RT_SAFE_COND_SIGNAL``).
  Three thread wake-up calls that could stall the audio thread are removed
  from it; the main loop wakes on its own timer instead.

- **No tempo updates while locating** (``ALB_TEMPO_ONLY_WHEN_ROLLING``).
  SooperLooper takes the JACK transport tempo only while the transport is
  rolling, avoiding a burst of tempo changes, which are not realtime-safe,
  during a locate.

.. rubric:: Tools

- ``slbigstate``: **a large-text loop state display.**  A separate
  program, installed alongside ``sooperlooper`` and ``slgui``, that shows
  each loop's state in large, colored text, readable from across a room.
  See `New Tool: slbigstate`_.
- ``sllatency``: **a latency calibration tool.**  Measures the audio round
  trip with a click and checks JACK's and SooperLooper's latency settings,
  including the path SooperLooper is heard through.  See `New Tool:
  sllatency`_.

The Late-Hit Sync Problem
---------------------------

With sync on, SooperLooper already handles an *early* press: it waits for
the next sync boundary and acts there.  It does nothing for a *late* one.  A
Record or Stop press that lands just after a boundary is taken as belonging
to the next boundary, a whole cycle away: the loop starts a cycle late, or
ends up a cycle too long.  In live playing, where a press is often a hair
behind the beat, this is a steady source of timing errors.

What the Patch Does
-------------------

A Record or Stop press that lands within a short window after a sync
boundary is treated as if it had arrived exactly at that boundary.  This
needs sync to be active, which means two things together:

- the loop's **Sync** checkbox is on, so its operations wait for
  boundaries at all; and
- a **sync source** is selected and producing pulses.  With no pulses
  there is no boundary to measure lateness from, and the correction never
  fires, whatever the checkbox says.

With a JACK or Internal source this also means Quantize set to Cycle or
8th: with Quantize Off or Loop those sources produce no boundaries.  The
correction is off under Rel Sync, where a recording deliberately starts
and ends away from the boundary.  The
window is 100 ms by default; see
`Record Late-Hit Window Size`_.

Late Record Start
~~~~~~~~~~~~~~~~~

Normally a late Record press waits for the next boundary, and the loop
starts a whole cycle late.  With the patch, the loop starts at the
boundary just passed.  The audio played between that boundary and the
press is not lost: SooperLooper keeps a continuous history of its input,
and copies that stretch into the start of the loop, so recording carries on
seamlessly from there.  The result is a loop that starts exactly on the
boundary, as if the button had been pressed on time.

Late Record Stop
~~~~~~~~~~~~~~~~

Normally a late press to end a recording waits for the next boundary, and
the loop comes out a whole cycle too long.  With the patch, the loop is
trimmed to end at the boundary just passed, and playback starts at once
from where it would be had the press been on time, so the loop is in time
from the start.

That is why, with the corrections in place, an accurate stop matters more
than an accurate start.  The beginning of the loop's *first* repeat -- the
moment between the boundary and the late press -- has already gone by and
is not heard; later repeats play in full.  In a DAW recording, a first
repeat cut short this way can be repaired by copying one of the full
repeats over it.

Both Late
~~~~~~~~~

The two corrections are independent and combine correctly: a late start
with an on-time stop, an on-time start with a late stop, or both late,
all work the same way.

On-Time Presses, and Sync Off
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When a press is on time, when sync is off, or when no sync pulse has
arrived recently enough, the patch changes nothing: SooperLooper behaves
exactly as the unpatched version.

How the start and stop corrections work inside the engine is in
`Late-Hit Record Start and Stop: Implementation`_ in the appendix.

Commands for a Switch's Long or Double Press
--------------------------------------------

Many footswitches and MIDI controllers can send a different message for a
quick tap, a long hold, and a double tap -- set up in the device itself,
or in software between it and SooperLooper.  Normally such a switch has to
wait before sending anything: a tap is only known to be a tap once the
hold time has passed, or once no second tap has followed.  That wait
delays the tap's command, and for ``record`` or ``overdub``, where timing
is the point, the delay can cost more than the extra gesture is worth.

The way around it is *eager firing*: the switch sends its main command the
moment it is pressed, every time, and then sends a second message if the
press turns out to be a hold or a double tap.  The main command gets no
added delay.  The catch is that by the time the hold or double tap is
recognised, the main command has already acted.  The two commands below
are made to be that second message: each first reverses what its
switch's main command has just done, according to what that was, then
does its own job, and does nothing where there is nothing to reverse.
Each belongs on the switch whose command it follows: ``cancel_record_mute``
after ``record``, ``cancel_overdub_undo`` after ``overdub``.

For this to work, the switch must send its main command on the press, not
on the release, and only once per gesture -- not again on the second tap
of a double tap.  The setup guide
(``sooperlooper_and_ardour_setup_guide.rst``, section 1.2) shows one way
to set this up, with a MIDI remapping program between the pedal and
SooperLooper.

New Command: ``cancel_record_mute``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cancels a recording that has started, or one still waiting for the beat,
and brings back the previous take.  It mutes the loop only if something was
actually recorded; otherwise the previous take simply keeps playing.  In
every other situation it does nothing, which is what makes it safe to put
on the same footswitch as ``record``.

The intended use is one switch that records, plays and stops.  Press to
record, press again to play, and hold to stop.  The hold's press fires a new
``record`` at once, as every press on the switch does; the long press then
fires ``cancel_record_mute``, which cancels that new recording and leaves
the loop you made muted.  Unmuting resumes it where it is in its cycle;
``mute_trigger`` restarts it from the top.

It is fired by an optional second gesture that always follows the eager
``record``: a long hold (the pedal is already down), or a quick double tap
whose second tap is mapped to ``cancel_record_mute`` instead of another
``record``.  The command does not care which; it only needs an eager
``record`` to have landed.  The first press always fires at once: waiting
to see whether a second gesture follows would cost the timing accuracy the
late-hit correction exists to protect.

A separate button bound to ``mute_trigger`` does the same job for a
playing loop.  The hold saves that second pedal, and is an extension of the
gesture already in progress rather than one more control to reach for under
performance pressure.

It acts only on a recording whose ``record`` press interrupted a playing
loop (playing or one-shot).  A recording started from mute, pause or an
empty loop is left alone and carries on.  From there a hold has nothing to
stop, so it can only be a slow press, and cancelling would throw away the
take you meant to start.  In practice the record switch can be held as
long as you like, except when starting a recording over a playing loop,
where a hold means stop.

What happens, by what the loop is doing when the command arrives:

.. list-table::
   :header-rows: 1
   :widths: 30 38 32

   * - The loop is ...
     - What happens
     - What you hear
   * - recording, started from mute, pause or an empty loop, or waiting
       for the beat to start such a recording
     - Nothing.  The recording carries on.
     - The new take, as normal.
   * - recording, started over a playing loop
     - The recording is discarded and the previous take restored.
     - Silence: the loop is muted.  Unmute to hear the previous take.
   * - waiting for the beat to start a recording over a playing loop
     - The queued recording is cancelled.
     - The previous take keeps playing, uninterrupted: the hold does not
       stop it.
   * - waiting for the beat to end a recording
     - Nothing.  The take is kept.
     - The new take plays from the beat.
   * - playing, muted, empty, or anything else
     - Nothing.
     - No change.

A ``redo`` after a cancel does nothing.  The cancelled recording cannot
be redone, and neither can anything undone before it: starting that
recording already cleared redo, as starting any new recording does.  (A
cancel of a recording still waiting for the beat started nothing, so redo
still works as before it.)

The hold usually lands after the recording has started, so the loop stops.
It can land while the record is still waiting for the beat -- mostly when
record was pressed early in a long cycle -- and then the loop keeps playing.
The third row is the accident the design has to tolerate: pressing to end a
recording and lingering on the pedal must not destroy the take.  How the
command tells these cases apart, and what it changes internally, is in
`cancel_record_mute Implementation`_ in the appendix.

New Command: ``cancel_overdub_undo``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A general undo button, bound on top of the overdub switch.  A tap records
an overdub as usual; a hold undoes the loop's last change -- usually the
last overdub, if it was bad or has worn out, but equally a multiply, a
replace, or whatever was done last.  Read that way, the cases below follow
naturally: the hold is an ordinary undo, with the switch's own press taken
out of the way first.

That last step is what the command adds.  Every press of the switch fires
``overdub`` at once, so overdubs start with no added delay; a long press (or
double press) then fires ``cancel_overdub_undo``, which undoes as if that
overdub press had not happened.  Plain ``undo`` in that position would act
on whatever the press had just done -- usually undoing only the new, empty
overdub the press started.

What happens, by what the overdub press just before it did:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - The overdub press ...
     - What happens
   * - started an overdub on a playing loop
     - That new overdub is thrown away, then an ordinary undo takes back
       the loop's last change before it -- usually the previous overdub,
       but whatever was done last, such as a multiply or a replace.  If
       that would reach the loop's first take, only the new overdub is
       thrown away.
   * - started an overdub on a muted loop
     - The same, and the loop goes back to muted.
   * - was waiting for the beat to start an overdub
     - The waiting overdub is cancelled, and the loop's last change
       undone, as above.
   * - stopped the overdub in progress (at once, or waiting for the beat)
     - That overdub is undone.
   * - ended a recording into an overdub
     - The overdub is dropped; the recording is kept and plays.
   * - found the loop multiplying, inserting, replacing, substituting,
       in delay, paused, empty, or waiting to start a recording
     - Nothing.  If the press itself started an overdub from there, that
       overdub keeps going.
   * - (no overdub press came just before: the command arrived on its
       own, or another command came in between)
     - Nothing.

It never removes a loop's first take -- the oldest layer in its undo
history -- only what was added on top of it.  This is for safety in
performance, not a technical limit: a hold on the overdub switch that
lasts a moment too long should never wipe out the whole loop.  (Plain
``undo`` would empty a loop that has only ever had one take.)  To silence
the loop instead, mute it.  A ``redo`` brings back an
overdub it undid, but never the new one the press had started.  Waiting
for the beat only happens with SooperLooper's "overdub quantized" option
on.

Unlike the record switch, the overdub switch must always be tapped.  The
hold is a real undo wherever it acts, so a press that is only slow still
undoes something: a slow start on a playing loop throws away the new
overdub and undoes the layer under it (never the first take), so a part
drops out; a slow stop
undoes the overdub just played.  Neither is a glitch -- there is no
click, and the loop keeps its place and its sync -- but the loss is
audible.  A ``redo`` brings the undone layer back.  This is the price of
a general undo on the same switch: tap to record an overdub, hold to
remove it.  If slow presses become a problem, a longer long-press time
on the overdub switch alone is the simplest fix.

A long press is the safer gesture for it.  Two quick taps on an overdub
switch are sometimes meant as two taps -- to overdub a short phrase and
stop -- and with this command on the double tap, they would undo
instead.  Bind it to the switch's long (or double) press output in
SooperLooper's MIDI bindings, like any command.  How it works inside the
engine is in `cancel_overdub_undo Implementation`_ in the appendix.

New Feature: record failsafe
-----------------------------------

If a recording is left running longer than ``record_failsafe_seconds``,
SooperLooper cancels it exactly as ``cancel_record_mute`` would: the take is
discarded, the previous take comes back, and the loop ends muted.

It is meant for this case: you press ``record`` to stop a playing loop,
intending the long press (or double tap) that fires ``cancel_record_mute``,
but forget it.  A new recording then starts over the loop and keeps
running -- recording has no automatic end -- with the playing take replaced
by whatever is being recorded.  The failsafe equally catches any other
accidental record trigger over a playing loop, such as a bumped pedal.

**It only fires for a recording that cut off a playing loop** -- one started
while the loop was playing and audible.  It never fires for a first
recording into an empty loop, or for one started from Mute or Pause: with
nothing playing, a record press cannot have been meant to stop playback, so
it is taken as a deliberate new take and runs as long as it likes, as
before.

**Trade-off:** if you really did mean to start a new take right after
ending a loop's playback, that take can now last at most
``record_failsafe_seconds``, like any other recording started over a
playing loop.  SooperLooper cannot tell "meant to record" from "meant to
stop instead" when the press lands, only from what happens afterwards.

**Off by default.**  Because it can discard a whole take with no press at
all, it is opt-in, unlike the late-hit corrections.  To turn it on, set
``record_failsafe_seconds`` to a positive number of seconds, in the session
file's ``<Globals>`` line or live over OSC (see `OSC Control`_).  Zero or
a negative value turns it off.  Choose a value comfortably longer than your
longest real take.

How it is implemented is in `Record Failsafe Implementation`_ in the
appendix.

New Feature: mute/unmute late-hit correction
-----------------------------------------------

With mute-quantize on, a Mute or Unmute press normally waits for the next
sync boundary -- up to a whole cycle muted when you meant to be playing, or
playing when you meant to be muted.  With this correction, a press that
lands just after a boundary acts as if it had arrived exactly at that
boundary, in both directions.  It applies to ``mute``, ``mute_on`` and
``mute_off``.  Nothing in the loop is recorded or trimmed; only *when* the
mute takes effect changes, so the loop stays exactly in time.

**Its own window.**  The setting is ``mute_late_hit_ms``, in
milliseconds, default 100 -- on by default, matching the Record late-hit
window.  It is a plain millisecond value, with no cycle-fraction cap
like the Record window's.
That cap exists to keep a late Record press unambiguously tied to one
boundary before real audio is copied or trimmed; mute and unmute never
touch the audio, so a generous value can only make mute-quantize act a
little more eagerly at very fast tempos, never mis-correct anything.  It is
independent of ``late_hit_window_ms`` and ``record_failsafe_seconds``.

It defaults on because, unlike the record failsafe, it can only turn "wait
for the next boundary" into "act now", never lose anything -- the same
reasoning as the Record late-hit window.

To change or disable it, set ``mute_late_hit_ms`` (``0`` for off, otherwise
the window in milliseconds) in the session file's ``<Globals>`` line or
live over OSC (see `OSC Control`_).

How it is implemented is in `Mute/Unmute Late-Hit Implementation`_ in the
appendix.

Record Late-Hit Window Size
----------------------------

This section is about the Record window (``late_hit_window_ms`` and
``late_hit_max_quant_unit_fraction``).  The mute/unmute window,
``mute_late_hit_ms``, is a separate, single millisecond value; see
`New Feature: mute/unmute late-hit correction`_.

The window has two limits, and the smaller one applies.

**Millisecond cap.**  The setting is ``late_hit_window_ms``, default
100 ms: how late a press can be and still count as on time.

**Cycle-fraction cap.**  The setting is
``late_hit_max_quant_unit_fraction``, default 100.0, which in effect turns
it off so the millisecond cap always governs.  Set below 1.0, it limits the
window to that fraction of one quantize cycle at the current tempo and
Eighths/Cycle setting, so the window scales with tempo.  For example, 0.25
allows at most a quarter of a cycle at any tempo.  At slow tempos the
millisecond cap usually governs; the fraction cap takes over at fast tempos
or with short cycles.

Both are stored under those names in the session file's ``<Globals>`` line,
the same line that holds ``tempo`` and ``eighth_per_cycle``.  Edit the file
and restart SooperLooper, or change them live over OSC (see `OSC
Control`_).  A session file without them uses the built-in defaults.

The window can never be longer than about 682 ms at 48 kHz (32767 samples):
that is how much input history SooperLooper keeps for a late start to copy
from, and a larger ``late_hit_window_ms`` is silently limited to it.  One
use of this: set ``late_hit_window_ms`` large (say 2000) and
``late_hit_max_quant_unit_fraction`` to the cycle fraction you want, so the
fraction alone governs at all normal tempos.

OSC Control
~~~~~~~~~~~

All four settings -- ``late_hit_window_ms``,
``late_hit_max_quant_unit_fraction``, ``record_failsafe_seconds`` and
``mute_late_hit_ms`` -- can be read and changed while SooperLooper runs,
with the same global OSC ``/set`` and ``/get`` messages used for ``tempo``,
``eighth_per_cycle`` and the other ``<Globals>`` values:

.. code-block::

    /set  s:param  f:value                     -- no reply
    /get  s:param  s:return_url  s:return_path  -- replies to return_path
                                                    with  i:-2  s:param  f:value

For example, to read and then set the record failsafe from a client at
``osc.udp://127.0.0.1:9999/`` listening on ``/reply``::

    /get "record_failsafe_seconds" "osc.udp://127.0.0.1:9999/" "/reply"
    /set "record_failsafe_seconds" 45.0

A negative value sent with ``/set`` is ignored, leaving the setting as it
was.

These settings do not appear in ``slgui``, and ``/register_auto_update``
cannot watch them for changes, because they are plain settings rather than
SooperLooper controls (see `Why Not a LADSPA Port`_ in the appendix).
Plain ``/set`` and ``/get`` work normally.

JACK MIDI and JACK Sync
-----------------------

JACK MIDI Ports
~~~~~~~~~~~~~~~

SooperLooper gets two JACK MIDI ports:

- ``sooperlooper:midi_in`` lets SooperLooper take MIDI commands directly
  from JACK MIDI controllers and programs, with no ALSA MIDI connection or
  ``a2jmidid`` needed.  Commands are bound in ``slgui`` as usual, and OSC
  and ALSA MIDI still work as before.
- ``sooperlooper:midi_out`` sends 24-ppqn MIDI clock whenever a tempo is
  known, each tick placed to the sample, and MIDI Start and Stop when the
  JACK transport starts and stops -- for driving external hardware without
  ``a2jmidid``.

**Commands act at their exact sample.**  A JACK MIDI command carries its
position within the current audio buffer.  SooperLooper splits the buffer
there, running the audio up to that point first, so the command takes
effect at that exact sample.  Commands over OSC or ALSA MIDI carry no
position and take effect at the start of a buffer, so JACK MIDI commands
are timed more precisely.

**MIDI clock.**  Clock sent to ``midi_in`` is counted and sets the tempo,
as clock through ALSA does.  (Until a fix in this fork, clock on
``midi_in`` never set the tempo: those events carried no timestamp.)  With
MIDI clock sync, turn off "auto 8th"; see `Known Issues and Future
Work`_.

How this is done inside the engine, including how it interacts with
SooperLooper's trigger latency setting, is in `JACK MIDI Input:
Implementation`_ in the appendix.

JackSync Beat Tracking
~~~~~~~~~~~~~~~~~~~~~~

With the sync source set to JACK, stock SooperLooper places its sync pulses
on a fixed grid counted from JACK frame 0. A transport master that keeps
its beat continuous when the tempo changes while the transport is rolling
-- as Hydrogen does, and a DAW playing through a tempo change -- drifts off
that grid, by up to half a cycle, and stays off until the next relocate,
such as stopping and restarting the transport. Loops then start and stop
off the beat, which shows up as a loop landing an eighth note away from
where it should.

This fork counts beats as the music plays, at whatever tempo JACK reports
at each moment, so the pulses stay on the master's beat through any number
of tempo changes; on a relocate it lines up again with where the master
puts its beat.  At a steady tempo it gives the same pulses as stock.  It
also stops SooperLooper reading some of JACK's transport information from
uninitialised memory, a stock bug.

On by default (``ALB_JACK_BEAT_TRACKING``), and used through full live
sessions with frequent tempo changes.  How it works is in `JackSync Beat
Tracking: Implementation`_ in the appendix.

Eighths/Cycle Changes While Running
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

With JACK or MIDI clock as the sync source, SooperLooper counts its
cycles from transport position 0 (JACK) or from the last MIDI Start or
Continue.  When Eighths/Cycle changes in the middle of a song -- say from
8 to 6 for a section in 3/4 -- the new three-beat cycles are still counted
from there, so they line up with the new bars only if the section happens
to start a multiple of three beats in.  Internal sync, by contrast, starts
the new cycles from the last boundary.

With ``ALB_CYCLE_CHANGE_REANCHOR`` defined in ``engine.hpp``, the new
cycles start at the old cycles' boundary nearest the change -- the bar
line, if the old cycle was a bar.  A change sent a little before the bar
line it is meant for still lands on it, since the nearest boundary is
used, not the last one.  A Quantize change alone does not move the grid,
and a relocate or a MIDI Start counts from the start again, as before.
JACK needs ``ALB_JACK_BEAT_TRACKING`` (the default).

**On by default.**  It is tested -- unit tests, and probes with JACK and
with a scripted MIDI clock -- but not yet tried live.  Without it, the
workaround is to relocate the transport to its start with the change.

New Tool: ``slbigstate``
--------------------------

A separate program that shows one or more loops' current state (Record,
Playing, Overdub, Mute, ...) in large, high-contrast text on a background
color for each state -- readable from across a room during a performance,
where ``slgui``'s own state indicators are too small.  It is its own
window, not part of ``slgui``.

It is built and installed with ``sooperlooper`` and ``slgui`` by the same
``configure``/``make``/``make install``, into the same directory (e.g.
``/usr/local/bin``).  The source is in ``src/big_state_display/``.

Usage::

    slbigstate [options]

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Option
     - Meaning
   * - ``-H``, ``--host``
     - Engine host to connect to (default ``127.0.0.1``).
   * - ``-P``, ``--port``
     - Engine OSC port to connect to (default ``9951``).
   * - ``-l``, ``--loop``
     - Show exactly one loop, the original single-window mode. Mutually
       exclusive with ``-L``/``--loops``.
   * - ``-L``, ``--loops``
     - Comma/range list of loops to stack in one window, e.g.
       ``0,2-4``, in the order given. If neither ``-l`` nor ``-L`` is
       given, all of the engine's loops are shown, auto-detected --
       see *Multiple loops* below.
   * - ``-X``, ``--screen-location``
     - Window position, format ``<xpos>:<ypos>``.
   * - ``-Z``, ``--size``
     - Per-row window size, format ``<width>x<height>`` -- the size one
       loop's row takes; the whole window's height is this times the
       number of loops shown.
   * - ``-T``, ``--stay-on-top``
     - Keep the window above other applications.
   * - ``-M``, ``--margin``
     - Pixels reserved on every side of the label when fitting the
       font (default 20). Turn this down for text that sits closer to
       each row's edges, or up for more breathing room.

The window title includes which loop(s) are shown -- "SooperLooper
State - Loop 0" for a single loop, "SooperLooper State - Loops 0,2,3"
for a stack -- since more than one instance can be running at once.

**Text size adjusts to fit.**  The text grows or shrinks to fill the
window with the labels showing right now, and is recalculated whenever the
window is resized or a state changes.  All rows of a stack share one size,
so they never disagree.  Sizing for the labels actually on screen, rather
than the longest possible one ("Substitute"), lets a short label like "Off"
fill the space; the trade-off is that the text size can change when a
state changes to a longer or shorter word.

**Multiple loops: one window, stacked.**  With no ``-l`` or ``-L``,
``slbigstate`` shows every loop SooperLooper has, one row per loop,
stacked in loop order, each row the size a single-loop window would be.
With one loop, that is simply a single-loop window.  A thin black line
separates the rows, so two loops in the same state don't read as one
block.  To show a chosen set in a chosen order, use ``-L``: ``slbigstate
-L 2,0-1`` shows loop 2, then loops 0 and 1.  To show exactly one loop and
skip the automatic loop count, use ``-l N``.  ``-l`` and ``-L`` cannot be
combined.

**Following SooperLooper.**  It talks to SooperLooper over OSC, as a
separate client, and works the same whether SooperLooper was started by
``slgui`` or runs on its own.  For each loop shown it asks SooperLooper to
send that loop's state whenever it changes, and renews the request every
20 seconds, so the display recovers by itself if SooperLooper restarts.

Without ``-l`` or ``-L`` it also keeps the number of rows in step with
SooperLooper's loop count, rebuilding within a few seconds when a loop is
added or removed, or when SooperLooper restarts with a different number of
loops.  At startup it first asks how many loops there are.  If
SooperLooper is not answering yet -- for example because ``slbigstate`` was
started first -- it shows loop 0, says so, and switches to the real loop
count once SooperLooper answers.

With ``-l`` or ``-L`` the rows are fixed: a count change elsewhere leaves
them alone, and a row whose loop is removed stays at its last state.

How it does this is in `slbigstate: Implementation`_ in the appendix.

**Remembering its window position.**  It keeps its own file,
``~/.sooperlooper/slbigstate_config.xml``, next to ``slgui``'s
``gui_config.xml`` but separate from it, so the two programs never
overwrite each other's settings.  A stack of two or more loops uses a
second file, ``~/.sooperlooper/slbigstate_stack_config.xml``, so a stack
and a single-loop window each remember their own position.  Showing
exactly one loop, however chosen, always uses the single-loop file.


New Tool: ``sllatency``
-------------------------

A command-line tool that measures latency with a click, the way
``jack_iodelay`` does, and goes on to check SooperLooper's own latency
compensation.  It plays a short click out of JACK, records it as it comes
back through a mic or a cable, and times the return to the sample.  From
that it reports:

- whether JACK's latency settings match the measured round trip, and if
  not, the values to set;
- where a loop recorded in SooperLooper lands relative to what was played,
  as heard -- through whatever buses and plugins SooperLooper's output goes
  through before the speakers or headphones;
- the SooperLooper input and output latency that would make loops land on
  time, which it offers to apply and to save in the session file.

It is installed with ``sooperlooper`` and ``slgui`` by the same ``make
install``, into the same directory, as ``sllatency``.  It is a Python 3
script and needs ``numpy`` and the Python ``jack`` module (Ubuntu packages
``python3-numpy`` and ``python3-jack-client``; or ``pip install numpy
JACK-Client``).  Choosing the session file in a file picker also needs
tkinter (Ubuntu package ``python3-tk``; included with Python on Windows and
macOS); without it the file name is typed instead.  The source is
``tools/sllatency.py``.

**Running it.**  Start JACK and SooperLooper with the usual session, and
the DAW if SooperLooper is monitored through one.  Leave loop 1 empty or
muted.  Then run ``sllatency`` and follow the prompts:

1. It lists the physical outputs and what feeds each, and asks which to
   play the click on.  The suggested ones (marked ``*``) are the outputs
   SooperLooper's loop 1 is heard through: connected directly, or through
   a DAW bus named in Ardour's style (``BUS/audio_in N`` to
   ``BUS/audio_out N``).  If that cannot be traced, it suggests what a
   DAW's master bus feeds, and says the suggestion is a guess.
2. Put the headphones fed by those outputs on or right next to the mic
   loop 1 records from -- balancing them on the mic works -- or hold the
   mic right against a speaker, or connect a cable from an output to that
   input.  With a speaker, keep the mic within a few centimetres: sound
   takes about 3 ms per metre, and that would be counted as latency.  With
   a cable into a mic input, turn the input gain down first.  Keep it quiet
   by the mic: turn down or switch off amps and anything else that hums or
   buzzes.  The tool listens only around the click's own pitch and ignores
   returns too early to be real, so noise makes it report that the clicks
   did not come back clearly rather than give a wrong figure, but steady
   noise can still hide a quiet click.
3. Press Enter.  If loop 1 records from more than one input (a stereo
   loop), a few clicks first find which input hears them best; that one is
   used as the mic input.
4. The measurement takes about 25 seconds: clicks for the round trip,
   recorded meanwhile in a temporary SooperLooper loop, then a few clicks
   played into the ports loop 1's output feeds, to measure the path
   SooperLooper is heard through.  Don't touch the pedals or record in the
   DAW meanwhile.  The temporary loop is removed afterwards, and the tempo,
   sync and other settings it changed are restored.

**The JACK result.**  A table shows JACK's reported capture and playback
latency, each split into buffering (set by the period size and number of
periods) and JACK's extra input and output latency (set in QJackctl's
Setup, Advanced tab, or with ``jack_control``, or jackd's ``-I`` and
``-O``), beside the measured round trip.  Within the tolerance (1 ms by
default) there is nothing to change.  Otherwise it gives the new extra
latencies to enter -- values to enter, not amounts to add -- with the
correction shared evenly between input and output, and where to set them.
The current extra latencies can be read only when JACK runs through
jackdbus; otherwise it gives the total change.

**The SooperLooper result.**  It reports how far the recorded loop lands
from the live click at SooperLooper's output, the delay measured after
SooperLooper (next to what JACK reports for that path, which a DAW can get
wrong), and the sum of the two: where loops land as heard.  A table then
compares, in samples:

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Row
     - Meaning
   * - ``current (manual):``
     - The loop's latency settings now (``autoset`` instead of ``manual``
       when SooperLooper's automatic latency is on).
   * - ``SooperLooper's autoset would use:``
     - What SooperLooper's automatic setting would choose: the latency JACK
       reports for its ports, following SooperLooper's own rule for which
       ports it reads (the main ports when a loop uses them).
   * - ``measured best:``
     - The total that makes loops land on time as heard, split in JACK's
       proportions (its reported input and output latency along the loop's
       path) with the correction shared evenly.

Only the total decides where loops play.  Input latency on its own decides
where each recording starts, so a wrong split moves the recording's edges
by the error.  A loopback measures only the total; the overdub test in the
setup guide checks the split directly.

**Applying and saving.**  It then asks whether to apply the measured-best
values to every loop in the running SooperLooper, and whether to save them
in the session file SooperLooper loads at startup.  Saving changes only the
``input_latency`` and ``output_latency`` values in that file (and sets
``auto_latency`` to ``no`` so they are used); everything else in the file
stays as it was.

Before asking, it says how the file is backed up.  On yes, a file picker
opens to choose the session file -- the one SooperLooper loads at startup
(the file given to ``slgui`` or ``sooperlooper`` with ``-L``).  It starts
at the file chosen last time, which is remembered in
``~/.sooperlooper/sllatency.conf`` only for this purpose.  Cancelling
saves nothing.  When no picker can be shown -- no tkinter, or no display,
as over SSH -- the file name is typed instead, with the one chosen last
time as the default.  ``--session-file`` skips the choice.

A backup is made first, next to the file, named after it with the date and
time added: ``my_session.slsess.bak-YYYYMMDD-HHMMSS``.  If the session file
is a symlink, the file it points to is changed and backed up.  The changed
file keeps the old one's permissions, and is written in full before it
replaces the old one, so an interruption never leaves it half written.
Copy the backup back to undo.

**Without SooperLooper.**  ``--no-sl`` measures only the round trip and
JACK's settings -- for calibrating JACK for any program, such as a DAW.  It
does the same if SooperLooper is not running.  With ``--out`` and ``--in``
set to a JACK program's input and output instead of hardware ports, it
measures the delay through that program (for example a DAW bus and its
plugins), with no mic.  To the program that is a feedback loop, which some
programs react to -- Ardour lights its "No Align" warning and reports more
latency afterwards -- so restart the program before relying on it again.

**Quick values.**  ``--reported`` plays nothing and records nothing: it
suggests SooperLooper values from the latencies JACK (and a DAW) report --
a starting point, not a measurement.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Option
     - Meaning
   * - ``--out PORT``
     - JACK port to play the click to (repeatable).  Default: ask.
   * - ``--in PORT``
     - JACK port the click comes back on.  Default: the capture port
       feeding loop 1 (the one that hears the click best, if several), else
       the first physical capture port.
   * - ``--bpm N``
     - Click tempo (default 120).
   * - ``--level X``
     - Click level, 0-1 (default 0.3, about -10 dB below full scale).
   * - ``--max-latency MS``
     - Largest round trip searched (default 250 ms).
   * - ``--tolerance MS``
     - Difference treated as nothing to fix (default 1 ms).
   * - ``--sl-host``, ``--sl-port``, ``--sl-client``
     - Where SooperLooper is: OSC host and port (default ``127.0.0.1``,
       ``9951``) and JACK client name (default ``sooperlooper``).
   * - ``--session-file FILE``
     - The session file SooperLooper loads at startup, to save new values
       in.  Remembered for later runs.
   * - ``--reported``
     - Quick values from JACK's reports; no clicks.
   * - ``--no-monitor-check``
     - Skip measuring the path SooperLooper is heard through; use the
       latency JACK reports for it.
   * - ``--no-sl``
     - Measure the round trip only; leave SooperLooper alone.
   * - ``--yes``
     - No questions: use the suggested outputs; apply or save only with
       ``--apply`` / ``--save``.
   * - ``--apply``, ``--save``
     - With ``--yes``: apply the measured-best values; with ``--save`` and
       ``--session-file`` too, also save them in that session file.

**Limits.**  It measures loop 1 and applies the result to every loop, so
it assumes all loops are recorded and monitored the same way.  The click
must come back on the same audio interface it was played from; two
separately clocked devices drift against each other.  It tests a
quantized recording, not the timing of a live pedal press (trigger
latency).

**Platforms.**  Tested on Linux with jackd (started by QJackctl).  It is
written to run anywhere JACK, Python 3 and the ``jack`` module do, but is
untested elsewhere:

- *Linux with PipeWire's JACK:* the measurements should work.  PipeWire
  has no extra input/output latency options, so the advice to change them
  does not apply there; adjust the device's latency in PipeWire's own
  settings instead.
- *macOS:* should work, with JACK installed.  The advice gives only the
  total change, since JACK's current extra latencies can be read only
  through jackdbus, which is Linux-only.
- *Windows:* not installed by the Windows build; running the script
  directly should work where JACK and the ``jack`` module do, with the
  same limitation as on macOS.

The file picker needs a graphical desktop; elsewhere the file name is
typed.

Compile-Time Flags
------------------

All flags are collected here for reference.  Each is also documented at its
definition site.  Flags in ``plugin.cc`` are purely local to that translation
unit.  Two flags are defined in headers rather than the ``.cpp``/``.cc``
files they primarily affect: ``ALB_JACK_BBT_TRACKING`` in
``audio_driver.hpp``, because it controls struct layout visible to
multiple translation units,
and ``ALB_SYNC_PULSE_LOG`` in ``engine.hpp``, for the same reason (it adds
a member to ``Engine``). ``ALB_TEMPO_ONLY_WHEN_ROLLING``,
``ALB_RT_SAFE_COND_SIGNAL`` and ``ALB_MIDI_SYNC_CLEAR_FIX`` are local to
``engine.cpp``.

.. list-table::
   :header-rows: 1
   :widths: 35 20 15 30

   * - Flag
     - File
     - Default
     - Effect
   * - ``LATE_HIT_WINDOW_MS``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_late_hit_window_ms`` (ms cap).
       Overridden at runtime by the session file or OSC ``/set``.  Do not
       use directly in logic; read ``g_late_hit_window_ms`` instead.
   * - ``LATE_HIT_MAX_QUANT_UNIT_FRACTION``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_late_hit_max_quant_unit_fraction``
       (fraction cap).  100.0 effectively disables the fraction cap so the
       ms cap always governs.  Overridden at runtime by the session file or
       OSC ``/set``.
   * - ``RECORD_FAILSAFE_SECONDS_DEFAULT``
     - ``plugin.cc``
     - ``0.0f``
     - Compile-time initial value for ``g_record_failsafe_seconds``.  ``0``
       means the failsafe ships **off**; a record left running in
       ``STATE_RECORD`` past this many seconds auto-fires
       ``cancel_record_mute``'s branch (see `New Feature: record failsafe`_
       above).  Overridden at runtime by the session file or OSC ``/set``.
   * - ``LATE_HIT_MULTIPLY_CORRECTION``
     - ``plugin.cc``
     - ``1``
     - When 1, the late-hit correction is applied to Multiply end
       (``multStopInWindow`` path).  Set to 0 to disable Multiply-end
       correction only, leaving Record start/stop correction active.
   * - ``MUTE_LATE_HIT_MS_DEFAULT``
     - ``plugin.cc``
     - ``100.0f``
     - Compile-time initial value for ``g_mute_late_hit_ms``.  On by
       default, matching ``LATE_HIT_WINDOW_MS`` exactly (see `New
       Feature: mute/unmute late-hit correction`_ above for why this
       defaults on rather than off, unlike record failsafe).  ``0``
       disables the correction entirely.  Overridden at runtime by the
       session file or OSC ``/set``.
   * - ``PAUSE_FROM_MUTE_FIX``
     - ``plugin.cc``
     - ``1``
     - When 1 (default), a stock 1.7.9 bug fix is active: quantized
       Pause from Muted correctly resolves to Paused instead of
       Playing (see `Quantized Pause from Muted Now Resolves to
       Paused`_ in the appendix, below).  Set to 0 to revert to exact stock (buggy)
       behaviour.  Unlike the other flags/globals here, this is a bug
       fix with an escape hatch, not a tunable parameter -- no
       session-file or OSC equivalent.
   * - ``SYNC_MODE_LATE_HIT_RESET_FIX``
     - ``plugin.cc``
     - ``1``
     - When 1 (default), switching a loop's Sync back on resets the
       late-hit correction's pulse tracking, as a sync source change
       already does (see `Late-Hit Correction Robustness`_).  Fixes a bug
       in this fork's own late-hit code.  Set to 0 to remove the reset.
   * - ``SYNCWDEBUG``
     - ``plugin.cc``
     - *(commented out)*
     - Master switch for ALL debug output.  When defined: a sentinel
       (``sl_loaded.txt``) is written at binary-load time; a debug log
       (``sl_debug.log``) is created and ``SL_LOG`` startup banners are
       written to it; ``SYNCWDBG()`` / ``SL_LOG_RT()`` per-press messages are
       written with an ``fflush()`` after every call.  Both files land in
       ``$HOME/.sooperlooper/logs/`` (created if it does not already exist --
       SooperLooper's own per-user directory, same one ``ALB_SYNC_PULSE_LOG``
       below uses), falling back to ``/tmp`` if ``$HOME`` is unset or that
       directory can't be created; the debug log falls back further to
       ``stderr`` as a last resort.  When **not** defined (default): no files
       are created at all; ``SL_LOG`` and ``SL_LOG_RT`` expand to nothing;
       ``SYNCWDBG(...)`` expands to nothing; the only unconditional output is
       the ``fprintf(stderr, ...)`` banner in ``engine.cpp`` after session load.
       Must be commented out in any production / performance build.  When
       enabled, the ``fflush()`` in ``SL_LOG_RT`` causes an immediate kernel
       ``write()`` on every button press, which is not RT-safe and will cause
       xruns.
   * - ``ALB_RT_SAFE_COND_SIGNAL``
     - ``engine.cpp``
     - defined
     - **When defined (default):** three ``pthread_cond_signal()`` calls in
       ``generate_sync()`` are suppressed; the main event loop wakes via its
       existing ``timedwait`` timeout instead.  Reduces xruns on transport
       locate events.  **When not defined:** stock behaviour — signals fire
       from the RT callback.
   * - ``ALB_MIDI_SYNC_CLEAR_FIX``
     - ``engine.cpp``
     - defined
     - **When defined (default):** with MIDI clock sync, the sync signal is
       cleared up to every clock event, so an old boundary mark cannot
       linger and make a waiting recording start at once.  A stock bug
       fix.  **When not defined:** stock behaviour -- cleared only up to
       boundary ticks.
   * - ``ALB_TEMPO_ONLY_WHEN_ROLLING``
     - ``engine.cpp``
     - defined
     - **When defined (default):** tempo is updated from JACK Transport only
       while rolling; during locate transitions the existing ``_tempo`` is
       preserved until rolling resumes, preventing non-RT-safe ``set_tempo()``
       calls from firing on every buffer during a locate.  **When not defined:**
       stock behaviour — tempo updated on every cycle regardless of state.
   * - ``ALB_JACK_BEAT_TRACKING``
     - ``engine.hpp``
     - defined
     - **When defined (default):** JackSync pulses follow a beat count
       carried through tempo changes, and ``TransportInfo`` is
       value-initialised (see `JackSync Beat Tracking`_).  **When not
       defined:** stock JackSync pulse placement.
   * - ``ALB_JACK_BBT_TRACKING``
     - ``audio_driver.hpp``
     - *(commented out)*
     - **When defined:** the beat count also follows the timebase master's
       own bar/beat/tick position, where the master publishes a changing
       one (a DAW, or Hydrogen in song mode).  Adds the bar/beat/tick
       fields to ``TransportInfo``, and the master's position to the
       pulse log.  Not yet tried with a real master.  **Needs**
       ``ALB_JACK_BEAT_TRACKING`` **defined too;** the build stops with an
       error otherwise.  **When not defined (default):** no effect.
   * - ``ALB_CYCLE_CHANGE_REANCHOR``
     - ``engine.hpp``
     - defined
     - **When defined (default):** when Eighths/Cycle changes while JACK or MIDI
       clock sync is running, the new cycles start at the old grid's
       nearest boundary (a bar line, if the old cycle was a bar), as they
       already do with Internal sync.  A Quantize change alone does not
       move them; a relocate or MIDI Start counts from the start again.
       Tested, not yet tried live.  **Needs** ``ALB_JACK_BEAT_TRACKING``.
       **When not defined:** new cycles count from transport
       position 0 (JACK) or the last MIDI Start, as in stock.
   * - ``ALB_SYNC_PULSE_LOG``
     - ``engine.hpp``
     - *(commented out)*
     - A diagnostic log for investigating JackSync timing problems, in the
       same spirit as ``SYNCWDEBUG`` above -- a maintainer's debugging aid,
       not something a general build should enable by default.  **When
       defined:** logs one line per JackSync cycle-boundary pulse (frame,
       tempo, and bar/beat fields when available), plus a line for each
       transport relocate and each tempo change, to
       ``$HOME/.sooperlooper/logs/sl_sync_pulse.log``
       (created if needed, falling back to ``/tmp`` only if ``$HOME`` is
       unset).  RT side only pushes a POD through a lock-free
       ``RingBuffer``, same pattern as ``_sync_queue``; the actual file
       write happens on the non-RT ``mainloop()`` thread, never the RT
       callback -- so unlike ``SYNCWDEBUG`` this carries no real RT-safety
       cost even when enabled, but it still defaults off, since the
       reason to keep it off is who it's for, not what it costs.
       **When not defined (default):** none of this struct/queue/file
       exists; nothing is compiled in.

Appendix: Minor Fixes and Implementation Details
--------------------------------------------------

Smaller corrections, design notes, and implementation-level detail that
don't carry the same weight as the main narrative above -- collected here
so they don't crowd the sections a first-time reader actually needs.

Where the Changes Are in the Source
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

| **Base version:** SooperLooper 1.7.9
| **Modified files:** ``src/plugin.cc``, ``src/plugin.hpp``, ``src/engine.cpp``,
| ``src/engine.hpp``, ``src/looper.cpp``, ``src/looper.hpp``, ``src/audio_driver.hpp``,
| ``src/jack_audio_driver.hpp``, ``src/jack_audio_driver.cpp``, ``src/event.hpp``,
| ``src/command_map.cpp``, ``src/event.cpp``, and some test files in
| ``src/test_plugin``.
| **New files** (not modifications, so not listed above) are documented in
| their own sections: ``src/big_state_display/`` in `New Tool: slbigstate`_,
| ``src/beat_tracker.hpp`` in `JackSync Beat Tracking`_, new test files in
| `Test Suite`_.
| **All new code sections** are bracketed with ``albBegin`` / ``albEnd`` markers.
| In ``plugin.cc`` and ``plugin.hpp`` the markers are ``//====== albBegin`` /
| ``//====== albEnd``; in all other files they are ``//------ albBegin`` /
| ``//------ albEnd``. Search for ``albBegin`` to locate every changed block.

Late-Hit Record Start and Stop: Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

"Sync active" means both conditions together: the Sync checkbox controls
whether operations wait for boundaries at all (``fSyncMode`` 1; Rel Sync
is 2, and the corrections are off there); the sync
source selection determines what signal arrives on the sync input and
therefore when ``lGlobalSamplesSinceSync`` resets.  With no source, *T*
(the samples since the last boundary) grows without bound and the window
never fires regardless of the checkbox.

**Late start.**  The press would normally go to ``STATE_TRIG_START`` and
wait for the next boundary.  Instead:

1. The loop buffer is created immediately via ``pushNewLoopChunk()``.
2. The *T* samples of audio that elapsed since the boundary are copied
   ("backfilled") from the continuous input ring buffer into the start of
   the loop buffer at positions ``[0 .. T)``.
3. The record position ``dCurrPos`` is set to *T*, so live recording
   continues from position *T* onward, seamlessly after the backfilled audio.
4. The engine enters ``STATE_RECORD`` directly rather than
   ``STATE_TRIG_START``.

**Late stop.**  The press would normally go to ``STATE_TRIG_STOP`` and wait
for the next boundary, producing a loop that is *T* samples too long and
whose tail drifts on every repeat.  Instead:

1. The loop length is trimmed by *T* samples: ``lLoopLength = dCurrPos − T``.
2. The play position ``dCurrPos`` is set to ``lLoopLength + lOutputLatency +
   lInputLatency + T`` -- the same formula as the normal ``STATE_TRIG_STOP``
   path, plus *T* to compensate for having entered ``STATE_PLAY`` *T*
   samples late in real time.
3. The engine enters ``STATE_PLAY`` directly.

After the first natural wrap, ``dCurrPos`` lands at ``lOutputLatency +
lInputLatency`` -- identical to a normal on-time loop.  No rewind or special
first-play handling is needed.

**Gating.**  All new code paths are gated by ``withinWindow`` /
``stopWithinWindow`` / ``multStopInWindow``.  These flags are false when the
press is on time, when sync is off (``fSyncMode == 0``), or when no sync
pulse has arrived recently (*T* exceeds the window), and the engine then
behaves identically to the unpatched version.  Quantize mode is not a
condition.

Record Failsafe Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Not a new command: a second, automatic way to reach the same
``CANCEL_RECORD_MUTE`` branch, so it inherits that command's guard table and
fade-residue fix unchanged.

Which recordings qualify: ``bRecordEnteredFromActivePlay``
(a field on ``SooperLooperI``) is set once, at the moment ``RECORD`` is
pressed, to ``(state == STATE_PLAY || state == STATE_ONESHOT)`` -- true only
when an actively playing, audible loop existed and was unmuted at that instant.

Implementation: a sample counter, ``lRecordFailsafeSamples`` (a field on
``SooperLooperI``), accumulates ``SampleCount`` once per buffer while
``state == STATE_RECORD`` **and** ``bRecordEnteredFromActivePlay`` is true,
and resets to 0 the instant state is anything else -- so it measures
unbroken time in that one state, only for a take that qualifies, and never
leaks across separate takes. A flag, ``bRecordFailsafeFired``, is true when
that counter exceeds ``g_record_failsafe_seconds * fSampleRate`` and is ORed into
the existing ``lMultiCtrl == MULTI_CANCEL_RECORD_MUTE`` branch condition in
``plugin.cc`` -- zero new state-transition logic. Deliberately **not**
gated on ``STATE_TRIG_STOP``: that state is a brief, self-resolving wait
for the next quantize boundary (at most one loop cycle), unlike
``STATE_RECORD``, which has no automatic exit at all.

The default is ``RECORD_FAILSAFE_SECONDS_DEFAULT`` = ``0.0f`` (off); any
value at or below zero disables it, matching the late-hit window's own
"0 disables" convention.

Same non-LADSPA-port mechanism as ``g_late_hit_window_ms``, for the same
reasons (see `Why Not a LADSPA Port`_ below): a plain ``extern`` global,
saved/loaded as the ``record_failsafe_seconds`` property in the
``<Globals>`` node by ``engine.cpp``'s ``save_session``/``load_session``.

Files: ``plugin.hpp`` (``lRecordFailsafeSamples`` and
``bRecordEnteredFromActivePlay`` fields), ``plugin.cc``
(``RECORD_FAILSAFE_SECONDS_DEFAULT`` / ``g_record_failsafe_seconds``
globals, the ``bRecordEnteredFromActivePlay`` set statement at the top of
``MULTI_RECORD``, the counter-update statements, the
``|| bRecordFailsafeFired`` addition), ``engine.cpp`` (``extern``
declaration, XML load/save, OSC get/set, startup banner). No new
``Event::command_t``, no ``command_map.cpp`` entry, and no pedal/MIDI
mapping -- purely internal to the engine, invisible to any external MIDI
mapping and the ``.slb`` bindings.

Mute/Unmute Late-Hit Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

It covers the ``MUTE``, ``MUTE_ON`` and ``MUTE_OFF`` commands.

Unlike Record's correction, neither direction reconstructs buffer
content: ``dCurrPos`` keeps advancing every buffer while muted (the
loop's content is already sitting at the correct phase throughout), so
this is a pure branch-selection decision -- immediate vs. wait -- not a
backfill/trim operation.

Same non-LADSPA-port mechanism as the other three globals (see `Why Not
a LADSPA Port`_ below): a plain ``extern`` global, saved/loaded as the
``mute_late_hit_ms`` property in the ``<Globals>`` node by
``engine.cpp``'s ``save_session``/``load_session``.

Files: ``plugin.cc`` (``MUTE_LATE_HIT_MS_DEFAULT``/``g_mute_late_hit_ms``
globals, the shared window check at the top of the ``MULTI_MUTE``/
``MULTI_PAUSE`` case, one added disjunct in the unmute branch and one in
the entering-mute branch), ``engine.cpp`` (``extern`` declaration, XML
load/save, OSC get/set). No new ``Event::command_t``, no
``command_map.cpp`` entry, and no pedal/MIDI remapping.

Record Late-Hit Window: Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Both limits are evaluated each buffer by ``effectiveLateHitWindow()`` in
``plugin.cc``, which returns the smaller value in samples:
``min(g_late_hit_window_ms, g_late_hit_max_quant_unit_fraction × cycle_samples)``.  The ms cap is
additionally clamped to ``lInputBufSize − 1`` (32767 samples, approximately
682 ms at 48 kHz), ensuring the window never exceeds the depth of the input
ring buffer regardless of what ``late_hit_window_ms`` is set to.  If a key
is absent from the session file, the compile-time defaults
``LATE_HIT_WINDOW_MS`` (100 ms) and ``LATE_HIT_MAX_QUANT_UNIT_FRACTION``
(100.0) are used.

JACK MIDI Input: Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``midi_out`` port is registered in ``engine.cpp``; it generates the
clock with a phase accumulator for sample-accurate tick placement, and
sends MIDI Start (``0xFA``) and Stop (``0xFC``).

JACK MIDI events carry an exact sample offset within the current buffer
(``jack_ev.time``). The engine uses this offset to split the processing buffer at the
event boundary, running audio up to the event position before applying the command. This
is the standard JACK buffer-splitting technique for sample-accurate event handling.

A JACK MIDI command arriving at sample *N* within a buffer is processed with
``_pending_event_offset = N``.  The ``run()`` call that follows sees this non-zero
offset and zeros ``TriggerLatency`` for that segment, on the reasoning that the
event is already placed at its exact sample.  ALSA MIDI and OSC events, which carry
no sub-buffer position, always have ``_pending_event_offset = 0`` and keep the full
``TriggerLatency``; so does a JACK MIDI event at offset 0.  ``TriggerLatency`` is
read only where overdub, multiply and insert (and replace and substitute, through
overdub) start taking input (``lFramesUntilInput = input_latency −
trigger_latency``); plain record does not use it.

**MIDI clock on the JACK MIDI input.**  Clock, Start and Stop messages on
``midi_in`` go through the same path as commands, into SooperLooper's MIDI
clock handling in ``generate_sync()``.  That code measures the tempo from
event timestamps, which ALSA provides and the JACK MIDI path does not.
Events created with a buffer offset now carry a timestamp of 0 (stock left
it uninitialised, ``event.cpp``), and ``generate_sync()`` times such events
from a 64-bit frame count, ``(_total_frames + offset) / sample_rate`` --
seconds, like ALSA's host-time stamps; only differences between them are
used.  ALSA events are unchanged.

The same branch (``ALB_MIDI_SYNC_CLEAR_FIX``) now clears the sync buffer up
to every clock event.  Stock cleared it only up to boundary ticks, so a
boundary mark written early in one buffer could survive into later ones
and read as a pulse in every buffer.

JackSync Beat Tracking: Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When ``ALB_JACK_BEAT_TRACKING`` is defined in ``engine.hpp`` (the default;
field-tested over a full live session with frequent tempo changes),
``generate_sync()`` places JackSync pulses, and
the GUI's beat indicator, from a running beat count instead of
``fmod(framepos, _tempo_frames)``:

* each rolling cycle it advances by ``nframes × bpm / (60 × SR)``, using the
  tempo JACK reports for that cycle, so a tempo change alters the rate but not
  the position;
* while stopped it does not advance, and a stop/start that resumes at the
  same frame continues where it left off;
* on a relocate it re-anchors to ``framepos × bpm / (60 × SR)``, which is where
  a master that recomputes its position from the frame puts its beat.

At constant tempo since the last relocate it gives the same pulses as the
stock formula, to within one sample.

It also changes how ``TransportInfo`` is read: the struct is
value-initialised (stock reads some fields uninitialised), the
tempo is taken from JACK only when the master publishes one, and the
backwards-locate reset of the loops' relative-sync counters uses the
tracker's record of the previous frame.

With ``ALB_CYCLE_CHANGE_REANCHOR`` (on by default), the cycle grid has an
origin, in beats, that is 0 unless Eighths/Cycle has changed the cycle
length since the last relocate: ``generate_sync()`` then calls
``reanchor_cycle()`` with the old step, which moves the origin to the old
grid's boundary nearest the cycle's start, and passes the origin to
``boundaries()`` for the cycle grid only (the beat indicator still counts
from 0).  A relocate resets it.  The MIDI clock branch does the same in
ticks, with the origin reset on Start/Continue.

The arithmetic lives in ``src/beat_tracker.hpp``; ``src/test_beat_tracker``
has its unit tests and a JACK integration test.  ``make check`` there
builds and runs the unit tests twice, the second time with
``ALB_CYCLE_CHANGE_REANCHOR`` defined, which adds its tests.

slbigstate: Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~

**How it talks to the engine:** plain OSC, the same generic mechanism
``tempo``/``smart_eighths``/etc. already use elsewhere in this patch.
For each loop shown, it sends one ``/sl/<loop>/register_auto_update``
request for that loop's ``state`` control; the engine then pushes an
update whenever that value actually changes (not on a fixed schedule),
and each loop's registration is refreshed defensively every 20 seconds
so the display recovers on its own if the engine restarts *with the
same loop count* -- N loops means N independent registrations sharing
one process and one window, not N separate processes.

**If the loop count actually changes** -- a loop added or removed live
from within a running engine (SooperLooper's own ``/loop_add``/
``/loop_del``, e.g. from ``slgui`` itself), *or* the engine coming back
from a restart with a different count already set from its own
startup -- the *auto-detect* stack window (no ``-l``/``-L`` given, the
default) notices and rebuilds itself within a few seconds, tearing
down and rebuilding its rows to match, in either direction, without
needing to be restarted itself. Covers both cases because it uses two
mechanisms together: the engine's own config-changed push (immediate,
for a live add/remove on an engine it is already watching) plus a
periodic active re-check (catches a *new* engine instance that already
had a different count from the moment it started, which a passive push
alone cannot -- nothing "changes" for it to push if the count was
already different before anyone was watching). An explicit ``-l``/
``-L`` window deliberately does not do any of this -- you asked for a
specific loop or set, so a count change elsewhere leaves it alone; if a
loop *you explicitly chose* is itself removed, that row just freezes at
its last known state, same as a plain engine restart with the same
loop count but different content.
When no ``-l``/``-L`` is given, one extra,
one-shot exchange happens first, before any window opens: a plain
``/ping`` to ask the engine how many loops it currently has (the same
reply -- host, version, loop count -- ``slgui`` itself already reads
for its own reconnect handling), so the display knows how many rows to
build; a 2-second timeout with no reply falls back to showing loop 0
alone rather than failing to start. No connection to ``libslcore.a`` or
any of this patch's engine-side code either way -- it is an
independent client of the engine's existing OSC interface, and works
identically against a GUI-managed engine (started by ``slgui``) or a
standalone headless ``sooperlooper``.

When the first ``/ping`` gets no answer within 2 seconds, the auto-detect
window starts with loop 0 and prints a note to that effect; its periodic
re-check then picks up the real count once the engine answers.

Late-Hit Correction Robustness
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A slave channel in internal sync mode always uses the stock timing path
(``bLateHitBypassed``), preventing L/R divergence from independently
computed *T* values.  Changing the sync source resets the correction's
tracked state (``Looper::reset_late_hit_state()`` clears
``bSyncPulseReceived``, ``lGlobalSamplesSinceSync``, and
``lLastSyncPeriodSamples``), so stale period data from the old source
can't affect the first presses on the new one.  Switching a loop's Sync
off and on again resets the same fields (``SYNC_MODE_LATE_HIT_RESET_FIX``):
while Sync is off the time-since-pulse count stops, and without the reset
a press soon after Sync comes back on could be corrected from that stale
count.

``cancel_record_mute`` Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The situations in the user-facing table map to engine states:

.. list-table::
   :header-rows: 1
   :widths: 40 60

   * - Situation
     - Engine state
   * - recording
     - ``STATE_RECORD``
   * - waiting for the beat to start a recording
     - ``STATE_TRIG_START``
   * - waiting for the beat to end a recording
     - ``STATE_TRIG_STOP``
   * - anything else
     - any other state

.. important::

   The two pending states must be distinguished, and the guard must key on
   ``STATE_TRIG_START`` rather than ``waitingForSync`` -- that flag is also
   set for deferred *mute* and *play* transitions.  A ``state ==
   STATE_RECORD`` guard alone silently does nothing in the quantized case
   the feature exists for; a loose "anything record-ish" guard destroys a
   take in the case it is supposed to tolerate.

Internally the command discards the take just captured and restores the
previous loop, muted.  That is implementation, not intent: the performer
is not undoing a mistake but choosing silence over play, on the same pedal,
in one motion.

It lives in the engine because whatever maps the pedal's MIDI cannot see
SooperLooper's state: it cannot tell a committed record from one still
pending on a boundary, or whether the loop was already muted, so a
retraction done there is unreliable.  The engine has ``state``,
``waitingForSync``, ``wasMuted`` and ``headLoopChunk``, and acts in a
single realtime dispatch.

A state-based remap in ``plugin.cc`` (the same idiom as ``MULTI_MUTE_ON``),
rewriting ``lMultiCtrl`` rather than adding a dispatch case:

- ``STATE_RECORD`` -> set ``wasMuted``, set the four mute fade deltas, remap
  to ``MULTI_UNDO``.  The existing undo body then lands in ``STATE_MUTE``.
- ``STATE_TRIG_START`` -> cancel in place (clear ``waitingForSync`` /
  ``nextState``, restore state from ``headLoopChunk``) and set
  ``lMultiCtrl = -1``.  It must **not** remap to ``MULTI_UNDO``: no chunk is
  pushed on the pending path (``pushNewLoopChunk`` lives in the late-hit
  branch only), so the undo would pop the existing take.
- anything else -> ``lMultiCtrl = -1``.

The fade deltas mirror ``transitionToNext``'s ``STATE_MUTE`` case, including
the ``*Src*`` pair.  Setting only the non-Src pair leaves stale source-loop
gains that corrupt a later ``MULTIPLY`` (``beginMultiply`` sets no fade
deltas of its own; ``beginOverdub`` does, which is why overdub is immune).

Files: ``event.hpp`` (enum appended before ``LAST_COMMAND``),
``command_map.cpp`` (one line -- OSC and ``.slb`` bindings follow
automatically), ``plugin.cc`` (``#define``, drift ``static_assert``\ s, the
remap branch).

.. warning::

   The enum **must** be appended at the end.  Elsewhere, ``plugin.cc``
   hardcodes ``MULTI_UNDO``..``MULTI_PAUSE_OFF`` as integer literals 0-23 mirroring the
   first 24 ``Event::command_t`` values; inserting inside that range shifts
   the enum but not the literals, silently remapping commands **with no
   compile error**.  Six ``static_assert``\ s now guard this.


``cancel_overdub_undo`` Implementation
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The command is a remap in ``run()``, next to ``cancel_record_mute``'s.
Each command dispatch records whether it is an overdub press, and the
state it found the loop in (``lLastOverdubFrom``; any other command resets
it), so ``cancel_overdub_undo`` knows what the press just before it did.
From there:

- *A stop* (it found the loop overdubbing): a stop still waiting for the
  beat is cancelled and the command becomes an ordinary ``undo``, which
  discards the running overdub; a stop that happened at once becomes an
  ordinary ``undo`` of the layer just committed.
- *A start from playing*: the new overdub layer is discarded at once
  (``discardEagerOverdubLayer()``), then an ordinary ``undo`` removes the
  layer below it, if that is not the first take.  A start still waiting
  for the beat is cancelled, then the same ``undo``.
- *A start from muted*: as above, but the undo is done directly and the
  loop fades back to muted, because ``undo``'s own muted branch only holds
  the play level, which the overdub had raised.
- *A record ended into an overdub*: the overdub layer is discarded and the
  loop plays the take; if the change was still waiting for the beat, the
  record simply ends there as usual.

Discarding the new layer is instant rather than crossfaded.  It is not
heard: the overdub writes behind the play position, by the latency, so at
the play position the discarded layer and the one below it hold the same
audio.  Input writing stops at once, so the layer below -- which may later
be undone and redone -- takes none of it; and the discarded layer is
unlinked from the undo history, so ``redo`` cannot bring it back.  Tests:
``testdef_cancel_overdub_undo.py``, one per case above, each checking the
loop's audio, including after ``redo``.

Quantized Pause from Muted Now Resolves to Paused
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**A stock SooperLooper 1.7.9 bug, not introduced by this fork** --
confirmed present verbatim in the untouched upstream reference copy
kept in this tree. Found by accident while testing the mute/unmute
late-hit feature earlier in this document, then fixed on its own merits since it is
unrelated to late-hit correction.

**The bug:** with mute-quantize on and sync active, pressing Pause while
a loop is Muted has to wait for the next sync boundary before acting --
the same "wait for sync" mechanism mute-quantize always uses. Once that
boundary arrived, the loop ended up **Playing**, not **Paused**. The
performer pressed Pause and got audible playback instead.

**Why this was confidently a bug, not intended behaviour:** SooperLooper
already handles this exact transition correctly when it acts
*immediately* (mute-quantize off, or sync/quantize off) -- pressing
Pause while Muted goes straight to Paused, explicitly, and the reverse
(pressing Mute while Paused) goes straight back to Muted, also
explicitly. Both directions, named in the code, deliberate. The
*deferred* "wait for sync" version of the same logic only implemented
one of those two symmetric directions; the other silently fell through
to the same generic branch used for an ordinary unmute. Quantization is
only supposed to delay *when* an action happens, never change *what*
the action resolves to -- a deferred press and an immediate press
pressing the same button in the same state should always end up in the
same place, just possibly later. Implementing only one of two mirrored
cases the code already implements immediately above is the signature of
an omission made when that deferred branch was originally written, not
a deliberate design choice.

**The fix has two parts.** The obvious one -- adding the missing case so
a deferred Mute->Pause resolves to Paused -- turned out to have a
non-obvious second half: the engine also tracks whether a loop *was*
muted before it changed state, so that a later Undo can correctly put it
back in Mute rather than Play. That tracking needed fixing too, or the
primary fix alone would have landed in the right state but with an Undo
fired later resuming to the wrong place. Both parts sit inside the same
generic function the engine already uses for every other deferred state
transition, so the fix is small and self-contained -- about four lines
total, not counting the safety switch below.

**Guarded by a compile-time flag, off if wanted.** ``PAUSE_FROM_MUTE_FIX``
(``plugin.cc``, default ``1``) wraps both parts, matching
``LATE_HIT_MULTIPLY_CORRECTION``'s own established role in this file: a
correction with its own independent escape hatch, in case it ever needs
pulling without touching anything else. Set it to ``0`` and rebuild to
revert to exact stock (buggy) behaviour. This is compile-time, not a
session-file/OSC value like the four late-hit globals -- there is nothing to
tune, only a bug to have fixed or not, so a rebuild-time switch fits
better than a runtime one.

Why Not a LADSPA Port
~~~~~~~~~~~~~~~~~~~~~

Every other engine parameter travels through the LADSPA port system: ``looper.cpp``
allocates a float, calls ``connect_port()``, and ``plugin.cc`` reads it via a pointer on
``SooperLooperI``. That is the correct mechanism for a value that needs full GUI exposure
(a slider in ``slgui``) or push-on-change OSC updates.

The four globals themselves -- ``g_late_hit_window_ms``,
``g_late_hit_max_quant_unit_fraction``, ``g_record_failsafe_seconds``, and
``g_mute_late_hit_ms`` -- are intentionally **not** LADSPA ports. Each is a plain
file-scope global in ``plugin.cc``, declared ``extern`` in ``engine.cpp``, written during
session load from the ``<Globals>`` node, and also reachable through the engine's generic
``/set``/``/get`` dispatch (see `OSC Control`_ earlier in this document). This still avoids
the LADSPA port descriptor table and ``slgui`` wiring. The trade-off is that it only works
in the standalone build where both translation units share an address space. When
``plugin.cc`` is loaded as a true LADSPA plugin inside a DAW, ``engine.cpp`` is absent and
the value stays at the compile-time default for the session, which is acceptable
behaviour. If any of the four is ever promoted to a GUI control, the correct migration
path is to add a proper LADSPA port and remove the global.

For OSC this is a smaller step than full exposure: no ``command_map.cpp``
``add_global_control`` registration was added for any of the four, so none
appear in ``slgui``, and ``/register_auto_update`` -- which keys a
listener's registration off ``CommandMap::to_control_t()`` -- will not
target them by name.  A negative value sent to ``/set`` is ignored by the
same ``value >= 0.0f`` guard ``load_session`` applies to the session-file
keys, needed because a negative float would otherwise reach an
``(unsigned long)`` cast in ``plugin.cc``'s threshold checks.

Live-Performance Audit
~~~~~~~~~~~~~~~~~~~~~~~

A companion document, ``live_performance_review.rst``, is a
live-performance-suitability code audit of the modifications, graded by
severity, live-performance impact, and confidence, with every item's
resolution status tracked explicitly.  It was written before most of its
own findings were addressed, so it now reads as a closed-out historical
record rather than a live list of concerns -- every CRITICAL item is
either implemented or deliberately deferred with stated rationale.  It is
kept, and referenced here, because two source-code comments (in
``looper.cpp`` and ``plugin.cc``) cite specific findings by issue number,
e.g. ``C1``.


Glossaries
----------

Some terminology and code variables related to the patch.

General Terminology
~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Term
     - Description
   * - sync boundary
     - The moment when the looper's sync source crosses a beat or cycle boundary.
       Sync boundaries arrive as a short pulse on the looper's internal sync bus
       (``pfSyncInput[n] > 1.5``).
   * - sync mode (per channel)
     - Controls how a channel responds to a button press. Mode 0 (sync checkbox
       off) means "act immediately on the press." Mode 1 (sync checkbox on) means
       "wait for the next sync boundary before acting." The late-hit correction only
       activates in mode 1.
   * - sync source
     - The signal that generates sync boundaries. Can be JACK Transport, MIDI
       clock, an internal tempo generator, or another loop channel. With no source
       selected, no sync pulses arrive and the late-hit correction never fires.
   * - late-hit window
     - The maximum elapsed time between a sync boundary and a button press for the
       press to qualify as "late" rather than simply off-beat. Bounded by both an
       absolute millisecond cap and a fraction of one full quantize cycle at the
       current tempo and Eighths/Cycle setting; whichever is smaller applies.
   * - T
     - The number of audio samples that elapsed between the most recent sync
       boundary and the moment the button press was detected. The correction undoes
       this displacement: T samples are backfilled on a late start, or T samples
       are trimmed on a late stop.
   * - backfill
     - The process of copying the most recent T samples from the continuous input
       ring buffer into the beginning of the newly created loop buffer, so that the
       loop contains audio from the sync boundary rather than from the press time.
   * - internal sync mode
     - A configuration where the looper uses its own loop playback as the sync
       source rather than an external clock. In SooperLooper's GUI this corresponds
       to selecting Loop 1 as the sync source with the sync checkbox on. Late-hit
       correction is disabled on slave channels in this mode to prevent stereo
       channel divergence; both channels use the stock timing path instead.

Constants and Globals (``plugin.cc``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Variables marked **[NEW]** were added by this patch. All others are pre-existing
SooperLooper variables whose roles are clarified here because the patch depends on them.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``LATE_HIT_WINDOW_MS``
     - **[NEW]**
     - ``#define float``
     - Compile-time default for the late-hit window in milliseconds. Remains defined so it
       can initialise ``g_late_hit_window_ms`` and appear in startup banners. Do not use
       directly in runtime logic — use ``g_late_hit_window_ms``.
   * - ``LATE_HIT_MAX_QUANT_UNIT_FRACTION``
     - **[NEW]**
     - ``#define float``
     - Compile-time default for the quantize-unit fraction cap (default 100.0,
       which effectively disables the fraction cap so the ms cap always governs).
       Initialises ``g_late_hit_max_quant_unit_fraction``.
   * - ``g_late_hit_window_ms``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Runtime window size in milliseconds. Initialised from ``LATE_HIT_WINDOW_MS``.
       Overwritten by ``engine.cpp`` during session load if ``late_hit_window_ms`` is
       present in the ``<Globals>`` node of the session file. Read at buffer-processing
       time via ``effectiveLateHitWindow()``. See the comment block in ``plugin.cc`` for
       the full rationale on why this is a global rather than a LADSPA port.
   * - ``g_late_hit_max_quant_unit_fraction``
     - **[NEW]**
     - ``float`` (file-scope global)
     - Fraction of one full quantize cycle that defines the upper limit of the late-hit
       window. ``effectiveLateHitWindow()`` multiplies this directly by
       ``lLastSyncPeriodSamples`` (the full cycle period in samples). At 0.25 the
       window is capped at 25 % of the current cycle length at any tempo or
       Eighths/Cycle setting. The compile-time default is 100.0, making the ms cap
       always the binding limit until a lower value is set in the session file.
   * - ``effectiveLateHitWindow()``
     - **[NEW]**
     - ``static inline`` function
     - Computes the window in samples as ``min(ms_cap, fraction_cap)``, where
       ``ms_cap = g_late_hit_window_ms × 0.001 × fSampleRate`` (clamped to
       ``lInputBufSize − 1``) and ``fraction_cap = g_late_hit_max_quant_unit_fraction
       × lLastSyncPeriodSamples``. Returns the ms cap alone if
       ``lLastSyncPeriodSamples`` is at its uninitialised sentinel (``ULONG_MAX``).
   * - ``sl_dbg_file``
     - **[NEW]**
     - ``static FILE*``
     - Handle for the debug log file (``$HOME/.sooperlooper/logs/sl_debug.log``,
       falling back to ``/tmp/sl_debug.log``, then ``stderr``). Opened lazily
       by ``sl_dbg_open()``.  Only declared and used when ``SYNCWDEBUG`` is
       defined; absent entirely in production builds.
   * - ``SL_LOG(...)``
     - **[NEW]**
     - macro
     - When ``SYNCWDEBUG`` is defined: calls ``sl_dbg_open()`` if needed, then writes to
       ``sl_dbg_file`` with an immediate ``fflush``.  Use for non-RT messages (startup
       banners, session load).  When ``SYNCWDEBUG`` is not defined: expands to nothing —
       no file is created and no output is produced.
   * - ``SL_LOG_RT(...)``
     - **[NEW]**
     - macro
     - When ``SYNCWDEBUG`` is defined: writes to ``sl_dbg_file`` (if open) and calls
       ``fflush()`` after every write.  Used inside ``SYNCWDBG()`` in the RT callback.
       Not RT-safe: ``fprintf`` acquires a stdio lock and ``fflush`` issues a kernel
       ``write()``; causes xruns when enabled.  The ``fflush`` ensures log data survives
       a crash but is the primary xrun risk.  When ``SYNCWDEBUG`` is not defined:
       expands to nothing.
   * - ``SYNCWDEBUG``
     - **[NEW]**
     - ``#define`` (flag)
     - Master switch for ALL debug output in ``plugin.cc``.  When defined, enables
       the ``sl_loaded.txt`` sentinel, ``sl_debug.log``, startup banners, and
       per-press diagnostics, all under ``$HOME/.sooperlooper/logs/`` (falling
       back to ``/tmp``).  When not defined (default): all ``SL_LOG`` /
       ``SL_LOG_RT`` / ``SYNCWDBG`` calls expand to nothing; no files are
       written.  Commented out by default; must remain disabled in any
       performance build.
   * - ``SYNCWDBG(x)``
     - **[NEW]**
     - macro
     - Expands to ``x`` when ``SYNCWDEBUG`` is defined; expands to nothing otherwise.
       Wraps ``SL_LOG_RT()`` calls in the hot path that are only wanted during debugging.
   * - ``sl_load_probe``
     - **[NEW]**
     - ``static struct SL_LoadProbe``
     - C++ static initialiser that fires when the binary is loaded (before ``main``).
       When ``SYNCWDEBUG`` is defined, writes a ``sl_loaded.txt`` sentinel
       (under ``$HOME/.sooperlooper/logs/``, falling back to ``/tmp``)
       confirming the experimental build is running.  When ``SYNCWDEBUG`` is
       not defined the constructor body is empty and no files are written.


Fields on ``SooperLooperI`` (``plugin.hpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``SooperLooperI`` is the per-instance state struct for the LADSPA plugin. One instance
exists per loop channel.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``lGlobalSamplesSinceSync``
     - **[NEW]**
     - ``unsigned long long``
     - Continuously-running sample counter that increments every sample in **all** engine
       states and resets to zero on each sync pulse (``pfSyncInput[n] > 1.5``). Valid at
       button-press time regardless of which state the engine is in. Contrast with the
       pre-existing ``lSamplesSinceSync``, which only increments during ``STATE_RECORD``
       and ``STATE_PLAY`` and is therefore useless at button-press time from
       ``STATE_OFF``. Widened to ``unsigned long long`` (from ``unsigned int``) to
       eliminate overflow at slow tempos or long gaps between syncs.
   * - ``bSyncPulseReceived``
     - **[NEW]**
     - ``bool``
     - Set to ``true`` when the first sync pulse arrives after instantiation or after a
       sync-source change. Guards against the startup case where
       ``lGlobalSamplesSinceSync`` is counting "time since engine start" rather than
       "time since last pulse." All three window conditions include
       ``&& pLS->bSyncPulseReceived``. Reset to ``false`` by ``reset_late_hit_state()``
       on sync-source change.
   * - ``lLastSyncPeriodSamples``
     - **[NEW]**
     - ``unsigned long``
     - Most recently measured full-cycle sync period in samples. Seeded from
       ``fTempo`` every buffer (BLOCK 7, immune to loop-coordination pulse noise) and
       updated from pulse measurement when a pulse passes the sanity floor (BLOCK 8).
       Used by ``effectiveLateHitWindow()`` to compute the fraction cap. Initialised to
       ``ULONG_MAX`` (sentinel meaning "no period known yet"); reset to ``ULONG_MAX``
       by ``reset_late_hit_state()`` on sync-source change.
   * - ``bLateHitBypassed``
     - **[NEW]**
     - ``bool``
     - Set ``true`` by ``Looper::run()`` on slave channels (index > 0) when in internal
       loop-sync mode (one looper driving another). Prevents late-hit correction on those
       channels to avoid L/R divergence; both channels then behave identically using the
       stock timing path. Set unconditionally each buffer by ``looper.cpp`` BLOCK 5 so it
       self-corrects within one buffer period if sync mode changes.
   * - ``bLateStartPending``
     - removed
     - ``bool``
     - Was present in an earlier design where the late-stop correction applied a different
       formula when preceded by a late start. That distinction was eliminated; the late-
       stop formula is now the same regardless of whether the start was late. Removed.
   * - ``lSamplesSinceSync``
     - pre-existing
     - ``unsigned long``
     - Per-instance sync phase counter. Increments only during ``STATE_RECORD`` and
       ``STATE_PLAY``. **Not used** by the late-hit code for the reason above.
   * - ``lInputBufWritePos``
     - pre-existing
     - ``unsigned long``
     - Write cursor into ``pfInputLatencyBuf``. Points to the **start** of the current
       LADSPA buffer block and is not advanced until the end of ``runSooperLooper()``. The
       backfill calculation relies on this: ``lInputBufWritePos − T`` is the read position
       for the T samples that elapsed since the last sync boundary.
   * - ``lInputBufSize``
     - pre-existing
     - ``unsigned long``
     - Length of ``pfInputLatencyBuf`` in samples (32768, hardcoded). Used as the modulus
       for the wrap-around read in the backfill, and as the hard upper bound on the
       effective window in ``effectiveLateHitWindow()``.
   * - ``lInputBufMask``
     - pre-existing
     - ``unsigned long``
     - ``lInputBufSize − 1``. Used with bitwise AND to wrap indices into
       ``pfInputLatencyBuf``.
   * - ``lBufferSizeMask``
     - pre-existing
     - ``unsigned long``
     - ``lBufferSize − 1`` (power-of-two). Used with bitwise AND to wrap write indices
       into ``pSampleBuf`` during the backfill.


Fields on ``LoopChunk`` (``plugin.hpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``LoopChunk`` represents one recorded loop. Multiple ``LoopChunk``\\ s can be stacked on a
single channel for overdub layers.

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - [STRIKEOUT:``lFirstPlayOffset``]
     - removed
     - ``unsigned long``
     - Was present from an earlier design where the late-stop correction used a per-play
       rewind. That design was abandoned; the field was always 0 and never read. Removed.
   * - [STRIKEOUT:``hasFirstPlayOffset``]
     - removed
     - ``bool``
     - Companion flag to ``lFirstPlayOffset``. Also always ``false``, never read. Removed.
   * - ``dCurrPos``
     - pre-existing
     - ``double``
     - Current read/write position within the loop buffer, in samples. In the late-stop
       path this is advanced by *T* beyond the normal ``STATE_TRIG_STOP`` value to
       compensate for entering ``STATE_PLAY`` *T* samples late in real time.
   * - ``lLoopLength``
     - pre-existing
     - ``unsigned long``
     - Length of the recorded loop in samples. In the late-stop path this is set to
       ``dCurrPos − T`` (trimming the overrun audio) before entering ``STATE_PLAY``.
   * - ``lLoopStart``
     - pre-existing
     - ``unsigned long``
     - Absolute start offset of this loop in ``pSampleBuf``. The backfill writes to
       ``pSampleBuf[(lLoopStart + i) & lBufferSizeMask]`` for ``i`` in ``[0 .. T)``.


Buffer Variables (local to ``runSooperLooper()``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Name
     - Status
     - Type
     - Description
   * - ``lGlobalSyncAtButtonPress``
     - **[NEW]**
     - ``unsigned long long`` (local)
     - Snapshot of ``pLS->lGlobalSamplesSinceSync`` taken **before** scanning the current
       buffer for sync pulses. This is the value used for all window tests. The snapshot
       is necessary because a sync pulse arriving mid-buffer should not retroactively
       change the phase reading for a button press that logically arrived at the buffer
       start.
   * - ``shiftSamples``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Result of ``effectiveLateHitWindow(pLS, eighthPerCycle)``: the effective window in
       samples, bounded by both the ms cap and the fraction cap. Computed fresh at each
       button-press test.
   * - ``T``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Alias for ``lGlobalSyncAtButtonPress`` within each late-hit block. Represents the
       number of samples that have elapsed since the last sync boundary. Reused as the
       trim/backfill size: *T* samples are backfilled on a late start, or *T* samples are
       trimmed on a late stop.
   * - ``withinWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Late-start gate: true when ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && !bLateHitBypassed``. Quantize mode is not a condition; the
       correction fires for any external sync source.
   * - ``stopWithinWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Late-stop gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && loop ≠ NULL && !bLateHitBypassed``.
   * - ``trimmedLength``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Computed loop length after trim: ``(unsigned long)dCurrPos − T``. Protected by a
       guard requiring at least ``xfadeSamples`` of remaining content; if the guard fails,
       the code falls back to ``STATE_TRIG_STOP``.
   * - ``readPos``
     - **[NEW]**
     - ``unsigned long`` (local)
     - Start offset into ``pfInputLatencyBuf`` for the backfill read:
       ``lInputBufWritePos − T``. The ``lInputLatency`` term that appears in the normal
       recording path is absent here: ``pfInputLatencyBuf`` stores samples written at
       physical time, and matching the sample that normal recording would have captured
       from the sync boundary works out to ``lInputBufWritePos − T`` with no additional
       offset. Wrap-around handled for all valid values of T up to ``lInputBufSize − 1``.
   * - ``multStopInWindow``
     - **[NEW]**
     - ``bool`` (local)
     - Multiply-end gate: ``T > 0 && T ≤ shiftSamples && fSyncMode > 0 &&
       bSyncPulseReceived && lCycles ≥ 2 && !bLateHitBypassed``.
       The ``lCycles ≥ 2`` guard prevents decrementing to a zero-length loop. Only active
       when ``LATE_HIT_MULTIPLY_CORRECTION`` is 1.
   * - ``pfInputLatencyBuf``
     - pre-existing
     - ``LADSPA_Data*``
     - The continuous 32768-sample input ring buffer written every block from ``pfInput``.
       The source for the backfill audio.
   * - ``pfSyncInput``
     - pre-existing
     - ``LADSPA_Data*``
     - Per-sample sync signal buffer. A value ``> 1.5`` in any sample position is a sync
       pulse. Scanned every block to maintain ``lGlobalSamplesSinceSync``.
   * - ``xfadeSamples``
     - pre-existing
     - ``unsigned long`` (local)
     - Minimum safe loop length for crossfade purposes (128 samples). Used as the lower
       bound in the trim guard.



Session File Key (``engine.cpp``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Key
     - Status
     - Description
   * - ``late_hit_window_ms``
     - **[NEW]**
     - Floating-point XML property in the ``<Globals>`` node of the ``.slsess`` session
       file — the same node as ``tempo`` and ``eighth_per_cycle``. Written by
       ``Engine::save_session()`` and read by ``Engine::load_session()``. If absent (older
       session files), the compile-time default is used. Edit with a text editor; a
       restart of the engine is required for the change to take effect.
   * - ``late_hit_max_quant_unit_fraction``
     - **[NEW]**
     - Floating-point XML property in the ``<Globals>`` node of the ``.slsess`` session
       file. Written by ``Engine::save_session()`` and read by
       ``Engine::load_session()``. If absent, the compile-time default
       ``LATE_HIT_MAX_QUANT_UNIT_FRACTION`` (100.0) is used. Edit with a text editor;
       a restart is required. Note: the older key ``late_hit_max_8th_fraction`` had
       different units (fraction of one eighth note rather than one full cycle); session
       files containing the old key will use the compile-time default on load.

Architecture Notes
------------------

-  **Why** ``lGlobalSamplesSinceSync`` **instead of** ``lSamplesSinceSync``:
   ``lSamplesSinceSync`` only increments during ``STATE_RECORD`` and ``STATE_PLAY``.
   Pressing Record from ``STATE_OFF`` gives a value of 0, which makes it impossible to
   distinguish an on-time press from a late one. The new counter runs in all states.

-  **Why a pre-buffer snapshot:** LADSPA processes audio in blocks. A sync pulse mid-
   block should not alter the phase measurement for a button press that arrived at the
   block boundary. The snapshot captures the phase as of the start of the current block.

-  **Why backfill rather than a playback offset for late start:** The input ring buffer
   already contains the audio from the boundary onward. Copying it into the loop buffer
   directly is simpler and more robust than skipping the first *T* samples on every play
   pass with a per-play offset.

-  **Why** ``dCurrPos += T`` **rather than** ``lFirstPlayOffset`` **for late stop:** Detecting
   the "first wrap" reliably to apply a rewind proved fragile (the wrap check fires
   inside a ``recenter`` guard at the wrong moment). Advancing ``dCurrPos`` by *T* at
   entry to ``STATE_PLAY`` achieves the same correction with no wrap detection needed.

-  **Why a global for** ``g_late_hit_window_ms`` **rather than a LADSPA port:** See
   `Why Not a LADSPA Port`_ above and the comment block in ``plugin.cc``. The short
   version: it is a setup-time preference, not a real-time control, and avoiding the port
   system keeps the change entirely within ``plugin.cc`` and ``engine.cpp``.

-  **Why the backfill read position is** ``lInputBufWritePos − T`` **(no lInputLatency):**
   ``pfInputLatencyBuf`` is a verbatim copy of ``pfInput`` with its write pointer at the
   current physical time. Normal recording from the sync boundary would have captured
   physical audio at ``T_sync − lInputLatency + i`` for sample *i*. Working backwards
   from ``lInputBufWritePos`` (which points to physical time ``T_now − lInputLatency``)
   this maps to offset ``lInputBufWritePos − T + i``, so ``readPos = lInputBufWritePos −
   T``. The ``lInputLatency`` term cancels exactly.

-  **Why** ``lLastSyncPeriodSamples`` **is seeded from tempo rather than measured pulses:**
   The sync bus carries two kinds of pulses above the 1.5 threshold: the true beat
   boundaries and lower-amplitude loop-coordination signals. Seeding from ``fTempo``
   (BLOCK 7) gives a period that is immune to these sub-threshold pulses, while BLOCK 8
   provides a measured update gated by a sanity floor to catch cases where no tempo is
   available.

Known Issues and Future Work
----------------------------

-  **Smart eighths and MIDI clock (stock behaviour).**  The "auto 8th"
   option, on by default, doubles Eighths/Cycle whenever the tempo is
   below 60, and the tempo measured from MIDI clock starts low for a few
   beats after every Start.  Eighths/Cycle can then double, once or more,
   and stays doubled.  With MIDI clock sync, turn "auto 8th" off.

-  **Only tested on Ubuntu 24.04:** no other environments yet tested.

-  **Only tested in basic looping setups:** only basic looping and overdubs have
   currently been tested.  Multi-loop setups not yet tested (but should revert
   to stock behavior for syncing to a loop).  Tests with syncing assume the sync
   source is Jack.

-  **Internal sync mode:** when SooperLooper drives itself (one loop as the sync source
   for others), late-hit correction is **disabled on all slave channels**. Both channels
   of a stereo pair then use the stock timing path and stay in phase with each other.
   Enabling correction on all channels in this mode would require a broader audit of the
   state machine; deferred. See ``bLateHitBypassed`` in the Glossary.

-  **Window size and sample rate:** the effective maximum window is
   ``(lInputBufSize − 1) / fSampleRate`` seconds — approximately 682 ms at 48 kHz,
   743 ms at 44.1 kHz, 341 ms at 96 kHz. Setting ``late_hit_window_ms`` above this
   value is silently clamped; it effectively makes the fraction cap dominant for all
   musically relevant tempos. To raise the ceiling, increase ``lInputBufSize`` in
   ``instantiate()`` (inexpensive: 128 KB per channel at 48 kHz).

-  **Spurious trigger at startup:** ``bSyncPulseReceived`` (see Glossary) prevents the
   window condition from evaluating until at least one real sync pulse has arrived, so a
   press shortly after startup cannot trigger a spurious correction.

-  **Multiply end:** correction is implemented and pending test confirmation (disable
   with ``LATE_HIT_MULTIPLY_CORRECTION 0`` if regressions appear).

-  **Multiply start:** deferred. The frontfill machinery in ``beginMultiply()`` makes the
   position adjustment more involved; low priority since Multiply start snaps to the
   current loop's cycle boundary which is a natural and musically acceptable snap point.

-  **Insert end:** not needed — with Quantize on, Insert end waits for the next
   cycle boundary on its own (a forward wait, not a missed boundary).

-  **Insert start:** deferred for the same reason as Multiply start.

-  **Double log lines:** each button press produces two identical log entries because
   both channel instances (0:0 and 0:1) receive the same event and log independently.
   Expected behaviour, not a bug.

-  **Sync-source change:** the late-hit state (``bSyncPulseReceived``,
   ``lGlobalSamplesSinceSync``, ``lLastSyncPeriodSamples``) is reset to its initial
   condition whenever ``Engine::update_sync_source()`` is called — i.e., whenever the
   user changes the sync source. This prevents stale period measurements from the old
   source affecting the first presses on the new one.

-  ``SYNCWDEBUG`` **is off by default.** Uncomment ``//#define SYNCWDEBUG``
   in ``plugin.cc`` to enable all debug output.  When enabled:

   - a ``sl_loaded.txt`` sentinel is written at binary-load time confirming
     the experimental build is running.
   - ``sl_debug.log`` is created and overwritten fresh on each engine
     start. Both files land in ``$HOME/.sooperlooper/logs/`` (created if it
     does not already exist -- SooperLooper's own per-user directory, the
     same one ``ALB_SYNC_PULSE_LOG`` uses), falling back to ``/tmp`` if
     ``$HOME`` is unset or that directory can't be created; the debug log
     falls back further to ``stderr`` as a last resort.
   - ``SL_LOG`` startup banners (instantiate, activate) go to the log file.
   - ``SYNCWDBG()``-wrapped ``SL_LOG_RT()`` calls write per-press timing
     diagnostics to the same file.

   To watch the log in real time::

       tail -f ~/.sooperlooper/logs/sl_debug.log

   When ``SYNCWDEBUG`` is enabled, ``SL_LOG_RT`` calls ``fflush()`` after
   every write.  This forces an immediate kernel ``write()`` syscall on every
   button press, acquiring the C stdio mutex.  Both operations can block the
   RT thread; **xruns are the expected cost of enabling this mode.**  The
   fflush ensures log data is flushed to disk even if the process crashes
   mid-session, which makes post-mortem diagnosis reliable.  When
   ``SYNCWDEBUG`` is not defined (the default), none of this code exists:
   ``SL_LOG`` / ``SL_LOG_RT`` / ``SYNCWDBG`` expand to nothing, and the only
   output is the ``fprintf(stderr, ...)`` banner in ``engine.cpp`` after
   session load.


Test Suite
----------

The stock test harness lives in ``src/test_plugin/``.  It wraps ``plugin.cc``
via SWIG and runs each looper instance as a real JACK client, taking it through
its states from Python.  JACK must be running before the tests are started; a
dummy backend suffices::

    jackd -d dummy &
    make
    python3 -m pytest

Note: use ``python3 -m pytest`` rather than the bare ``pytest`` command to
ensure all imports resolve against the active virtual environment.

Python 3 migration
~~~~~~~~~~~~~~~~~~

The original tests targeted Python 2.7 with ``nose`` / ``nose_parameterized``.
The following files were updated to run under Python 3 with ``pytest``:

.. list-table::
   :header-rows: 1

   * - File
     - Change
   * - ``engine_wrapper.py``
     - ``dict.iteritems()`` → ``dict.items()``; integer division ``/ 2`` → ``// 2``
   * - ``testdef_simple_state_after_record.py``
     - Removed unused ``from nose_parameterized import parameterized`` import
   * - ``testdef_simple_state_off.py``, ``_offmuted.py``, ``_record_undo.py``, ``_record_undoall.py``
     - ``from nose_parameterized import parameterized`` → ``from parameterized import parameterized``
   * - ``makefile``
     - ``-I/usr/include/python2.7/`` replaced with ``$(shell python3-config --includes)``
   * - ``pytest.ini``
     - New file.  Sets ``python_files = testdef_*.py`` so ``pytest`` collects
       the ``testdef_`` prefix convention and ignores the SWIG-generated
       ``test_engine.py``.
   * - ``README``
     - Updated with Python 3 dependencies, build, and run instructions.

``testdef_simple_state.py`` and ``testdef_simple_state_overdub.py`` required no
changes.  Install dependencies with::

    pip install pytest parameterized swig

Test harness additions for late-hit testing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The C++ test harness (``test_looper.hpp`` / ``test_looper.cpp``,
``test_engine.hpp``, ``test_engine.swg``) was extended with the following
additions to support the late-hit tests:

.. list-table::
   :header-rows: 1

   * - Addition
     - Description
   * - ``TestLooper::sync_inject_value`` (``float``, public)
     - When ``sync_inject_enabled`` is true, ``run()`` fills ``_our_syncin_buf``
       with this value before each JACK callback.  Use ``2.0`` to inject a sync
       pulse (above the ``> 1.5`` threshold that sets ``bSyncPulseReceived``);
       use ``0.0`` for no signal.  Note: ``1.0`` must not be used as a
       background value — ``STATE_TRIG_START`` and ``STATE_TRIG_STOP`` exit on
       ``pfSyncInput[n] > 0.0``, so a constant ``1.0`` would cause them to
       leave the wait states immediately.
   * - ``TestLooper::sync_inject_enabled`` (``bool``, public)
     - Enables the sync buffer injection above.
   * - ``TestLooper::process_count`` (``volatile int``, public)
     - Incremented at the top of every ``run()`` call.  Python tests busy-wait
       on this to synchronise with exact JACK callback counts rather than using
       ``time.sleep()``, making timing independent of JACK period size.
   * - ``TestLooper::get_buffer_size()`` (returns ``int``)
     - Returns ``_buffersize`` (JACK period size in samples).  Return type is
       ``int`` rather than ``nframes_t`` so that SWIG maps it to a plain Python
       integer rather than an opaque ``SwigPyObject``.
   * - ``TestEngine::get_sample_rate()`` (returns ``int``)
     - Returns ``jack_get_sample_rate(jack)``.  Used together with
       ``get_buffer_size()`` to convert buffer counts to milliseconds when
       setting the correction window.
   * - ``g_late_hit_window_ms`` exposed via SWIG
     - Declared ``extern float g_late_hit_window_ms;`` in ``test_engine.swg``
       (both inside and outside the ``%{ %}`` block).  Accessible in Python as
       ``test_engine.cvar.g_late_hit_window_ms``.  SWIG places writable C
       globals in a ``cvar`` object on the module; writing directly to
       ``test_engine.g_late_hit_window_ms`` silently creates a Python attribute
       and does not affect the C variable.
   * - ``g_late_hit_max_quant_unit_fraction`` exposed via SWIG
     - Same mechanism as above.  Accessible as
       ``test_engine.cvar.g_late_hit_max_quant_unit_fraction``.

Test harness additions for audio-content testing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Added alongside ``cancel_record_mute``.  Before these the harness could only
assert on *state* values; it can now assert on what actually landed in the
loop buffer, which is how the fade-residue bugs were found and fixed.

.. list-table::
   :header-rows: 1

   * - Addition
     - Description
   * - ``TestLooper::input_inject_value`` (``float``, public)
     - The constant fed to the plugin's audio input each block.  ``run()``
       has never read the real JACK input port -- it always synthesised a
       buffer -- so this only makes that constant settable.  Defaults to
       ``-0.7``, the previously hardcoded value, so every test written
       before this behaves identically.
   * - ``TestLooper::output_min`` / ``output_max`` (``float``, public)
     - Running min/max of channel 0's output, sampled after
       ``descriptor->run()``.  They accumulate across blocks because one
       block covers only a fraction of a loop period, so a single block can
       miss a contaminated region entirely.
   * - ``TestLooper::output_last`` (``float``, public)
     - Last sample of the most recent block.
   * - ``TestLooper::reset_output_stats()``
     - Resets the three above.  Typical use: reset, spin a known number of
       callbacks, then read the range.
   * - ``engine_wrapper.command_ordinals``
     - Command name -> ordinal, parsed from ``event.hpp``.  The number passed
       to ``request_cmd()`` is the ``Event::command_t`` ordinal, and the
       positional ``commands`` tuple only covers the first 24 values, so
       anything past those must come from the header.

Set ``DryLevel`` to 0 from the test so the output is purely loop content.

.. note::

   The harness is still hardcoded to **one** channel
   (``test_engine.cpp``: ``TestLooper(jack, 0, 1)``) while the live rig runs
   ``sooperlooper -c 2``.  Making it configurable was considered and
   deferred.  Note that channels above 0 have State, LoopLength,
   LoopPosition and output wired to slave/dummy ports, so raising the
   count alone would add no observability.

Late-hit test cases (``testdef_late_hit.py``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The base class ``LateHitTestCase`` sets the correction window to exactly
``(WITHIN_BUFS + 1)`` buffer-periods via ``test_engine.cvar.g_late_hit_window_ms``
in ``setUp()``, and restores the default (100 ms) in ``tearDown()``.  All
timing uses ``wait_buffers(n)`` (busy-wait on ``process_count``) rather than
``time.sleep()`` so the tests are independent of JACK period size.  One
``wait_buffers(1)`` settle call follows every ``engine.request()`` before any
``assertState()`` to avoid a race between ``request_pending`` clearing and
``descriptor->run()`` completing.

.. list-table::
   :header-rows: 1

   * - Test
     - What it verifies
   * - ``TestLateStart.testLateStart_DirectlyRecording``
     - Core late-start case: RECORD within the window after a pulse enters
       ``STATE_RECORD`` directly.  Would fail on unpatched code (gets
       ``WaitStart`` instead).
   * - ``TestLateStart.testNoCorrection_SyncOff``
     - Regression: sync OFF still reaches ``STATE_RECORD`` immediately (stock
       path unaffected).
   * - ``TestLateStart.testNoCorrection_NoPulse``
     - ``bSyncPulseReceived`` guard: no correction fires before the first sync
       pulse has been seen.
   * - ``TestLateStart.testNoCorrection_TooLate``
     - Window boundary: T = ``OUTSIDE_BUFS × buf`` > window →
       ``STATE_TRIG_START`` (no correction).
   * - ``TestLateStart.testNoCorrection_OnTime``
     - On-time press (pulse held on during RECORD) produces ``Recording`` or
       ``WaitStart`` and does not crash.
   * - ``TestLateStop.testLateStop_DirectlyPlaying``
     - Core late-stop case: second RECORD within window after a pulse enters
       ``STATE_PLAY`` directly.
   * - ``TestLateStop.testNoCorrection_Stop_TooLate``
     - Window boundary on stop: T > window → ``STATE_TRIG_STOP`` (no
       correction).
   * - ``TestLateStop.testNoCorrection_Stop_NoPriorPulse``
     - ``bSyncPulseReceived`` guard on stop: recording started with sync off
       (no pulse ever seen); stop goes to ``WaitStop``.
   * - ``TestLateStop.testLateStop_LoopLengthTrimmed``
     - Loop length after a late stop is positive and less than the total
       recording duration (the T-sample trim is visible).
   * - ``TestWindowControl.testWindowBoundaryExact``
     - Pins the window arithmetic from both sides in a single test: T =
       ``WITHIN_BUFS × buf`` fires when window = ``WITHIN_BUFS + 1`` buffers
       and does not fire when window = ``WITHIN_BUFS − 1`` buffers.
   * - ``TestSyncSourceReset.testAfterSyncOff_NoCorrectionOnNextRecord``
     - After toggling sync off and back on without a new pulse, the first
       press does not fire the correction spuriously.

Not yet tested
~~~~~~~~~~~~~~

The following behaviours are covered by the patch but do not yet have
automated tests:

-  **Exact T offset after late start.**  The tests verify that ``STATE_RECORD``
   is entered directly, but do not assert that ``dCurrPos == T`` (the loop
   content begins at the sync boundary).

-  **Backfill audio content.**  The ring buffer read position
   (``lInputBufWritePos − T``) and the per-sample copy are not verified
   against the expected source samples.

-  **Multiply-end correction** (``multStopInWindow``, guarded by
   ``LATE_HIT_MULTIPLY_CORRECTION``).

-  **Two-channel** ``bLateHitBypassed`` behaviour in internal sync mode.  A
   two-channel ``TestLooper`` (``_chan_count = 2``) and a sync source pointing
   to one of the looper instances would be needed to test this.

-  **C6 reset via an actual sync-source change engine event.**  The current
   test toggles the ``Sync`` port, which does not call
   ``Engine::update_sync_source()``.  A test that sends an OSC sync-source
   change and verifies ``bSyncPulseReceived`` is cleared would more faithfully
   exercise the C6 fix.

