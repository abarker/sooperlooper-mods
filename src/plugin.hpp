/*
** Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
**  
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**  
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**  
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**  
*/

#ifndef __sooperlooper_plugin_hpp__
#define __sooperlooper_plugin_hpp__

#include "ladspa.h"

//------ albBegin ----------------------------------
// REL_SYNC_STALE_PHASE_FIX: compile-time switch for a STOCK bug fix; the
// full writeup is the comment block near the top of plugin.cc.  Defined
// here, not in plugin.cc, because looper.cpp and the test harness use it
// too (the tracked count's copy to the other channels, and its reset on
// a sync source change).  Set to 0 to revert to the stock behaviour.
#define REL_SYNC_STALE_PHASE_FIX 1
#if REL_SYNC_STALE_PHASE_FIX
#include <climits>
// lSamplesSinceSync / lTrackedSinceSync value meaning "not known".  Far
// above any real count; the ++ in every state would take over 12 hours
// at 48 kHz with no pulse to carry it past UINT_MAX.
#define REL_SYNC_PHASE_UNKNOWN (UINT_MAX / 2)
#endif
// REL_SYNC_LATE_HIT: compile-time switch for a NEW behaviour (on for the
// rig trial; set to 0 for the previous behaviour); the writeup is the
// comment block near the top of plugin.cc.  A Rel Sync record that starts at the press starts
// trigger_latency + rel_sync_preroll_ms earlier, from the input history,
// and ends as much earlier; a Record stop pressed just past the take's
// end point ends it there instead of recording another Quantize unit.
// Here, not in plugin.cc, for the LoopChunk fields below.
#define REL_SYNC_LATE_HIT 1
#if REL_SYNC_LATE_HIT && !REL_SYNC_STALE_PHASE_FIX
#error "REL_SYNC_LATE_HIT needs REL_SYNC_STALE_PHASE_FIX (a start at the press needs a known count)"
#endif
// SYNC_PREROLL: compile-time switch for a NEW behaviour (on for the rig
// trial, inert until sync_preroll_ms is set above 0; set to 0 for the
// previous behaviour); the writeup is the comment block near the top of
// plugin.cc.  A plain Sync recording starts sync_preroll_ms before the
// boundary, from the input history, and ends as much before its closing
// boundary.  Here, not in plugin.cc, for the LoopChunk field below.
#define SYNC_PREROLL 1
// TRIGGER_LATENCY_ALL_EDGES: compile-time switch for a NEW behaviour, on
// by default since its rig try; set to 0 for
// stock's edges.
// trigger_latency is taken to mean exactly the command latency: the time
// from the press to the engine acting on the command.  Stock uses it only
// at overdub, Multiply and Insert starts (waiting input_latency -
// trigger_latency for the sound played at the press); every other edge
// acts when the command arrives, as if the command latency were
// input_latency.  This switch makes these edges wait input_latency -
// trigger_latency too, so they land on the presses:
//   - the Record command's own start and stop, with Sync off (stock's own
//     TODO in STATE_TRIG_START) -- increment 1;
//   - every other end of a take or a layer that acts when its command
//     arrives (each command's own condition, so some with Sync on too):
//     the same command again, Record -> Overdub, a take ended by any
//     command, a layer ended by any command -- 2a, 2b.
// Takes then begin and end that much later than now; their length (the
// press interval) and playback phase do not change.  The command's own
// effect (a mute, a new layer) comes that much later too; a restart in
// the slack window (Sync on) keeps its phase.  At trigger_latency =
// input_latency it changes nothing.  With trigger_latency >
// input_latency, a take starts trigger_latency - input_latency early,
// from the input history, and ends as much early -- (c).  Unsynced
// Trigger and Once restarts play from the press -- (d).  The late-hit,
// mute, and slack windows are counted from the press -- (e).  Here, not
// in plugin.cc, for the SooperLooperI fields below.
#define TRIGGER_LATENCY_ALL_EDGES 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// ONESHOT_ONE_PASS_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves the same; checked on a dummy server).
//
// The bug: a oneshot (Once) starts at the loop's sync point and ends when
// the position reaches the end of the buffer, so it plays one loop length
// only when the sync point is the buffer's start (lSyncPos 0: Sync off,
// or a take started on the pulse).  A Rel Sync take started anywhere else
// plays part of a pass.  The fork's RESTART_POSITION_FIX makes it worse:
// it starts IL + OL further in, after the wrap, so the buffer's end comes
// IL + OL sooner, and a start in its last IL + OL samples ends the
// oneshot on its first sample.
//
// The fix: every place a oneshot starts sets dOneshotLeft to one loop
// length, less what a slack-window start skips (startOneshotPass() in
// plugin.cc); the play loop counts it down by the rate's size and mutes
// when it runs out, in place of its two buffer-end tests.  A oneshot then
// plays the loop once from its beat 1, wherever that lies in the buffer,
// and ends with the loop's pass.  Here, not in
// plugin.cc, for the SooperLooperI field below.
// Set to 0 to revert to the stock (buggy) behaviour.
#define ONESHOT_ONE_PASS_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// TL_OVER_IL_LAYER_FIX: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves the same; measured on a dummy server).
//
// The bug: when trigger_latency is larger than input_latency, the sound
// played at the press reached the input k = TL - IL samples before the
// command did.  An Overdub, Replace or Substitute started by its command
// reads the input k samples back (lFramesUntilInput = -k, never counted
// up), which is right, but writes it at the usual position, OL + IL
// behind the play position, so the whole layer lands k samples late.
//
// The fix: such a layer writes k further back too (lLayerLag); its fill
// marks allow for it, and the stretch between its write position and the
// play position is filled from the previous layer at once, not as the
// write position crosses it (beginOverdub()).  Both its
// edges then fall on the presses: its first and last samples are the
// ones played at them.  Only a layer started at once by its own command
// (beginOverdub()'s useTriggerLag); one started at a sync pulse, or out
// of a take, reads the current input and keeps the stock timing.
// Multiply and Insert the same (beginMultiply(), beginInsert(); a
// lagged Multiply's rounded end waits lLayerLag longer, and its
// unrounded end fills the whole OL + IL + lLayerLag stretch at once).
// The layer's tail keeps the lag too: STATE_PLAY
// (and the states sharing its block) writes the fade-out lLayerLag
// further back, from the input lLayerLag back, so it follows the layer's
// last sample; the lag goes once the fades are through, and any state
// that does not play the loop on drops it.  Without that the fade-out
// came k after the layer's end, with the old loop between: a hard cut,
// then a separate bump.  Here, not in plugin.cc, for the SooperLooperI
// field below.
// Set to 0 to revert to the stock (buggy) behaviour.
#define TL_OVER_IL_LAYER_FIX 1
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// FREE_PREROLL: compile-time switch for a NEW behaviour, on since its rig
// try; inactive while free_preroll_ms is 0.  Needs
// TRIGGER_LATENCY_ALL_EDGES, whose take edges it moves.
//
// Neither Sync pre-roll reaches a take that does not start on a boundary.
// With this, an unsynced take (Sync off, no threshold) starts P =
// free_preroll_ms (g_free_preroll_ms, plugin.cc) before the press, from
// the input history, so an attack struck with the press, or a pickup just
// before it, is in the loop.  P counts as command latency at both of the
// take's edges: it ends P before the stop press too, so its length is
// still the press interval, and the first pass after the stop is in
// phase.  The loop's sync position (beat 1) is at P, where the start
// press was: a restart puts that point on the restart's press.  At 0
// (the default) nothing changes.  Here, not in plugin.cc, for the fields
// below.
#define FREE_PREROLL 1
#if FREE_PREROLL && !TRIGGER_LATENCY_ALL_EDGES
#error "FREE_PREROLL needs TRIGGER_LATENCY_ALL_EDGES"
#endif
//------ albEnd ----------------------------------

