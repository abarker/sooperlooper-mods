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

// BigStateStackWindow: N single-loop displays stacked vertically in one
// window -- "just a bunch of the same things, stacked on top of each
// other" (user's own framing).
//
// Sizing, per the user's own instruction: each row is exactly the size a
// single-loop BigStateWindow would be (same --size/default semantics,
// same font); the window's total height is simply that row size times
// the number of loops. This is what makes the one-loop case fall out
// correct for free -- a 1-row stack is pixel-identical in size to the
// original single-loop window, not a special case to test separately.
//
// 2026-09-06 refactor: this class is now "a frame containing N
// StateRowPanel rows plus N-1 divider lines," full stop -- all of the
// actual rendering (colour, label, centering, font) is StateRowPanel's
// job, shared with BigStateWindow rather than duplicated or reached
// into via cross-class static calls. See StateRowPanel's own header
// comment for the live-found bug history this replaces: a
// SetMinSize()-based sizer-uniformity hack, a hand-rolled SetSize()
// centering routine, a shared measurement-probe widget, and
// reentrancy/de-dup guards on on_size() are all gone, not because they
// were individually wrong, but because the architecture that needed
// them (a child wxStaticText positioned by hand) is gone.

#ifndef __big_state_display_big_state_stack_window__
#define __big_state_display_big_state_stack_window__

#include <vector>

#include <wx/wx.h>

#include "plugin.hpp"
#include "display_config.hpp"

class OscStateClient;
class StateRowPanel;

class BigStateStackWindow
	: public wxFrame
{
  public:

	// clients: one OscStateClient per row, in display order (top to
	// bottom). NOT owned by this class -- same ownership shape as
	// BigStateWindow's single client: BigStateApp constructs and deletes
	// them, this class only holds references for the lifetime of the
	// window. row_size: the size of ONE row, matching a single-loop
	// BigStateWindow exactly (see header comment above) -- the window's
	// actual size is row_size with the height multiplied by
	// clients.size().
	// margin: pixels reserved on every side of each row's label when
	// deciding what font size fits -- see main.cpp's --margin option.
	BigStateStackWindow (std::vector<OscStateClient *> & clients,
	                       DisplayConfig & config,
	                       const wxPoint & pos = wxDefaultPosition,
	                       const wxSize & row_size = wxSize(400, 200),
	                       long style = wxDEFAULT_FRAME_STYLE,
	                       const wxString & name = wxT("BigStateStackWin"),
	                       int margin = -1);

	virtual ~BigStateStackWindow();

  protected:

	void init();
	void on_state_check_timer (wxTimerEvent & ev);
	void on_timer (wxTimerEvent & ev);
	void on_size (wxSizeEvent & ev);
	void apply_state (size_t row, SooperLooper::LooperState state);

	// Recomputes and applies the ONE shared font size for every row's
	// CURRENT label against the frame's CURRENT client size -- called
	// after either changes. See BigStateWindow::update_font()'s own
	// comment and state_render.hpp's fit_font_size() for why this is
	// keyed on the labels actually being shown right now rather than
	// the worst case across every possible one.
	void update_fonts ();

	std::vector<OscStateClient *> & _clients;
	DisplayConfig &  _config;
	wxSize           _row_size;
	int              _margin;
	std::vector<StateRowPanel *> _rows;
	std::vector<SooperLooper::LooperState> _last_states;
	wxTimer *        _maintain_timer;
	wxTimer *        _state_check_timer;

	// De-dup key for on_size() -- see BigStateWindow's own field of the
	// same name; purely a performance guard (StateRowPanel::
	// set_font_point_size() already no-ops on an unchanged value), not
	// a correctness one.
	wxSize           _last_processed_client;

  private:
	DECLARE_EVENT_TABLE()
};

#endif
