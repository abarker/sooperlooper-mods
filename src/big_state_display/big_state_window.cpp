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

#include "big_state_window.hpp"
#include "osc_state_client.hpp"

using namespace SooperLooper;

enum {
	ID_MaintainTimer = 9000
};

BEGIN_EVENT_TABLE(BigStateWindow, wxFrame)
	EVT_IDLE(BigStateWindow::on_idle)
	EVT_TIMER(ID_MaintainTimer, BigStateWindow::on_timer)
END_EVENT_TABLE()

BigStateWindow::BigStateWindow (OscStateClient & osc_client, DisplayConfig & config,
                                  const wxPoint& pos, const wxSize& size,
                                  long style, const wxString& name)
	: wxFrame(0, -1,
	          wxString::Format(wxT("SooperLooper State - Loop %d"),
	                             osc_client.get_loop_index()),
	          pos, size, style, name)
	, _osc_client(osc_client)
	, _config(config)
	, _panel(0)
	, _label(0)
	, _maintain_timer(0)
	, _last_state(LooperStateUnknown)
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
}

void BigStateWindow::init()
{
	_panel = new wxPanel(this, -1);

	wxFont font(72, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
	_label = new wxStaticText(_panel, -1, wxT("---"), wxDefaultPosition, wxDefaultSize,
	                            wxALIGN_CENTRE);
	_label->SetFont(font);

	wxBoxSizer * panelsizer = new wxBoxSizer(wxVERTICAL);
	panelsizer->AddStretchSpacer(1);
	panelsizer->Add(_label, 0, wxALIGN_CENTRE | wxALL, 10);
	panelsizer->AddStretchSpacer(1);
	_panel->SetSizer(panelsizer);

	wxBoxSizer * mainsizer = new wxBoxSizer(wxVERTICAL);
	mainsizer->Add(_panel, 1, wxEXPAND);
	SetSizer(mainsizer);

	apply_state(LooperStateUnknown);

	SetSizeHints(200, 100);

	// Drives OscStateClient::maintain() -- that call itself decides
	// whether REREGISTER_INTERVAL_SECS has actually elapsed, so this
	// timer just needs to tick more often than that, not match it
	// exactly. No connection to state refresh at all; that's EVT_IDLE,
	// same as the in-process version.
	_maintain_timer = new wxTimer(this, ID_MaintainTimer);
	_maintain_timer->Start(5000);
}

void BigStateWindow::on_timer (wxTimerEvent & ev)
{
	_osc_client.maintain();
}

void BigStateWindow::on_idle (wxIdleEvent & ev)
{
	// Same fix as the in-process version's own bug (found live,
	// 2026-09-01): call get_state() unconditionally rather than gating
	// on any "is this new" flag, and de-dup locally against _last_state.
	// No shared-flag race is even possible here -- this OscStateClient
	// has exactly one reader -- but there is also no reason to
	// reintroduce that shape of bug on principle.
	LooperState state;

	if (_osc_client.get_state(state) && state != _last_state) {
		apply_state(state);
		_last_state = state;
	}

	ev.RequestMore();
}

void BigStateWindow::apply_state (LooperState state)
{
	wxString label;
	wxColour color;
	state_display (state, label, color);

	_panel->SetBackgroundColour (color);
	_label->SetBackgroundColour (color);
	_label->SetForegroundColour (*wxWHITE);
	_label->SetLabel (label);

	// Same fix as the in-process version's other bug (found live,
	// 2026-09-01): SetLabel() changes the wxStaticText's natural size,
	// but the centering sizer only recomputes on an explicit Layout()
	// call, not just because the text changed.
	_panel->Layout();
	_panel->Refresh();
}

// Identical table to the in-process version (src/gui/big_state_window.cpp)
// -- kept in sync by hand since there is no shared source between this
// standalone binary and slgui. See that file's own comment for the
// per-state reasoning (labels match the main interface's button
// vocabulary; colors grouped by family).
void BigStateWindow::state_display (LooperState state, wxString & label, wxColour & color)
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
		label = wxT("Substitute");     color = wxColour(220, 120, 0);
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