//------ albBegin ----------------------------------
// RECORD_OVER_PLAY_FADE: compile-time safety switch for a STOCK bug
// (stock 1.7.9 behaves the same; measured on a dummy server).
//
// The bug: Record pressed while a loop plays (or records a layer) cut the
// loop off within a sample, with no fade: STATE_TRIG_START, the wait for
// the take to start (the beat with Sync on, a threshold, or
// TRIGGER_LATENCY_ALL_EDGES's countdown), and STATE_RECORD play only the
// dry input.  A click whenever the loop was not near a zero crossing, and
// with Sync on, silence until the beat.
//
// The fix: the loop plays on through the wait, as in STATE_PLAY (its
// fills, and the fade-out of a layer the press ended), and the take fades
// it out over fade_samples from its first sample, from its old chunk,
// which the new take does not touch (it is written after it in the
// buffer; if it would be, the fade stops there).  Only when the loop was
// heard at the press (Play, Oneshot, Overdub, Replace, Substitute); a
// muted loop stays silent.  Also with a threshold: the loop plays until
// the input crosses it.  Here, not in plugin.cc, for the fields below.
// Set to 0 to revert to the stock (buggy) behaviour.
#define RECORD_OVER_PLAY_FADE 1
//------ albEnd ----------------------------------

// TODO, move the whole looping core into a class


