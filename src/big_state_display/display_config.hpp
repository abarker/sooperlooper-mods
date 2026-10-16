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

// DisplayConfig: remembers this window's screen position and size across
// runs. Lives alongside slgui's own config, ~/.sooperlooper/
// slbigstate_config.xml -- but deliberately its OWN file, NOT slgui's
// gui_config.xml: two independent processes writing one shared file with
// no coordination between them is the same problem shape as the
// LoopControl flag-sharing bug already found and fixed once, just at the
// filesystem layer instead of in-memory. Sharing the *directory* has
// no such risk, only the file -- an earlier version of this class used a
// separate ~/.big_state_display/ directory too, over-applying that
// reasoning; corrected 2026-09-01.
//
// Uses pbd/xml++.h's XMLTree/XMLNode -- the exact class main_panel.cpp
// already uses for gui_config.xml itself -- rather than inventing a new
// mechanism; already linked into this binary via libpbd.a. Same
// load/save shape as MainPanel::load_rc()/save_rc(), same
// fails-gracefully-if-missing and always-writes-fresh behaviour
// (verified in that file before writing this one, not assumed): a
// missing file or directory is not an error, defaults are used, and the
// next successful save recreates both.

#ifndef __big_state_display_display_config__
#define __big_state_display_display_config__

#include <wx/gdicmn.h>

class DisplayConfig
{
  public:
	// filename: which file under config_dir() this instance reads/writes.
	// Default is the original single-loop file, unchanged -- the
	// multi-loop stack window passes a different name so its own remembered
	// position doesn't collide with a single-loop instance's, if both
	// happen to run at once. Same "own file, not a shared one" reasoning
	// as this class already applies to slgui's gui_config.xml above --
	// just a second instance of that same reasoning, not a new one.
	DisplayConfig (const wxString & filename = wxT("slbigstate_config.xml"));

	// Fills in whatever was saved last time; leaves defaults (150,150 /
	// 400x200) untouched for anything not found or if the file/directory
	// does not exist yet.
	void load ();

	// Always writes a complete, fresh file, creating the directory first
	// if needed -- same as MainPanel::save_rc().
	void save () const;

	wxPoint get_position () const { return _position; }
	void set_position (wxPoint pos) { _position = pos; }
	wxSize get_size () const { return _size; }
	void set_size (wxSize size) { _size = size; }

  protected:
	wxString config_dir () const;
	wxString config_path () const;

	wxString _filename;
	wxPoint _position;
	wxSize  _size;
};

#endif
