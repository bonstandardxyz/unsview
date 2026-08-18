#!/usr/bin/env bash
set -euo pipefail

# Point the Makefile at the conda env (the only prefix that should matter).
export NETCDF_PREFIX="$PREFIX"
export PNG_PREFIX="$PREFIX"
export X11_PREFIX="$PREFIX"

# Make sure compilers see only the conda env, not /opt/X11 or /usr.
export CFLAGS="${CFLAGS:-} -I${PREFIX}/include"
export LDFLAGS="${LDFLAGS:-} -L${PREFIX}/lib"

# `source: path: ..` copies the working tree verbatim, stale src/*.o and a
# previously built ./unsview included. Without this clean, make finds
# everything up to date, does nothing, and `make install` copies the
# developer's local binary into the package -- one linked against Homebrew and
# XQuartz rather than the conda prefix, which then fails to load anywhere else.
make clean

# HAVE_X11=1 forces the GUI in on every platform rather than letting the
# Makefile auto-detect it. Detection that quietly fails would produce a headless
# package that still installs and still runs -- the failure mode is invisible
# until a user types `unsview file.nc` and gets no window -- so a missing Xaw
# must be a build error instead. X11_PREFIX above keeps the header/lib search
# inside the conda prefix, so a host XQuartz at /opt/X11 cannot leak in.
make -j"${CPU_COUNT:-2}" V=1 HAVE_X11=1

# install into the conda prefix
make install \
    PREFIX="$PREFIX" \
    DEST="$PREFIX" \
    DATADIR="$PREFIX/share/unsview"
