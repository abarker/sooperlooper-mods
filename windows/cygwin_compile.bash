#!/bin/bash
#
# Builds and installs this SooperLooper fork on Windows under Cygwin,
# dependencies included.  The Windows counterpart of ubuntu_compile.bash;
# see windows/README.md.
#
# Run it from a Cygwin terminal:
#
#    bash sooperlooper-mods/windows/cygwin_compile.bash
#
# Each step checks whether it is already done and skips itself if so, so
# the same command does the first build and every rebuild.  Steps that
# install on Windows (Cygwin packages, JACK) ask for administrator
# approval.

set -e
trap 'echo "Stopped: line $LINENO failed: $BASH_COMMAND"' ERR

liblo_version=0.32
rubberband_version=3.3.0
jack_version=1.9.22

# SHA-256 of the three third-party downloads, recorded 2026-10-24 from the
# URLs below.  A file that differs (changed upstream, or tampered with)
# stops the script.  Update them together with the versions.
liblo_sha256=5df05f2a0395fc5ac90f6b538b8c82bb21941406fd1a70a765c7336a47d70208
rubberband_sha256=d9ef89e2b8ef9f85b13ac3c2faec30e20acf2c9f3a9c8c45ce637f2bc95e576c
jack_sha256=c46847404c3cbd01ea3473e53f7221078e34142308e911c0e08a40dc44d26a19

# Cygwin packages the build needs (python3-numpy only for sllatency).
cygwin_packages="gcc-g++,make,autoconf,automake,libtool,pkg-config,\
gettext-devel,git,wget,meson,ninja,\
libxml2-devel,libncurses-devel,libsndfile-devel,libsamplerate-devel,\
libfftw3-devel,libsigc2.0-devel,libwx_baseu3.0-devel,libwx_gtk3u3.0-devel,\
xorg-server,xinit,python3,python3-numpy"

force_autogen=false # Rerun autogen.sh even if configure exists.
force_config=false  # Rerun configure even if a Makefile exists.

repo_dir="$(realpath "$(dirname "$0")/..")"
deps_dir="$HOME/sooperlooper_build_deps"  # downloads and library builds
jack_win_dir="/cygdrive/c/Program Files/JACK2"

export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
export JACK_CFLAGS=-I/usr/local/include
export JACK_LIBS="-L/usr/local/lib -ljack64"

mkdir -p "$deps_dir"

# Downloads with PowerShell, which every Windows has, so this works before
# wget is installed.  TLS 1.2 is set because older Windows PowerShell does
# not use it by default, and GitHub requires it.
download() {   # url, output file
   powershell.exe -NoProfile -Command \
      "\$ProgressPreference = 'SilentlyContinue';
       [Net.ServicePointManager]::SecurityProtocol = 'Tls12';
       Invoke-WebRequest -Uri '$1' -OutFile '$(cygpath -w "$2")'"
}

check_sha256() {   # file, expected hash
   if ! echo "$2  $1" | sha256sum -c --quiet -; then
      echo "Checksum mismatch for $1; not using it."
      exit 1
   fi
}

is_installed() {   # Cygwin package name, matched exactly
   cygcheck -c -d "$1" | awk -v p="$1" '$1 == p { found = 1 } END { exit !found }'
}

