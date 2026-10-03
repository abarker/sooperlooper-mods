.. SooperLooper + Ardour live-looping setup and latency guide

=========================================================================
Live Looping with SooperLooper and Ardour: A Low-Budget Example Setup
=========================================================================

This guide describes a practical, low-budget live-looping setup on Linux using
a fork of SooperLooper, Hydrogen as the JACK transport (timebase) master, and
Ardour for recording and latency verification. It covers software
configuration, signal routing, and -- in the most detail -- latency calibration.

The forked SooperLooper used in this setup is at
https://github.com/abarker/sooperlooper-mods (branch ``late-hit-and-jack-midi``).

**Contents**

- `Overview`_
- `System Description`_
- `1. Setup`_
- `2. Latency: Concepts and Calibration`_
- `3. Automating the Setup`_

.. note::

   This guide assumes the modified SooperLooper 1.7.9 from the fork above.
   Among other things, it adds **late-hit corrections** (a press just after
   the beat counts as on time), sync that stays on the JACK transport master's
   beat through tempo changes, a command that -- bound to a long press on
   record -- stops a playing loop and keeps it, muted, and **direct JACK
   MIDI** input and output. Stock SooperLooper 1.7.9 has no JACK MIDI; with it
   you route MIDI over ALSA using ``aconnect`` and **a2jmidid**. With this
   fork you can connect directly over JACK and skip ALSA entirely. The fork's
   ``mod_docs/late_hit_patch_summary.rst`` lists every change in detail.


Overview
========

This is a fairly low-budget setup built around commodity Linux audio hardware
and open-source software. It lets you build loops locked to a Hydrogen drum
pattern and record the result in Ardour with sample-accurate alignment. The
Hydrogen beat plays continuously in the background as a click track and sync
source. It is not captured in a loop, but can be recorded in the DAW for
mixing and played through a PA. Most of the work in getting it to feel right
is **latency calibration** -- making sure the loop you record lines up with
what you actually heard when you played it. That is covered in detail in
`2. Latency: Concepts and Calibration`_.

The signal/control chain at a glance:

* A four-button **ActitioN** MIDI footswitch sends momentary CC events
  (press + release).
* A **remapper** program turns those raw events into higher-level gestures
  (chords, long presses, repeat-on-hold, double taps) and switches banks.
  One bank controls Hydrogen -- patterns, tempo, and so on -- so drum
  patterns can be switched mid-performance from the footswitch, for example
  from a 4/4 to a 3/4 pattern. The remapper is optional, but its gestures
  let more commands fit on a fixed number of buttons.
* SooperLooper receives the mapped MIDI and records and plays loops, locked
  to the Hydrogen beat while JACK transport is running -- and staying on it
  through tempo changes.
* Hydrogen runs in pattern mode as the JACK timebase master. Its drum
  pattern acts as a click track -- it can be any Hydrogen pattern, simple or
  complex.
* Big Tempo watches the JACK transport (which Hydrogen, as timebase master,
  starts and stops). When the transport starts or stops, Big Tempo sends MIDI
  to switch SooperLooper's sync and mute-quantize on or off to match, so loops
  can be recorded either locked to the Hydrogen beat or free-running. It also
  shows a large tempo readout for the performer.
* Ardour records everything and can be used to measure and verify latency.


System Description
==================

============================  ===================================================
Component                     Detail
============================  ===================================================
OS                            Ubuntu 24.04
Kernel                        Generic kernel (low-latency settings applied)
JACK                          JACK2 standalone server
SooperLooper                  1.7.9, forked (see the note above)
MIDI footswitch               ActitioN four-button MIDI controller
Audio interface               Behringer UMC404HD (four inputs)
Mixer                         Behringer Xenyx 802S
============================  ===================================================

.. note::

   The dedicated low-latency kernel is probably the better audio choice, but
   the generic kernel is used here because it includes a driver needed for
   other hardware that the low-latency kernel currently lacks.

.. note::

   Big Tempo and the remapper both depend on ``mididings``, which requires
   JACK and is built for Linux. A community-maintained fork of it also lists
   macOS as supported, though that has not been tried for this setup.
   Windows is not supported by ``mididings``.


1. Setup
=========

1.1 JACK
---------

