## Modifications to SooperLooper 1.7.9

This is a modified version of SooperLooper which adds late-hit correction
for record, mute/unmute, and multiply operations, two new commands around
recording safety, direct JACK MIDI ports, JACK sync that stays on the
beat through tempo changes, and a standalone at-a-distance loop-state
display.

**Late-hit correction.** When sync is active, a press that lands within a
configurable window after a sync boundary is corrected back to that
boundary instead of waiting for the next one — covering Record start/end,
Mute/Unmute, and Multiply end. The window defaults to 100ms, narrowable to
a fraction of the current quantize cycle, and is configurable via the
session file or live over OSC. Also fixes a stock 1.7.9 bug where a
quantized Pause press while Muted resolved to Playing instead of Paused.

**`cancel_record_mute` and record failsafe.**  Plain `record` already
carries a loop from recording into play on its own — that part needs
nothing new.  What `cancel_record_mute` adds is a clean way to stop that
play again without needing a second button.  An optional second trigger on the same switch does the
stopping: a long hold or a quick double tap, always following the eager
`record` that started it.  It always fires immediately and eagerly;
waiting to see whether a second trigger will follow would cost the very
timing accuracy this patch exists to protect.  A separate button bound to
`mute_trigger` would do the same job for a playing loop, but costs a
second pedal and is one more control to remember under performance
pressure.  Internally
`cancel_record_mute` works by discarding the take and restoring the
previous loop muted, but that's implementation, not intent — the performer
isn't undoing a mistake, they're choosing to stop play in favor of
silence, on the same pedal.  A companion setting, record failsafe (off by
default), covers the
case where that second trigger never comes at all — a record simply left
running too long, with no press of any kind.

**JACK sync through tempo changes.**  With a JACK timebase master such
as Hydrogen, stock SooperLooper's sync pulses drift off the master's beat
-- by up to half a beat -- after a tempo change made while the transport
is rolling, and stay off until the next relocate.  This fork counts beats
through tempo changes, so the pulses stay on the beat.

**Direct JACK MIDI ports.**  A `sooperlooper:midi_in` port accepts MIDI
commands with sub-buffer sample-accurate timestamps.  A
`sooperlooper:midi_out` port generates 24-ppqn MIDI clock and sends JACK
Transport Start/Stop messages, replacing the need for `a2jmidid` when
driving external hardware.

**`slbigstate`.**  A standalone program showing one or more loops' current
state in large, high-contrast text — readable from across a room, unlike
slgui's own small indicators.

**`sllatency`.**  A latency calibration tool.  It plays a click, times it
coming back through a mic (or a cable), and checks JACK's latency settings
and SooperLooper's own input and output latency, including the delay of
whatever the loops are monitored through.  It then offers to apply and
save corrected SooperLooper values.

For full details see the documentation in [`mod_docs/`](mod_docs/):

- [`late_hit_patch_summary.rst`](mod_docs/late_hit_patch_summary.rst) —
  complete description of all modifications, configuration, and known issues.
- [`sync_quantize_round_relsync.rst`](mod_docs/sync_quantize_round_relsync.rst) —
  background on SooperLooper's sync and quantize model.
- [`sooperlooper_and_ardour_setup_guide.rst`](mod_docs/sooperlooper_and_ardour_setup_guide.rst) —
  a practical low-budget live-looping setup and latency-calibration guide.

All new code is bracketed with `albBegin` / `albEnd` markers; search for
`albBegin` to locate every changed block.

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