echo
echo "== Cygwin packages"
missing=""
for p in ${cygwin_packages//,/ }; do
   is_installed "$p" || missing="$missing,$p"
done
if [ -n "$missing" ]; then
   echo "Installing:${missing//,/ }"
   setup_exe="$deps_dir/setup-x86_64.exe"
   download https://cygwin.com/setup-x86_64.exe "$setup_exe"
   chmod +x "$setup_exe"
   # -q unattended, -W wait for the elevated installer, -P packages.
   # Without -g, installed packages are not upgraded, so the Cygwin DLL
   # this shell is using is left alone.
   "$setup_exe" -q -W -n -d -P "${missing#,}"
   echo "Packages installed.  If a later step fails, open a new Cygwin"
   echo "terminal and run this script again."
else
   echo "All present."
fi

echo
echo "== JACK for Windows"
if [ ! -f "$jack_win_dir/include/jack/jack.h" ]; then
   echo "Installing JACK2 $jack_version (server, QjackCtl, development files)..."
   jack_exe="$deps_dir/jack2-win64-v$jack_version.exe"
   download "https://github.com/jackaudio/jack2-releases/releases/download/v$jack_version/jack2-win64-v$jack_version.exe" "$jack_exe"
   check_sha256 "$jack_exe" "$jack_sha256"
   chmod +x "$jack_exe"
   "$jack_exe" /SILENT /COMPONENTS="jackserver,qjackctl,dev" \
      || echo "The JACK installer exited with code $?."
   if [ ! -f "$jack_win_dir/include/jack/jack.h" ]; then
      echo "JACK's headers are not in $jack_win_dir/include/jack."
      echo "Run $(cygpath -w "$jack_exe") by hand, tick the development"
      echo "files, then run this script again."
      exit 1
   fi
else
   echo "Present."
fi
mkdir -p /usr/local/include /usr/local/lib
cp -R "$jack_win_dir/include/jack" /usr/local/include
cp /cygdrive/c/Windows/libjack64.dll /usr/local/lib

echo
echo "== liblo $liblo_version"
if ! pkg-config --exists liblo; then
   cd "$deps_dir"
   wget -N "https://github.com/radarsat1/liblo/releases/download/$liblo_version/liblo-$liblo_version.tar.gz"
   check_sha256 "liblo-$liblo_version.tar.gz" "$liblo_sha256"
   tar xzf "liblo-$liblo_version.tar.gz"
   cd "liblo-$liblo_version"
   ./configure --disable-shared --disable-examples
   make -j"$(nproc)"
   make install
else
   echo "Present."
fi

echo
echo "== Rubber Band $rubberband_version"
if ! pkg-config --exists rubberband; then
   cd "$deps_dir"
   wget -N "https://breakfastquay.com/files/releases/rubberband-$rubberband_version.tar.bz2"
   check_sha256 "rubberband-$rubberband_version.tar.bz2" "$rubberband_sha256"
   tar xjf "rubberband-$rubberband_version.tar.bz2"
   cd "rubberband-$rubberband_version"
   # Cygwin's headers declare posix_memalign and sincos, both used here,
   # only outside strict c++11 (Rubber Band's own setting) and with
   # _GNU_SOURCE respectively.
   rm -rf build
   meson setup build --prefix=/usr/local \
      -Dcpp_std=gnu++11 -Dcpp_args=-D_GNU_SOURCE \
      -Djni=disabled -Dladspa=disabled -Dlv2=disabled -Dvamp=disabled \
      -Dcmdline=disabled -Dtests=disabled
   ninja -C build
   ninja -C build install
else
   echo "Present."
fi

echo
echo "== SooperLooper"
cd "$repo_dir"
# A tree copied from another machine carries that machine's build, which
# config.status gives away by recording a different directory.
if [ -f config.status ] && ! grep -qxF "ac_pwd='$PWD'" config.status; then
   echo "This tree holds a build made elsewhere (copied from Linux?)."
   echo "Remove it with  git clean -Xdf  here (deletes only files git"
   echo "ignores; git clean -Xdn lists them first), then run this again."
   exit 1
fi
if $force_autogen || [ ! -x configure ]; then
   echo "Running autogen..."
   ./autogen.sh
fi
# _GNU_SOURCE because the build asks for strict c++11, under which
# Cygwin's headers hide strdup and other POSIX functions (on Linux, g++
# defines it anyway).
config_args=(--prefix=/usr/local --with-wxconfig-path=/usr/bin/wx-config-3.0
             CPPFLAGS=-D_GNU_SOURCE)
need_config=$force_config
[ -f Makefile ] || need_config=true
for a in "${config_args[@]}"; do   # rerun after a change to config_args
   $need_config || ./config.status --config | grep -qF -- "$a" || need_config=true
done
if $need_config; then
   echo "Running configure..."
   ./configure "${config_args[@]}"
fi
echo "Running make..."
make -j"$(nproc)"

echo
echo -n "run install? "
read yesno
if [ "$yesno" == "y" ]; then
   make install
   install -m 755 windows/start_slgui.bash /usr/local/bin/start_slgui
   install -m 755 windows/start_slbigstate.bash /usr/local/bin/start_slbigstate
   echo
   echo "Installed in /usr/local/bin.  To run: start_slgui"
   echo "(starts JACK and the X server if needed, then slgui)."
fi