This setup uses the **JACK2 standalone server**, started from QJackctl or a
startup script. Ubuntu 24.04 ships a PipeWire-JACK bridge that presents
itself to applications as JACK, but it does not offer QJackctl's hardware
latency fields (Setup -> Advanced -> Latency I/O), which this setup relies
on. Use the standalone JACK2 server instead.

**Transport / timebase master**

Set **Hydrogen** as the JACK timebase master.

**Zero-latency monitoring**

To hear your own instrument without any delay -- which matters more the
larger the JACK buffer -- monitor it through the interface's zero-latency
path rather than through the computer. The UMC404HD has 1/4" TRS insert
points on all four input channels (tip = send, ring = return, sleeve =
ground). To tap a zero-latency feed, short the send (tip) and return (ring)
together and use that junction as the signal, with the sleeve as ground.
Route it to the **Xenyx 802S** mixer, which feeds the headphones or other
monitor. The insert cables are easy to make.

**JACK buffer settings**

The buffer size and period count affect both latency and stability: lower
values reduce latency but risk xruns, depending on the hardware and system
load. This setup uses **256 frames/period** and **3 periods/buffer**. See
`2. Latency: Concepts and Calibration`_ for how to measure round-trip latency
and how these values feed into the QJackctl settings.

**JACK connections**

The key audio and MIDI connections needed for this setup are:

