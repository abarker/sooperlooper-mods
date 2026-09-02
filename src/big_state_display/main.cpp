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

// big_state_display: standalone at-a-distance state display for one
// SooperLooper loop, talking to a running engine (headless sooperlooper
// or one spawned by slgui, either works) over plain OSC. Design and
// rationale throughout: mod_docs/ROADMAP.rst entry 6.

#include <wx/wx.h>
#include <wx/cmdline.h>

#include "osc_state_client.hpp"
#include "big_state_window.hpp"
#include "display_config.hpp"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9951

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
	  wxT_2("loop index to display (default 0)"), wxCMD_LINE_VAL_NUMBER },
	{ wxCMD_LINE_OPTION, wxT_2("X"), wxT_2("screen-location"),
	  wxT_2("window position, format <xpos>:<ypos>") },
	{ wxCMD_LINE_OPTION, wxT_2("Z"), wxT_2("size"),
	  wxT_2("window size, format <width>x<height>") },
	{ wxCMD_LINE_SWITCH, wxT_2("T"), wxT_2("stay-on-top"),
	  wxT_2("keep window on top of other applications") },
	{ wxCMD_LINE_NONE }
};

class BigStateApp : public wxApp
{
  public:
	virtual bool OnInit ();
	virtual int OnExit ();

  protected:
	OscStateClient * _client;
	BigStateWindow * _window;
	DisplayConfig   _config;
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
	long loop_index = 0;
	bool stay_on_top = parser.Found (wxT("T"));

	parser.Found (wxT("H"), &host);
	parser.Found (wxT("P"), &port);
	parser.Found (wxT("l"), &loop_index);

	_config.load();
	wxPoint pos = _config.get_position();
	wxSize size = _config.get_size();

	wxString posstr;
	if (parser.Found (wxT("X"), &posstr)) {
		wxArrayString strarr = wxSplit (posstr, wxChar(':'));
		if (strarr.size() == 2) {
			long x = pos.x, y = pos.y;
			strarr[0].ToLong (&x);
			strarr[1].ToLong (&y);
			pos = wxPoint (x, y);
		}
	}

	wxString sizestr;
	if (parser.Found (wxT("Z"), &sizestr)) {
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
	_window = new BigStateWindow (*_client, _config, pos, size, style);

	SetTopWindow (_window);
	_window->Show (true);

	return true;
}

int
BigStateApp::OnExit ()
{
	// Segfault found live, 2026-09-01: this used to read _window's
	// position/size here and save them -- but by the time OnExit() runs,
	// wx has already destroyed _window as part of its normal
	// close-the-last-frame handling, so that was a use-after-free on a
	// dangling pointer. The save now happens inside BigStateWindow's own
	// destructor instead, while its wxFrame base is still valid; nothing
	// left to do with _window here at all.
	delete _client;

	return wxApp::OnExit();
}
