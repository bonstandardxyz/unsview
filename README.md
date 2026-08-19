# unsview — lightweight unstructured-mesh viewer

`ncview`-class viewer for unstructured meshes: **MPAS**, **ICON**, **CF-UGRID**,
**cubed-sphere** (GEOS, FV3) and **FVCOM**. No remap step, no Java, no Electron —
pure C, X11/Xaw GUI, with a headless PNG mode for HPC compute nodes.

unsview is sketch-level plotting by design — point it at a mesh, see the field a
second later, decide whether the file is what you thought. For publication
figures, reach for a higher-level library; for *"is this file what I think it
is?"*, this is the tool.

## Why

`ncview` only handles regular `(j, i)` grids. MPAS / ICON / FVCOM / FESOM /
SCHISM produce unstructured meshes that ncview cannot render. The standard
workaround is to remap onto a regular grid first; this tool draws the native
mesh polygons directly.

## Install

The conda package is the default way in. It carries the X11 GUI on both Linux
and macOS, the coastline overlays, and all six test fixtures:

```sh
conda create -n unsview -c conda-forge unsview
conda activate unsview
unsview -h                    # options; --model lists the conventions
```

> **Not on conda-forge yet.** Until the first release lands there, build the
> package from a checkout — the result is the same package:
>
> ```sh
> conda build recipe -c conda-forge
> conda create -n unsview -c local -c conda-forge unsview
> ```

The fixtures install into the environment, so set this once and every command
below works as written:

```sh
S="$CONDA_PREFIX/share/unsview/samples"     # after conda install
# S=samples                                 # or, from a source checkout
```