* Footswitch MIDI (the interface's MIDI input) -> remapper MIDI in
* Remapper MIDI out -> SooperLooper MIDI in
* Remapper MIDI out -> Hydrogen MIDI in (drum bank commands)
* Big Tempo MIDI out -> SooperLooper MIDI in (sync/mute-quantize toggle)
* UMC404HD capture ports -> Ardour track inputs (guitar mic, etc.)
* UMC404HD capture ports -> SooperLooper loop inputs (what gets looped)
* SooperLooper audio out -> Ardour track inputs (for loop recording)
* Hydrogen audio out -> Ardour track input (for direct drum reference)
* Ardour/SooperLooper/Hydrogen audio out -> UMC404HD playback ports

The exact port names depend on the hardware and the sessions. In practice a
startup script makes the connections, using ``jack_connect`` or the JACK
Python API (``jacklib`` / ``jack``). Only the connections above need to be
made from outside; routing inside Ardour and Hydrogen is set up in those
applications.


1.2 SooperLooper
-----------------

SooperLooper is started with **slgui**, the GUI built from this fork.
Optionally, also start **slbigstate**, a separate window that is part of
this fork: it shows each loop's state (recording, playing, muted, and so on)
in large text, readable at a distance, for when your attention is on the
pedal rather than the screen.

**Connecting MIDI**

With this fork you can connect SooperLooper directly over **JACK MIDI**, to
its ``midi_in`` port -- no ALSA, no a2jmidid. (Stock SooperLooper 1.7.9 must
be reached over ALSA with ``aconnect`` and ``a2jmidid``.)

If the pedals go through the remapper, start it with ``--jack-rt``. Commands
then reach SooperLooper one JACK period sooner (about 5 ms at 256 frames), as
early as with the pedal connected directly. The option runs the remapper's
handler inside JACK's audio thread, so watch JACK's xrun count for a session
or two afterwards, and drop the option if xruns go up.

**Learning and saving MIDI bindings**

Choose whatever MIDI channel and messages you want to drive SooperLooper,
then bind them to its commands with slgui's **learn** function. While
learning, check that SooperLooper recognizes each incoming message before
moving on.

When the bindings are right, **save them** -- slgui does not keep them
automatically. SooperLooper stores bindings in an ``.slb`` file; this setup
uses ``sl_midi_config_actition_bindings.slb``. (Hydrogen remembers its MIDI
setup by itself.)

.. warning::

   **A binding's Type decides whether it fires on press, on release, or on
   both -- and the names read backwards.** In slgui's binding editor, a
   control-change binding can be Type **CC**, **CC On**, or **CC Off**. Of
   the three, **CC Off** fires only on the press (it suppresses the
   release/zero message), **CC On** fires only on the release, and plain
   **CC** fires on both.

   This matters for command buttons like record: bind one as plain **CC**
   and the same press-then-release pair fires the command twice -- record
   switches on and straight back off. Every command-style binding in this
   setup therefore uses **CC Off**, which avoids that.

On the ActitioN footswitch, each button sends a **momentary CC** event -- a
press followed by a release. The remapper builds its gestures and bank
switching on top of these.

The core SooperLooper commands used:

============== ==============================================================
Button         Action
============== ==============================================================
record         Records on the first press; plays the recording on the second.
mute_trigger   Mutes active playback; pressed again while muted, restarts the
               loop from the top. Effectively the **Stop** button.
undo           (optional) Undo the last action.
overdub        (optional) Layer additional material onto the loop.
============== ==============================================================

.. note::

   A muted loop keeps cycling silently, so ``mute_trigger`` works as the
   looper's stop button. Pressing it again restarts the loop from the top,
   while a plain unmute resumes it where it is in its cycle.

**Sync and quantize settings**

These are the settings currently in use -- one example, not a universal
recommendation:

* **Sync** -> Jack/Host
* **Quantize** -> Cycles
* **8ths per cycle** -> 2  (one example choice, giving quarter-note
  quantization)
* **Crossfade (xfade)** -> ~200 samples for smoother transitions. The right
  value depends on material, tempo, and timing accuracy. Set it to **0**
  during latency calibration.

When JACK transport is running, SooperLooper is locked to the Hydrogen beat,
and with this fork it stays on that beat through tempo changes. Hydrogen, as
the JACK timebase master, starts and stops the transport; Big Tempo detects
this and sends MIDI to SooperLooper to switch sync on and off, which is what
allows recording a loop either locked to the beat or free-running.

**Late-hit correction.** With sync on, a press that lands a moment after the
beat normally waits for the *next* beat -- a whole cycle late. This fork
treats such a press as on time instead. A record press just after the beat
starts the loop on that beat, keeping the audio played in between; a press to
end a recording just after the beat ends the loop there, so it is not a cycle
too long. Ending a Multiply works the same way. It is on by default with a
100 ms window. To change the window, add the key to the session file's
``<Globals>`` line and restart::

    late_hit_window_ms="100"

(milliseconds; ``0`` disables it). A second, optional key,
``late_hit_max_quant_unit_fraction``, also caps the window at a fraction of
the quantize cycle, which helps at fast tempos -- the fork's summary document
describes it.

.. note::

   You *can* loop a drum part -- for example, if you play a beat on a drum
   controller and loop it. But in normal use the Hydrogen pattern simply runs
   in the background as the click and sync source and is not captured into the
   loop.

**A pedal-gesture arrangement (example)**

This is the specific footswitch arrangement currently in use. The two main
looping buttons are:

* **record/play** -- records, plays, and on a long press stops, leaving the
  loop muted (see *Long press on record* below);
* **mute_trigger** -- stops the loop, muted, and pressed again restarts it
  from the top. It has no gestures on it, so it fires the instant it is
  pressed.

A third button handles **overdub**. The four-button switch's fourth button
toggles the Hydrogen drums on and off, and on a long press returns to the
drum-control bank (patterns, tempo, and so on).

Gestures normally cost latency. To tell a plain press from a long press, the
remapper has to wait for the release: a quick release means a plain press, so
the plain command fires on the release, as late as the press was long. A
double tap costs more: a plain press can only fire once the double-tap window
has passed without a second press. Record is **the most latency-sensitive
action in the setup**, so it cannot afford even the wait for the release. The
remapper avoids the cost with an **eager single**: the single-press command
fires *immediately*, and then -- only if the button is still held past the
long-press threshold -- the gesture's additional command(s) fire as well. The
first press keeps zero added latency; only the follow-up gesture is delayed.

A long press is used here rather than a double tap. A double tap could be
triggered accidentally by a bad or doubled record hit in the heat of a
performance, which on the record button is unacceptable; a deliberate hold is
much harder to trigger by accident.

.. note::

   Overdub is less latency-sensitive than record, but not immune. Unlike a
   record press, an overdub press does not define a loop boundary, so a small
   delay does not create a sync error -- it only shifts where the overdub window
   opens and closes. With **unquantized** overdub (used in this setup) a delayed
   press clips a little material at the edges of the overdub region; with
   **quantized** overdub the boundary snapping absorbs small delays entirely,
   but a delay large enough to cross a quantize boundary jumps the start or end
   by a whole quantize unit.

**Long press on record: stop, keeping the loop.** With the eager single,
one switch does the whole cycle: press to record, press again to play, and
hold to stop. The hold's press fires a new ``record`` at once, as every press
on this switch does; the long press then sends ``cancel_record_mute``, a
command added to SooperLooper for this, which cancels that new recording and
leaves the loop you made **muted**. Unmuting resumes it where it is in its
cycle; **mute_trigger** restarts it from the top.

The command is **state-aware**, which is what lets one switch carry all of
this. What it does depends on what the loop was doing when record was
pressed, and how far that record has got:

.. list-table::
   :header-rows: 1
   :widths: 34 66

   * - State when the long press lands
     - Result
   * - Recording, or waiting to record, started from mute, pause or an
       empty loop
     - **Nothing happens**: the recording carries on. There was nothing
       playing to stop, so a slow press doesn't lose the take.
   * - Recording over a loop that was playing
     - The new recording is discarded, the previous loop is restored, and
       the looper is left **muted**. Only that one recording is undone.
   * - Record over a playing loop, still waiting for the sync boundary
     - The waiting record is cancelled and the loop **keeps playing** --
       nothing was recorded.
   * - Anything else (for example, just after a press that ended a
       recording)
     - **Nothing at all happens.**

A remapper sending plain ``mute_on`` and ``undo`` cannot tell these cases
apart, which is why this is a command inside SooperLooper.

What this means in practice:

* **The hold usually lands after the recording has started**, so the loop
  stops, muted. If record was pressed early in a long cycle, the hold can
  land while the record is still waiting for the beat; then the loop simply
  keeps playing. Press **mute_trigger** to stop it.
* **A long press cannot abandon a recording in progress.** Pressing to end a
  recording finishes it at once -- the new take plays -- and the long press
  that follows does nothing. That is also what makes the gesture safe:
  lingering on the pedal after ending a recording keeps the take. To stop a
  recording in progress without hearing it, press **mute_trigger** instead:
  it ends the recording into a muted loop (at the next beat when
  mute-quantize is on), keeping the new take.
* **The one trade-off that cannot be avoided:** the start of a real record
  press looks exactly like the start of a long press, so a foot resting on
  the **record/play** switch past the long-press threshold (400 ms here)
  cancels a take you meant to keep. Keep the threshold comfortably longer
  than a normal stomp.

.. note::

   Bind these commands in slgui as plain press actions (**CC Off**, as
   described above) and let the remapper do all the gesture recognition. Do
   **not** also use SooperLooper's own Note long-press: its long press for
   Undo is *Undo All*, which clears the whole loop stack rather than
   cancelling a single recording.

**Record failsafe: an optional timeout for a forgotten stop.** A tap on
record over a playing loop starts a real recording at once. If that tap was
meant to be the hold that stops the loop, or the record was triggered by
accident -- a bumped pedal, say -- and nobody notices, the recording runs on
forever, with the loop you had gone underneath it. This fork adds a timeout
for that case: if a recording that cut off a playing loop runs longer than
``record_failsafe_seconds``, it is cancelled just as the long press would
have done, and the previous take comes back, muted. It never fires for a
first recording into an empty loop, or for one started from Mute or Pause.

It ships **off by default**. To turn it on, add the key to the session
file's ``<Globals>`` line and restart::

    record_failsafe_seconds="45"

(seconds; ``0`` or omitted means off). Pick a value comfortably longer than
your longest real take: a deliberate new take recorded over a playing loop
counts too, and would be cut off at the same point.

**Mute/unmute late-hit correction.** With mute-quantize on, pressing
mute or unmute a moment after the beat normally waits for the *next*
beat before acting -- up to a full extra cycle either muted when you
meant to be playing, or playing when you meant to be muted. This lets a
press shortly after the beat act immediately instead, the same idea as
the Record late-hit correction above, applied to mute and unmute.

It ships **on by default** (100 ms, matching the Record late-hit
correction above) -- no setup needed to get it. To turn it off, or to
use a different window, add the key to the session file's ``<Globals>``
line and restart::

    mute_late_hit_ms="0"

(milliseconds; ``0`` disables it). Keep it small if you tune it -- this
is meant to catch a slightly-late press, not to make mute-quantize
sloppy.


1.3 Ardour
-----------

Ardour's track alignment setting determines whether JACK's latency compensation
is applied when recording. The correct setting differs by source:

* **Hydrogen and SooperLooper tracks** -> align to **capture time**. Both
  reach Ardour directly through JACK, with no trip through the audio
  hardware, so there is no latency for Ardour to compensate; recording at
  the raw capture position is correct.
* **Guitar, vocal, and other hardware-source tracks** -> align to **existing
  material**. These signals travel through the full hardware round-trip (output
  latency to the headphones/monitor, input latency back from the mic or
  instrument). Ardour compensates using JACK's reported latency to keep them
  in sync with the rest of the session.

Right-click a track header and choose *Alignment* to set this per track.

For multitracking to work correctly, JACK must have accurate hardware latency
information. See `2.4 Setting the QJackctl hardware latency`_ in the next section.


2. Latency: Concepts and Calibration
======================================

This is the part that makes or breaks the feel of the setup. The goal:
**a loop you record should line up exactly with what you heard when you played
it.** This means accounting for the full delay between your instrument and the
recorded sample.

2.1 Where latency comes from
-----------------------------

Round-trip audio latency through JACK has three main parts::

   total_roundtrip_frames = capture_hw_latency
                          + (periods_per_buffer × frames_per_period)
                          + playback_hw_latency

Here ``total_roundtrip_frames`` is the total delay in audio samples (frames)
from the moment a signal enters the ADC until the resulting output is heard
from the DAC -- essentially the number of samples of end-to-end delay in the
system.

To convert frames to milliseconds::

   latency_ms = frames / sample_rate × 1000

2.2 Latency asymmetry: software vs. hardware sources
------------------------------------------------------

Not all signals in this setup travel the same path, so they accumulate
different latencies:

* **Hydrogen drums -> headphones.** Hydrogen generates audio entirely in
  software. It reaches the player's ears via JACK output buffering and the DAC
  only -- so the delay is **output/playback latency only**.
* **Guitar or vocal -> headphones**, when monitored through the computer
  (for example through Ardour). The signal goes all the way through ADC ->
  JACK input buffer -> JACK output buffer -> DAC -- the **full round-trip
  latency**. (Monitored through the interface's zero-latency path from
  section 1.1, it has no delay at all.)
* **Multitracked guitar/vocal recorded in Ardour** (with *align to existing
  material*). Ardour compensates automatically, using the round-trip latency
  JACK reports -- so that must be right, via QJackctl's hardware latency
  settings, for the alignment to be accurate.

2.3 Measuring round-trip latency
----------------------------------

The easiest is this fork's ``sllatency`` tool; the other two are manual.

**The sllatency tool:** installed with SooperLooper (``make install``).
Start JACK, SooperLooper and Ardour as usual, leave loop 1 empty or muted,
and run ``sllatency`` in a terminal.  It asks which outputs to play a click
on -- suggesting the ones SooperLooper is heard through -- and then asks
you to balance the headphones on the mic loop 1 records from (or hold the
mic right against a speaker, or connect a cable from an output to that
input).  Keep the room quiet while it runs -- turn down or switch off the
amp and anything else that hums or buzzes near the mic.  In about half a
minute it measures the round trip, checks JACK's
latency settings, and checks where SooperLooper's loops land as you hear
them, including any delay from the Ardour bus they are monitored through.
It shows the values to set, and offers to apply them to SooperLooper and
save them in its session file, which you choose in a file picker (a backup
copy is kept next to it).  It needs the Python packages ``numpy`` and
``jack`` (on Ubuntu, ``python3-numpy`` and ``python3-jack-client``), and
``python3-tk`` for the file picker.  Use ``sllatency --no-sl`` to check JACK alone,
for example for Ardour without SooperLooper.

**jack_iodelay:** connect one of the interface's outputs back to one of its
inputs with a cable and run ``jack_iodelay``; it measures the total round
trip in frames and ms.

**Drum-beat recording method:** record a Hydrogen beat directly into Ardour
(Track A), then play Track A back while recording it back in through the
interface -- a cable from an output to an input, or headphones on a mic --
into a new track (Track B). With the tracks set to *align to capture time*,
the offset in samples between matching peaks in Track A and Track B is the
total round-trip latency: the same number ``jack_iodelay`` gives, but
measured through the actual signal chain. Zoom in on the drum waveform to
find a sharp transient or other recognizable feature where the two tracks
should line up, or simply use the very start of the recording.

.. note::

   When recording reference tracks for latency analysis, **renormalize** them
   in Ardour after recording. This brings peaks to a consistent level and makes
   waveform features much easier to compare visually.

All three give the same round trip; ``sllatency`` and the drum-beat method
also check the real signal chain end to end.

2.4 Setting the QJackctl hardware latency
-------------------------------------------

QJackctl's **Setup -> Advanced -> Latency I/O** has two fields:

* **Input Latency** -- the delay the interface adds on the way in: the time
  it takes to turn an incoming analog signal (mic, instrument) into digital
  audio that JACK can see, including its converter and USB buffering.
* **Output Latency** -- the same on the way out: the time from JACK's digital
  output to an analog signal at the headphone or line output.

JACK does not know these hardware delays by itself. It already counts its
own buffering (set by the frames/period and periods/buffer settings), so
leave that out when working out what to enter here.

**Using sllatency:** leave the fields as they are and run it.  It shows
JACK's current values beside the measured round trip and, if they
disagree, the new values to enter in these two fields.

**Using jack_iodelay:** set both fields to zero first, then connect a patch
cable from an output back to an input and run ``jack_iodelay``. It measures
the total round trip and reports the values to enter. If later testing shows
the compensation is a little too high or too low, adjust the values
slightly.

**Using the drum-beat method:** subtract JACK's own buffering from the
measured total and split the remainder evenly between the two fields::

   hw_latency_total = measured_offset - nperiods × period_size
   Input Latency    = hw_latency_total / 2
   Output Latency   = hw_latency_total / 2

Setting these correctly is necessary for **Ardour multitracking**: any time you
record guitar, vocals, or other tracks in Ardour and expect them to line up
with existing material, JACK needs accurate latency information to apply
automatic compensation.

.. note::

   SooperLooper's own input and output latency settings (section 2.6) are a
   *separate* compensation. The QJackctl values correct what JACK reports to
   every program; SooperLooper's values shift its recorded material
   directly. The two are set independently.

2.5 Worked example (this setup)
---------------------------------

Settings: **256 frames/period**, **3 periods/buffer**, **48 kHz**.

JACK's internal buffer contribution::

   nperiods × period_size = 3 × 256 = 768 frames

Measured round-trip (``jack_iodelay`` or drum-beat method): approximately
**1458 frames** (**30.4 ms**).

QJackctl hardware latency::

   hw_total   = 1458 - 768 = 690 frames  (~14.4 ms)
   each box   = 690 / 2    = 345 frames  (~7.2 ms)

This setup has 343 in each box, which JACK then reports as a 1454-frame round
trip -- a few frames short of the measurement, well within what matters.

SooperLooper latency settings (compensating for the full round trip
directly, independent of QJackctl): a total of about **1490 frames**
(**31.0 ms**): the round trip plus 32 frames (measured) for the plugins on
the Ardour bus SooperLooper is monitored through. This setup splits it
evenly, **743 frames** each for SL input and SL output latency (see
section 2.8).

2.6 Why SooperLooper needs its own latency settings
-----------------------------------------------------

When you press **record**, SooperLooper captures audio arriving at its input
*now*. But the timing reference you played against -- the Hydrogen beat in your
headphones -- reached your ears only after the output/playback latency. The
full round-trip latency separates "when you heard it" from "when the soundcard
saw it." SooperLooper's input and output latency settings shift newly recorded
material earlier so the loop sits where you heard it, not where it physically
arrived at the hardware.

As a practical guide when calibrating: **increasing** either latency value
moves the newly-recorded material to the **left** (earlier in time);
**decreasing** it moves material to the **right** (later in time).

.. note::

   Setting these values from a measurement -- with ``sllatency`` (section
   2.3), or by hand as in section 2.7 -- is recommended over
   SooperLooper's automatic option (the autoset toggle in slgui's
   **Preferences -> Latency**):

   * Autoset reads the latency of SooperLooper's **main** inputs and
     outputs, not each loop's own ports. Loops connected individually --
     the usual way when each loop gets its own track or bus in the DAW --
     leave the main ports unconnected, and autoset then sets both values
     to 0.
   * Turning autoset on replaces the hand-set values in the running
     SooperLooper, and turning it off again does not bring them back.
     Reload the session to recover them.
   * At best it copies the numbers JACK reports, which are only as
     accurate as the QJackctl latency fields; a measurement is better.

   SooperLooper's main ports are the ones JACK lists as
   ``sooperlooper:common_in_1``, ``common_out_1`` and so on; each loop also
   has its own (``loop0_in_1``, ``loop0_out_1``, ...). If you use the main
   ports, autoset reads the right ones. If you connect loops individually
   and want autoset anyway, edit the session file while SooperLooper is not
   running: on each loop's line, change ``use_common_ins`` and
   ``use_common_outs`` to ``"no"`` and ``auto_latency`` to ``"yes"``. (A
   loop's **main in** checkbox in slgui is the same setting as
   ``use_common_ins``; there is no slgui control for ``use_common_outs``.)
   This also stops those loops taking input from, and sending output to, the
   main ports, which only matters if something is connected to them.

