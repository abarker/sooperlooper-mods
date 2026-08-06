## Modifications to SooperLooper 1.7.9

This is a modified version of SooperLooper which adds late-hit correction
for record start and record end operations, direct JACK MIDI ports,
BBT-based sync pulse placement, and a `cancel_record_mute` command.

**Late-hit record correction.** When sync is active, a Record press that
arrives within a configurable window after a sync boundary is corrected back
to that boundary, as if the button had been pressed on time.  This applies
to both the press that starts recording and the press that ends it (a second
Record press, or another button such as Mute that exits the record state).
Late record starts are perfectly corrected.  Late record ends are corrected
on all plays except at the beginning of the first loop of an immediate-play
loop (this is due to obvious realtime constraints).

The default correction window is 100ms beyond the sync points; it can be
narrowed by a fractional cap tied to the current quantize cycle length.  Both
values are saved in and loaded from the session file `<Globals>` node:

```xml
<Globals tempo="108" eighth_per_cycle="1" ... sync_source="-1"
         late_hit_window_ms="100" late_hit_max_quant_unit_fraction="0.5"/>
```

**Direct JACK MIDI ports.**  A `sooperlooper:midi_in` port accepts MIDI
commands with sub-buffer sample-accurate timestamps.  A
`sooperlooper:midi_out` port generates 24-ppqn MIDI clock and sends JACK
Transport Start/Stop messages, replacing the need for `a2jmidid` when
driving external hardware.

**`cancel_record_mute` command.**  Abandons a take you have just started:
it cancels a record that is in progress or still pending on a sync
boundary, keeps the previous loop, and does nothing at all in any other
state.  A record that had already begun capturing is discarded and the
looper is left muted; one still waiting on the boundary is simply
cancelled, leaving the loop playing.  This lets a single footswitch carry
both record and "abandon that take" without adding latency to the record
press.  Bind it like any other command, over MIDI or OSC.

For full details see the documentation in [`mod_docs/`](mod_docs/):

- [`late_hit_patch_summary.rst`](mod_docs/late_hit_patch_summary.rst) —
  complete description of all modifications, configuration, and known issues.
- [`sync_quantize_round_relsync.rst`](mod_docs/sync_quantize_round_relsync.rst) —
  background on SooperLooper's sync and quantize model.

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
keys, nothing to waste qbut time if you find it doesn't suit your
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
