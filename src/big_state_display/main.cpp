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

// big_state_display: standalone at-a-distance state display for one or
// more SooperLooper loops, talking to a running engine (headless
// sooperlooper or one spawned by slgui, either works) over plain OSC.
// Design and rationale throughout: mod_docs/ROADMAP.rst entry 6.

#include <cstdio>
#include <vector>

#include <wx/wx.h>
#include <wx/cmdline.h>

#include <lo/lo.h>

#include "osc_state_client.hpp"
#include "big_state_window.hpp"
#include "big_state_stack_window.hpp"
#include "display_config.hpp"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9951
#define PING_TIMEOUT_MS 2000

// wxT_2 (a plain narrow-string passthrough, wx/chartype.h), not wxT
// (which expands to a wide L"..." literal): these struct fields need
// const char*. Same fix gui_app.cpp's own cmdLineDesc[] already uses.
static const wxCmdLineEntryDesc cmdLineDesc[] =
{
	{ wxCMD_LINE_SWITCH, wxT_2("h"), wxT_2("help"), wxT_2("show this help"),
	  wxCMD_LINE_VAL_NONE, wxCMD_LINE_OPTION_HELP },
	{ wxCMD_LINE_OPTION, wxT_2("H"), wxT_2("host"),
	  wxT_2("engine host to connect to (default 127.0.0.1)") },
	{ wxCMD_LINE_OPTION, wxT_2("P"), wxT_2("port"),
	  wxT_2("engine OSC port to connect to (default 9951)"), wxCMD_LINE_VAL_NUMBER },
	{ wxCMD_LINE_OPTION, wxT_2("l"), wxT_2("loop"),
	  wxT_2("loop index to display, single-loop window (default: see --loops)"),
	  wxCMD_LINE_VAL_NUMBER },
	{ wxCMD_LINE_OPTION, wxT_2("L"), wxT_2("loops"),
	  wxT_2("comma/range list of loops to stack, e.g. 0,2-4 (default: all loops, auto-detected)") },
	{ wxCMD_LINE_OPTION, wxT_2("X"), wxT_2("screen-location"),
	  wxT_2("window position, format <xpos>:<ypos>") },
	{ wxCMD_LINE_OPTION, wxT_2("Z"), wxT_2("size"),
	  wxT_2("per-row window size, format <width>x<height>") },
	{ wxCMD_LINE_OPTION, wxT_2("M"), wxT_2("margin"),
	  wxT_2("pixels reserved around the label when fitting the font (default 20)"),
	  wxCMD_LINE_VAL_NUMBER },
	{ wxCMD_LINE_SWITCH, wxT_2("T"), wxT_2("stay-on-top"),
	  wxT_2("keep window on top of other applications") },
	{ wxCMD_LINE_NONE }
};