2.7 The monitor-feedback calibration method
--------------------------------------------

The ``sllatency`` tool (section 2.3) makes this measurement automatically.
The method below is the manual way to do it, and shows what is being
measured.

In normal performance the signal path is:

  Hydrogen beat -> headphones -> **player hears, plays along** -> guitar ->
  guitar mic -> recorded

The calibration method short-circuits the player out of this loop. The
headphones (playing the Hydrogen beat) are placed on or near the guitar mic,
so the mic picks up the drum signal as a stand-in for a live guitar
performance. What gets recorded on the guitar track is the same signal the
player would have been hearing -- a proxy for the guitar part that would have
been played. When SooperLooper captures a loop, the drum signal in the loop
stands in for the looped guitar, for both plain loops and overdubs. With the
player removed, all three reference signals can be compared as waveforms and
any latency offset shows up directly.

For this test, **turn SooperLooper's playback down in the headphones**, so
the mic picks up only the Hydrogen beat. That gives a clean reference track
of what a player would have heard, with no loop playback bleeding in to
confuse the comparison.

Use a drum pattern where the downbeat is easy to pick out, and set
SooperLooper's crossfade (``xfade``) to **0** so the loop's edges are clean.
After recording, **renormalize** the reference tracks so waveform features
are easier to compare.

