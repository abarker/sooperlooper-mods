# Copyright (C) 2004 Jesse Chappell <jesse@essej.net>
#  
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without 
# modifications, as long as this notice is preserved.
# 
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

EXTRA_DIST = autogen.sh configure OSC top_marker README_Windows.md windows
SUBDIRS =  libs src

#------ albBegin ----------------------------------
# sllatency: the latency measurement tool (tools/sllatency.py), installed
# as "sllatency" next to sooperlooper and slgui.  A Python 3 script; it
# needs numpy and the Python jack module at run time, not to build.
EXTRA_DIST += tools/sllatency.py tools/test_sllatency.py

install-exec-local:
	$(MKDIR_P) $(DESTDIR)$(bindir)
	$(INSTALL_SCRIPT) $(srcdir)/tools/sllatency.py $(DESTDIR)$(bindir)/sllatency

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/sllatency
#------ albEnd ----------------------------------

