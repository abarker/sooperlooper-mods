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

// StateRowPanel: one self-contained "loop state" row -- a solid-colour
// panel with one big, centered, bold text label. Used both as the
// single row inside BigStateWindow and as each of N rows inside
// BigStateStackWindow.
//
// 2026-09-06 refactor, replacing a wxPanel+child-wxStaticText design in
// both window classes that had, over several live-debugging rounds,
// accumulated a SetMinSize()-based sizer-uniformity hack, a hand-rolled
// SetSize()-based centering routine (replacing an earlier centering
// sizer that could not be made to recompute reliably), a shared
// "measurement probe" widget borrowed across rows with its own
// save/restore and reentrancy concerns, and reentrancy/de-dup guards on
// both classes' on_size() handlers. Even after all of that, real
// resize bugs remained live: shrinking the window's height could still
// cut off lower rows instead of shrinking all of them evenly, and the
// font could appear to never change size at all. The root cause was
// architectural, not any one of those individual bugs: three separate
// subsystems (wxStaticText::GetBestSize(), wxSizer's SetMinSize()
// negotiation, and this class's own manual SetSize() positioning) all
// had to agree exactly on "how big is this text," and a per-row
// SetMinSize() computed from the CURRENT font meant a wxSizer could be
// asked for more total minimum height than a shrunk window actually
// had -- at which point wxSizer's own documented behaviour is to let
// later items get clipped rather than shrink every item evenly, which
// is exactly the "cuts off the lower boxes" symptom.
//
#ifndef __big_state_display_state_row_panel__
#define __big_state_display_state_row_panel__

#include <wx/wx.h>

// This class collapses all of that to one mechanism: it paints itself
// directly (OnPaint), measuring and drawing the label with the exact
// same wxDC call sequence, so there is nothing left to disagree. It
// declares no minimum size tied to its current font at all -- it just
// draws to fit whatever client size it is actually given -- so a
// plain wxBoxSizer with equal proportion-1 EXPAND rows divides height
// evenly among them unconditionally, at any window size, because
// their declared minimums (wxPanel's own small, fixed default) can
// never sum past what is available.
class StateRowPanel
	: public wxPanel
{
  public:
	StateRowPanel (wxWindow * parent, wxWindowID id = -1);

	// Sets this row's background colour and label text together -- they
	// always change together, from the same state_display() lookup. A
	// no-op (no Refresh()) if neither actually changed.
	void set_state (const wxColour & color, const wxString & label);

	// The point size to draw the label at, in this row's own current
	// font. Pushed down from the owning frame, which computes ONE
	// shared size across every row (StateRender::fit_font_size()) so
	// the font never visibly differs between rows in different states,
	// or jumps size on a state change alone. A no-op if unchanged.
	void set_font_point_size (int pt);

  protected:
	void on_paint (wxPaintEvent & ev);
	void on_size (wxSizeEvent & ev);

	wxColour _color;
	wxString _label;
	int      _font_pt;

  private:
	DECLARE_EVENT_TABLE()
};

#endif
