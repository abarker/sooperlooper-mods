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

// OscStateClient: a minimal, standalone OSC client for one loop's "state"
// control -- deliberately NOT a port of LoopControl. No spawn-config, no
// session management, no MIDI bindings, no multi-loop tracking: this
// program's only job is displaying one loop's state, so it only implements
// the one thing that needs. How it talks to the engine: "slbigstate: Implementation"
// in mod_docs/late_hit_patch_summary.rst.
//
// Reconnect strategy is deliberately the simpler of two options:
// unconditional periodic re-registration, not a
// full ping/uid mirror of LoopControl's own reconnect logic. Verified
// safe before choosing this (control_osc.cpp's RegisterAuto handler
// matches on the existing (url, retpath) pair and just refreshes the
// timeout rather than appending a duplicate -- re-registering the same
// thing repeatedly cannot cause growing/duplicate push traffic). Engine
// restarts are self-healed within one REREGISTER_INTERVAL_SECS, since a
// new engine instance's registration table starts empty and the next
// blind re-send reaches it as long as it is listening on the same
// host:port. No /ping/pingack tracking needed for this narrower purpose.

#ifndef __big_state_display_osc_state_client__
#define __big_state_display_osc_state_client__

#include <string>
#include <lo/lo.h>

#include "plugin.hpp"

class OscStateClient
{
  public:
	OscStateClient (const std::string & host, int port, int loop_index);
	~OscStateClient ();

	// True once at least one state value has ever been received.
	bool get_state (SooperLooper::LooperState & state) const;

	// Call periodically (a few times a minute is plenty) to keep the
	// auto-update registration alive across an engine restart.
	void maintain ();

	// So the window title can show which loop this instance is showing.
	int get_loop_index () const { return _loop_index; }

	static const int REREGISTER_INTERVAL_SECS = 20;

  protected:
	void send_register ();

	static int _state_handler (const char *path, const char *types, lo_arg **argv,
	                             int argc, void *data, void *user_data);
	int state_handler (lo_arg **argv, int argc);

	lo_address _engine_addr;
	lo_server_thread _server;
	std::string _my_url;
	int _loop_index;

	SooperLooper::LooperState _state;
	bool _have_state;

	long _last_register_time;
};

#endif
