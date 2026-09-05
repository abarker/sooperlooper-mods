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

#include "big_state_stack_window.hpp"
#include "state_row_panel.hpp"
#include "state_render.hpp"
#include "osc_state_client.hpp"

using namespace SooperLooper;

enum {
	ID_StackMaintainTimer = 9100,
	ID_StackStateCheckTimer = 9101
};

// Thickness in pixels of the solid divider line drawn between adjacent
// rows -- see the comment at its one use site in init() for why a fixed
// solid-black panel rather than the default-themed wxStaticLine.
static const int DIVIDER_HEIGHT = 3;

// See BigStateWindow.cpp's own STATE_CHECK_MS comment -- same fixed,
// bounded-interval replacement for the EVT_IDLE/RequestMore() busy-poll
// bug found live 2026-09-06.
static const int STATE_CHECK_MS = 50;

BEGIN_EVENT_TABLE(BigStateStackWindow, wxFrame)
	EVT_TIMER(ID_StackMaintainTimer, BigStateStackWindow::on_timer)
	EVT_TIMER(ID_StackStateCheckTimer, BigStateStackWindow::on_state_check_timer)
	EVT_SIZE(BigStateStackWindow::on_size)
END_EVENT_TABLE()

namespace {

// Builds "SooperLooper State - Loop N" for one loop (byte-for-byte the
// same format BigStateWindow's own title uses, so a 1-row stack reads
// identically to the original single-loop window -- not just the same
// size, the same title too) or "SooperLooper State - Loops A,B,C" for
// more than one.
wxString
build_title (const std::vector<OscStateClient *> & clients)
{
	if (clients.size() == 1) {
		return wxString::Format (wxT("SooperLooper State - Loop %d"),
		                           clients[0]->get_loop_index());
	}

	wxString list;
	for (size_t i = 0; i < clients.size(); ++i) {
		if (i > 0) {
			list += wxT(",");
		}
		list += wxString::Format (wxT("%d"), clients[i]->get_loop_index());
	}
	return wxT("SooperLooper State - Loops ") + list;
}

} // anonymous namespace

BigStateStackWindow::BigStateStackWindow (std::vector<OscStateClient *> & clients,
                                             DisplayConfig & config,
                                             const wxPoint & pos, const wxSize & row_size,
                                             long style, const wxString & name, int margin)
	: wxFrame(0, -1, build_title (clients), pos,
	          wxSize (row_size.GetWidth(), row_size.GetHeight() * (int) clients.size()),
	          style, name)
	, _clients(clients)
	, _config(config)
	, _row_size(row_size)
	, _margin(margin >= 0 ? margin : StateRender::DEFAULT_LABEL_MARGIN)
	, _maintain_timer(0)
	, _state_check_timer(0)
	, _last_processed_client(-1, -1)
{
	init();
}

BigStateStackWindow::~BigStateStackWindow()
{
	// Same fix as BigStateWindow's own segfault-on-exit (found live,
	// 2026-09-01): save here, in this object's own destructor, while its
	// wxFrame base is still valid -- not from BigStateApp::OnExit(),
	// which runs after wx has already destroyed the window.
	//
	// Saves the PER-ROW size, not the stack's total height, by dividing
	// back out by the row count -- so a later run with a different
	// number of loops still gets the remembered row size, just stacked
	// to whatever height that run's loop count needs, rather than a
	// stale total height left over from a different N.
	_config.set_position (GetScreenPosition());
	wxSize total = GetSize();
	int rows = (int) _rows.size();
	if (rows > 0) {
		_config.set_size (wxSize (total.GetWidth(), total.GetHeight() / rows));
	}
	_config.save ();

	if (_maintain_timer) {
		_maintain_timer->Stop();
		delete _maintain_timer;
	}
	if (_state_check_timer) {
		_state_check_timer->Stop();
		delete _state_check_timer;
	}
}