namespace SooperLooper {

enum ControlPort {
	TriggerThreshold = 0,
	DryLevel,
	WetLevel,
	Feedback,
	Rate,
	ScratchPosition,
	Multi,
	TapDelayTrigger,
	UseFeedbackPlay,
	Quantize,
	Round,
	RedoTap,
	Sync,
	UseRate,
	FadeSamples,
	TempoInput,
	PlaybackSync,
	EighthPerCycleLoop,
	UseSafetyFeedback,
	InputLatency,
	OutputLatency,
	TriggerLatency,
	MuteQuantized,
	OverdubQuantized,
	SyncOffsetSamples,
	RoundIntegerTempo,
	LASTCONTROLPORT,
	RelativeSync // not an enum
};

enum OutputPort {
	State = 26,
	LoopLength,
	LoopPosition,
	CycleLength,
	LoopFreeMemory,
	LoopMemory,
	Waiting,
	TrueRate,
	NextState,
	LASTPORT
};

enum AudioPort {
	AudioInputPort=35,
	AudioOutputPort,
	SyncInputPort,
	SyncOutputPort,
	PORT_COUNT // must be last
};

enum {
	QUANT_OFF=0,
	QUANT_CYCLE,
	QUANT_8TH,
	QUANT_LOOP
};

enum LooperState
{
	LooperStateUnknown = -1,
	LooperStateOff = 0,
	LooperStateWaitStart,
	LooperStateRecording,
	LooperStateWaitStop,
	LooperStatePlaying,
	LooperStateOverdubbing,
	LooperStateMultiplying,
	LooperStateInserting,
	LooperStateReplacing,
	LooperStateDelay,
	LooperStateMuted,
	LooperStateScratching,
	LooperStateOneShot,
	LooperStateSubstitute,
	LooperStatePaused,
    LooperStateUndoAll,
    LooperStateTriggerPlay,
    LooperStateUndo,
    LooperStateRedo,
    LooperStateRedoAll,
	LooperStateOffMuted = 20
};

	
};

/*****************************************************************************/

// defines all a loop needs to know to cycle properly in memory
// one of these will prefix the actual loop data in our buffer memory
typedef struct _LoopChunk {

	/* pointers in buffer memory. */
	//LADSPA_Data * pLoopStart;
	//LADSPA_Data * pLoopStop;    
	
	// index into main sample buffer
	unsigned long lLoopStart;
	//unsigned long lLoopStop;    
	unsigned long lLoopLength;
	
	// adjustment needed in the case of multiply/insert
	unsigned long lStartAdj;
	unsigned long lEndAdj;
	unsigned long lInsPos; // used only by INSERT mode
	unsigned long lRemLen; // used only by INSERT mode
	
	// markers needed for frontfilling and backfilling
	unsigned long lMarkL;
	unsigned long lMarkH;    
	unsigned long lMarkEndL;
	unsigned long lMarkEndH;        

	unsigned long lSyncOffset; // used for rel sync
	//------ albBegin ----------------------------------
#if REL_SYNC_LATE_HIT || SYNC_PREROLL
	// lStartEarly: samples this take started before its anchor -- a Rel
	// Sync take's press (REL_SYNC_LATE_HIT), a plain Sync take's boundary
	// (SYNC_PREROLL) -- 0 if none; its end comes as much before its end
	// point.  Set by plugin.cc.
	unsigned long lStartEarly;
#endif
#if FREE_PREROLL
	// lFreePreroll: the free pre-roll this take started with
	// (FREE_PREROLL), 0 if none.  Set by plugin.cc.
	unsigned long lFreePreroll;
#endif
#if REL_SYNC_LATE_HIT
	// lRelSyncLastPoint: loop position where a Rel Sync take last passed
	// its end point while recording.  Set by plugin.cc.
	unsigned long lRelSyncLastPoint;
#endif
	//------ albEnd ----------------------------------
	long lSyncPos; // used for retriggering
	long lOrigSyncPos;
	
	int firsttime;
	int frontfill;
	int backfill;
	int valid;
	int mult_out; // used for multi-increase
	
	unsigned long lCycles;
	unsigned long lCycleLength;
	
	// current position is double to support alternative rates easier
	double dCurrPos;
	
	// used when doing frontfills
	LADSPA_Data dOrigFeedback;

	// the loop where we should be frontfilled and backfilled from
	struct _LoopChunk* srcloop;
	
	struct _LoopChunk* next;
	struct _LoopChunk* prev;
	
} LoopChunk;


