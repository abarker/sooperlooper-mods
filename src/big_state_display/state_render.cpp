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

#include <vector>

#include <wx/dcmemory.h>
#include <wx/bitmap.h>
#include <wx/font.h>

#include "state_render.hpp"

using namespace SooperLooper;

namespace {

const int MAX_FONT_SIZE = 72;
const int MIN_FONT_SIZE = 8;

// Widest/tallest extent across every label in `labels`, at a given
// font size. Measured via a throwaway 1x1 wxMemoryDC rather than any
// real, shown widget -- see state_render.hpp's own header comment for
// why that is the whole point of this refactor: a memory DC's font
// metrics come from the font/Pango backend directly, not from any
// particular window's realization state, so there is no probe widget
// left to borrow, warm up, or accidentally corrupt via a reentrant
// call.
wxSize
measure_max_extent (const std::vector<wxString> & labels, int font_size)
{
	wxBitmap dummy (1, 1);
	wxMemoryDC dc;
	dc.SelectObject (dummy);
	dc.SetFont (wxFont (font_size, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));

	int max_w = 0, max_h = 0;
	for (size_t i = 0; i < labels.size(); ++i) {
		wxSize sz = dc.GetTextExtent (labels[i]);
		if (sz.GetWidth()  > max_w) max_w = sz.GetWidth();
		if (sz.GetHeight() > max_h) max_h = sz.GetHeight();
	}
	return wxSize (max_w, max_h);
}

// Every label state_display() can produce, gathered by asking it
// directly rather than duplicating its table -- covers the full
// LooperState range (plugin.hpp: LooperStateUnknown=-1 through
// LooperStateOffMuted=20), including states that share a label
// (Waiting, Mute, ---) via its own grouped/default cases. Only used by
// min_size() now (see its own comment for why that one still needs the
// full worst case, unlike fit_font_size()).
std::vector<wxString>
all_possible_labels ()
{
	std::vector<wxString> labels;
	for (int s = (int) LooperStateUnknown; s <= (int) LooperStateOffMuted; ++s) {
		wxString label;
		wxColour unused_color;
		StateRender::state_display ((LooperState) s, label, unused_color);
		labels.push_back (label);
	}
	return labels;
}

} // anonymous namespace

const int StateRender::DEFAULT_LABEL_MARGIN = 20;

int
StateRender::fit_font_size_raw (const wxSize & available, const std::vector<wxString> & labels)
{
	int avail_w = available.GetWidth();
	int avail_h = available.GetHeight();
	if (avail_w < 1) avail_w = 1;
	if (avail_h < 1) avail_h = 1;

	// Real bug found live, 2026-09-06: on at least one system/font
	// configuration, ONE SPECIFIC point size (confirmed by direct
	// isolated testing: exactly 70, for every label tried, completely
	// reproducible, freshly-created DC each time) resolves to a wildly
	// different and much smaller font than every neighbouring size --
	// e.g. "Record" measured 327x130 at 71pt and 317x126 at 69pt, but
	// only 41x17 at 70pt. Almost certainly a fontconfig/backend
	// substitution quirk (some installed font's fixed-size bitmap
	// "strike" coincidentally matching that one pixel size) rather than
	// anything wrong with the search logic -- but left unguarded, it
	// let the search accept that bogus tiny extent as a genuine fit for
	// nearly any panel size, well before ever reaching the size that
	// actually belonged there. Observed live as the displayed font
	// seeming to only ever take one of two sizes ("large" or "tiny")
	// regardless of the window's actual dimensions -- confirmed by a
	// direct isolated reproduction (test harness, not guesswork) rather
	// than assumed.
	//
	// Guarded generically, not by special-casing the value 70: real
	// font metrics scale smoothly from one point size to the next, so
	// a candidate whose extent is implausibly smaller (more than half)
	// than the last TRUSTED, larger size's own extent is treated as an
	// untrustworthy reading and skipped, rather than accepted as a
	// genuine fit. Only compares against the last reading that was
	// itself trusted, not simply the immediately preceding one, so a
	// bogus reading cannot cascade into rejecting the correct answer
	// right after it.
	wxSize prev_good_extent (-1, -1);
	for (int size = MAX_FONT_SIZE; size >= MIN_FONT_SIZE; --size) {
		wxSize extent = measure_max_extent (labels, size);

		if (prev_good_extent.GetHeight() > 0
		     && extent.GetHeight() * 2 < prev_good_extent.GetHeight()) {
			continue;
		}
		prev_good_extent = extent;

		if (extent.GetWidth() <= avail_w && extent.GetHeight() <= avail_h) {
			return size;
		}
	}

	return MIN_FONT_SIZE;
}