.. note::

   With late-hit correction on, **do not use the loop's first beat** as a
   reference point. If the press that ended the recording came in late and
   was corrected, the start of the loop's first repeat is cut short. Measure
   from the second beat or later.

The three signals to compare:

1. **Hydrogen recorded directly into Ardour.** The timing reference -- no
   analog chain, no hardware latency.

2. **The guitar-mic signal through the full chain.** Headphones placed on or
   near the guitar mic; the mic records the beat after it has passed through
   the complete output -> analog -> input path. This represents the guitar
   signal a player would have recorded while hearing that beat.

3. **The SooperLooper loop**, caught in **sync** mode. This represents the
   looped guitar -- for both plain loops and overdubs.

All three should align at the start of each beat:

* If the **mic'd signal (2)** does not line up with the direct Hydrogen track
  (1), the offset is JACK I/O latency. Measure it (see below) and adjust the
  QJackctl hardware latency values accordingly.
* If the **looper (3)** does not line up, adjust SooperLooper's input/output
  latency settings.

*Measuring the offset in Ardour:* first make the selection clock visible if
it isn't already: in **Edit -> Preferences -> Toolbar**, check the box to show
the selection clock, then right-click the clock and choose **Samples** as the
unit. It appears in the toolbar above the tracks and shows the **Start**,
**End** and **Length** of any selected range; the Start and End values in the
images below come from it. Read the offset as End minus Start. Don't rely on
the Length field: depending on the Ardour version and clock settings, it may
not show that number.

