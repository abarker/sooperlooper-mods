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
// User documentation: "New Tool: slbigstate" in
// mod_docs/late_hit_patch_summary.rst.

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
	// Which config file _config currently points at -- true for the
	// classic single-loop file, false for the stack file. Tracked
	// separately rather than added to DisplayConfig's own small API,
	// since only rebuild_stack_window() below ever needs to know it (to
	// decide whether a live loop-count change crossed the 1-vs-many
	// boundary and _config needs to be swapped for the other file).
	bool _config_is_single_file;

	// Old DisplayConfig objects retired by rebuild_stack_window() when a
	// live loop-count change crosses the 1-vs-many boundary -- see that
	// method's own comment for why they cannot be deleted immediately
	// (the just-Destroy()-ed old BigStateStackWindow's own destructor,
	// which still holds a reference to one of these, has not actually
	// run yet at that point -- wx defers a top-level window's real C++
	// destruction to its next idle pass). Freed for real in OnExit(),
	// by which point every window is long gone.
	std::vector<DisplayConfig *> _retired_configs;

	// Saved from the command line so rebuild_stack_window() can use them
	// again later -- only ever relevant to the auto-detect stack path
	// (see _watching_loop_count below), never touched by an explicit
	// --loop/--loops invocation.
	wxString _host;
	long     _port;
	long     _margin;
	bool     _stay_on_top;

	// Real bug found live, 2026-09-05: user pointed out that loops can
	// be added or removed from a RUNNING engine (SooperLooper's own
	// /loop_add and /loop_del OSC commands, e.g. from slgui itself),
	// not just by restarting it with a different -l N -- so "restart
	// slbigstate alongside sooperlooper" (this feature's own documented
	// fix for an engine *restart* with a different count) does not
	// cover this case at all: nothing about the engine restarts, so
	// there is no natural moment to think to restart slbigstate either.
	//
	// Fixed properly, not documented as a workaround, once the
	// mechanism was actually read rather than assumed: the engine
	// already pushes an updated /pingack (s:hosturl s:version
	// i:loopcount, control_osc.cpp's send_all_config(), called from
	// both on_loop_added() and on_loop_removed()) to every address that
	// has sent it a plain /register <returl> <retpath> -- the *same*
	// push-not-poll shape every other OSC interaction in this program
	// already uses, not a new pattern invented for this. Only wired up
	// for the auto-detect stack path (neither --loop nor --loops given)
	// -- an explicit --loop/--loops means the user asked for a specific
	// loop or set, which a live count change elsewhere should not
	// override.
	bool              _watching_loop_count;
	lo_server_thread  _config_server;
	wxTimer *         _config_watch_timer;
	// Set from the OSC callback (a DIFFERENT thread, liblo's own
	// server thread, never wx's main thread) -- kept as plain data,
	// exactly the same shape as OscStateClient's own _state/_have_state
	// fields, and read/cleared only from on_config_watch_timer() on the
	// main thread via wxTimer, the same split every other OSC handler
	// in this codebase already uses to keep wx calls off a non-main
	// thread.
	volatile bool     _config_changed;
	volatile int      _pushed_loop_count;

	void build_stack_window (const std::vector<int> & loop_list,
	                           const wxPoint & pos, const wxSize & row_size);
	void rebuild_stack_window (int new_count);
	void start_loop_count_watch ();
	void send_register_and_ping ();
	void on_config_watch_timer (wxTimerEvent & ev);

	static int _config_ack_handler (const char *path, const char *types, lo_arg **argv,
	                                   int argc, void *data, void *user_data);
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
	_config_is_single_file = true;
	_host = host;
	_port = port;
	_margin = margin;
	_stay_on_top = stay_on_top;
	_watching_loop_count = false;
	_config_server = 0;
	_config_watch_timer = 0;
	_config_changed = false;
	_pushed_loop_count = 0;

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
			// Normal when slbigstate starts before the engine: the
			// config watch timer below keeps pinging, and the stack
			// follows the real loop count once the engine answers.
			fprintf (stderr, "slbigstate: SooperLooper at %s:%ld did not "
			         "answer within %dms; showing loop 0 until it does, "
			         "then following its loop count.\n",
			         static_cast<const char *> (host.mb_str()), port,
			         PING_TIMEOUT_MS);
			count = 1;
		}
		for (int i = 0; i < count; ++i) {
			loop_list.push_back (i);
		}
	}

	_config_is_single_file = (loop_list.size() == 1);
	_config = new DisplayConfig (
		_config_is_single_file ? wxT("slbigstate_config.xml")
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

	build_stack_window (loop_list, pos, row_size);

	// Only for the auto-detect default (neither --loop nor --loops) --
	// see _watching_loop_count's own header comment for why an explicit
	// --loop/--loops deliberately does not get this.
	if (!have_loops) {
		start_loop_count_watch ();
	}

	return true;
}

