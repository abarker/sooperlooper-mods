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

// BigStateWindow: standalone-binary port of src/gui/big_state_window.hpp.
// Same wxFrame/wxPanel/sizer/wxStaticText structure, same state_display()
// table, same EVT_IDLE refresh pattern, and both bugs already found and
// fixed in the in-process version (the is_updated() shared-flag race, the
// missing Layout() call after SetLabel()) carried over rather than
// re-risked. The only real change: talks to a standalone OscStateClient
// instead of slgui's LoopControl -- no shared LoopControl instance to be
// starved by, by construction. Full design: mod_docs/ROADMAP.rst entry 6.

#ifndef __big_state_display_big_state_window__
#define __big_state_display_big_state_window__

#include <wx/wx.h>

#include "plugin.hpp"
#include "display_config.hpp"

class OscStateClient;

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
	BigStateWindow (OscStateClient & osc_client, DisplayConfig & config,
	                 const wxPoint& pos = wxDefaultPosition,
	                 const wxSize& size = wxSize(400, 200),
	                 long style = wxDEFAULT_FRAME_STYLE,
	                 const wxString& name = wxT("BigStateWin"));

	virtual ~BigStateWindow();

	// Look up the display label and background color for a state.
	static void state_display (SooperLooper::LooperState state,
	                            wxString & label, wxColour & color);

  protected:

	void init();
	void on_idle (wxIdleEvent & ev);
	void on_timer (wxTimerEvent & ev);
	void apply_state (SooperLooper::LooperState state);

	OscStateClient & _osc_client;
	DisplayConfig &  _config;
	wxPanel *      _panel;
	wxStaticText * _label;
	wxTimer *      _maintain_timer;
	SooperLooper::LooperState _last_state;

  private:
	DECLARE_EVENT_TABLE()
};

#endif
