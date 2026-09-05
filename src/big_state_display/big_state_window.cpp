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

#include "big_state_window.hpp"
#include "state_row_panel.hpp"
#include "state_render.hpp"
#include "osc_state_client.hpp"

using namespace SooperLooper;

enum {
	ID_MaintainTimer = 9000,
	ID_StateCheckTimer = 9001
};

// How often to check for a state change -- see _state_check_timer's own
// comment in the header for why this replaced an EVT_IDLE +
// RequestMore() busy-poll. 50ms is imperceptibly fast for a status
// light (20 checks/sec) while actually sleeping between checks.
static const int STATE_CHECK_MS = 50;

BEGIN_EVENT_TABLE(BigStateWindow, wxFrame)
	EVT_TIMER(ID_MaintainTimer, BigStateWindow::on_timer)
	EVT_TIMER(ID_StateCheckTimer, BigStateWindow::on_state_check_timer)
	EVT_SIZE(BigStateWindow::on_size)
END_EVENT_TABLE()

BigStateWindow::BigStateWindow (OscStateClient & osc_client, DisplayConfig & config,
                                  const wxPoint& pos, const wxSize& size,
                                  long style, const wxString& name, int margin)
	: wxFrame(0, -1,
	          wxString::Format(wxT("SooperLooper State - Loop %d"),
	                             osc_client.get_loop_index()),
	          pos, size, style, name)
	, _osc_client(osc_client)
	, _config(config)
	, _row(0)
	, _margin(margin >= 0 ? margin : StateRender::DEFAULT_LABEL_MARGIN)
	, _maintain_timer(0)
	, _state_check_timer(0)
	, _last_state(LooperStateUnknown)
	, _last_processed_client(-1, -1)
{
	init();
}

BigStateWindow::~BigStateWindow()
{
	// Must happen here, not in BigStateApp::OnExit() -- see the header
	// comment. GetScreenPosition()/GetSize() are still safe to call on
	// this object's own wxFrame base at this point.
	_config.set_position (GetScreenPosition());
	_config.set_size (GetSize());
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

void BigStateWindow::init()
{
	_row = new StateRowPanel(this);

	wxBoxSizer * mainsizer = new wxBoxSizer(wxVERTICAL);
	mainsizer->Add(_row, 1, wxEXPAND);
	SetSizer(mainsizer);

	// GetClientSize() already reflects the size passed to the
	// constructor at this point, even before Show() -- same assumption
	// SetSizeHints() below and main.cpp's own geometry handling already
	// make.
	_last_processed_client = GetClientSize();
	apply_state(LooperStateUnknown);

	// A best-effort floor -- not every window manager honours
	// SetSizeHints(), and this refactor deliberately does not try to
	// force it any more (see on_size()'s own comment for why the
	// earlier active-enforcement mechanism, itself the source of two
	// live-found bugs, one a CPU-pegging runaway, was dropped rather
	// than carried over).
	wxSize min_size = StateRender::min_size (_margin);
	SetSizeHints (min_size.GetWidth(), min_size.GetHeight());

	// Drives OscStateClient::maintain() -- that call itself decides
	// whether REREGISTER_INTERVAL_SECS has actually elapsed, so this
	// timer just needs to tick more often than that, not match it
	// exactly. No connection to state refresh at all; that's the
	// separate _state_check_timer below.
	_maintain_timer = new wxTimer(this, ID_MaintainTimer);
	_maintain_timer->Start(5000);

	// Checks for a state change at a fixed, sane interval -- replaced
	// an EVT_IDLE handler with an unconditional RequestMore() here,
	// found live 2026-09-06 to busy-poll the CPU at ~100% permanently
	// (present since this class's first build, 2026-09-01; not
	// introduced by today's other fixes, just never noticed before).
	// See _state_check_timer's own comment in the header.
	_state_check_timer = new wxTimer(this, ID_StateCheckTimer);
	_state_check_timer->Start(STATE_CHECK_MS);
}

void BigStateWindow::on_timer (wxTimerEvent & ev)
{
	_osc_client.maintain();
}

void BigStateWindow::on_size (wxSizeEvent & ev)
{
	// 2026-09-06 refactor: this used to also actively enforce a minimum
	// size (SetClientSize() to snap back above a computed floor) and
	// call Layout() unconditionally on every invocation to force
	// mainsizer to redistribute _panel's space around a font-dependent
	// SetMinSize(). Both are gone now, on purpose, not just trimmed:
	// StateRowPanel declares no font-dependent minimum size at all (see
	// its own header comment), so there is nothing left for a manual
	// Layout() call to fix up, and the active-floor mechanism was
	// itself the source of two separate live-found bugs (a
	// non-terminating resize feedback loop, and a synchronous
	// Layout()-triggers-another-EVT_SIZE storm that pegged the CPU) for
	// a benefit -- preventing a user from resizing the window smaller
	// than is legible -- that an ordinary small, unreadable window
	// already achieves on its own without any of that machinery. wx's
	// own default EVT_SIZE handling (run via ev.Skip() below) already
	// resizes _row to fill the frame correctly; all that is left to do
	// here is recompute the shared font size for the new space.
	wxSize client = GetClientSize();
	if (client != _last_processed_client) {
		_last_processed_client = client;
		update_font ();
	}
	ev.Skip();
}

void BigStateWindow::update_font ()
{
	wxString label;
	wxColour unused_color;
	StateRender::state_display (_last_state, label, unused_color);

	std::vector<wxString> labels (1, label);
	int pt = StateRender::fit_font_size (GetClientSize(), labels, _margin);
	_row->set_font_point_size (pt);
}

void BigStateWindow::on_state_check_timer (wxTimerEvent & ev)
{
	// Same fix as the in-process version's own bug (found live,
	// 2026-09-01): call get_state() unconditionally rather than gating
	// on any "is this new" flag, and de-dup locally against _last_state.
	// No shared-flag race is even possible here -- this OscStateClient
	// has exactly one reader -- but there is also no reason to
	// reintroduce that shape of bug on principle.
	LooperState state;

	if (_osc_client.get_state(state) && state != _last_state) {
		// apply_state() itself updates _last_state now (it needs the new
		// value before _last_state, to recompute the font against the
		// right label) -- see its own comment.
		apply_state(state);
	}
}

void BigStateWindow::apply_state (LooperState state)
{
	wxString label;
	wxColour color;
	StateRender::state_display (state, label, color);
	_row->set_state (color, label);

	// The font is now sized to the CURRENT label, not a fixed worst
	// case (see state_render.hpp's own comment) -- so a state change
	// needs to recompute it, not just a resize.
	_last_state = state;
	update_font ();
}
