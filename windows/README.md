## Building and running SooperLooper on Windows (Cygwin)

SooperLooper builds on Windows under Cygwin, a POSIX layer, and talks to
the Windows build of JACK.  The GUI programs (`slgui`, `slbigstate`) use
wxWidgets for GTK, so they need an X server.  MinGW is not an option:
some of SooperLooper's libraries (for example `pbd`) rely on POSIX
headers it lacks.

**Status.**  This guide is compiled from stock's two Cygwin guides,
`README_Windows.md` at the top of the repository (2019) and the earlier
version of this file (2020, at the tag `stock-1.7.9`), with current
information on Cygwin's packages, JACK2 1.9.22 and this fork's
additions.  The build script has been run once, on Windows 10 in a
virtual machine: after fixes made along the way, it built and installed
everything, and `slgui` ran with the engine on JACK's dummy driver.  The
start script, the MIDI fix and MIDI itself have not been tried yet.  If a
step fails, the error and the step number are what's needed to fix it.


### Quick way: the build script

`cygwin_compile.bash` in this folder does steps 1 to 4 below, including
the dependencies:

1. Install Cygwin: download `setup-x86_64.exe` from
   https://cygwin.com/install.html and run it.  The defaults are fine;
   on the package page, search for `git` and select it.
2. Open a Cygwin terminal and run:

   ```bash
   git clone https://github.com/abarker/sooperlooper-mods
   bash sooperlooper-mods/windows/cygwin_compile.bash
   ```

The script installs the missing Cygwin packages and JACK (each asks for
administrator approval), builds liblo and Rubber Band, then builds
SooperLooper and asks before installing it.  Each step skips itself if
already done, so the same command also rebuilds after a `git pull`.
Downloads and library builds go in `~/sooperlooper_build_deps`.  Then
see step 5 to run it.

A copy of a tree already built on Linux works too, once that build is
removed with `git clean -Xdf` in it (this deletes only files git
ignores; `git clean -Xdn` lists them first).  The script stops with that
advice if it finds such a build.

The steps by hand follow.  Everything runs in a Cygwin terminal unless
it says otherwise.


### 1. Install JACK

Install JACK2 for Windows, 64-bit, from
https://jackaudio.org/downloads/ (1.9.22 at the time of writing,
`jack2-win64-v1.9.22.exe`).  In
the installer, make sure the **development files** component ("dev") is
ticked; the build needs it.  The installer puts:

- the JACK server, QjackCtl and the `jack_*` tools in
  `C:\Program Files\JACK2`;
- the headers in `C:\Program Files\JACK2\include\jack`;
- `libjack64.dll`, which every JACK program loads, in `C:\Windows`.


### 2. Install Cygwin and its packages

Download `setup-x86_64.exe` from https://cygwin.com/install.html.  Run
it once to install the base system, then, from a Windows Command Prompt
in the folder holding it, add the packages this build needs (the
installer can also be re-run and the packages picked by hand):

```bat
setup-x86_64.exe -q -P gcc-g++,make,autoconf,automake,libtool,pkg-config,gettext-devel,git,wget,meson,ninja
setup-x86_64.exe -q -P libxml2-devel,libncurses-devel,libsndfile-devel,libsamplerate-devel,libfftw3-devel,libsigc2.0-devel,libwx_baseu3.0-devel,libwx_gtk3u3.0-devel
setup-x86_64.exe -q -P xorg-server,xinit
```

The last line is Cygwin's own X server, for the GUI.  If a package name
is not found, search for it in the installer: names occasionally change.

The 2019 guide needed an older libsigc++ (2.4) because a header was
missing.  This fork builds against libsigc++ 2.12 on Ubuntu, so the
current Cygwin package should do.


### 3. Build two libraries from source

Cygwin has no packages for these.  Both install under `/usr/local`, so
first tell `pkg-config` to look there (add this line to `~/.bashrc` to
keep it):

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

**liblo** (OSC).  This fork builds with liblo 0.32 and earlier.

```bash
wget https://github.com/radarsat1/liblo/releases/download/0.32/liblo-0.32.tar.gz
tar xzf liblo-0.32.tar.gz
cd liblo-0.32
./configure --disable-shared --disable-examples
make && make install
cd ..
```

**Rubber Band** (time stretching; SooperLooper does not build without
it).  Version 3.3 is what Ubuntu 24.04 ships, and its built-in FFT and
resampler mean no extra libraries.  Turning off the plugins and the
Java binding avoids the LADSPA, Vamp and JNI dependencies that the 2019
guide had to work around.

```bash
wget https://breakfastquay.com/files/releases/rubberband-3.3.0.tar.bz2
tar xjf rubberband-3.3.0.tar.bz2
cd rubberband-3.3.0
meson setup build --prefix=/usr/local \
    -Dcpp_std=gnu++11 -Dcpp_args=-D_GNU_SOURCE \
    -Djni=disabled -Dladspa=disabled -Dlv2=disabled -Dvamp=disabled \
    -Dcmdline=disabled -Dtests=disabled
ninja -C build && ninja -C build install
cd ..
```