void BigStateStackWindow::init()
{
	wxBoxSizer * mainsizer = new wxBoxSizer(wxVERTICAL);

	size_t n = _clients.size();
	_rows.resize (n);
	_last_states.assign (n, LooperStateUnknown);

	for (size_t i = 0; i < n; ++i) {
		// Demarcation line between rows -- without it, two adjacent loops
		// that happen to be in the same state (both Playing, say) would
		// show as one undivided block of color with no visible boundary,
		// since each row is otherwise just a solid-colour panel abutting
		// the next. A plain, fixed-height, solid-black panel rather than
		// the default-themed wxStaticLine: every state colour in
		// state_display() is fairly saturated but none is pure black, so
		// this stays visible against all of them regardless of system
		// theme, where a thin system-grey line might not.
		if (i > 0) {
			wxPanel * divider = new wxPanel (this, -1, wxDefaultPosition, wxSize(-1, DIVIDER_HEIGHT));
			divider->SetBackgroundColour (*wxBLACK);
			mainsizer->Add (divider, 0, wxEXPAND);
		}

		StateRowPanel * row = new StateRowPanel (this);
		mainsizer->Add (row, 1, wxEXPAND);
		_rows[i] = row;

		// NOT apply_state() here -- it calls update_fonts(), which
		// iterates every row in _rows, including ones this loop has not
		// constructed yet (still null at this point, since the whole
		// vector was resize()'d up front). Seed the initial colour/label
		// directly instead; update_fonts() runs once, safely, after
		// every row actually exists, right below.
		wxString label;
		wxColour color;
		StateRender::state_display (LooperStateUnknown, label, color);
		row->set_state (color, label);
	}

	SetSizer(mainsizer);

	// row_size (the constructor's own per-row size) is exactly each
	// row's actual initial size -- the frame was constructed with
	// height = row_size.height * N precisely so this holds; no need to
	// ask any row for its size before the first Show()/layout pass.
	// GetClientSize() already reflects that constructed size at this
	// point, even before Show() -- same assumption BigStateWindow's own
	// init() makes.
	_last_processed_client = GetClientSize();
	update_fonts ();

	// A best-effort floor, scaled by row count plus divider heights --
	// not every window manager honours SetSizeHints(), and this
	// refactor deliberately does not try to force it any more (see
	// on_size()'s own comment for why the earlier active-enforcement
	// mechanism, itself the source of two live-found bugs, was
	// dropped rather than carried over).
	wxSize min_row = StateRender::min_size (_margin);
	SetSizeHints (min_row.GetWidth(),
	               min_row.GetHeight() * (int) n + ((int) n - 1) * DIVIDER_HEIGHT);

	// One shared timer drives every client's maintain() call -- same
	// 5000ms interval as BigStateWindow, same reasoning: maintain()
	// itself decides whether REREGISTER_INTERVAL_SECS has elapsed per
	// client, so this only needs to tick more often than that, not
	// match it exactly.
	_maintain_timer = new wxTimer(this, ID_StackMaintainTimer);
	_maintain_timer->Start(5000);

	// Checks every row for a state change at a fixed, sane interval --
	// see BigStateWindow's own _state_check_timer comment for why this
	// replaced an EVT_IDLE/RequestMore() busy-poll found live,
	// 2026-09-06, to peg the CPU at ~100% permanently.
	_state_check_timer = new wxTimer(this, ID_StackStateCheckTimer);
	_state_check_timer->Start(STATE_CHECK_MS);
}

void BigStateStackWindow::on_timer (wxTimerEvent & ev)
{
	for (size_t i = 0; i < _clients.size(); ++i) {
		_clients[i]->maintain();
	}
}

void BigStateStackWindow::on_size (wxSizeEvent & ev)
{
	// 2026-09-06 refactor: this used to also actively enforce a minimum
	// size, give every row panel an identical font-dependent
	// SetMinSize() (working around wxSizer not distributing height
	// evenly among rows with different current-text minimums), and call
	// Layout() unconditionally on every invocation. All of that is gone
	// now, on purpose: StateRowPanel declares no font-dependent minimum
	// size at all (see its own header comment), so wxBoxSizer's default
	// equal-proportion distribution among rows whose only real minimum
	// is wxPanel's own small fixed default just works, unconditionally,
	// at any window size -- which is what actually fixes "shrinking the
	// window cuts off the lower rows instead of resizing all of them":
	// that was wxSizer's own documented response to per-row minimums
	// that could sum past the space available, not a bug in the
	// centering logic layered on top of it. wx's own default EVT_SIZE
	// handling (run via ev.Skip() below) already resizes every row and
	// divider to fill the frame correctly; all that is left to do here
	// is recompute the one shared font size for the new row height.
	int n = (int) _rows.size();
	if (n < 1) {
		ev.Skip();
		return;
	}

	wxSize client = GetClientSize();
	if (client != _last_processed_client) {
		_last_processed_client = client;
		update_fonts ();
	}

	ev.Skip();
}

void BigStateStackWindow::update_fonts ()
{
	int n = (int) _rows.size();
	if (n < 1) {
		return;
	}

	wxSize client = GetClientSize();
	int row_height = (client.GetHeight() - (n - 1) * DIVIDER_HEIGHT) / n;
	if (row_height < 1) {
		row_height = 1;
	}

	// One shared font size across every row, computed from whatever
	// each row is ACTUALLY showing right now -- see
	// state_render.hpp's own fit_font_size() comment for why this
	// replaced a fixed worst-case-across-all-states computation.
	std::vector<wxString> labels (n);
	for (int i = 0; i < n; ++i) {
		wxColour unused_color;
		StateRender::state_display (_last_states[i], labels[i], unused_color);
	}

	int pt = StateRender::fit_font_size (wxSize (client.GetWidth(), row_height), labels, _margin);
	for (int i = 0; i < n; ++i) {
		_rows[i]->set_font_point_size (pt);
	}
}

void BigStateStackWindow::on_state_check_timer (wxTimerEvent & ev)
{
	// Same unconditional get_state()+local-dedup pattern as
	// BigStateWindow's own state-check handler (its fix for the
	// shared-flag race found live, 2026-09-01) -- each row has its own
	// OscStateClient, so that race cannot occur here either, but there
	// is no reason to structure this any differently.
	for (size_t i = 0; i < _clients.size(); ++i) {
		LooperState state;
		if (_clients[i]->get_state (state) && state != _last_states[i]) {
			// apply_state() itself updates _last_states[i] now (it needs
			// the new value before update_fonts() reads every row's
			// current label) -- see its own comment.
			apply_state (i, state);
		}
	}
}

void BigStateStackWindow::apply_state (size_t row, LooperState state)
{
	wxString label;
	wxColour color;
	StateRender::state_display (state, label, color);
	_rows[row]->set_state (color, label);

	// The shared font is now sized to what every row is CURRENTLY
	// showing, not a fixed worst case (see state_render.hpp's own
	// comment) -- so any one row's state change needs to recompute it
	// for all of them, not just a resize.
	_last_states[row] = state;
	update_fonts ();
}
