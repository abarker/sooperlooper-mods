## Modifications to SooperLooper 1.7.9

This is a modified fork of SooperLooper 1.7.9.  It adds late-hit
correction, pre-rolls that keep what was played just before a recording
starts, trigger latency at every recording edge, two new commands and a
record failsafe, direct JACK MIDI ports, JACK sync that stays on the
beat through tempo changes, and two standalone tools: a large-text loop
state display and a latency calibration program.  It also fixes a number
of timing and audio bugs in stock 1.7.9.  Thanks to Jesse Chappell for
the original SooperLooper.

**Late-hit correction.**  When sync is active, a press that lands within
a configurable window after a sync boundary acts at that boundary instead
of waiting for the next one.  This covers starting and ending a recording
(with Record, Overdub, or a quantized Mute), Mute and Unmute, Multiply
end, and Rel Sync recordings.  A corrected start or end gives the same
loop an on-time press would, to the sample, fades included.  The window
defaults to 100 ms, is never more than half the Quantize unit (an eighth
or a cycle), can be capped at a smaller fraction of that unit, and is set
in the session file or live over OSC.  Mute and Unmute have a window of
their own.

**Pre-rolls.**  A synced recording can keep a short stretch from just
before the boundary it starts on (`sync_preroll_ms`, and
`rel_sync_preroll_ms` for Rel Sync), and an unsynced one from just before
the Record press (`free_preroll_ms`), so a strum attack or pickup played
slightly early is in the loop.  The end moves back by as much, so the
loop's length does not change.  Off until set.

**Trigger latency at every edge.**  Stock SooperLooper uses its trigger
latency setting (how late a command reaches it after the press) only
where an overdub, Multiply, or Insert starts; everywhere else it acts as
if that delay were the input latency.  In this fork every recording start
and end uses it, so with a measured value each one lands on its press.
It can be measured with `sllatency --trigger`, and autoset now sets it to
the input latency it reads (at least one period) instead of one period.

**New commands and a record failsafe.**  The two new commands are meant
for the long press or double tap of a switch whose press has already sent
its usual command, so that command keeps its timing:

- On the Record switch, `cancel_record_mute` cancels the recording the
  press just started and brings back the previous take, muted.  One
  switch can then record, play, and stop.
- On the Overdub switch, `cancel_overdub_undo` undoes as if the overdub
  press had not happened.  A slow press counts as a long press, so this
  switch has to be tapped.

The record failsafe (off by default) does the same as `cancel_record_mute`
when a recording that cut off a playing loop runs longer than a set time.

**JACK sync.**  With a JACK timebase master such as Hydrogen or a DAW,
stock SooperLooper's sync pulses drift off the master's beat, by up to
half a cycle, after a tempo change made while the transport rolls, and
stay off until the next relocate.  This fork counts beats through tempo
changes, so the pulses stay on the beat.  When the drums themselves jump
off the loops (Hydrogen skipping a processing cycle), `realign_transport`
moves the transport so they land back on SooperLooper's beat without
moving the loops, on request or automatically after tempo changes
(`auto_realign`).  Changing Eighths/Cycle while synced starts the new
cycles at the old cycles' nearest boundary.

**Direct JACK MIDI ports.**  A `sooperlooper:midi_in` port takes MIDI
commands straight from JACK, each acted on at its exact sample within the
audio buffer, and MIDI clock to sync to.  A `sooperlooper:midi_out` port
sends 24-ppqn MIDI clock, and MIDI Start and Stop when the JACK transport
starts and stops, for driving external hardware.  No ALSA bridge or
`a2jmidid` is needed.

**Stock bug fixes.**  The main ones:

- Restarts and oneshots (`trigger`, `mute_trigger` from Mute, `once`)
  play in time.  Stock starts them late by the input plus output latency
  (31 ms at 743/743), with sync on too.
- A multiplied loop no longer drops out for the input plus output latency
  at its end on every pass.
- Overdub, Replace, Substitute, Multiply, and Insert keep all of their
  own audio and the old loop's, land in time, and fade cleanly at their
  edges.  Stock loses a few milliseconds of one or the other in several
  cases, and clicks or lands late in others when the latency settings
  differ.
- Record over a playing loop fades the loop out instead of cutting it off
  with a click, and with Sync on the loop plays on until the take starts.