Make a range with one end on a recognizable point in the in-sync waveform and
the other on the matching point in the off-sync one. Zooming in on the drum
waveform makes matching transients easy to find -- a sharp attack at the
start of a beat works well -- though the very start of the recording is also
fine. Ranges are easy to drag, so line the ends up precisely before reading
the value.

.. note::

   Ardour's waveform display is slightly coarse at high zoom: range ends do
   not move in exact single-sample steps, so a measured offset may be off by
   a few samples. After applying a first correction, repeat the test and
   adjust slightly if the alignment still looks imperfect. For example, a
   measured total of ~1476 samples might be entered as 738/738, and a closer
   look at the transients may then show 743/743 matching marginally better --
   a small difference, but one that can be larger than the display seems to
   resolve.

The images below show the error state (latency 0/0, offset ~1475 samples) at
normal and zoomed views, then the corrected state (738/738). In these
recordings the vocal mic was on the floor next to the footswitch, catching a
spike on every button press -- the sharp transients on the vocal track. Some
of that click noise also bled into the guitar mic, which muddies the loop
transitions slightly. The offset is End minus Start in the selection clock
above the tracks.

.. image:: images/latency_error_example_not_zoomed.png
   :width: 700px
   :alt: Ardour session showing loop playback with incorrect latency (0/0), overview
   :target: images/latency_error_example_not_zoomed.png

