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

# /tmp is not writable (or not permitted) on plenty of HPC nodes, so honour
# $TMPDIR when the site sets one and otherwise stay inside the checkout.
OUT="${TMPDIR:-.}"; OUT="${OUT%/}/unsview_readers"   # %/ : $TMPDIR often ends in a slash
mkdir -p "$OUT"
fails=0 skips=0

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

# The fixtures above are deliberately well-behaved -- int32 connectivity, no fill
# values, corners always present. int64 connectivity, transposed arrays and
# centers-without-corners only show up in files real models wrote, so exercise one
# per reader when samples/testdata/ has been populated. That directory is
# gitignored and optional: ./tests/fetch_testdata.sh fills it, and every line here
# skips cleanly without it, so this stays the no-network check it has always been.
# One file per reader, deliberately -- fetch_testdata.sh --check is the exhaustive
# pass over all 15.
real() {
    label=$1; want=$2; file=$3; shift 3
    if [ ! -s "$file" ]; then
        printf '  skip %-28s (not fetched)\n' "$label"; skips=$((skips + 1)); return
    fi
    png="$OUT/real_$(printf '%s' "$label" | tr -cs 'a-zA-Z0-9' '_').png"
    log=$(./unsview --no-rc "$file" "$@" -o "$png" 2>&1)
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

echo "real model output (samples/testdata/, optional):"
D=samples/testdata
real "mpas   x1.10242 terrain"   mpas  $D/x1.10242.static.nc -v ter
real "icon   R02B04 bathymetry"  icon  $D/icon_R02B04_ocean.nc -v cell_elevation
real "ugrid  fesom sst on nodes" ugrid $D/fesom_sst.nc --grid $D/fesom_mesh_diag.nc -v sst
real "cs     GEOS c12, corners"  cs    $D/geos_c12.nc -v PHIS
real "cs     GEOS-IT c180, none" cs    $D/geos_it_c180_const.nc4 -v PHIS

echo
[ "$skips" -gt 0 ] && \
    echo "$skips real-data check(s) skipped -- ./tests/fetch_testdata.sh downloads them"
if [ "$fails" -eq 0 ]; then
    echo "all readers OK. PNGs in $OUT/"
else
    echo "$fails failed" >&2
    exit 1
fi