/* Instance data */
typedef struct {
    
	LADSPA_Data fSampleRate;

	/* the sample memory */
	//LADSPA_Data * pfSampleBuf;
	LADSPA_Data * pSampleBuf;
    
	unsigned int lLoopIndex;
	unsigned int lChannelIndex;
	
	/* Buffer size, IS necessarily a power of two. */
	unsigned long lBufferSize;
	unsigned long lBufferSizeMask;

	LADSPA_Data * pInputBuf;
	unsigned long lInputBufSize;
	unsigned long lInputBufMask;
	unsigned long lInputBufReadPos;
	unsigned long lInputBufWritePos;
	long lFramesUntilInput; // used for input latency compensation
	long lFramesUntilFilled; // used to fill the gaps right after a record
	//------ albBegin ----------------------------------
#if TRIGGER_LATENCY_ALL_EDGES
	// Samples left before a deferred edge fires: input_latency -
	// trigger_latency at the command, counted down once per sample in
	// STATE_TRIG_START (record start) and STATE_TRIG_STOP (record stop).
	long lEdgeDelay;
	// The other way (trigger_latency > input_latency, increment (c)):
	// samples of input history an unsynced take starts with, set by
	// MULTI_RECORD for STATE_TRIG_START's first sample; 0 for none.
	unsigned long lEdgeEarly;
	// And at a take's end by another command: samples STATE_TRIG_STOP
	// trims off the take when it fires (at once); 0 for none.
	unsigned long lEdgeTrim;
#if FREE_PREROLL
	// The free pre-roll MULTI_RECORD chose for the take STATE_TRIG_START
	// starts next (FREE_PREROLL); 0 for none.
	unsigned long lFreePrerollNext;
#endif
	// A deferred take end (Record, or another command pressed while
	// recording): when it fires, send fEdgeStopPulse and redo what the
	// immediate branch does that STATE_TRIG_STOP does not.
	bool bEdgeStopDeferred;
	// The sync pulse a deferred take end sends when it fires (what the
	// command's immediate branch sends, for loops synced to this one; 0
	// for none).
	float fEdgeStopPulse;
	// A deferred overdub, Replace or Substitute end: the layer goes on
	// until lEdgeDelay runs out, then ends as the immediate branch would
	// have (the same command again, increment 2a; another command, 2b-ii).
	bool bEdgeEndDeferred;
	// Where a deferred layer end goes: STATE_PLAY (the same command
	// again, 2a, inline) or another command's state (2b-ii, through
	// transitionToNext()).
	int lEdgeEndNext;
	// A deferred take end by Reverse: negate the rate when it fires.
	bool bEdgeStopReverse;
	// The sync pulse a deferred layer end sends when it fires (what the
	// command's immediate branch sends; 0 for none).
	float fEdgeEndPulse;
	// A deferred take end by Scratch: reverse and place it as Scratch's
	// immediate branch does when it fires.
	bool bEdgeStopScratch;
#endif
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
#if ONESHOT_ONE_PASS_FIX
	// What is left of a oneshot's pass, in samples at rate 1; the play
	// loop mutes when it runs out.
	double dOneshotLeft;
#endif
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
#if TL_OVER_IL_LAYER_FIX
	// How much further back than OL + IL the current layer writes (TL -
	// IL, or 0), and its tail after it; set by beginOverdub(),
	// beginMultiply(), and beginInsert(), 0 once the tail is through.
	long lLayerLag;
#endif
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
#if RECORD_OVER_PLAY_FADE
	// Set by a Record press over a loop that is heard: STATE_TRIG_START
	// plays it on.  The take's start clears it.
	bool bTrigStartPlays;
	// The old loop the take fades out (NULL: none), where it plays, and
	// its rate.
	LoopChunk * pRecFadeOutLoop;
	double dRecFadeOutPos;
	float fRecFadeOutRate;
#endif
	//------ albEnd ----------------------------------
	
	// the loopchunk pool
	LoopChunk * pLoopChunks;
	LoopChunk * lastLoopChunk;
	unsigned long lLoopChunkCount;
	
	LADSPA_Data fTotalSecs;	
	
	/* the current state of the sampler */
	int state;

	int nextState;

	bool wasMuted;
	double dPausedPos;

	bool safetyFeedback;
	
	int waitingForSync;
	bool recSyncEnded;
	
	long lLastMultiCtrl;

	//------ albBegin ----------------------------------
	// CANCEL_OVERDUB_UNDO: the state the most recent command found the
	// loop in, if that command was an overdub press; -1 if the most recent
	// command was anything else.  Set at the start of every command
	// dispatch in run(), read (and cleared) by cancel_overdub_undo.
	long lLastOverdubFrom;
	//------ albEnd ----------------------------------

	// initial location of params
	LADSPA_Data fQuantizeMode;
	LADSPA_Data fRoundMode;    
	LADSPA_Data fRedoTapMode;
	LADSPA_Data fSyncMode;
	LADSPA_Data fMuteQuantized;
	LADSPA_Data fOverdubQuantized;
	
	bool bReplaceQuantized;
    
	// used only when in DELAY mode
	int bHoldMode;

	bool donePlaySync;
	bool rounding;
	
	unsigned long lTapTrigSamples;

	LADSPA_Data fLastOverTrig;    
	unsigned long lOverTrigSamples;    

	unsigned long lRampSamples;
    
	LADSPA_Data fCurrRate;
	LADSPA_Data fNextCurrRate;

	LADSPA_Data fLastScratchVal;
	unsigned long lScratchSamples;
	LADSPA_Data fCurrScratchRate;
	LADSPA_Data fLastRateSwitch;
	int bRateCtrlActive;
    
	LADSPA_Data fLastTapCtrl;
	int bPreTap;
    
	// linked list of loop chunks
	LoopChunk * headLoopChunk;
	LoopChunk * tailLoopChunk;    
	unsigned int lHeadLoopChunk;
	unsigned int lTailLoopChunk;
    
	LADSPA_Data fWetCurr;
	LADSPA_Data fWetTarget;

	LADSPA_Data fDryCurr;
	LADSPA_Data fDryTarget;

	LADSPA_Data fRateCurr;
	LADSPA_Data fRateTarget;

	LADSPA_Data fScratchPosCurr;
	LADSPA_Data fScratchPosTarget;

	LADSPA_Data fFeedbackCurr;
	LADSPA_Data fFeedbackTarget;

	LADSPA_Data fLoopFadeAtten;
	LADSPA_Data fLoopFadeDelta;

	LADSPA_Data fLoopSrcFadeAtten;
	LADSPA_Data fLoopSrcFadeDelta;

	LADSPA_Data fPlayFadeAtten;
	LADSPA_Data fPlayFadeDelta;

	LADSPA_Data fFeedFadeAtten;
	LADSPA_Data fFeedFadeDelta;

	LADSPA_Data fFeedSrcFadeAtten;
	LADSPA_Data fFeedSrcFadeDelta;

	LADSPA_Data fLoopXfadeTime;

	unsigned int lSamplesSinceSync;

	//------ albBegin ----------------------------------
	// lGlobalSamplesSinceSync: continuously-running phase counter for the
	// late-hit window feature.  Incremented every sample in ALL states
	// (including STATE_OFF and STATE_TRIG_START) and reset to 0 on each
	// incoming sync pulse.  Unlike lSamplesSinceSync (which only runs
	// during recording), this counter is valid at button-press time in
	// MULTI_RECORD and gives the elapsed samples since the most recent
	// sync boundary.
	// A pre-buffer snapshot (lGlobalSyncAtButtonPress, taken before the
	// sync-pulse scan each buffer) is used so that a sync pulse arriving
	// mid-buffer does not corrupt the phase reading for button events
	// processed at the buffer boundary.
	unsigned long long lGlobalSamplesSinceSync;
	// bSyncPulseReceived: set true on the first real sync pulse after
	// instantiation.  Guards against the late-hit window firing at startup
	// before any sync has arrived: calloc() zeroes the struct, so
	// lGlobalSamplesSinceSync would otherwise look like a freshly-reset 0
	// and satisfy T <= shiftSamples spuriously.
	bool bSyncPulseReceived;
	// lLastSyncPeriodSamples: length of the most recently *completed* sync
	// period, in samples.  Captured from lGlobalSamplesSinceSync
	// immediately before each sync-pulse reset in the pre-buffer scan.
	// Zero until a second pulse has been observed (one full period).
	// Used together with eighthPerCycle by effectiveLateHitWindow() to
	// apply an optional fraction-of-eighth cap that tracks live tempo.
	unsigned long lLastSyncPeriodSamples;
	// fSyncModeLastBuffer: *pfSyncMode as read at the end of the previous
	// run() call, used solely to detect an off->on transition of the
	// Sync port itself (see SYNC_MODE_LATE_HIT_RESET_FIX below, where
	// this drives a reset of the three fields directly above). Distinct
	// from the unrelated, pre-existing fSyncMode member elsewhere in
	// this struct (a per-channel master/slave role flag, not a port
	// snapshot -- do not confuse the two).
	LADSPA_Data fSyncModeLastBuffer;
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
#if REL_SYNC_STALE_PHASE_FIX
	// REL_SYNC_STALE_PHASE_FIX (plugin.cc): samples since the last sync
	// pulse, tracked on channel 0 in every state whatever the Sync
	// setting, and the longest pulse spacing since it became known (0 =
	// not known).
	// REL_SYNC_PHASE_UNKNOWN when not known.  Looper::run() copies channel
	// 0's count to the other channels before each run().
	// fQuantModeLastBuffer: the previous buffer's Quantize, to see it change.
	unsigned int lTrackedSinceSync;
	unsigned int lTrackedSyncPeriod;
	LADSPA_Data  fQuantModeLastBuffer;
#endif
#if REL_SYNC_LATE_HIT
	// REL_SYNC_LATE_HIT (plugin.cc): how much earlier than the press a
	// Rel Sync record entering STATE_TRIG_START this buffer will start,
	// if it starts at the press; 0 once it waits instead.
	unsigned long lRelSyncPressEarly;
#endif
	// How many times a late start, a pre-roll or a Rel Sync early start
	// was refused because the input history (lInputBufSize) was too short,
	// and the most samples one of them needed.  Written only by the audio
	// thread; Looper::take_history_shortfall() reads them for the warning.
	volatile unsigned long lHistoryShortCount;
	volatile unsigned long lHistoryShortNeeded;
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
	// C2 fix: per-channel bypass flag set by Looper::run() each buffer.
	// True on slave channels (index > 0) when the loop uses its own
	// sync buffers (internal sync mode).  Gates the three late-hit window
	// checks in plugin.cc (withinWindow, stopWithinWindow,
	// multStopInWindow): when true, the late-hit
	// correction is skipped, preventing stereo divergence between a
	// master channel (fSyncMode=0, always immediate) and its slave
	// (fSyncMode=1, late-hit could otherwise fire).
	// In JACK sync mode this flag is always false: zero JACK regression
	// risk.  calloc() zeroes the struct so default is false;
	// activateSooperLooper() also sets it explicitly for clarity.
	bool bLateHitBypassed;
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
	// lRecordFailsafeSamples: elapsed samples spent continuously in
	// STATE_RECORD.  Reset to 0 whenever state is anything else; compared
	// each buffer against g_record_failsafe_seconds (plugin.cc) to
	// auto-fire CANCEL_RECORD_MUTE if a record is left running unattended.
	unsigned long lRecordFailsafeSamples;
	//------ albEnd ----------------------------------

	//------ albBegin ----------------------------------
	// bRecordEnteredFromActivePlay: true iff the RECORD press that led to
	// the current STATE_RECORD span interrupted an actively playing,
	// unmuted loop (state was STATE_PLAY or STATE_ONESHOT at the moment
	// RECORD was pressed). Set once, at command-arrival time, in the
	// MULTI_RECORD case of plugin.cc's remap chain -- before the inner
	// switch(state) dispatches, so it is correct regardless of which path
	// (immediate late-hit entry or deferred STATE_TRIG_START) the press
	// then takes. Gates the record_failsafe counter (runSooperLooper(),
	// ahead of the remap chain): the failsafe now only ever fires for a
	// record that cut off a playing take, not a first/fresh recording
	// from an empty loop.
	// Also gates cancel_record_mute (plugin.cc): the command is ignored
	// for a record started from Mute, Paused or an empty loop.
	// calloc() zeroes the struct so default is false, matching
	// bLateHitBypassed's convention above.
	bool bRecordEnteredFromActivePlay;
	//------ albEnd ----------------------------------

	/* Ports:
	   ------ */

    

	LADSPA_Data * pfWet;
    
	LADSPA_Data * pfDry;

    
	/* Feedback 0 for none, 1 for infinite */
	LADSPA_Data * pfFeedback;

	/* Trigger level for record and stop record */
	LADSPA_Data * pfTrigThresh;


	/* The rate of loop playback, if RateSwitch is on */
	LADSPA_Data * pfRate;

	/* The destination position in the loop to scratch to. 0 is the start */
	/*  and 1.0 is the end of the loop.  Only active if RateSwitch is on */
	LADSPA_Data * pfScratchPos;

	/* The multicontrol port.  Each value from (0-127) has a
	 * meaning.  This is considered a momentary control, thus
	 * ANY change to a value within the value range is only
	 * noticed at the moment it changes from something different.
	 *  If you want to do two identical values in a row, you must change
	 * the value to something outside our range for a cycle before using
	 * the real value again.
	 */
	LADSPA_Data * pfMultiCtrl;

	LADSPA_Data * pfUseFeedbackPlay;
    
	/* changes on this control signal with more than TAP_THRESH_SAMP samples
	 * between them (to handle settle time) is treated as a a TAP Delay trigger
	 */
	LADSPA_Data *pfTapCtrl;

	/* non zero here toggle quantize and round mode
	 *  WARNING: the plugin may set this value internally... cause I want
	 *  it controllable (via mute mode)
	 */
	LADSPA_Data *pfQuantMode;
	LADSPA_Data *pfRoundMode;    
	LADSPA_Data *pfSyncMode;    
	LADSPA_Data *pfPlaybackSyncMode;    
	LADSPA_Data *pfRateCtrlActive;
	LADSPA_Data *pfMuteQuantized;
	LADSPA_Data *pfOverdubQuantized;
	LADSPA_Data *pfSyncOffsetSamples;
	LADSPA_Data *pfRoundIntegerTempo;

	LADSPA_Data *pfXfadeSamples;

	LADSPA_Data *pfTempo;
	LADSPA_Data *pfEighthPerCycle;
	LADSPA_Data *pfUseSafetyFeedback;

	LADSPA_Data *pfInputLatency;
	LADSPA_Data *pfOutputLatency;
	LADSPA_Data *pfTriggerLatency;

	
	/* if non zero, the redo command is treated like a tap trigger */
	LADSPA_Data *pfRedoTapMode;
    
	/* Input audio port data location. */
	LADSPA_Data * pfInput;
    
	/* Output audio port data location. */
	LADSPA_Data * pfOutput;

	LADSPA_Data * pfSyncInput;
	LADSPA_Data * pfSyncOutput;

    
	/* Control outputs */

	LADSPA_Data * pfStateOut;
	LADSPA_Data * pfLoopLength;
	LADSPA_Data * pfLoopPos;        
	LADSPA_Data * pfCycleLength;

	/* how many seconds of loop memory free and total */
	LADSPA_Data * pfSecsFree;
	LADSPA_Data * pfSecsTotal;    

	LADSPA_Data * pfWaiting;    
	LADSPA_Data * pfRateOutput;
	LADSPA_Data * pfNextStateOut;    
	
} SooperLooperI;



// reads loop audio into buffer, up to frames length, starting from loop_offset.  if fewer frames are
// available returns amount read.  if 0 is returned loop is done.
extern unsigned long sl_read_current_loop_audio (LADSPA_Handle instance, float * buf, unsigned long frames, unsigned long loop_offset);

// override current samples since sync
extern void sl_set_samples_since_sync (LADSPA_Handle instance, unsigned long frames);

extern void sl_set_replace_quantized (LADSPA_Handle instance, bool value);
extern bool sl_get_replace_quantized (LADSPA_Handle instance);
extern void sl_set_loop_index (LADSPA_Handle instance, unsigned int index, unsigned int chan);

extern bool sl_has_loop (const LADSPA_Handle instance);

#endif