void
BigStateApp::build_stack_window (const std::vector<int> & loop_list,
                                    const wxPoint & pos, const wxSize & row_size)
{
	fprintf (stderr, "slbigstate: displaying loop%s:",
	         loop_list.size() == 1 ? "" : "s");
	for (size_t i = 0; i < loop_list.size(); ++i) {
		fprintf (stderr, " %d", loop_list[i]);
	}
	fprintf (stderr, "\n");

	for (size_t i = 0; i < loop_list.size(); ++i) {
		_stack_clients.push_back (
			new OscStateClient (std::string (_host.ToAscii()), (int) _port, loop_list[i]));
	}

	long style = wxDEFAULT_FRAME_STYLE | (_stay_on_top ? wxSTAY_ON_TOP : 0);
	_stack_window = new BigStateStackWindow (_stack_clients, *_config, pos, row_size, style,
	                                            wxT("BigStateStackWin"), (int) _margin);

	SetTopWindow (_stack_window);
	_stack_window->Show (true);
}

void
BigStateApp::rebuild_stack_window (int new_count)
{
	if (new_count < 1) {
		new_count = 1;
	}

	// Capture the OLD window's current (possibly user-dragged) geometry
	// explicitly, ourselves, rather than relying on its own destructor's
	// equivalent save into _config -- wx defers a top-level window's
	// actual C++ destruction from Destroy() to idle time, so _config
	// would not reflect it yet by the time this function reads it,
	// moments later in this same call.
	wxPoint pos = _config->get_position();
	wxSize row_size = _config->get_size();
	if (_stack_window) {
		pos = _stack_window->GetScreenPosition();
		wxSize total = _stack_window->GetSize();
		int old_rows = (int) _stack_clients.size();
		row_size = (old_rows > 0)
			? wxSize (total.GetWidth(), total.GetHeight() / old_rows)
			: total;

		_stack_window->Destroy();
		_stack_window = 0;
	}

	for (size_t i = 0; i < _stack_clients.size(); ++i) {
		delete _stack_clients[i];
	}
	_stack_clients.clear();

	std::vector<int> loop_list;
	for (int i = 0; i < new_count; ++i) {
		loop_list.push_back (i);
	}

	bool want_single_file = (loop_list.size() == 1);
	if (want_single_file != _config_is_single_file) {
		// Real bug found live, 2026-09-05: deleting the OLD _config here
		// synchronously is a use-after-free waiting to happen.
		// _stack_window->Destroy() above only SCHEDULES the old
		// BigStateStackWindow's actual C++ destruction for wx's next
		// idle pass -- it has not run yet at this point in this same
		// function call. That destructor still holds a DisplayConfig&
		// reference to whatever _config pointed to when IT was
		// constructed, and writes through it (saving final position/
		// size) whenever it does eventually run. Deleting that object
		// out from under it here corrupts/crashes on that later write
		// (confirmed live: "I/O error: Is a directory" from a garbled
		// filename read back out of freed memory). Retiring it into
		// _retired_configs instead keeps it alive and valid for that
		// still-pending destructor call, at the cost of a few dozen
		// bytes staying allocated until OnExit() -- only on an actual
		// 1-vs-many crossing, not on every rebuild, so bounded and rare
		// even across a very long-running session.
		_retired_configs.push_back (_config);
		_config = new DisplayConfig (
			want_single_file ? wxT("slbigstate_config.xml")
			                    : wxT("slbigstate_stack_config.xml"));
		_config->load ();
		_config_is_single_file = want_single_file;
	}
	// Regardless of which file _config now points at, keep showing the
	// window right where it already was rather than snapping to
	// whatever position that file happened to have saved from a past
	// run at this same loop count.
	_config->set_position (pos);
	_config->set_size (row_size);

	fprintf (stderr, "slbigstate: loop count changed live -- rebuilding\n");
	build_stack_window (loop_list, pos, row_size);
}