Prefer not to use conda? See [Building from source](#building-from-source).

> **Platform note:** the conda X11 packages are client libraries only, so on
> macOS the GUI also needs an X server: `brew install --cask xquartz`, then log
> out and back in once so `$DISPLAY` is set. Headless PNG output (`-o`) needs no
> display anywhere.

## One command, two modes

**The GUI is the default.** Give unsview a file and a variable, and it opens a
window:

```sh
unsview $S/synthetic.nc -v wave
```

Left-click a cell for a time series or vertical profile, drag to pan, scroll to
zoom, and use the buttons to cycle colormaps and toggle the coast, border and
state overlays.

**Add `-o` and the very same command writes a PNG and exits** — no display
involved, which is what makes it work over `ssh` and inside batch jobs:

```sh
unsview $S/synthetic.nc -v wave -o wave.png
```

That one flag is the entire difference between the two modes — same mesh, same
colours, same everything. So **every command in this document is written in GUI
form, and appending `-o out.png` to any of them gives you a file instead.**

The GUI needs a display: XQuartz running on macOS, or `ssh -Y` on Linux. `-o`
needs neither.

> Scripted checks below add `--no-rc`, so a `.unsviewrc` of your own cannot
> change what is being measured. Leave it off in normal use.

## The 60-second check

```sh
unsview $S/synthetic.nc -v wave -o /tmp/check.png && echo OK
```

From a source checkout, a fuller version renders all six fixtures and reports
pass/fail per reader:

```sh
./tests/run_all_readers.sh
```

If that passes, unsview is working; the rest of this document is about *your*
data.

## Supported meshes

The convention is detected automatically; `--model` forces one, which is mostly
useful to get a clearer error out of a file that will not load.

| Model | `--model` | Detected by | Notes | Fixture |
|---|---|---|---|---|
| MPAS | `mpas` | `nCells` + `verticesOnCell` | radians, 1-based connectivity | `$S/synthetic.nc` |
| ICON | `icon` | `clon` + `vertex_of_cell` | radians, 1-based, transposed `(nv, cell)` | `$S/synthetic_icon.nc` |
| CF-UGRID | `ugrid` | a variable with `cf_role = "mesh_topology"` | degrees, `start_index` honoured, `_FillValue`-padded ragged faces | `$S/synthetic_ugrid.nc` |
| Cubed sphere | `cs` | `corner_lons`/`grid_lon` corners, or `lons`/`geolon` centers | structured faces, **no connectivity array** | `$S/synthetic_cs.nc`, `$S/synthetic_cs_centers.nc` |
| FVCOM | `fvcom` | `nv` + `lonc` | triangles, transposed 1-based `nv(three, nele)`, degrees | `$S/synthetic_fvcom.nc` |

The generic CF-UGRID reader is what covers most models: **SCHISM**, **FESOM**,
**ADCIRC** and **D-Flow FM** all write UGRID, so they work without a
model-specific reader. Because UGRID files name their own mesh variables, those
are hidden from the variable list automatically.

Where a UGRID file omits `face_coordinates`, face centers are derived from the
corner nodes, unwrapped across the date line first.

### Node-centered data

MPAS, ICON and the cubed sphere store fields on cell centers. FVCOM and most
UGRID ocean models store them on mesh **nodes** instead. Those are averaged onto
the surrounding faces and render like anything else — the variable list marks
them `(on nodes)`:

```sh
unsview sst.fesom.1948.nc --grid fesom.mesh.diag.nc -v sst
```

## Usage

```
unsview FILE.nc                                   # GUI (requires DISPLAY)
unsview FILE.nc -v t2m                            # GUI, preselect variable
unsview FILE.nc -v t2m -t 0 -o out.png            # headless PNG (no DISPLAY)
unsview DIAG.nc --grid GRID.nc -v t2m -o out.png  # mesh in separate file
unsview hist.*.nc -v t2m                          # many files, one time axis

  -v VAR        select variable
  -t INDEX      time index (or 1st extra dim, default 0)
  -l INDEX      vertical level (or 2nd extra dim, default 0)
  -c CMAP       colormap (default viridis; see list below)
  -W WIDTH      output width (default 1024)
  -H HEIGHT     output height (default 512)
  -o PATH       headless: write PNG and exit
  --model NAME     mesh convention: auto (default), mpas, icon, ugrid,
                   cs (GEOS/FV3 cubed sphere), fvcom
  --grid GRID.nc   load mesh from a separate grid file (for stream-split
                   diag/history files that lack mesh metadata); repeat
                   once per tile with --tiles
  --tiles          treat the input files as tiles of one cubed sphere and
                   stitch them into a globe, rather than as time steps
  --vmin VALUE     fix colorscale lower bound (default: data min)
  --vmax VALUE     fix colorscale upper bound (default: data max)
  --coast PATH     overlay coastline polylines (default white)
  --borders PATH   overlay country borders (default white)
  --states PATH    overlay state/province lines (default white)
  --coast-data     auto-load all three bundled overlays (on by default)
  --no-coast-data  disable the default overlay auto-load
  --lines PATH[:HEX]  overlay any polyline file with an optional color
                   (e.g. --lines roads.txt:ff0000); repeatable
  --global         force the global view (override regional auto-fit)
  --no-rc          ignore .unsviewrc (for reproducible batch runs)
  -h               this help
```

Colormaps for `-c` (18):

```
viridis  plasma  magma  inferno  cividis  turbo
jet      rdbu    brbg   seismic  hot      cool
terrain  gray    wbr    rainbow  bwr      3gauss
```

Passing several files concatenates them along the time axis, so a run split
across many history files behaves like one long series. The mesh is taken from
the first file (or from `--grid`), and files whose cell count disagrees are
skipped with a warning. The exception is `--tiles`, which concatenates along the
**cell** axis instead — see [Stitching the six tiles into a
globe](#stitching-the-six-tiles-into-a-globe).

### Defaults: `.unsviewrc`

Anything you would otherwise retype every run can live in a config file. Keys
are the long option names without the dashes:

```ini
# ~/.unsviewrc
model     = icon
cmap      = rdbu
width     = 1920
height    = 960
global    = yes
data-dir  = /shared/apps/unsview/share
```

The first of `$UNSVIEWRC`, `./.unsviewrc`, `~/.unsviewrc` that exists is used —
they are not merged. Precedence, highest first: **command line → environment →
rc file**, so an explicit `export UNSVIEW_DATA_DIR=...` is never silently
overridden by a stale rc file. `--no-rc` skips the file entirely, which is what
you want in batch scripts that must render identically everywhere.

Boolean keys accept `yes` / `true` / `on` / `1` (or a bare key name).

### Fixed colorscale (for comparison plots)

To compare multiple files or time steps on the same colour scale — without this,
nothing is comparable between steps, because each render rescales to its own
data range:

```sh
for t in 0 6 12 18; do
    unsview daily.nc -v t2m -t $t --vmin 250 --vmax 320 \
              -W 1920 -H 960 -o frame_$t.png
done
```

In GUI mode, the toolbar has `min -` `min +` `max -` `max +` `auto range`
`set range...` buttons. The latter pops up a dialog where you can type
exact `vmin vmax` values.

### Coastline / border overlays

Coastlines, country borders, and state/province lines are drawn **by default**
(from the bundled overlays) in both the GUI and headless PNG modes. In the GUI,
the `coast` / `borders` / `states` buttons cycle each layer white → black → off.
Pass `--no-coast-data` to start with them disabled.

Overlays are also the cheapest correctness check available: if the bundled
coastlines line up with your land/sea boundary, the mesh geometry is right.

To override a layer with your own polyline file, use `--coast PATH` (or
`--borders` / `--states`); an explicit layer replaces the bundled file of that
type:

```sh
unsview x1.10242.static.nc -v ter --coast samples/coastlines_110m.txt
```

#### Polyline file format

Plain ASCII, one `lon lat` pair per line in **degrees**. Blank lines (or
`NaN NaN`) separate polylines. `#` starts a comment.

```
# Natural Earth 110m coastline
130.0 35.0
130.5 35.2
...

131.0 40.0    # blank line above ended the previous polyline
...
```

A bundled `samples/coastlines_110m.txt` (Natural Earth 1:110m, ~5 K points,
public domain) is provided. To build a higher-fidelity overlay or add
country / state borders, convert any GeoJSON / shapefile into the same
format, e.g.:

```python
import json
d = json.load(open('ne_50m_admin_0_boundary_lines_land.json'))
out = open('borders_50m.txt', 'w')
for feat in d['features']:
    g = feat['geometry']
    lines = (g['coordinates'] if g['type'] == 'MultiLineString'
             else [g['coordinates']])
    for line in lines:
        for lon, lat in line:
            out.write(f'{lon} {lat}\n')
        out.write('\n')
```

### GUI controls

- **Variable list (left pane)** — click to switch field
- **`<< t` / `t >>`** — step through time
- **`<< lvl` / `lvl >>`** — step through vertical levels
- **`cmap`** — cycle viridis → jet → rdbu → gray
- **`reset view`** — return to global extent
- **`coast` / `borders` / `states`** — cycle each overlay white → black → off
- **Mouse drag on map** — pan
- **Scroll wheel** — zoom
- **Left-click a cell** — pop up a vertical profile (value vs level) and/or time
  series (value vs time) for that cell; the popup's toggle switches between them
  when the variable has both dimensions
- **Status bar** — live `lon, lat, cell, value` under the cursor

---

# Checking unsview against each convention

Every convention below gets two levels: a **bundled fixture** that needs no
network, and **real model output** you download or stream, to confirm the reader
against files a model actually wrote.

## MPAS

**Fixture (no network):**

```sh
unsview $S/synthetic.nc -v wave
```

**Real data** — the MPAS-Atmosphere static file carries `ter`, `landmask`,
`greenfrac`, `soiltemp` on the native 10242-cell mesh. `fetch_testdata.sh`
downloads and unpacks it for you:

```sh
./tests/fetch_testdata.sh
unsview samples/testdata/x1.10242.static.nc -v ter -c terrain
```

Substitute any resolution from the [MPAS-Atmosphere mesh
downloads](https://mpas-dev.github.io/atmosphere/atmosphere_meshes.html) page —
`x1.163842` (60-km, 164 K cells) and above are useful stress tests.

**A correct render** shows the Tibetan Plateau as the highest terrain, then the
Andes, Rockies, Greenland and Antarctica, with coastlines sitting exactly on the
land/ocean boundary. Ocean is flat at 0.

**Stream-split output.** MPAS diag/history files carry data but no mesh, so
point `--grid` at any grid or static file with the same cell count:

```sh
unsview DIAG.nc --grid x1.10242.static.nc -v t2m
```

To exercise that path without waiting on a model run, pass the static file as
its own grid — it holds both, so it stands in for the pair:

```sh
unsview samples/testdata/x1.10242.static.nc \
        --grid samples/testdata/x1.10242.static.nc -v ter -c terrain
```

That renders byte-identically to the command above it, which is the point: when
`--grid` is wired up correctly it changes nothing about the picture, only where
the mesh was read from.

## ICON

**Fixture:**

```sh
unsview $S/synthetic_icon.nc -v wave
```

**Real data.** Prefer an **ocean** grid (`..._O.nc`) from the
[MPI-M archive](http://icon-downloads.mpimet.mpg.de/grids/public/): unlike the
atmosphere grids, which hold only mesh metadata (`cell_area`, `cell_index`, …),
those ship real bathymetry and a land mask.

```sh
./tests/fetch_testdata.sh
unsview samples/testdata/icon_R02B04_ocean.nc -v cell_elevation -c terrain
```

**A correct render** shows the Mid-Atlantic Ridge and continental shelves, with
land left unpainted (the ocean grid has no cells there) and coastlines hugging
the mesh boundary. Elevation spans roughly −9500 m to +3400 m.

> **DWD's operational ICON open data is GRIB2 only**, which unsview does not
> read — `opendata.dwd.de/weather/nwp/icon/` serves no netCDF at all. Convert
> with `cdo -f nc setgrid,<grid>.nc in.grib2 out.nc` first, or use the netCDF
> grids above.

## CF-UGRID

This is the reader that covers the most models: **SCHISM**, **FESOM**,
**ADCIRC** and **D-Flow FM** all write UGRID, so none of them needs a
model-specific reader.

**Fixture** — deliberately built from the awkward parts of the spec (degrees,
`start_index = 1`, int64 connectivity with `_FillValue`-padded ragged rows, no
`face_coordinates`, plus a node-centered variable):

```sh
unsview $S/synthetic_ugrid.nc -v wave        # on faces
unsview $S/synthetic_ugrid.nc -v wave_node   # on nodes, averaged onto faces
```

**Real data** — FESOM2 sea surface temperature on a real ocean mesh:

```sh
./tests/fetch_testdata.sh                # downloads ~71 MB into samples/testdata/
unsview samples/testdata/fesom_sst.nc \
        --grid samples/testdata/fesom_mesh_diag.nc -v sst
```

**A correct render** puts SST between about −1.9 °C and 29.5 °C with a smooth
equator-to-pole gradient, land unpainted, and coastlines matching the ocean
mesh boundary.

**Checking your own UGRID file.** The reader takes everything from the topology
variable, so start there:

```sh
ncdump -h yours.nc | grep -A8 'cf_role.*mesh_topology'
```

You need `node_coordinates` and `face_node_connectivity`. `face_dimension`,
`face_coordinates` and `start_index` are optional — unsview defaults
`start_index` to 0 per the convention and derives face centers from the corner
nodes when `face_coordinates` is absent.

## Cubed sphere (GEOS, FV3)

A cubed sphere is *not* an unstructured mesh: six logically-rectangular faces,
and no connectivity array anywhere in the file. unsview synthesises the quads by
index arithmetic from the cell corners — using the corner arrays when the file
has them, and reconstructing them from the cell centers when it does not. The
two families spell it differently:

| | GEOS | FV3 (`grid_spec.tileN.nc`) | FV3 (`oro_data.tileN.nc`) |
|---|---|---|---|
| corners | `corner_lons(nf, Y+1, X+1)` | `grid_lon(grid_y, grid_x)` | *none* |
| centers | `lons(nf, Y, X)` | `grid_lont(grid_yt, grid_xt)` | `geolon(lat, lon)` |
| faces | all six, `nf = 6` | one tile per file | one tile per file |

GEOS data variables span three dimensions — `(time, nf, Ydim, Xdim)` — rather
than one, which unsview handles transparently.

**Corners are often missing, and that is normal.** Operational GEOS (GEOS-IT)
keeps geometry in a separate gridspec file and ships cell *centers* only; FV3's
`oro_data`/`sfc` files carry `geolon`/`geolat` centers. unsview reconstructs the
cell boundaries in that case and says so:

```
cs: no corner coordinates in file; derived them from lons/lats centers
```

That message is informational, not a warning. Reconstruction is accurate to
O(h²) — measured against real corners on a deliberately coarse c12 grid (778 km
cells) the worst corner is 1.7% of a cell out, and it shrinks with resolution.
You can check it yourself on any file that carries both:

```sh
cc -O2 -I$(brew --prefix netcdf)/include -L$(brew --prefix netcdf)/lib \
   -o tests/check_corners tests/check_corners.c -lnetcdf -lm
./tests/check_corners samples/testdata/geos_c12.nc
```

**Fixtures** — the same mesh with and without corners:

```sh
unsview $S/synthetic_cs.nc          -v wave   # corners in the file
unsview $S/synthetic_cs_centers.nc  -v wave   # corners reconstructed
```

**Real GEOS data** (all six faces at once):

```sh
./tests/fetch_testdata.sh
unsview samples/testdata/geos_c12.nc -v PHIS -c terrain

# GEOS-IT c180 native: 194,400 cells, centers only
unsview samples/testdata/geos_it_c180_const.nc4 -v PHIS -c terrain
unsview samples/testdata/geos_it_c180_const.nc4 -v FRLAND
```

`FRLAND` is the sharpest check available: a land fraction must line up with the
bundled coastline overlay everywhere, poles included.

**Real FV3 data** from NOAA EMC's public fix files. `fetch_testdata.sh` pulls
C48 tile 1 and all six C192 tiles:

```sh
unsview samples/testdata/fv3_grid_spec.tile1.nc -v area
unsview samples/testdata/fv3_oro_data.tile1.nc  -v orog_filt -c terrain
```

The same server carries **C48, C96, C192, C384, C768, C1152 and C3072**, six
tiles each — swap the resolution in the URL and the filename. Resolution is
where this gets interesting: one C768 tile is 589,824 cells against C48's 2,304,
and the orography sharpens from a blur into the Atlas, Tibesti and Ethiopian
Highlands. Sizes stay manageable: a C192 `oro_data` tile is ~2.8 MB, C384
~11 MB, C768 ~45 MB.

```sh
B=https://www.ftp.emc.ncep.noaa.gov/static_files/public/UFS/GFS/fix/fix_fv3
curl -o C768_oro_data.tile1.nc $B/C768/C768_oro_data.tile1.nc
unsview C768_oro_data.tile1.nc -v orog_filt -c terrain
```

**A correct render** of `area` peaks at the face centre and shrinks toward the
corners — the gnomonic signature — inside a pincushion-shaped outline. `PHIS`
and `orog_filt` should resolve real mountain ranges with coastlines aligned.

**Three traps specific to FV3:**

- **`C48_grid.tileN.nc` is the supergrid**, at twice the model resolution
  (`nx = 96` for C48). Use `C48_grid_spec.tileN.nc` for model cells.
- **`oro_data` and `grid_spec` disagree about dimension names** — `lat`/`lon`
  with `geolon` centers versus `grid_yt`/`grid_xt` with `grid_lont`. unsview
  handles both. `--grid` is optional for `oro_data`, since its `geolon`/
  `geolat` centers are enough to reconstruct the cells.
- **`geolon`/`geolat` carry no `units` attribute.** unsview falls back to
  magnitude (radians cannot exceed 2π) and reports what it assumed. If a file
  ever renders as a smear of blocks across the whole globe, this is the reason
  to check first.

> Cells touching a pole are a special case: the pole is a single node, but in an
> equirectangular projection it is the whole top edge. unsview splits that corner
> so polar cells span the edge instead of collapsing to a triangle, which would
> otherwise leave ~4% of the map unpainted.

### Stitching the six tiles into a globe

FV3 puts each face in its own file. By default several files are read as steps
of a **time** series, so pass `--tiles` to concatenate them in space instead:

```sh
D=samples/testdata
unsview $D/fv3_C192_oro_data.tile?.nc --tiles -v orog_filt -c terrain
```

That prints `stitched 6 cubed-sphere tiles: 221184 cells` and draws a whole
globe. If your data files have no geometry of their own, give **one `--grid` per
tile, in the same order as the data files**:

```sh
unsview $D/fv3_C192_oro_data.tile1.nc ... \
        --grid $D/fv3_C192_grid_spec.tile1.nc ... --tiles -v orog_filt
```

A mesh/data cell-count mismatch is an error, not a silent misdraw. Without
`--tiles`, unsview points out that it is treating the files as time steps.

> **Why not automatic?** Six tile files and six single-step time files are
> indistinguishable from the outside, and guessing wrong would silently mislabel
> a time axis. GEOS needs none of this — its six faces are already in one file.

## FVCOM

FVCOM predates UGRID and carries no `cf_role`, so it has its own reader:
nodes in `lon`/`lat`, element centers in `lonc`/`latc`, and `nv(three, nele)`
transposed and 1-based. Most FVCOM fields (`zeta`, `temp`, `salinity`) sit on
**nodes** and are averaged onto elements before drawing — the variable list
marks them `(on nodes)`.

**Fixture:**

```sh
unsview $S/synthetic_fvcom.nc -v wave   # on elements
unsview $S/synthetic_fvcom.nc -v zeta   # on nodes
```

**Real data — no download needed.** unsview links a netCDF with DAP enabled, so
it opens OPeNDAP URLs directly. NOAA CO-OPS runs several FVCOM forecast systems
(LEOFS, LMHOFS, LOOFS, LSOFS, NGOFS2, SSCOFS). Their paths are date-stamped, so
find a current one rather than copying a URL:

```sh
B=https://opendap.co-ops.nos.noaa.gov/thredds
CAT=$B/catalog/NOAA/LEOFS/MODELS
Y=$(curl -s $CAT/catalog.html          | grep -oE 'href="[0-9]{4}/' | tail -1 | tr -dc 0-9)
M=$(curl -s $CAT/$Y/catalog.html       | grep -oE 'href="[0-9]{2}/' | tail -1 | tr -dc 0-9)
D=$(curl -s $CAT/$Y/$M/catalog.html    | grep -oE 'href="[0-9]{2}/' | tail -1 | tr -dc 0-9)
F=$(curl -s $CAT/$Y/$M/$D/catalog.html | grep -oE '<code>[^<]*fields[^<]*\.nc' | head -1 | sed 's/<code>//')

unsview "$B/dodsC/NOAA/LEOFS/MODELS/$Y/$M/$D/$F" -v h -c terrain
```

**A correct render** of LEOFS `h` (bathymetry) is unmistakably **Lake Erie**:
a shallow western basin, a deep eastern basin reaching about 62 m, the Bass
Islands showing as holes in the mesh, and Long Point on the north shore. Try
`-v zeta` for water level.

Local FVCOM files work the same way, with no `--grid` needed — the mesh travels
with the data.

## Everything at once

```sh
./tests/run_all_readers.sh          # six fixtures, no network
./tests/fetch_testdata.sh           # ~71 MB of real model output
./tests/fetch_testdata.sh --check   # render every one, report pass/fail
open /tmp/uv_*.png                  # macOS; xdg-open on Linux
```

`./conda_test.sh` goes further: it builds the conda package, installs it into a
scratch environment and runs the same assertions against the *packaged* binary.

## What a wrong render looks like

The renderer fails loudly in a few recognisable ways.

| Symptom | Usual cause |
|---|---|
| Polygons stretched across the whole map | Face centers wrong, so date-line unwrapping picks the wrong reference |
| Wedges of blank map at the top and bottom | A mesh with nodes exactly on a pole that were not split along the top edge |
| Everything one flat colour | Fill value not recognised — check `_FillValue` / `missing_value` |
| Coastlines offset from the data | Degrees read as radians; check the `units` attribute on the coordinates |
| A smear of huge blocks over the whole globe | Degrees read as radians, but because the coordinate variable has **no** `units` attribute at all. unsview normally catches this and says so |
| Only a sixth of the globe, and N tile files became N time steps | Cubed-sphere tiles passed without `--tiles` |
| Mesh metadata in the variable list | The reader did not recognise those variables as mesh variables |

## When the GUI will not open

The conda package carries the GUI on Linux and macOS alike. On macOS it still
needs **XQuartz** running: conda ships the X11 *client* libraries, and macOS has
no built-in X *server*.

```sh
brew install --cask xquartz  # once; log out and back in so $DISPLAY is set
open -a XQuartz              # macOS, once per login
```

The interactive parts are worth exercising by hand, because no script covers
any of them: left-click a cell (opens a vertical profile and/or time series),
drag to pan, scroll to zoom, and the `cmap` / `coast` / `borders` / `states` /
`reset view` / `set range...` buttons.

Two failures look alike and have opposite fixes:

| Message | Meaning | Fix |
|---|---|---|
| `GUI not compiled in` | the build has no Xaw | use the conda package, or rebuild and check for `X11 GUI: enabled` |
| `Error: Can't open display:` | GUI is present, no X server reachable | start XQuartz, or `ssh -Y` instead of `ssh` |

To tell which one you have without needing a display at all:

```sh
DISPLAY= unsview $S/synthetic.nc -v wave
```

`Can't open display` here is the *good* answer — it proves the GUI is compiled
in and only the server was missing.

## Troubleshooting

**"no unstructured mesh dimension found"** — the file has no mesh unsview
recognises. Either it is data-only (use `--grid`), or the convention is not
supported. `ncdump -h yours.nc | head -40` will show which.

**Forcing a reader for a better error.** Auto-detection reports only that
nothing matched; forcing names what was missing:

```sh
unsview --model icon yours.nc -o /dev/null
# icon: need 1-D variables clon (cells) and vlon (vertices)
```

**Wrong variable picked.** With no `-v`, unsview renders the first plottable
variable alphabetically. Pass `-v` explicitly; the full list is printed on every
run.

**"cs: no corner coordinates in file; derived them from ..."** — informational,
not a warning. The file gave cell centers without cell boundaries (normal for
operational GEOS and for FV3 `oro_data`), so unsview reconstructed them. If you
would rather feed it exact corners, point `--grid` at the matching gridspec or
`grid_spec.tileN.nc`.

**"--tiles: the mesh has N cells but the M data files supply K"** — you gave
fewer `--grid` files than data files. Give one per tile, in the same order, or
drop `--grid` entirely when the data files carry their own coordinates.

**"note: N cubed-sphere files given without --tiles"** — unsview is reading your
tiles as a time series. Add `--tiles`. It cannot decide this for you: N tile
files and N single-step time files look identical from outside.

---

## Building from source

Only needed if you are not using conda, or you are changing the code. unsview is
plain C99 with three dependencies:

- C99 compiler (gcc / clang)
- `libnetcdf`
- `libpng`
- `libX11`, `libXt`, `libXaw` — for the GUI

All of these are present on every HPC login node and on macOS via Homebrew
(`netcdf`, `libpng`) + XQuartz (X11/Xt/Xaw).

```sh
# macOS
brew install netcdf libpng
brew install --cask xquartz          # the GUI needs this
make
make install DEST=$HOME/.local       # binary, overlays and fixtures

# Debian / Ubuntu, x86_64 or arm64 (incl. ChromeOS Crostini)
sudo apt install libnetcdf-dev libpng-dev libxaw7-dev
make
make install DEST=$HOME/.local

# Linux / HPC, user-space
module load netcdf gcc               # adjust to your site
make NETCDF_PREFIX=$NETCDF
make install DEST=$HOME/.local
```

**The GUI is the default and the build tells you if it did not get one.** `make`
finishes with `X11 GUI: enabled` or `disabled`; when it is disabled it also
prints how to fix it, because a headless binary installs and renders PNGs
perfectly well and would otherwise go unnoticed until someone omits `-o`.

Detection looks for `libXaw` in the usual prefixes plus the Debian/Ubuntu
multiarch directories for both `x86_64` and `aarch64`. On any other layout it
finds nothing and quietly drops the GUI, so force it and let a missing Xaw be a
build error instead:

```sh
make HAVE_X11=1
```

`make install` puts the fixtures under `DEST/share/unsview/samples`, so `$S`
works the same way it does under conda:

```sh
S="$HOME/.local/share/unsview/samples"
```

HPC sites often split the X11 libraries across separate trees; `./install.sh
--help` lists a prefix flag per library, and `INSTALL_HPC.md` covers the rest.
`tests/` and its scripts are source-checkout only — they are not part of any
package.

## Layout

```
src/
  main.c        # arg parse, .unsviewrc, mode dispatch
  nc_io.{c,h}   # netCDF wrapper + variable enumeration
  mesh.{c,h}    # MPAS / ICON / UGRID / cubed-sphere / FVCOM readers -> one UnsMesh
  raster.{c,h}  # polygon scanline fill, date-line wrap, text, colorbar
  colormap.{c,h}# 18 palette LUTs
  polylines.{c,h}# coast/border/state overlay loader
  gui_x11.c     # Xt/Xaw GUI (compiled in iff X11 headers detected)
  png_export.c  # libpng output for headless mode
tests/
  make_sample.c      # fixture generator (mpas|icon|ugrid|fvcom|cs|cs_centers)
  run_all_readers.sh # render every fixture, pass/fail per reader
  check_corners.c    # measure corner reconstruction against real corners
  strip_mesh.c       # strips mesh vars, to simulate stream-split output
  fetch_testdata.sh  # download real model files, then --check renders them
recipe/         # conda package recipe
```

## For maintainers: what the fixtures prove

Four of the six fixtures are the *same mesh and the same field* in four
encodings, so they should render the same picture. They fall into two groups:

- `synthetic.nc` (MPAS) and `synthetic_icon.nc` render **byte-identical**. Both
  store radians, so nothing is converted.
- `synthetic_ugrid.nc` and `synthetic_fvcom.nc` render **byte-identical to each
  other**, and differ from the pair above by about 0.05% of pixels — purely
  because their coordinates round-trip through degrees.
- `synthetic_cs.nc` is necessarily a different mesh (a real gnomonic cubed
  sphere) carrying the same field function, so compare it by eye.
- `synthetic_cs_centers.nc` is that same cubed sphere written **without corner
  coordinates**, the way operational GEOS ships. It exercises the
  reconstruction path, and differs from `synthetic_cs.nc` by ~0.6% of pixels at
  c12 — that gap *is* the reconstruction error, and it shrinks as cells do
  (~0.02% at C192). `tests/check_corners.c` measures it in kilometres instead
  of pixels.

A larger divergence than that is a reader bug, not floating-point noise.

## Out of scope

- WRF, ROMS, NEMO, MOM6 and any other rectilinear or 2-D curvilinear structured
  grid — `ncview` already serves those well.
- FESOM's ASCII mesh format (`nod2d.out` / `elem2d.out`). Its netCDF
  `mesh.diag` output is UGRID and does work.
- Per-vertex (Gouraud) shading — node data is averaged onto faces instead.
- 3D / vertical-section views.
- Map projections beyond equirectangular.
- Vector overlays, contours, streamlines.

---

## Command summary

Everything above, in one place. Each command opens the **GUI**; append
`-o out.png` to any of them to write a file and exit instead.

```sh
S="$CONDA_PREFIX/share/unsview/samples"     # ships with the package
```

### Bundled fixtures — no network, no checkout

| Convention | Command |
|---|---|
| MPAS | `unsview $S/synthetic.nc -v wave` |
| ICON | `unsview $S/synthetic_icon.nc -v wave` |
| CF-UGRID | `unsview $S/synthetic_ugrid.nc -v wave` |
| CF-UGRID, on nodes | `unsview $S/synthetic_ugrid.nc -v wave_node` |
| Cubed sphere | `unsview $S/synthetic_cs.nc -v wave` |
| Cubed sphere, centers only | `unsview $S/synthetic_cs_centers.nc -v wave` |
| FVCOM | `unsview $S/synthetic_fvcom.nc -v wave` |
| FVCOM, on nodes | `unsview $S/synthetic_fvcom.nc -v zeta` |

### Real model output — needs a checkout, then `./tests/fetch_testdata.sh` (~71 MB)

| What | Command |
|---|---|
| MPAS terrain | `unsview samples/testdata/x1.10242.static.nc -v ter -c terrain` |
| MPAS stream-split | `unsview samples/testdata/x1.10242.static.nc --grid samples/testdata/x1.10242.static.nc -v ter -c terrain` |
| ICON bathymetry | `unsview samples/testdata/icon_R02B04_ocean.nc -v cell_elevation -c terrain` |
| FESOM SST (UGRID, on nodes) | `unsview samples/testdata/fesom_sst.nc --grid samples/testdata/fesom_mesh_diag.nc -v sst` |
| GEOS c12, all six faces | `unsview samples/testdata/geos_c12.nc -v PHIS -c terrain` |
| GEOS-IT c180, centers only | `unsview samples/testdata/geos_it_c180_const.nc4 -v PHIS -c terrain` |
| GEOS-IT c180 land mask | `unsview samples/testdata/geos_it_c180_const.nc4 -v FRLAND` |
| FV3 C48 grid | `unsview samples/testdata/fv3_grid_spec.tile1.nc -v area` |
| FV3 C48 orography | `unsview samples/testdata/fv3_oro_data.tile1.nc -v orog_filt -c terrain` |
| FV3 C192, six tiles → globe | `unsview samples/testdata/fv3_C192_oro_data.tile?.nc --tiles -v orog_filt -c terrain` |
| FVCOM live over OPeNDAP | date-stamped URL — see [FVCOM](#fvcom) |

### Suites and tools — source checkout only

| What | Command |
|---|---|
| All six fixtures, pass/fail per reader | `./tests/run_all_readers.sh` |
| Download real model output | `./tests/fetch_testdata.sh` |
| Render every real file, pass/fail | `./tests/fetch_testdata.sh --check` |
| Build the conda package and test it | `./conda_test.sh` |
| Measure corner reconstruction error | `./tests/check_corners samples/testdata/geos_c12.nc` |
| Regenerate a fixture | `./tests/make_sample OUT.nc [mpas\|icon\|ugrid\|fvcom\|cs\|cs_centers]` |
| Is the GUI compiled in? | `DISPLAY= unsview $S/synthetic.nc -v wave` |
| Force a reader for a clearer error | `unsview --model icon yours.nc -o /dev/null` |

The C helpers in `tests/` are not built by `make`; compile whichever you need:

```sh
cc -O2 -I$(brew --prefix netcdf)/include -L$(brew --prefix netcdf)/lib \
   -o tests/check_corners tests/check_corners.c -lnetcdf -lm
#     ^ or make_sample / strip_mesh, same line otherwise
```

## License

MIT — see [`LICENSE`](LICENSE).

The bundled coastline, border, and state overlays in `samples/*.txt` are derived
from [Natural Earth](https://www.naturalearthdata.com/), which is public domain
and not covered by the MIT license above.
