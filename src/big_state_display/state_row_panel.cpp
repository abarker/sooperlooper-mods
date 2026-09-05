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

#include <wx/dcbuffer.h>

#include "state_row_panel.hpp"

BEGIN_EVENT_TABLE(StateRowPanel, wxPanel)
	EVT_PAINT(StateRowPanel::on_paint)
	EVT_SIZE(StateRowPanel::on_size)
END_EVENT_TABLE()

StateRowPanel::StateRowPanel (wxWindow * parent, wxWindowID id)
	: wxPanel (parent, id)
	, _color (30, 30, 30)
	, _label (wxT("---"))
	, _font_pt (24)
{
	// This panel paints its own background (see on_paint()) -- telling
	// wx not to separately erase it first avoids a visible one-frame
	// flicker to the default system colour on every repaint, and is the
	// standard idiom for a fully custom-drawn window.
	SetBackgroundStyle (wxBG_STYLE_PAINT);
}

void
StateRowPanel::set_state (const wxColour & color, const wxString & label)
{
	if (color == _color && label == _label) {
		return;
	}
	_color = color;
	_label = label;
	Refresh();
}

void
StateRowPanel::set_font_point_size (int pt)
{
	if (pt == _font_pt) {
		return;
	}
	_font_pt = pt;
	Refresh();
}

void
StateRowPanel::on_size (wxSizeEvent & ev)
{
	// A resize changes where "centered" is, even though neither the
	// colour, the label text, nor the font size changed -- explicit
	// Refresh() rather than relying on the platform to always repaint
	// the exposed region correctly on every resize.
	Refresh();
	ev.Skip();
}

void
StateRowPanel::on_paint (wxPaintEvent & /*ev*/)
{
	wxAutoBufferedPaintDC dc (this);

	wxSize size = GetClientSize();
	dc.SetBackground (wxBrush (_color));
	dc.Clear();

	dc.SetFont (wxFont (_font_pt, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
	dc.SetTextForeground (*wxWHITE);

	wxSize text_size = dc.GetTextExtent (_label);

	// Centers against THIS row's own actual current size, queried
	// fresh on every paint -- there is no separate "declared minimum"
	// or cached size to keep in sync with reality, unlike the previous
	// design's SetMinSize()/manual-SetSize() split. The owning frame is
	// expected to keep the shared font size fitting every row's
	// current size (see BigStateWindow/BigStateStackWindow's own
	// on_size()), so clamping to non-negative here is a defensive
	// floor for an odd in-between frame, not the primary fit mechanism.
	int x = (size.GetWidth()  - text_size.GetWidth())  / 2;
	int y = (size.GetHeight() - text_size.GetHeight()) / 2;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	dc.DrawText (_label, x, y);
}
