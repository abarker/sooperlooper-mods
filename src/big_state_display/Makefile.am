bin_PROGRAMS = slbigstate

# Standalone at-a-distance state display, talking to a running
# sooperlooper engine over plain OSC -- no dependency on libslcore.a or
# any engine/plugin code, just the LooperState enum from plugin.hpp for
# compile-time safety on the state values.
# Binary named slbigstate, matching slgui's own naming
# convention (sl prefix, no hyphens), so it reads as an SL tool once
# installed system-wide alongside sooperlooper/slgui.

AM_CXXFLAGS = -I.. @LOSC_CFLAGS@ @XML_CFLAGS@ @WX_CFLAGS@

slbigstate_LDADD = @BASE_LIBS@ @LOSC_LIBS@ @WX_LIBS@ @XML_LIBS@

slbigstate_SOURCES = \
	main.cpp \
	osc_state_client.cpp \
	state_render.cpp \
	state_row_panel.cpp \
	big_state_window.cpp \
	big_state_stack_window.cpp \
	display_config.cpp

noinst_HEADERS = $(wildcard *.hpp *.h)
