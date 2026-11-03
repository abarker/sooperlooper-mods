#!/bin/bash
#
# Starts SooperLooper on Windows under Cygwin: the JACK server and the X
# server if they are not already running, then slgui, which starts the
# engine.  See windows/README.md.  Installed as start_slgui by
# cygwin_compile.bash.  start_slbigstate.bash starts the X server the
# same way; keep the two in step.
#
# A JACK server already running (from QjackCtl, say) is used as it is.
# Otherwise this one starts jackd with jack_driver_args below, and stops
# it again when slgui exits.  The dummy driver needs no sound card; for a
# real one, use for example
#    jack_driver_args=(-d portaudio -d "MME::Speakers (High Definition Audio Device)")
# with a device name from QjackCtl's Setup, Interface list.

jack_driver_args=(-d dummy -r 48000)
jack_midi_args=(-X winmme)   # Windows MIDI devices as JACK MIDI ports

jackd_exe="/cygdrive/c/Program Files/JACK2/jackd.exe"
jack_log="/tmp/jackd.log"
x_log="/tmp/startxwin.log"
x_socket="/tmp/.X11-unix/X0"

started_jack=""
if tasklist | grep -qi '^jackd\.exe'; then
   echo "JACK server already running."
else
   echo "Starting JACK: jackd -S ${jack_midi_args[*]} ${jack_driver_args[*]}"
   echo "   (output in $jack_log)"
   "$jackd_exe" -S "${jack_midi_args[@]}" "${jack_driver_args[@]}" \
      >"$jack_log" 2>&1 &
   started_jack=$!
   sleep 3
   if ! kill -0 "$started_jack" 2>/dev/null; then
      echo "JACK did not start.  Its output:"
      cat "$jack_log"
      exit 1
   fi
fi

if ps -ef | grep -q '[X]Win'; then
   echo "X server already running."
else
   echo "Starting the X server..."
   # With no XWin running, a socket left from an earlier run is stale;
   # waiting for it would start slgui before the server is up.
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
echo "Starting slgui."
slgui "$@"

if [ -n "$started_jack" ]; then
   echo "Stopping JACK."
   taskkill /PID "$(cat /proc/$started_jack/winpid)" >/dev/null 2>&1 \
      || kill "$started_jack" 2>/dev/null
fi
