#!/bin/bash
#
# Tests SooperLooper's JACK MIDI input without a MIDI device, on Windows
# under Cygwin: builds jack_midi_send (from jack_midi_send.c in this
# folder, into ~/sooperlooper_build_deps) if needed, then walks through
# learning a MIDI binding for Record and using it.  Start SooperLooper
# first (start_slgui).  See windows/README.md.
#
#    bash sooperlooper-mods/windows/test_jack_midi.bash [port]
#
# Without a port, jack_midi_send finds the engine's midi_in port itself.
# Also runs on Linux (links -ljack there).

note=60

src="$(realpath "$(dirname "$0")")/jack_midi_send.c"
build_dir="$HOME/sooperlooper_build_deps"
sender="$build_dir/jack_midi_send"

if [ "$(uname -o)" = Cygwin ]; then
   jack_flags=(-I/usr/local/include -L/usr/local/lib -ljack64)
else
   jack_flags=(-ljack)
fi

if [ "$src" -nt "$sender" ]; then   # also true when not built yet
   echo "Building $sender"
   mkdir -p "$build_dir"
   gcc -Wall -o "$sender" "$src" "${jack_flags[@]}" || exit 1
fi

send() {
   if ! "$sender" "$note" "$@"; then
      echo
      echo "jack_midi_send failed; its output above shows the step."
      exit 1
   fi
}

echo
echo "1. In slgui, right-click a loop's Record button and choose"
echo "   Learn MIDI Binding (a purple ring shows it is waiting)."
read -r -p "   Then press Enter here to send note $note... "
send "$@"
echo "   slgui should now have learned it (the purple ring goes away)."
echo
read -r -p "2. Press Enter to send it again: the loop should start recording. "
send "$@"
echo
read -r -p "3. Press Enter to send it once more: recording should stop. "
send "$@"
echo
echo "Done.  If each step did what it said, JACK MIDI input works."
