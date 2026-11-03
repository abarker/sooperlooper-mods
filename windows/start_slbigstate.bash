#!/bin/bash
#
# Starts slbigstate, the fork's big state display, on Windows under
# Cygwin: the X server if it is not already running, then slbigstate
# with this script's arguments (slbigstate --help lists them).  It shows
# a running engine's state, so start the engine first, with start_slgui
# for example.  See windows/README.md.  Installed as start_slbigstate by
# cygwin_compile.bash.  The X server part is the same as in
# start_slgui.bash; keep the two in step.

x_log="/tmp/startxwin.log"
x_socket="/tmp/.X11-unix/X0"

if ps -ef | grep -q '[X]Win'; then
   echo "X server already running."
else
   echo "Starting the X server..."
   # With no XWin running, a socket left from an earlier run is stale;
   # waiting for it would start slbigstate before the server is up.
   rm -f "$x_socket"
   startxwin >"$x_log" 2>&1 &
   waited=0
   until [ -S "$x_socket" ]; do
      if [ $waited -ge 60 ]; then
         echo "The X server did not start within 60 s.  See $x_log"
         echo "and /var/log/xwin/XWin.0.log."
         exit 1
      fi
      sleep 1
      waited=$((waited + 1))
   done
fi

export DISPLAY=:0
exec slbigstate "$@"