.. image:: images/latency_error_example_zoomed.png
   :width: 700px
   :alt: Zoomed Ardour view showing ~1475-sample range selected between misaligned waveforms
   :target: images/latency_error_example_zoomed.png

.. image:: images/latency_error_example_corrected.png
   :width: 700px
   :alt: Ardour session after correction with input latency 738, output latency 738
   :target: images/latency_error_example_corrected.png

2.8 Input vs. output split, and why it matters for overdubs
-------------------------------------------------------------

Both SL latency settings shift newly recorded material earlier. For a **plain
loop** -- record, then play -- where the loop lines up depends only on their
*sum*. The input latency alone also decides where a recording starts: set too
low, the recording picks up a little sound from before the start; set too
high, it cuts off the start.

The ``sllatency`` tool measures only the total, since a loopback cannot
tell input from output.  It splits the total in the same proportions as the
input and output latency JACK reports for SooperLooper's ports -- JACK's
own buffering is uneven, one period on the way in and two on the way out --
which is a better-founded starting point than an even split.  The overdub
test below checks the split directly.

**Overdubs** are different. An overdub plays the existing loop back *while*
recording new material on top, so playback delay and capture delay act on two
different signals. That is why the total is split roughly evenly (here 743
input / 743 output).

*Overdub split test:* for this test, **turn SooperLooper's playback up in the
headphones** (the opposite of the test in section 2.7). The guitar mic then
picks up both the Hydrogen beat *and* the loop playback -- a double-beat
signal. With sync on, engage overdub while the loop (which contains the
recorded drum beat) is playing; the loop repeats, so where in the cycle you
start does not matter. If the input/output split is right, the two beats merge
into a single waveform; if not, two offset peaks show. Measure the distance
between matching peaks and adjust the split.

