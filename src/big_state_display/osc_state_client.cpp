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

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include "osc_state_client.hpp"

using namespace SooperLooper;

// Update interval requested from the engine, in ms. Same value slgui
// itself uses for this control (loop_control.cpp), and the max the
// engine allows (AUTO_UPDATE_MAX, control_osc.hpp) -- 10 Hz worth of
// checking on the engine side, but real traffic only happens on an
// actual state change (control_osc.cpp's send_registered_auto_updates).
static const int AUTO_UPDATE_MS = 100;

OscStateClient::OscStateClient (const std::string & host, int port, int loop_index)
	: _engine_addr(0)
	, _server(0)
	, _loop_index(loop_index)
	, _state(LooperStateUnknown)
	, _have_state(false)
	, _last_register_time(0)
{
	char portbuf[16];
	snprintf (portbuf, sizeof(portbuf), "%d", port);
	_engine_addr = lo_address_new (host.c_str(), portbuf);

	_server = lo_server_thread_new (0, 0);   // 0 port = let the OS pick one
	char * url = lo_server_thread_get_url (_server);
	_my_url = url;
	free (url);

	lo_server_thread_add_method (_server, "/ctrl", "isf", _state_handler, this);
	lo_server_thread_start (_server);

	send_register ();
}

OscStateClient::~OscStateClient ()
{
	if (_server) {
		lo_server_thread_stop (_server);
		lo_server_thread_free (_server);
	}
	if (_engine_addr) {
		lo_address_free (_engine_addr);
	}
}

void
OscStateClient::send_register ()
{
	char path[64];
	snprintf (path, sizeof(path), "/sl/%d/register_auto_update", _loop_index);
	lo_send (_engine_addr, path, "siss", "state", AUTO_UPDATE_MS,
	         _my_url.c_str(), "/ctrl");

	// Real bug found live, 2026-09-01: register_auto_update alone is not
	// enough. Traced the engine's own send_registered_auto_updates()
	// (control_osc.cpp): it only pushes when the current value differs
	// from the last one sent to THIS client, looked up in a per-client
	// map. On a fresh registration that lookup finds nothing (a brand
	// new engine instance, e.g. after a restart, starts with an empty
	// map) -- and the comparison code dereferences that "not found"
	// iterator instead of guarding it, which happens to read back
	// approximately 0.0f in practice. LooperStateOff is exactly 0, so if
	// the loop's real state is Off at the moment of a fresh
	// registration, the comparison sees "0.0 != ~0.0" as false and never
	// sends anything at all -- the client is left silently showing
	// whatever it had before (confirmed live: shut down while Playing,
	// new engine came up Off, display stayed on "Playing" until state
	// changed to something non-zero). Latent in the engine's shared
	// auto-update code, not something introduced by this tool or by
	// today's other changes; slgui's own LoopControl never hits it
	// because it separately does a one-shot /get for "state" on every
	// connect (loop_control.cpp's request_values(), called via
	// request_all_values()) -- that reply path (finish_get_event(),
	// control_osc.cpp) has no such comparison, it always sends
	// unconditionally. Mirrored the same fix here rather than touching
	// the shared engine code: a one-shot /get alongside every
	// registration re-seeds the correct current value regardless of
	// whether the auto-update path itself manages to push anything.
	snprintf (path, sizeof(path), "/sl/%d/get", _loop_index);
	lo_send (_engine_addr, path, "sss", "state", _my_url.c_str(), "/ctrl");

	_last_register_time = (long) time(0);
}

void
OscStateClient::maintain ()
{
	long now = (long) time(0);
	if (now - _last_register_time >= REREGISTER_INTERVAL_SECS) {
		send_register ();
	}
}

bool
OscStateClient::get_state (LooperState & state) const
{
	if (!_have_state) {
		return false;
	}
	state = _state;
	return true;
}

int
OscStateClient::_state_handler (const char *path, const char *types, lo_arg **argv,
                                  int argc, lo_message data, void *user_data)
{
	OscStateClient * self = static_cast<OscStateClient *> (user_data);
	return self->state_handler (argv, argc);
}

int
OscStateClient::state_handler (lo_arg **argv, int argc)
{
	// isf: instance, ctrl name, value -- same reply shape as a one-shot
	// /get, matching what finish_get_event()/send_registered_auto_updates()
	// both send (control_osc.cpp). Only "state" was ever registered for,
	// so no need to check the ctrl-name string here.
	if (argc < 3) {
		return 0;
	}
	_state = (LooperState) (int) argv[2]->f;
	_have_state = true;
	return 0;
}