namespace {

// Parses "0,2-4,6" into an ordered list of loop indices, duplicates and
// all -- deliberately not de-duplicated or sorted: the display order is
// the order given, top row first, matching "stacked in sequence" rather
// than imposing a canonical order the user didn't ask for. Returns false
// on any malformed token (empty string, non-digits, a range with the
// second bound smaller than the first) so a typo fails loudly at
// startup instead of silently showing the wrong loops.
bool
parse_loop_list (const wxString & spec, std::vector<int> & out)
{
	wxArrayString tokens = wxSplit (spec, wxChar(','));

	for (size_t i = 0; i < tokens.size(); ++i) {
		wxString tok = tokens[i];
		tok.Trim(true).Trim(false);
		if (tok.IsEmpty()) {
			return false;
		}

		int dash = tok.Find (wxChar('-'));
		if (dash == wxNOT_FOUND) {
			long v;
			if (!tok.ToLong (&v) || v < 0) {
				return false;
			}
			out.push_back ((int) v);
		}
		else {
			long lo, hi;
			wxString lostr = tok.Mid (0, dash);
			wxString histr = tok.Mid (dash + 1);
			if (!lostr.ToLong (&lo) || !histr.ToLong (&hi)
			    || lo < 0 || hi < lo) {
				return false;
			}
			for (long v = lo; v <= hi; ++v) {
				out.push_back ((int) v);
			}
		}
	}

	return !out.empty();
}

// One-shot, synchronous /ping to find out how many loops the engine
// currently has, for the "no --loop/--loops given, default to all"
// case. Deliberately minimal -- no reconnect logic, no persistent
// server: this runs once at startup, before any window exists, and
// either gets an answer within PING_TIMEOUT_MS or gives up. Returns -1
// on timeout or malformed reply; the caller falls back to a single loop
// 0 rather than failing to start.
int
detect_loop_count (const std::string & host, int port)
{
	char portbuf[16];
	snprintf (portbuf, sizeof(portbuf), "%d", port);
	lo_address engine_addr = lo_address_new (host.c_str(), portbuf);
	if (!engine_addr) {
		return -1;
	}

	lo_server_thread server = lo_server_thread_new (0, 0);
	if (!server) {
		lo_address_free (engine_addr);
		return -1;
	}

	volatile int loop_count = -1;
	volatile bool got_reply = false;

	// Captures loop_count/got_reply via the user_data pointer -- lo's C
	// API has no closures, so a small local struct + a plain function
	// pointer is the idiomatic way to get a value out of a callback.
	// Deliberately NOT static: this function is only ever called once at
	// startup today, but a plain local (whose address is stable for the
	// whole function call, well past lo_server_thread_stop() below)
	// costs nothing and avoids a shared-state footgun if it's ever
	// called a second time (e.g. a future retry).
	struct ReplyBox { volatile int * count; volatile bool * got; };
	ReplyBox box_storage;
	box_storage.count = &loop_count;
	box_storage.got = &got_reply;

	struct Handler {
		static int fn (const char *path, const char *types, lo_arg **argv,
		                int argc, void *data, void *user_data) {
			ReplyBox * box = static_cast<ReplyBox *> (user_data);
			if (argc >= 3) {
				*(box->count) = argv[2]->i;
				*(box->got) = true;
			}
			return 0;
		}
	};

	lo_server_thread_add_method (server, "/pingack", "ssi", Handler::fn, &box_storage);
	lo_server_thread_start (server);

	char * url = lo_server_thread_get_url (server);
	lo_send (engine_addr, "/ping", "ss", url, "/pingack");
	free (url);

	// Bounded poll rather than a blocking wait -- lo_server_thread
	// already dispatches replies on its own thread, so this is just
	// waiting for got_reply to flip, with a hard ceiling.
	for (int waited = 0; waited < PING_TIMEOUT_MS && !got_reply; waited += 20) {
		wxMilliSleep (20);
	}

	lo_server_thread_stop (server);
	lo_server_thread_free (server);
	lo_address_free (engine_addr);

	return got_reply ? loop_count : -1;
}

} // anonymous namespace

class BigStateApp : public wxApp
{
  public:
	virtual bool OnInit ();
	virtual int OnExit ();

  protected:
	// Single-loop path (--loop N given): unchanged from before this
	// feature existed, byte-for-byte -- see OnInit().
	OscStateClient * _client;
	BigStateWindow * _window;

	// Stack path (--loops given, or the new default): N independent
	// clients, owned here exactly like _client above, just N of them.
	std::vector<OscStateClient *> _stack_clients;
	BigStateStackWindow * _stack_window;

	DisplayConfig * _config;
};

IMPLEMENT_APP(BigStateApp)

