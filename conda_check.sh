#!/usr/bin/env bash
# Automated conda build + smoke check for unsview.
# Works on Linux and macOS; both build the full GUI.
# Run from the project root — the directory that contains recipe/ and samples/:
#
#     ./conda_check.sh
#
# Prereqs: Miniconda installed and `conda activate base` done, plus conda-build:
#     conda install -n base -c conda-forge conda-build -y
#
# Override the scratch env name with:  UNSVIEW_ENV=myenv ./conda_check.sh
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $*"; }

SCRATCHENV="${UNSVIEW_ENV:-unsview-scratch}"

# Render targets. /tmp is not writable (or not permitted) on plenty of HPC
# nodes, so honour $TMPDIR when the site sets one and otherwise stay inside the
# checkout. Never hardcode /tmp.
OUT="${TMPDIR:-$here}"; OUT="${OUT%/}/unsview_check"   # %/ : $TMPDIR often ends in a slash
mkdir -p "$OUT"

# ---- 0. prerequisites -------------------------------------------------------
command -v conda >/dev/null 2>&1 \
    || fail "conda not found. Install Miniconda, then 'source <miniconda>/bin/activate'."
conda build --help >/dev/null 2>&1 \
    || fail "conda-build missing. Run: conda install -n base -c conda-forge conda-build -y"
[ -f recipe/meta.yaml ] \
    || fail "run this from the project root (recipe/meta.yaml not found)"

# ---- 1. build ---------------------------------------------------------------
echo "== 1/4 building conda package (recipe sources from this working tree) =="
conda build recipe -c conda-forge

# ---- 2. install into a fresh env -------------------------------------------
echo "== 2/4 creating scratch env '$SCRATCHENV' from the local build =="
conda env remove -n "$SCRATCHENV" -y >/dev/null 2>&1 || true
conda create -y -n "$SCRATCHENV" -c local -c conda-forge unsview

run() { conda run -n "$SCRATCHENV" "$@"; }
PREFIX="$(conda run -n "$SCRATCHENV" printenv CONDA_PREFIX)"

# ---- 3. smoke checks --------------------------------------------------------
echo "== 3/4 smoke checks =="

run unsview -h >/dev/null || fail "unsview -h"
ok "unsview -h runs"

test -f "$PREFIX/share/unsview/coastlines_110m.txt" \
    || fail "overlay data not installed under \$PREFIX/share/unsview"
ok "bundled overlay data installed"

# The fixtures have to be *in the package*. Everything else here runs them from
# the source tree, so only this notices if `make install` stops shipping them --
# and a conda user with no checkout would then have nothing to test against.
test -f "$PREFIX/share/unsview/samples/synthetic.nc" \
    || fail "synthetic fixtures not installed under \$PREFIX/share/unsview/samples"
run unsview --no-rc "$PREFIX/share/unsview/samples/synthetic.nc" -v wave -t 0 \
    -o "$OUT/unsview_installed.png" || fail "installed fixture failed to render"
test -s "$OUT/unsview_installed.png" || fail "installed-fixture PNG is empty"
ok "bundled fixtures installed and render"

# --no-rc throughout: a developer's ~/.unsviewrc setting `cmap = jet` or
# `width = 1920` would otherwise change the byte sizes compared below.

# headless PNG render — overlays are drawn by default
run unsview --no-rc samples/synthetic.nc -v wave -t 0 -o "$OUT/unsview_on.png" \
    || fail "headless render failed"
test -s "$OUT/unsview_on.png" || fail "$OUT/unsview_on.png is empty"
on=$(wc -c < "$OUT/unsview_on.png")
ok "headless PNG render ($on bytes)"

# Feature check: default overlays vs --no-coast-data must differ
run unsview --no-rc samples/synthetic.nc -v wave -t 0 --no-coast-data -o "$OUT/unsview_off.png" \
    || fail "--no-coast-data render failed"
off=$(wc -c < "$OUT/unsview_off.png")
[ "$on" -ne "$off" ] \
    || fail "overlays-on and --no-coast-data produced identical PNGs (overlays not default-on)"
ok "overlays on by default (on=$on vs off=$off bytes)"

# time-varying sample renders
run unsview --no-rc samples/synthetic.nc -v wave -t 1 -o "$OUT/unsview_wave.png" \
    || fail "synthetic render failed"
test -s "$OUT/unsview_wave.png" || fail "wave PNG is empty"
ok "time-varying sample renders"

# every mesh reader loads (cs is a different mesh, same field function)
for enc in icon ugrid cs cs_centers fvcom; do
    run unsview --no-rc "samples/synthetic_$enc.nc" -v wave -t 0 \
        -o "$OUT/unsview_$enc.png" || fail "$enc render failed"
    test -s "$OUT/unsview_$enc.png" || fail "$enc PNG is empty"
    ok "$enc mesh renders"
done

# node-centered data is averaged onto faces rather than rejected
run unsview --no-rc samples/synthetic_ugrid.nc -v wave_node -t 0 \
    -o "$OUT/unsview_node.png" || fail "node-centered render failed"
test -s "$OUT/unsview_node.png" || fail "node PNG is empty"
ok "node-centered variable renders"

# The X11 GUI must be compiled into the package. Every check above passes just
# as happily on a headless binary, so this is the only one that would notice.
# Emptying DISPLAY keeps it deterministic and windowless -- it can never reach a
# server, so it can never open a window and hang this script, and a GUI build
# then fails inside X rather than in the "not compiled in" branch.
gui_log=$(run env DISPLAY= unsview --no-rc samples/synthetic.nc -v wave 2>&1 || true)
if printf '%s' "$gui_log" | grep -q "GUI not compiled in"; then
    fail "package was built headless -- the X11 GUI is not compiled in"
fi
ok "X11 GUI compiled into the package"

# ---- 4. done ----------------------------------------------------------------
echo "== 4/4 PASS =="
echo
echo "Click-to-plot is interactive (GUI only) and not covered by this script."
echo "With a display -- 'ssh -Y' on Linux, XQuartz running on macOS -- try:"
echo "    conda run -n $SCRATCHENV unsview samples/synthetic.nc -v wave"
echo "  then left-click a cell -> a time-series popup opens."
echo
echo "Clean up the scratch env with:"
echo "    conda env remove -n $SCRATCHENV -y"
