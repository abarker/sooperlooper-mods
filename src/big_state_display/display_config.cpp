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

#include <wx/filename.h>
#include <wx/dir.h>
#include <wx/utils.h>

#include <pbd/xml++.h>

#include "display_config.hpp"

using namespace std;

DisplayConfig::DisplayConfig (const wxString & filename)
	: _filename(filename)
	, _position(150, 150)
	, _size(400, 200)
{
}

wxString
DisplayConfig::config_dir () const
{
	// ~/.sooperlooper/ -- the directory slgui's own gui_config.xml
	// already lives in, per the user's own correction (2026-09-01): the
	// earlier choice of a whole separate ~/.big_state_display/ directory
	// over-applied the "own file, not a shared one" reasoning to the
	// directory level too, when only the FILE needs to be exclusively
	// this program's own to avoid slgui's and this program's writes
	// racing --
	// there's no such risk in sharing the
	// directory itself.
	return wxGetHomeDir() + wxFileName::GetPathSeparator() + wxT(".sooperlooper");
}

wxString
DisplayConfig::config_path () const
{
	return config_dir() + wxFileName::GetPathSeparator() + _filename;
}

void
DisplayConfig::load ()
{
	// A missing file is normal (it is created on first exit with this
	// layout); skip parsing it so libxml does not print an "I/O warning:
	// failed to load external entity" to the terminal.
	if (!wxFileExists (config_path())) {
		return;
	}
	string configfname (static_cast<const char *> (config_path().fn_str()));
	XMLTree configdoc (configfname);

	// Fails gracefully -- same as MainPanel::load_rc(): a missing file
	// is not fatal, the defaults set in the constructor above stay in
	// effect, and save() below will (re)create it on next exit.
	if (!configdoc.initialized()) {
		return;
	}

	XMLNode * rootNode = configdoc.root();
	if (!rootNode || rootNode->name() != "BigStateConfig") {
		return;
	}

	XMLProperty *prop;

	if ((prop = rootNode->property ("x_pos")) != 0) {
		_position.x = atol (prop->value().c_str());
	}
	if ((prop = rootNode->property ("y_pos")) != 0) {
		_position.y = atol (prop->value().c_str());
	}
	if ((prop = rootNode->property ("width")) != 0) {
		_size.SetWidth (atol (prop->value().c_str()));
	}
	if ((prop = rootNode->property ("height")) != 0) {
		_size.SetHeight (atol (prop->value().c_str()));
	}
}

void
DisplayConfig::save () const
{
	wxString dirname = config_dir();

	if (!wxDirExists(dirname)) {
		if (!wxMkdir(dirname, 0755)) {
			fprintf (stderr, "big_state_display: could not create %s\n",
			         static_cast<const char *> (dirname.mb_str()));
			return;
		}
	}

	// Always builds a complete, fresh tree -- same as MainPanel::save_rc().
	// Nothing else lives in this file, so there is nothing to preserve
	// from a prior version the way save_rc() has to for gui_config.xml.
	XMLTree configdoc;
	XMLNode * rootNode = new XMLNode ("BigStateConfig");

	char buf[40];
	snprintf (buf, sizeof(buf), "%d", _position.x);
	rootNode->add_property ("x_pos", buf);
	snprintf (buf, sizeof(buf), "%d", _position.y);
	rootNode->add_property ("y_pos", buf);
	snprintf (buf, sizeof(buf), "%d", _size.GetWidth());
	rootNode->add_property ("width", buf);
	snprintf (buf, sizeof(buf), "%d", _size.GetHeight());
	rootNode->add_property ("height", buf);

	configdoc.set_root (rootNode);

	string path (static_cast<const char *> (config_path().fn_str()));
	if (!configdoc.write (path)) {
		fprintf (stderr, "big_state_display: could not write %s\n", path.c_str());
	}
}
