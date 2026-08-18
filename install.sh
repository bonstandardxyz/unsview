#!/usr/bin/env bash
# install.sh - one-shot build & install of `visualize` into $HOME/.local
#
# This script:
#   1. Verifies a C compiler is available
#   2. Locates libnetcdf (via nc-config, common env vars, or Homebrew)
#   3. Auto-detects whether the X11 GUI can be built
#   4. Runs `make` and `make install DEST=$HOME/.local`
#   5. Prints what to add to PATH
#
# Usage:
#   ./install.sh [--prefix=DIR]
#                [--netcdf-prefix=DIR] [--png-prefix=DIR]
#                [--x11-prefix=DIR]            # everything in one tree (default /opt/X11)
#                [--xaw-prefix=DIR]            # libXaw       (Athena widgets)
#                [--xt-prefix=DIR]             # libXt        (toolkit)
#                [--xmu-prefix=DIR]            # libXmu
#                [--xext-prefix=DIR]           # libXext
#                [--sm-prefix=DIR]             # libSM
#                [--ice-prefix=DIR]            # libICE
#                [--xproto-prefix=DIR]         # xproto/xorgproto headers (X11/X.h, Xfuncproto.h)
#                [--x11-cflags="-I..."] [--x11-ldflags="-L... -Wl,-rpath,..."]
#                [--force-x11]                 # skip detection, force GUI build
#                [--no-x11]                    # skip GUI even if detected
#
# Per-library prefixes are needed when an HPC site installs each X piece
# under its own tree (e.g. /sw/libXaw, /sw/libXt, /sw/libX11). Any prefix
# you don't set falls back to --x11-prefix.
#
# It does NOT call `module load` for you — load your site's netcdf module
# first if needed.

set -e

PREFIX="$HOME/.local"
NETCDF_PREFIX_OVR=""
PNG_PREFIX_OVR=""
X11_PREFIX_OVR=""
XAW_PREFIX_OVR=""
XT_PREFIX_OVR=""
XMU_PREFIX_OVR=""
XEXT_PREFIX_OVR=""
SM_PREFIX_OVR=""
ICE_PREFIX_OVR=""
XPROTO_PREFIX_OVR=""
X11_EXTRA_CFLAGS=""
X11_EXTRA_LDFLAGS=""
FORCE_X11=0
NO_X11=0
for arg in "$@"; do
    case "$arg" in
        --prefix=*)         PREFIX="${arg#*=}" ;;
        --netcdf-prefix=*)  NETCDF_PREFIX_OVR="${arg#*=}" ;;
        --png-prefix=*)     PNG_PREFIX_OVR="${arg#*=}" ;;
        --x11-prefix=*)     X11_PREFIX_OVR="${arg#*=}" ;;
        --xaw-prefix=*)     XAW_PREFIX_OVR="${arg#*=}" ;;
        --xt-prefix=*)      XT_PREFIX_OVR="${arg#*=}" ;;
        --xmu-prefix=*)     XMU_PREFIX_OVR="${arg#*=}" ;;
        --xext-prefix=*)    XEXT_PREFIX_OVR="${arg#*=}" ;;
        --sm-prefix=*)      SM_PREFIX_OVR="${arg#*=}" ;;
        --ice-prefix=*)     ICE_PREFIX_OVR="${arg#*=}" ;;
        --xproto-prefix=*)  XPROTO_PREFIX_OVR="${arg#*=}" ;;
        --x11-cflags=*)     X11_EXTRA_CFLAGS="${arg#*=}" ;;
        --x11-ldflags=*)    X11_EXTRA_LDFLAGS="${arg#*=}" ;;
        --force-x11)        FORCE_X11=1 ;;
        --no-x11)           NO_X11=1 ;;
        -h|--help)
            sed -n '2,33p' "$0"
            exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

cd "$(dirname "$0")"

echo "=== Checking C compiler ==="
if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    echo "ERROR: no C compiler (cc / gcc) found. Load a compiler module." >&2
    exit 1
fi
echo "ok: $(command -v cc 2>/dev/null || command -v gcc)"

echo
echo "=== Locating libnetcdf ==="
NETCDF_PREFIX="$NETCDF_PREFIX_OVR"
if [ -z "$NETCDF_PREFIX" ] && command -v nc-config >/dev/null 2>&1; then
    NETCDF_PREFIX="$(nc-config --prefix 2>/dev/null || true)"
fi
for v in "$NETCDF" "$NETCDF_DIR" "$NETCDF_ROOT" "$NETCDF_C_DIR" "$NETCDF_C_ROOT"; do
    if [ -z "$NETCDF_PREFIX" ] && [ -n "$v" ] && [ -d "$v/include" ]; then
        NETCDF_PREFIX="$v"
    fi
done
if [ -z "$NETCDF_PREFIX" ] && command -v brew >/dev/null 2>&1; then
    P="$(brew --prefix netcdf 2>/dev/null || true)"
    if [ -n "$P" ] && [ -d "$P/include" ]; then NETCDF_PREFIX="$P"; fi
fi
if [ -z "$NETCDF_PREFIX" ] || [ ! -f "$NETCDF_PREFIX/include/netcdf.h" ]; then
    echo "ERROR: cannot locate libnetcdf. Try one of:" >&2
    echo "    module load netcdf            # HPC clusters"  >&2
    echo "    export NETCDF=/path/to/netcdf" >&2
    echo "    ./install.sh --netcdf-prefix=/path/to/netcdf"  >&2
    echo "    brew install netcdf           # macOS"        >&2
    exit 1
