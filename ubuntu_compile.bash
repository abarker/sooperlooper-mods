#!/bin/bash
#
# This file is to compile and install on Ubuntu; version 24.04 in particular.
# Set all flags true for the first compile; after that they can all be false
# (unless some config variables are changed).

install_deps=false # Install dependencies.
run_autogen=false # Run the autogen (when compiling first time or system changes).
run_config=false # Run configure, first time or when reconfigured.

cd "$(dirname "$(realpath "$0")")" # Goto the dir containing this script.

if $install_deps; then
   echo "Installing dependencies..."
   # Note libwxgtk3.0-gtk3-dev (the Ubuntu 22.04 wxWidgets package) is not
   # available on Ubuntu 24.04.  Use libwxgtk3.2-dev instead; see also:
   # https://askubuntu.com/questions/1522811/how-to-install-wxwidgets-on-ubuntu-24-04
   # python3-numpy, python3-jack-client and python3-tk (its file picker)
   # are needed only to run the sllatency tool, not to build.
   sudo apt install -y \
      build-essential git autoconf automake libtool pkg-config \
      python3-numpy python3-jack-client python3-tk \
      libjack-jackd2-dev libasound2-dev \
      libwxgtk3.2-dev \
      libsigc++-2.0-dev \
      libsndfile1-dev libsamplerate0-dev \
      liblo-dev librubberband-dev \
      libfftw3-dev libxml2-dev
fi

if $run_autogen; then
   echo "Running autogen..."
   ./autogen.sh
fi

if $run_config; then
   echo "Running configure..."
   ./configure --prefix=/usr/local
fi

echo "Running make..."

make -j$(nproc) # The command `nproc` returns the number of cores, so use all of them.

echo
echo -n "run install? "
read yesno
if [ "$yesno" == "y" ]; then
   sudo make install
fi

