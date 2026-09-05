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

// StateRender: shared, stateless rendering helpers for BigStateWindow
// (one row) and BigStateStackWindow (N rows) -- state_display()'s
// label/colour table, and the font-fit search both window classes use
// to keep every row's text at one shared, legible point size.
//
// 2026-09-06 refactor: this used to be a set of static methods on
// BigStateWindow that BigStateStackWindow reached across to call, with
// the actual measurement done by borrowing one of BigStateWindow's own
// live wxStaticText widgets as a "probe" (saving/restoring its font and
// label around each call). That measure-via-a-real-widget design was
// itself a fix for an earlier problem (a throwaway hidden wxStaticText
// gave unreliable wxStaticText::GetBestSize() results on this GTK
// build) -- but it went on to cause most of this feature's hardest
// live-found bugs: a shared probe widget's font/label state could be
// left corrupted by a reentrant call landing mid-measurement, which
// then fed a wrong, permanent font into every row via SetFont(); and
// wxStaticText::GetBestSize() disagreeing slightly with a plain DC's
// own text-extent measurement was the root cause of at least one
// separate centering bug. This version measures with a small internal
// wxMemoryDC instead: font metrics come from the font/Pango backend
// itself, not from any particular widget's realization state, so there
// is no widget to borrow, warm up, or worry about corrupting, and (see
// StateRowPanel) the same DC-based measurement is what actually draws
// the text too, so the two can no longer disagree with each other by
// construction.
#ifndef __big_state_display_state_render__
#define __big_state_display_state_render__

#include <vector>

#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/colour.h>

#include "plugin.hpp"

namespace StateRender {

// Reserved on every side when deciding whether a label fits -- was a
// fixed, hardcoded constant; now a real default so it can be overridden
// (see main.cpp's --margin option, added per direct user request once
// the underlying sizing logic itself was confirmed working).
extern const int DEFAULT_LABEL_MARGIN;

// Look up the display label and background colour for a state. Labels
// use the main interface's button vocabulary (Record, Overdub,
// Multiply, ...), not LoopControl::state_map's own lowercase
// continuous-tense strings -- see mod_docs/ROADMAP.rst entry 6 for the
// full per-state reasoning (colors grouped by family; Muted/OffMuted
// share "Mute" rather than following state_map's Off/Muted grouping).
void state_display (SooperLooper::LooperState state,
                     wxString & label, wxColour & color);

// The largest bold-Swiss point size at which every label in `labels`
// fits within `available` (margin already reserved by the caller --
// this function does not subtract one itself; see fit_font_size()
// below for the version that does, using `margin` on every side).
//
// 2026-09-06: `labels` is deliberately the set of labels actually being
// shown right now (BigStateWindow's own current one; every row's
// current one for BigStateStackWindow), NOT every label
// state_display() can ever produce. An earlier version always checked
// against all 15 possible labels, so the font stayed small enough to
// fit the single longest one ("Substitute") even when every row was
// actually showing "Off" -- found live as "way too much margin around
// the small sizes" once the rest of this feature's resize bugs were
// fixed and the wasted space became the obviously remaining problem.
// Still one shared computation across every row of a stack (so rows
// never independently disagree on size at a given moment -- the
// original reason for sharing this at all), just no longer padded out
// for hypothetical labels nothing on screen is currently showing. The
// real trade-off, accepted deliberately: the font can now change size
// when a row's own state changes to a longer or shorter word, where it
// used to hold perfectly still across every state change. Recompute on
// both resize and on any state change (see the two window classes' own
// call sites) to keep it current either way.
int fit_font_size_raw (const wxSize & available, const std::vector<wxString> & labels);

// Convenience wrapper: subtracts `margin` from every side of
// `panel_size` itself (clamped to at least 1px each dimension) before
// calling fit_font_size_raw() -- what both window classes actually call.
int fit_font_size (const wxSize & panel_size, const std::vector<wxString> & labels,
                    int margin = DEFAULT_LABEL_MARGIN);

// The smallest size that can show even the MIN_FONT_SIZE rendering of
// the widest/tallest of every POSSIBLE label (not just those currently
// shown) without clipping -- the real floor fit_font_size() could ever
// need, at construction time before any real state is known yet. Used
// as a SetSizeHints() hint by both window classes (best-effort only --
// not every window manager honours it, and this refactor deliberately
// does not try to enforce it actively any more; see the two window
// classes' own on_size() comments for why the earlier
// active-enforcement mechanism was dropped).
wxSize min_size (int margin = DEFAULT_LABEL_MARGIN);

}  // namespace StateRender

#endif
