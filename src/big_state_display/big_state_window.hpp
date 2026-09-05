/*
** Copyright (C) 2026 Allen Barker
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

// BigStateWindow: a frame showing one loop's state, large, for reading
// at a distance. Talks to a standalone OscStateClient rather than
// slgui's shared LoopControl -- no shared instance to be starved by, by
// construction.
//
// 2026-09-06 refactor: this class used to own its rendering directly
// (a wxPanel + child wxStaticText, positioned and sized by hand) and
// shared that logic with BigStateStackWindow only via a handful of
// static methods BigStateStackWindow called across from its own file.
// All of the actual rendering is now StateRowPanel's job (shared, not
// duplicated, with BigStateStackWindow, which is just N of them) --
// see that class's own header comment for the live-found bugs this
// removes an entire architecture's worth of. This class is now just
// "a frame containing exactly one StateRowPanel, wired to one
// OscStateClient" -- see BigStateStackWindow for "a frame containing N
// of them."
#ifndef __big_state_display_big_state_window__
#define __big_state_display_big_state_window__

#include <wx/wx.h>

#include "plugin.hpp"
#include "display_config.hpp"

class OscStateClient;
class StateRowPanel;

class BigStateWindow
	: public wxFrame
{
  public:

	// Takes DisplayConfig by reference and saves into it from this
	// object's OWN destructor, not from BigStateApp::OnExit() -- found
	// live (segfault on exit, 2026-09-01): by the time OnExit() runs, wx
	// has already destroyed the window as part of its normal close
	// handling, so calling GetScreenPosition()/GetSize() on it there is
	// a use-after-free. A derived class's own destructor still has a
	// fully valid wxFrame base to call those on (the base subobject is
	// not torn down until after this destructor's body finishes) --
	// exactly how AppFrame::~AppFrame() already does the equivalent save
	// safely for the main slgui window.
	// margin: pixels reserved on every side of the label when deciding
	// what font size fits -- see main.cpp's --margin option (added per
	// direct user request once the underlying per-current-label sizing
	// fix below was confirmed working).
	BigStateWindow (OscStateClient & osc_client, DisplayConfig & config,
	                 const wxPoint& pos = wxDefaultPosition,
	                 const wxSize& size = wxSize(400, 200),
	                 long style = wxDEFAULT_FRAME_STYLE,
	                 const wxString& name = wxT("BigStateWin"),
	                 int margin = -1);

	virtual ~BigStateWindow();

  protected:

	void init();
	void on_state_check_timer (wxTimerEvent & ev);
	void on_timer (wxTimerEvent & ev);
	void on_size (wxSizeEvent & ev);
	void apply_state (SooperLooper::LooperState state);

	// Recomputes and applies the font size for _row's CURRENT label
	// against the frame's CURRENT client size -- called after either
	// changes (apply_state() when the state itself changes, on_size()
	// when the window is resized). See state_render.hpp's own
	// fit_font_size() comment for why this is keyed on the actual
	// current label rather than the worst case across every possible
	// one.
	void update_font ();

	OscStateClient & _osc_client;
	DisplayConfig &  _config;
	StateRowPanel *  _row;
	int              _margin;
	wxTimer *        _maintain_timer;
	// Drives the state check that used to be an EVT_IDLE handler with
	// an unconditional wxIdleEvent::RequestMore() -- found live,
	// 2026-09-06, to busy-poll the CPU at ~100% permanently (RequestMore()
	// tells wx to never let the event loop go idle, so it never sleeps).
	// Present since this class's very first build, 2026-09-01 -- not
	// something introduced by any of this session's font/resize work,
	// just never noticed before. A plain bounded-interval timer instead:
	// same responsiveness a person can perceive (50ms is imperceptibly
	// fast for a status light), but the process actually sleeps between
	// checks instead of spinning.
	wxTimer *        _state_check_timer;
	SooperLooper::LooperState _last_state;

	// De-dup key for on_size(): StateRender::fit_font_size() is cheap
	// but there is no reason to repeat it for a client size already
	// handled -- a live window drag fires many EVT_SIZE events for the
	// same effective size in a row. Purely a performance guard now, not
	// a correctness one: StateRowPanel::set_font_point_size() already
	// no-ops on an unchanged value on its own.
	wxSize           _last_processed_client;

  private:
	DECLARE_EVENT_TABLE()
};

#endif