- Rel Sync recordings come out the right length after Sync or the sync
  source or Quantize changes, and restart in time.  Internal sync no longer drops a
  cycle boundary, and MIDI clock sync no longer starts a synced recording
  early.
- A Once plays the whole loop, and a Trigger after Replace or Substitute
  keeps the loop.  Pause from Mute ends Paused.
- No crash when a recording waiting for its stop runs out of loop memory.
- Autoset sees latency reported by programs between the interface and
  SooperLooper, and `-S` selects the JACK server.
- Fewer xruns on transport locates.

Also: the default loop memory is 120 seconds (was 40), with a warning
when a loop nears the end of it, and the input kept for late starts and
pre-rolls is at least 4 seconds (`--input-history`).

**Large-text loop state (`slbigstate`).**  A standalone program showing
one or more loops' current state in large, high-contrast text, readable
from across a room, unlike slgui's own small indicators.

**Latency calibration (`sllatency`).**  Plays a click, times it coming
back through a mic or a cable, and checks JACK's latency settings and
SooperLooper's own input and output latency, including the delay of
whatever the loops are monitored through.  It then offers to apply and
save corrected values.  With `--trigger` it measures the trigger latency
from a mic placed near the pedal.

For full details see the documentation in [`mod_docs/`](mod_docs/):

- [`late_hit_patch_summary.rst`](mod_docs/late_hit_patch_summary.rst) —
  complete description of all modifications, configuration, build
  switches, and known issues.
- [`sync_quantize_round_relsync.rst`](mod_docs/sync_quantize_round_relsync.rst) —
  background on SooperLooper's sync and quantize model.
- [`latency_and_trigger_latency.rst`](mod_docs/latency_and_trigger_latency.rst) —
  the delays between the performer and SooperLooper, what each latency
  setting is for, and where each recording edge lands.
- [`sooperlooper_and_ardour_setup_guide.rst`](mod_docs/sooperlooper_and_ardour_setup_guide.rst) —
  a practical low-budget live-looping setup and latency-calibration guide.
- [`windows/README.md`](windows/README.md) — building and running on
  Windows under Cygwin, with a build script (built and run on Windows 10,
  MIDI through JACK included; a MIDI device not yet tried).

On Ubuntu 24.04 the script `ubuntu_compile.bash` builds and installs the
fork (see the summary).  It also builds with liblo 0.32, which stock 1.7.9
does not.  All new code is bracketed with `albBegin` / `albEnd` markers;
search for `albBegin` to locate every changed block.

---

*The original SooperLooper 1.7.9 README follows.*

---

SooperLooper -- Live Looping Sampler
Jesse Chappell <jesse@essej.net>
Copyright 2005

## DESCRIPTION

SooperLooper is a live looping sampler capable of immediate loop
recording, overdubbing, multiplying, reversing and more.  It allows
for multiple simultaneous multi-channel loops limited only by your
computer's available memory.  The feature-set and operation was
inspired by the impressive Gibson Echoplex Digital Pro (EDP). When used
with a low-latency audio configuration it is capable of truly realtime
live performance looping.
	  
The application is a standalone JACK client with an engine
controllable via OSC and MIDI.  It also includes a GUI which
communicates with the engine via OSC (even over a network) for
user-friendly control on a desktop.  However, this kind of live
performance looping tool is most effectively used via hardware (midi
footpedals, etc) and the engine can be run standalone on a computer
without a monitor.