void
BigStateApp::send_register_and_ping ()
{
	// Both messages share the same retpath ("/configack") and the same
	// handler (_config_ack_handler), and are safe to repeat
	// unconditionally on every call:
	//   /register -- confirmed idempotent via
	//     ControlOSC::finish_register_event() (de-duplicated by
	//     (returl, retpath)) -- keeps this process on the engine's
	//     push list for FUTURE /loop_add//loop_del events, including
	//     re-establishing it after a real engine restart (a new
	//     instance starts with an empty list, forgetting any earlier
	//     registration).
	//   /ping -- real bug found live, 2026-09-05: /register alone does
	//     not cover an engine that already came back from a restart
	//     with a DIFFERENT loop count from birth, with nothing further
	//     changing afterward -- ControlOSC::finish_register_event()
	//     only adds the caller to a list, it does not reply, so a
	//     freshly-(re-)registered client has no way to learn the
	//     CURRENT count, only future changes to it. /ping does reply
	//     immediately, with the exact same "ssi" /pingack shape
	//     send_all_config() itself pushes on a real change -- so
	//     resending one every tick, alongside /register, catches
	//     "count was already different by the time we noticed the
	//     engine restarted" too, not just a live change to an engine
	//     already being watched.
	char portbuf[16];
	snprintf (portbuf, sizeof(portbuf), "%ld", _port);
	lo_address engine_addr = lo_address_new (_host.ToAscii(), portbuf);
	if (engine_addr) {
		char * url = lo_server_thread_get_url (_config_server);
		lo_send (engine_addr, "/register", "ss", url, "/configack");
		lo_send (engine_addr, "/ping", "ss", url, "/configack");
		free (url);
		lo_address_free (engine_addr);
	}
}

void
BigStateApp::start_loop_count_watch ()
{
	_config_server = lo_server_thread_new (0, 0);
	if (!_config_server) {
		return;
	}
	lo_server_thread_add_method (_config_server, "/configack", "ssi", _config_ack_handler, this);
	lo_server_thread_start (_config_server);

	send_register_and_ping ();

	_watching_loop_count = true;
	_config_watch_timer = new wxTimer (this);
	Connect (_config_watch_timer->GetId(), wxEVT_TIMER,
	          wxTimerEventHandler (BigStateApp::on_config_watch_timer));
	// A few seconds, not milliseconds (the standing rule from the
	// 2026-09-01 live-rig OSC near-miss) -- send_register_and_ping()
	// itself is two small, cheap, idempotent sends; the real cost this
	// interval bounds is how often on_config_watch_timer() below might
	// call rebuild_stack_window() in the worst case (a genuine change
	// on every single tick), still nowhere near a real-time concern at
	// this cadence.
	_config_watch_timer->Start (5000);
}

int
BigStateApp::_config_ack_handler (const char *path, const char *types, lo_arg **argv,
                                     int argc, void *data, void *user_data)
{
	BigStateApp * app = static_cast<BigStateApp *> (user_data);
	if (argc >= 3) {
		app->_pushed_loop_count = argv[2]->i;
		app->_config_changed = true;
	}
	return 0;
}

void
BigStateApp::on_config_watch_timer (wxTimerEvent & ev)
{
	if (_config_changed) {
		_config_changed = false;
		// Only the /ping half of send_register_and_ping() can report a
		// count that already matches what is currently on screen (an
		// actual /loop_add//loop_del push only ever fires when
		// something really changed) -- guard here rather than in
		// _config_ack_handler(), which runs on liblo's own thread and
		// has no safe way to read _stack_clients (only ever touched
		// from the main thread). Skipping a no-op rebuild avoids a
		// pointless window teardown/recreate (and the accompanying
		// visible flicker) every single tick once nothing is actually
		// changing any more.
		if (_pushed_loop_count != (int) _stack_clients.size()) {
			rebuild_stack_window (_pushed_loop_count);
		}
	}

	send_register_and_ping ();
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

	for (size_t i = 0; i < _retired_configs.size(); ++i) {
		delete _retired_configs[i];
	}

	if (_config_watch_timer) {
		_config_watch_timer->Stop();
		delete _config_watch_timer;
	}
	if (_config_server) {
		lo_server_thread_stop (_config_server);
		lo_server_thread_free (_config_server);
	}

	return wxApp::OnExit();
}