fi
echo "ok: NETCDF_PREFIX=$NETCDF_PREFIX"

echo
echo "=== Checking libpng ==="
PNG_PREFIX="$PNG_PREFIX_OVR"
if [ -z "$PNG_PREFIX" ] && command -v brew >/dev/null 2>&1; then
    PNG_PREFIX="$(brew --prefix libpng 2>/dev/null || true)"
fi
if [ -z "$PNG_PREFIX" ] || [ ! -f "$PNG_PREFIX/include/png.h" ]; then
    PNG_PREFIX=""
    for d in /usr /usr/local; do
        if [ -f "$d/include/png.h" ]; then PNG_PREFIX="$d"; break; fi
    done
fi
if [ -z "$PNG_PREFIX" ]; then
    echo "WARNING: png.h not found in standard locations." >&2
    echo "         If build fails, install libpng-dev / libpng-devel," >&2
    echo "         or pass --png-prefix=DIR." >&2
    PNG_PREFIX="/usr"
fi
echo "ok: PNG_PREFIX=$PNG_PREFIX"

echo
echo "=== Checking X11 (optional, for interactive GUI) ==="
# umbrella prefix default
if [ -z "$X11_PREFIX_OVR" ]; then
    if [ -d /opt/X11 ]; then X11_PREFIX_OVR=/opt/X11; else X11_PREFIX_OVR=/usr; fi
fi
# fall through to umbrella for any unset per-lib prefix
: ${XAW_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${XT_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${XMU_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${XEXT_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${SM_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${ICE_PREFIX_OVR:=$X11_PREFIX_OVR}
: ${XPROTO_PREFIX_OVR:=$X11_PREFIX_OVR}

USE_X11=0
if [ "$NO_X11" = 1 ]; then
    echo "note: --no-x11 set, building headless-only"
elif [ "$FORCE_X11" = 1 ]; then
    echo "note: --force-x11 set, will build GUI without sanity check"
    USE_X11=1
else
    HAVE_XAW_H=""
    for h in "$XAW_PREFIX_OVR/include/X11/Xaw/Form.h" \
             "$X11_PREFIX_OVR/include/X11/Xaw/Form.h" \
             /opt/X11/include/X11/Xaw/Form.h \
             /usr/include/X11/Xaw/Form.h \
             /usr/include/X11/Xaw3d/Form.h; do
        if [ -f "$h" ]; then HAVE_XAW_H="$h"; break; fi
    done
    HAVE_XAW_LIB=""
    for d in "$XAW_PREFIX_OVR/lib" "$XAW_PREFIX_OVR/lib64" \
             "$X11_PREFIX_OVR/lib" "$X11_PREFIX_OVR/lib64" \
             /opt/X11/lib /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu; do
        if ls "$d"/libXaw.* >/dev/null 2>&1; then HAVE_XAW_LIB="$d"; break; fi
    done
    if [ -n "$HAVE_XAW_H" ] && [ -n "$HAVE_XAW_LIB" ]; then
        echo "ok: Xaw header   = $HAVE_XAW_H"
        echo "ok: Xaw lib      = $HAVE_XAW_LIB"
        USE_X11=1
    else
        [ -z "$HAVE_XAW_H" ]   && echo "note: no Xaw headers found"
        [ -z "$HAVE_XAW_LIB" ] && echo "note: no Xaw library found"
        echo "      building headless-only (-o out.png still works)"
        echo "      to force the GUI build, use --force-x11 plus the"
        echo "      --xaw-prefix=, --xt-prefix=, etc. flags"
    fi
fi

echo
echo "=== Building ==="
make clean >/dev/null 2>&1 || true

MAKE_ARGS=(
    NETCDF_PREFIX="$NETCDF_PREFIX"
    PNG_PREFIX="$PNG_PREFIX"
    DEST="$PREFIX"
)
if [ "$USE_X11" = 1 ]; then
    MAKE_ARGS+=(
        HAVE_X11=1
        X11_PREFIX="$X11_PREFIX_OVR"
        XAW_PREFIX="$XAW_PREFIX_OVR"
        XT_PREFIX="$XT_PREFIX_OVR"
        XMU_PREFIX="$XMU_PREFIX_OVR"
        XEXT_PREFIX="$XEXT_PREFIX_OVR"
        SM_PREFIX="$SM_PREFIX_OVR"
        ICE_PREFIX="$ICE_PREFIX_OVR"
        XPROTO_PREFIX="$XPROTO_PREFIX_OVR"
    )
    if [ -n "$X11_EXTRA_CFLAGS" ];  then MAKE_ARGS+=(X11_EXTRA_CFLAGS="$X11_EXTRA_CFLAGS"); fi
    if [ -n "$X11_EXTRA_LDFLAGS" ]; then MAKE_ARGS+=(X11_EXTRA_LDFLAGS="$X11_EXTRA_LDFLAGS"); fi
fi

make "${MAKE_ARGS[@]}"

echo
echo "=== Installing to $PREFIX ==="
make install DEST="$PREFIX"

echo
echo "=== Done ==="
echo "Add this to your shell rc if not already:"
echo "    export PATH=\"$PREFIX/bin:\$PATH\""
echo
echo "Then test:"
echo "    unsview -h"
echo
echo "Sample data is in $(pwd)/samples/"