bool
BigStateApp::OnInit ()
{
	wxCmdLineParser parser (argc, argv);
	parser.SetDesc (cmdLineDesc);
	if (parser.Parse() != 0) {
		return false;
	}

	wxString host = wxT(DEFAULT_HOST);
	long port = DEFAULT_PORT;
	bool stay_on_top = parser.Found (wxT("T"));

	parser.Found (wxT("H"), &host);
	parser.Found (wxT("P"), &port);

	// -1 (unset) tells both window classes to fall back to
	// StateRender::DEFAULT_LABEL_MARGIN themselves -- added per direct
	// user request, once the underlying per-current-label sizing fix
	// was confirmed working, so an unusually long or short label
	// vocabulary (or just personal taste on how tight the text should
	// sit) doesn't need a rebuild to adjust.
	long margin = -1;
	parser.Found (wxT("M"), &margin);

	bool have_loop = parser.Found (wxT("l"));
	long loop_index = 0;
	if (have_loop) {
		parser.Found (wxT("l"), &loop_index);
	}

	wxString loops_spec;
	bool have_loops = parser.Found (wxT("L"), &loops_spec);

	if (have_loop && have_loops) {
		fprintf (stderr, "slbigstate: --loop and --loops are mutually "
		         "exclusive -- pick one.\n");
		return false;
	}

	_client = 0;
	_window = 0;
	_stack_window = 0;
	_config = 0;

	wxString posstr;
	bool have_pos = parser.Found (wxT("X"), &posstr);
	wxString sizestr;
	bool have_size = parser.Found (wxT("Z"), &sizestr);

	if (have_loop) {
		// Unchanged single-loop path -- byte-for-byte what this program
		// did before --loops/auto-detect existed.
		_config = new DisplayConfig ();
		_config->load ();
		wxPoint pos = _config->get_position();
		wxSize size = _config->get_size();

		if (have_pos) {
			wxArrayString strarr = wxSplit (posstr, wxChar(':'));
			if (strarr.size() == 2) {
				long x = pos.x, y = pos.y;
				strarr[0].ToLong (&x);
				strarr[1].ToLong (&y);
				pos = wxPoint (x, y);
			}
		}
		if (have_size) {
			wxArrayString strarr = wxSplit (sizestr, wxChar('x'));
			if (strarr.size() == 2) {
				long w = size.GetWidth(), h = size.GetHeight();
				strarr[0].ToLong (&w);
				strarr[1].ToLong (&h);
				size = wxSize (w, h);
			}
		}

		_client = new OscStateClient (std::string (host.ToAscii()), (int) port,
		                                (int) loop_index);

		long style = wxDEFAULT_FRAME_STYLE | (stay_on_top ? wxSTAY_ON_TOP : 0);
		_window = new BigStateWindow (*_client, *_config, pos, size, style,
		                                wxT("BigStateWin"), (int) margin);

		SetTopWindow (_window);
		_window->Show (true);
		return true;
	}

	// Stack path: explicit --loops list, or (neither --loop nor --loops
	// given) auto-detected "all loops" -- the new default. A 1-loop
	// result here (an explicit "--loops 0", or auto-detect on a
	// single-loop session like the user's own rig) produces a 1-row
	// stack that is pixel- and title-identical to the old single-loop
	// window; see big_state_stack_window.hpp's header comment for why
	// that falls out for free rather than needing to be special-cased.
	std::vector<int> loop_list;

	if (have_loops) {
		if (!parse_loop_list (loops_spec, loop_list)) {
			fprintf (stderr, "slbigstate: could not parse --loops '%s' "
			         "(expected e.g. '0,2-4,6')\n",
			         static_cast<const char *> (loops_spec.mb_str()));
			return false;
		}
	}
	else {
		int count = detect_loop_count (std::string (host.ToAscii()), (int) port);
		if (count <= 0) {
			fprintf (stderr, "slbigstate: could not detect loop count from "
			         "%s:%ld within %dms, defaulting to loop 0 only. Pass "
			         "--loop or --loops explicitly to skip this.\n",
			         static_cast<const char *> (host.mb_str()), port,
			         PING_TIMEOUT_MS);
			count = 1;
		}
		for (int i = 0; i < count; ++i) {
			loop_list.push_back (i);
		}
	}

	_config = new DisplayConfig (
		loop_list.size() == 1 ? wxT("slbigstate_config.xml")
		                       : wxT("slbigstate_stack_config.xml"));
	_config->load ();
	wxPoint pos = _config->get_position();
	wxSize row_size = _config->get_size();

	if (have_pos) {
		wxArrayString strarr = wxSplit (posstr, wxChar(':'));
		if (strarr.size() == 2) {
			long x = pos.x, y = pos.y;
			strarr[0].ToLong (&x);
			strarr[1].ToLong (&y);
			pos = wxPoint (x, y);
		}
	}
	if (have_size) {
		wxArrayString strarr = wxSplit (sizestr, wxChar('x'));
		if (strarr.size() == 2) {
			long w = row_size.GetWidth(), h = row_size.GetHeight();
			strarr[0].ToLong (&w);
			strarr[1].ToLong (&h);
			row_size = wxSize (w, h);
		}
	}

	fprintf (stderr, "slbigstate: displaying loop%s:",
	         loop_list.size() == 1 ? "" : "s");
	for (size_t i = 0; i < loop_list.size(); ++i) {
		fprintf (stderr, " %d", loop_list[i]);
	}
	fprintf (stderr, "\n");

	for (size_t i = 0; i < loop_list.size(); ++i) {
		_stack_clients.push_back (
			new OscStateClient (std::string (host.ToAscii()), (int) port, loop_list[i]));
	}

	long style = wxDEFAULT_FRAME_STYLE | (stay_on_top ? wxSTAY_ON_TOP : 0);
	_stack_window = new BigStateStackWindow (_stack_clients, *_config, pos, row_size, style,
	                                            wxT("BigStateStackWin"), (int) margin);

	SetTopWindow (_stack_window);
	_stack_window->Show (true);

	return true;
}

int
BigStateApp::OnExit ()
{
	// Segfault found live, 2026-09-01: this used to read _window's
	// position/size here and save them -- but by the time OnExit() runs,
	// wx has already destroyed the window(s) as part of its normal
	// close-the-last-frame handling, so that was a use-after-free on a
	// dangling pointer. The save now happens inside BigStateWindow's/
	// BigStateStackWindow's own destructors instead, while their wxFrame
	// base is still valid; nothing left to do with either window here.
	delete _client;

	for (size_t i = 0; i < _stack_clients.size(); ++i) {
		delete _stack_clients[i];
	}

	delete _config;

	return wxApp::OnExit();
}