The two `cpp` options are for Cygwin's headers: they declare
`posix_memalign` only outside strict `c++11` (Rubber Band's own setting)
and `sincos` only with `_GNU_SOURCE`, and the build needs both.


### 4. Build SooperLooper

Make JACK's headers and DLL visible to the Cygwin compiler:

```bash
mkdir -p /usr/local/include /usr/local/lib
cp -R "/cygdrive/c/Program Files/JACK2/include/jack" /usr/local/include
cp /cygdrive/c/Windows/libjack64.dll /usr/local/lib
export JACK_CFLAGS=-I/usr/local/include
export JACK_LIBS="-L/usr/local/lib -ljack64"
```

Then clone with **Cygwin's** `git` and build:

```bash
git clone https://github.com/abarker/sooperlooper-mods
cd sooperlooper-mods
./autogen.sh
./configure --with-wxconfig-path=/usr/bin/wx-config-3.0 CPPFLAGS=-D_GNU_SOURCE
make && make install
```

`CPPFLAGS=-D_GNU_SOURCE` is needed because the build asks for strict
C++11, under which Cygwin's headers hide `strdup` and other POSIX
functions.

Cygwin's `git` keeps the files' Unix line endings.  Git for Windows
converts them to CRLF by default, which breaks the shell scripts; that
is why the 2019 guide ran `dos2unix` on the source.  With Cygwin's `git`
that step is not needed.

`make install` puts `sooperlooper`, `slgui`, `slconsole`,
`slregister`, `slbigstate` and `sllatency` in `/usr/local/bin`.


### 5. Run

The short way, from a Cygwin terminal:

```bash
start_sooperlooper
```

This script (`windows/start_sooperlooper.bash`, installed by the build
script) starts the JACK server and the X server if they are not already
running, then `slgui`, and stops JACK again when `slgui` exits if it
started it.  It uses a JACK server already started from QjackCtl as it
is; otherwise it starts one with JACK's dummy driver, which needs no
sound card.  For a real sound card, change `jack_driver_args` at the top
of the installed copy (`/usr/local/bin/start_sooperlooper`), or start
JACK from QjackCtl first.  Its arguments are passed on to `slgui`.

By hand:

1. Start JACK from QjackCtl (Start menu, or
   `C:\Program Files\JACK2\qjackctl\qjackctl.exe`).  If it fails with
   "Can't open default PortAudio device", pick a device under Interface
   in Setup, or set Driver to dummy.  In a VirtualBox VM, the audio
   input is off by default, which may be why the default device fails.
2. From a Cygwin terminal, start the X server and then the GUI:

   ```bash
   startxwin >/dev/null 2>&1 &
   until [ -S /tmp/.X11-unix/X0 ]; do sleep 1; done
   export DISPLAY=:0
   slgui &
   ```

   The `until` line waits for the X server, which can take several
   seconds to start (longer in a virtual machine); without it, or
   without `DISPLAY`, `slgui` stops with "Unable to initialize GTK+".
   The server keeps running, with an icon in the notification area,
   until you quit it there.
   `slgui` starts the `sooperlooper` engine itself.  To run the engine
   alone: `sooperlooper &`.  The fork's `slbigstate` runs the same way.

   `slgui` and `slbigstate` also print a warning that the program and
   library build versions don't match ("C++ ABI 1014" against "1019"):
   Cygwin's wxWidgets was built with an older GCC than the one now
   installed.  It is a warning, and `slgui` carries on past it.

**MIDI.**  Stock SooperLooper reads MIDI only from ALSA (Linux) and
CoreMIDI (macOS), so stock had no MIDI input on Windows.  This fork adds
JACK MIDI ports, `sooperlooper:midi_in` and `sooperlooper:midi_out`,
which should give Windows MIDI through JACK: start the JACK server with
its Windows MIDI driver (`-X winmme` on the `jackd` command line; in
QjackCtl, add it to the server command in Setup), then connect your
MIDI device's capture port to `sooperlooper:midi_in` in QjackCtl's
Graph or Connections window (`start_sooperlooper` starts JACK with
`-X winmme` already).  This is untried.  It needs this fork's engine to
create its MIDI handling without ALSA or CoreMIDI, which it now does;
before that fix, MIDI and the MIDI bindings were ignored on Windows.

To test MIDI input without a MIDI device, `jack_midi_send.c` in this
folder sends one note (60, channel 1) to `sooperlooper:midi_in`:

```bash
gcc -o jack_midi_send windows/jack_midi_send.c \
    -I/usr/local/include -L/usr/local/lib -ljack64
./jack_midi_send
```

With SooperLooper running, right-click a loop's Record button, choose
Learn MIDI Binding, and run `./jack_midi_send`: the binding is learned.
Each further run then presses Record.

**`sllatency`** is a Python script.  It needs Cygwin's `python3` and
`python3-numpy` packages and the `jack` module (`pip install
JACK-Client`), which has to find JACK's Windows DLL; untried.  On
Windows its advice gives only the total latency change (see its section
in the fork's `mod_docs/late_hit_patch_summary.rst`).


### Packaging for other Windows machines

`package.sh` in this folder copies the programs and the Cygwin DLLs they
need into a folder that runs without a Cygwin installation, and
`start_with_gui.cmd` starts it.  Both date from 2020: the DLL list
names versions current then (several have changed), they leave out
`slbigstate`, and the start script launches Xming, an X server whose
free version is no longer updated (Cygwin's `startxwin` or VcXsrv
can take its place).  For your own machine, run from
Cygwin as above.  To build the DLL list for a current install, run
`cygcheck /usr/local/bin/slgui.exe`.


### Not working yet: a native Windows GUI

Building wxWidgets with its native Windows port (wxMSW) would remove the
need for an X server.  In 2019 SooperLooper compiled that way, but
`slgui` started without showing a window and exited at once.  Notes from
that attempt (wxWidgets 3.1.2):

```bash
export CPPFLAGS='-w -fpermissive -D__USE_W32_SOCKETS'
export LDFLAGS='-L /lib/w32api/'
./configure --prefix=/usr/local --with-msw
# then add -lGL to EXTRALIBS_OPENGL in the Makefile
make && make install
```

- https://forums.wxwidgets.org/viewtopic.php?t=33849
- https://forums.wxwidgets.org/viewtopic.php?t=29596