The images below show an incorrect split (input latency 700, output 0) and the
resulting ~782-sample offset visible when zoomed in:

.. image:: images/overdub_error_example_test_not_zoomed.png
   :width: 700px
   :alt: Ardour session showing overdub test phases with incorrect latency split (input 700, output 0)
   :target: images/overdub_error_example_test_not_zoomed.png

.. image:: images/overdub_error_example_test_zoomed.png
   :width: 700px
   :alt: Zoomed view showing ~782-sample offset between overdubbed and loop beats
   :target: images/overdub_error_example_test_zoomed.png

2.9 Additional tests
---------------------

* **Footswitch-noise marker.** Put a mic on the floor next to the footswitch
  to catch the switch's mechanical click. This is most useful with the drums
  off: without sync, the loop starts playing the moment the button is pressed
  rather than waiting for the beat, so you can see the delay between the
  switch click and the start of loop playback directly.

* **Constant-signal / fan test.** Turn on a fan next to the guitar mic for a
  continuous signal (with or without drums). This can give a cleaner picture
  than the drum method, because there is usually a short silence before the
  first drum beat: the fan noise is there from the first sample, so the loop's
  start edge is easy to see. (The noise floor rising can often be spotted even
  before the first drum hit.)


3. Automating the Setup
========================

Once the setup is dialed in, automate the repetitive parts:

* **MIDI bindings.** Save from slgui as an ``.slb`` file (this setup uses
  ``sl_midi_config_actition_bindings.slb``). Hydrogen remembers its MIDI setup
  automatically.
* **SooperLooper session.** Save the session to restore the loop and sync
  configuration and the latency settings. Keep a separate file per buffer
  size, since the latency settings differ: ``SL_session_save_256.slsess`` and
  ``SL_session_save_512.slsess``. SooperLooper has two ways to save a
  session: one saves only the configuration -- loop counts, sync settings,
  latency values and so on -- and the other also saves the recorded audio.
  For a startup configuration, use the configuration-only one.
* **JACK connections.** Use a startup script with ``jack_connect`` or the JACK
  Python API to make the connections automatically; the list in `1.1 JACK`_
  has the key ones to include.