SooperLooper is currently supported on Linux/Unix and OS X platforms.
Macintosh OS X packages are available, and are usable with the newest
release of [JACK-OS X](http://jackaudio.org/) or as an AU.  
Future plans include a VST plugin version for both Windows and Mac.

This software is licensed under the GPL, thus the source code is free,
open and available.  It is fully functional, no nagging, no activation
keys, nothing to waste but time if you find it doesn't suit your
needs.  Best of all, because the source is open, there will always be
someone around who can fix any problems, or add new features.  You are
welcome to copy it, give copies to your friends, and modify it.  But
if you find it useful, then please consider making a donation via
PayPal at http://essej.net/sooperlooper/donate.html .

	  
## FEATURES

The feature-set is modeled in the spirit of the Echoplex Digital Pro
(EDP) LoopIII and LoopIV. Detailed descriptions of these features are
found in the Documentation pages.

SooperLooper's primary features are listed below:

- Multiple simultaneous multi-channel loops limited only by available RAM
- Record, manually triggered, or via input threshold
- Overdub for adding more audio on top of existing loop
- Multiply, allows increasing loop length by repeating the initial loop beneath
- Feedback control allows gradual loop fading, active during overdub/multiply and optionally during playback
- Replace audio in loop with new material
- Insert new audio into existing loop
- Substitute audio in loop with new material, while hearing existing material
- Reverse loop playback (even during overdub) at any time, or quantized to loop or cycle boundaries
- Trigger loop playback from start at any time, also supports OneShot triggering which will play the loop once then mute.
- Mute the loop output at anytime
- Undo/Redo allows nearly unlimited undo and redo to previous loop states
- Rate Shift allows arbitrary rate change of loop from 1/4 to 4x normal. Works anytime, even during loop record.
- Save/Load loops in WAV format
- Scratch feature allows DJ-like position scratching (work-in-progress)
- Tempo syncable to MIDI clock, JACK transport, manual or tap tempo, or existing loops.
- Sync Quantize operations to divisions defined by the tempo, and/or existing loops.
- Sync Playback can retrigger automatically to maintain external sync during playback
- SUS (Momentary) operation available for all commands for easy realtime granular
- Crossfading applied to prevent clicks on loop or edit operation boundaries (crossfade length is adjustable)
- MIDI Bindings are arbitarily definable and can be configured to emulate existing setup (EDP, etc)
- Key Bindings are arbitarily definable for the GUI
- OSC Interface provides the ultimate network-transparent control of the engine
- Timestretch and pitch shift

## USAGE

Complete documentation on the operation of SooperLooper can be found
on the website at: http://essej.net/sooperlooper/


The main engine is the executable 'sooperlooper'.  Either the GUI
'slgui' or the console based test client 'slconsole' can be run which
will communicate with a running sooperlooper engine.  slgui will attempt to 
start an engine if none are detected running already. 

Run 'sooperlooper -h' for command line option help.  Note the midi
binding file option -m and take a look at the examples oxy8.slb and
midiwizard.slb for the format to write your own.

The engine can be controlled with MIDI via the ALSA sequencer
interface on Linux and CoreMidi on OS X.  Do learn-style control
binding by choosing the learn option from the context menu's of a
button, slider or control.  Control-Middle-clicking on the button or
slider is a shortcut to start learning.  You can edit, load, and save
the midi bindings through the MIDI Bindings window in the GUI.

## OS X USAGE

For the OS X package, a double-clickable application is provided that
starts the GUI and and engine for you.  Before starting SooperLooper,
be sure to start the JACK system using the JackPilot application that
came with JACK-OSX.  Then use the Routing features of JackPilot to
connect the inputs and outputs of SooperLooper to the desired places.
To connect your MIDI device, you need to use a MIDI patchbay.


## BUILD REQUIREMENTS
| Package       | URL                                                                                                              | Comments                         |
|---------------|------------------------------------------------------------------------------------------------------------------|----------------------------------|
| JACK  >= 0.80 | http://jackaudio.org/                                                                                            |                                  |
| wxWidgets     | https://www.wxwidgets.org/                                                                                       | It should work with the 2.6.x, 2.8, or 3.x versions. For OS X you will want at least the 2.6.1 version of [wxMac](https://wiki.wxwidgets.org/Install#Mac_OS). |
| liblo >= 0.18 | http://liblo.sourceforge.net/                                                                                    | Lightweight OSC library          |
| sigc++ >= 2   | https://libsigcplusplus.github.io/libsigcplusplus/                                                               | You probably already have it.... |
| libsndfile    | http://www.mega-nerd.com/libsndfile/                                                                             |                                  |
| libsamplerate | http://www.mega-nerd.com/SRC/                                                                                    |                                  |
| ncurses       | https://invisible-island.net/ncurses/announce.html                                                               | you probably already have it |
| libxml2       | http://xmlsoft.org/downloads.html                                                                                | same here |
| RubberBand    | https://www.breakfastquay.com/rubberband/                                                                        |                                  |
| fftw3         | http://www.fftw.org/                                                                                             |  (needed by rubberband) |
