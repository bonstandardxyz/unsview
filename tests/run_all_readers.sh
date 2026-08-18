#!/usr/bin/env bash
# Render every committed fixture and report pass/fail per reader.
# No network, no downloads -- this is the 60-second "is unsview working" check.
#
#     ./tests/run_all_readers.sh
#
# For real model output see tests/fetch_testdata.sh, and README.md for how to
# check unsview against your own files. tests/check_corners.c measures how
# closely the centers-only path reproduces real corner coordinates.

set -u
cd "$(dirname "$0")/.."
[ -x ./unsview ] || { echo "build first: make" >&2; exit 1; }

OUT=${TMPDIR:-/tmp}/unsview_readers
mkdir -p "$OUT"
fails=0

check() {
    label=$1; file=$2; var=$3; want=$4
    png="$OUT/$(basename "$file" .nc)_$var.png"
    log=$(./unsview --no-rc "$file" -v "$var" -t 0 -o "$png" 2>&1)
    got=$(printf '%s\n' "$log" | sed -n 's/^loaded \([a-z]*\) mesh.*/\1/p')
    if [ ! -s "$png" ]; then
        printf '  FAIL %-28s (no output)\n' "$label"; fails=$((fails + 1)); return
    fi
    if [ "$got" != "$want" ]; then
        printf '  FAIL %-28s (detected "%s", expected "%s")\n' "$label" "$got" "$want"
        fails=$((fails + 1)); return
    fi
    printf '  ok   %-28s %s\n' "$label" \
        "$(printf '%s\n' "$log" | sed -n 's/^loaded [a-z]* mesh: \(.*\)/\1/p')"
}

# Six encodings: mpas/icon/ugrid/fvcom are one mesh and one field spelled four
# ways; the two cs files are a real cubed sphere carrying that same field, once
# with corner coordinates and once with centers only.
echo "committed fixtures (six encodings):"
check "mpas"              samples/synthetic.nc       wave      mpas
check "icon"              samples/synthetic_icon.nc  wave      icon
check "ugrid"             samples/synthetic_ugrid.nc wave      ugrid
check "fvcom"             samples/synthetic_fvcom.nc wave      fvcom
check "cs (cubed sphere)" samples/synthetic_cs.nc    wave      cs
check "cs, centers only"  samples/synthetic_cs_centers.nc wave cs

echo "node-centered data, averaged onto faces:"
check "ugrid, on nodes"   samples/synthetic_ugrid.nc wave_node ugrid
check "fvcom, on nodes"   samples/synthetic_fvcom.nc zeta      fvcom

# The encodings that share a coordinate unit must agree exactly. MPAS and ICON
# both store radians; UGRID and FVCOM both store degrees, so they differ from
# the radian pair only by that round-trip. Anything larger is a reader bug.
echo "cross-encoding equivalence:"
same() {
    a=$(shasum -a 256 "$OUT/$1" 2>/dev/null | cut -d' ' -f1)
    b=$(shasum -a 256 "$OUT/$2" 2>/dev/null | cut -d' ' -f1)
    if [ -n "$a" ] && [ "$a" = "$b" ]; then
        printf '  ok   %s\n' "$3"
    else
        printf '  FAIL %s (renders differ)\n' "$3"; fails=$((fails + 1))
    fi
}
same synthetic_wave.png       synthetic_icon_wave.png  "mpas == icon (both radians)"
same synthetic_ugrid_wave.png synthetic_fvcom_wave.png "ugrid == fvcom (both degrees)"

echo
if [ "$fails" -eq 0 ]; then
    echo "all readers OK. PNGs in $OUT/"
else
    echo "$fails failed" >&2
    exit 1
fi