int
StateRender::fit_font_size (const wxSize & panel_size, const std::vector<wxString> & labels,
                             int margin)
{
	return fit_font_size_raw (wxSize (panel_size.GetWidth()  - margin,
	                                    panel_size.GetHeight() - margin),
	                            labels);
}

wxSize
StateRender::min_size (int margin)
{
	wxSize extent = measure_max_extent (all_possible_labels (), MIN_FONT_SIZE);
	return wxSize (extent.GetWidth() + margin, extent.GetHeight() + margin);
}

// Identical table across every version this feature has ever had
// (src/gui/big_state_window.cpp's own in-process copy included) -- see
// mod_docs/ROADMAP.rst entry 6 for the per-state reasoning (labels
// match the main interface's button vocabulary; colors grouped by
// family).
void
StateRender::state_display (LooperState state, wxString & label, wxColour & color)
{
	switch (state) {
	case LooperStateRecording:
		label = wxT("Record");         color = wxColour(200, 20, 20);
		break;
	case LooperStatePlaying:
		label = wxT("Playing");        color = wxColour(20, 150, 20);
		break;
	case LooperStateOverdubbing:
		label = wxT("Overdub");        color = wxColour(220, 120, 0);
		break;
	case LooperStateReplacing:
		label = wxT("Replace");        color = wxColour(220, 120, 0);
		break;
	case LooperStateSubstitute:
		// Abbreviated, 2026-09-04, per direct user request: at 10
		// characters this was by far the longest of the 15 possible
		// labels (next longest, "Multiply"/"Waiting"/"Overdub"/etc.,
		// are 7-8), so it was also the single biggest font-size jump a
		// row could make when transitioning into or out of this state
		// under the current-label-based sizing in fit_font_size() (see
		// state_render.hpp's own comment). Shortening the one outlier
		// directly shrinks that worst-case jump instead of reverting
		// to sizing against every possible label again.
		label = wxT("Subst.");         color = wxColour(220, 120, 0);
		break;
	case LooperStateMultiplying:
		label = wxT("Multiply");       color = wxColour(200, 170, 0);
		break;
	case LooperStateInserting:
		label = wxT("Insert");         color = wxColour(200, 170, 0);
		break;
	case LooperStateDelay:
		label = wxT("Delay");          color = wxColour(120, 60, 160);
		break;
	case LooperStateScratching:
		label = wxT("Scratch");        color = wxColour(120, 60, 160);
		break;
	case LooperStateOneShot:
		label = wxT("Once");           color = wxColour(30, 90, 180);
		break;
	case LooperStatePaused:
		label = wxT("Pause");          color = wxColour(70, 100, 150);
		break;
	case LooperStateMuted:
	case LooperStateOffMuted:
		// Grouped with Muted, not Off, unlike LoopControl's own
		// state_map (loop_control.cpp), which lumps both "off" cases
		// together. Found live, 2026-09-01: pressed mute from Off,
		// engine correctly went to OffMuted, but the display read "Off"
		// -- technically correct per the old grouping, but reads as
		// "nothing happened" for an action the performer just took.
		// OffMuted IS muted (just also empty), so it gets Mute's label.
		label = wxT("Mute");           color = wxColour(90, 90, 90);
		break;
	case LooperStateWaitStart:
	case LooperStateWaitStop:
		label = wxT("Waiting");        color = wxColour(60, 60, 60);
		break;
	case LooperStateOff:
		label = wxT("Off");            color = wxColour(60, 60, 60);
		break;
	default:
		label = wxT("---");            color = wxColour(30, 30, 30);
		break;
	}
}
