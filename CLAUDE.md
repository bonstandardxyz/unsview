# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`unsview` — an `ncview`-class viewer for **unstructured meshes** (MPAS, ICON,
CF-UGRID, cubed sphere, FVCOM), written in plain C99. It rasterizes mesh polygons itself (no remap step,
no external graphics toolkit beyond X11/Xaw for the optional GUI, libpng for output).
Two modes from one binary: an X11/Xaw GUI, and a headless `-o out.png` mode for HPC
compute nodes.

Formerly `mpasview`; renamed before the first public release, so no released
artifact ever carried the old name.

There is no test framework — verification is smoke-test based (see Testing).

## Build / install

```sh
make                      # -> ./unsview ; prints "X11 GUI: enabled|disabled"
make clean
make install DEST=$HOME/.local     # binary -> DEST/bin, overlays -> DEST/share/unsview
./install.sh --prefix=$HOME/.local # one-shot: detects netcdf/png/X11, builds, installs
./install.sh --help                # full flag list (per-library X prefixes, spack, etc.)
```

The Makefile auto-detects X11 by probing for `X11/Xaw/Form.h` + `libXaw.*`. If absent,
`gui_x11.c` is dropped from `OBJS` and the binary is headless-only. Force with
`HAVE_X11=1`. Each X piece (Xaw/Xt/X11/Xmu/Xext/SM/ICE/xorgproto) accepts its own
`*_PREFIX` var because HPC sites split them across trees — see `INSTALL_HPC.md`.

`DATADIR` is baked into the binary as `-DUNSVIEW_DATA_DIR`; it is where `make install`
puts the bundled overlay `.txt` files.

Conda packaging lives in `recipe/` and builds from the working tree (`source: path: ..`).
Both platforms build the GUI: conda-forge ships osx-64/osx-arm64 `xorg-libxaw` as
well as linux, and `recipe/build.sh` passes `HAVE_X11=1` so a missing Xaw is a build
error rather than a silently headless package. Those are X11 *client* libraries, so
running the GUI on macOS still needs XQuartz for the server.

```sh
conda build recipe -c conda-forge
./conda_test.sh           # build + fresh env install + 5 smoke assertions, prints PASS
```

## Testing

`tests/` holds two standalone C generators plus a download script, not a suite.
Compile the generators by hand:

```sh
cc -O2 -I$(brew --prefix netcdf)/include -L$(brew --prefix netcdf)/lib \
   -o tests/make_sample tests/make_sample.c -lnetcdf -lm
./tests/make_sample samples/synthetic.nc              # MPAS   (default)
./tests/make_sample samples/synthetic_icon.nc  icon   # ICON
./tests/make_sample samples/synthetic_ugrid.nc ugrid  # CF-UGRID + node var
./tests/make_sample samples/synthetic_fvcom.nc fvcom  # FVCOM
./tests/make_sample samples/synthetic_cs.nc    cs     # cubed sphere
./tests/make_sample samples/synthetic_cs_centers.nc cs_centers  # same, no corners
./tests/strip_mesh in.nc out.nc                       # drops mesh vars -> --grid
```

**Four of the six fixtures are the same mesh and the same field in four
encodings**, and split into two groups by coordinate unit: MPAS and ICON render
byte-identical (both radians), UGRID and FVCOM render byte-identical to each
other (both degrees), and the two groups differ by ~0.05% of pixels from the
degrees round-trip. Treat a larger divergence as a reader bug; that is what
`tests/run_all_readers.sh` asserts. The two `synthetic_cs*.nc` are necessarily a
different mesh (a real gnomonic cubed sphere) carrying the same field function;
they are the same mesh as each other, written once with `corner_lons` and once
with centers only, so their renders differ by ~0.6% of pixels at c12 — that is
the reconstruction error, and it shrinks with cell size.

`tests/check_corners.c` measures that reconstruction directly against a file
carrying both centers and corners (compile it the same way as `make_sample`; it
`#include`s `src/mesh.c` to reach the static helper).

`README.md` is the single user-facing document: it merges the reference material
(CLI surface, `.unsviewrc`, overlays) with the per-convention testing guide that
used to live in a separate `TESTING.md`. Keep them in one file -- the install
instructions drifted apart when they were two.

Fastest single check that rendering still works end to end:

```sh
./unsview --no-rc samples/synthetic.nc -v wave -t 0 -o wave.png
```

`tests/run_all_readers.sh` also renders one real file per reader from
`samples/testdata/` when that directory has been populated, and skips those lines
cleanly when it has not -- so it stays the no-network check while still covering
files real models wrote. It asserts detected model, which `--check` does not.

None of the scripts write to `/tmp`: they use `$TMPDIR` when the site sets one and
otherwise the checkout, because `/tmp` is not writable on many HPC nodes.

`tests/fetch_testdata.sh` downloads real MPAS/ICON/UGRID/FESOM files into the
gitignored `samples/testdata/`; `--check` renders each. Run it after touching
`src/mesh.c` — the synthetic fixtures are too well-behaved to catch things like
int64 connectivity or transposed arrays.

`conda_test.sh` is the closest thing to a regression suite: it asserts `-h` runs, the
overlay data installed, a headless PNG renders non-empty, overlays-on and
`--no-coast-data` produce *different* PNG sizes, the time-varying sample renders,
all three readers load, and a node-centered variable renders. It passes `--no-rc`
everywhere so a stray `~/.unsviewrc` cannot change the byte sizes it compares.
GUI behavior (click-to-plot, pan/zoom) is not covered by any script.

**Known fixture wart:** `make_sample.c` averages the three corner longitudes
without unwrapping, so cells in the wrap-around column get a bogus center and the
rightmost column of `samples/synthetic*.nc` renders the wrong colour. It is a
generator bug, not a renderer bug — real ICON/FESOM/geoflow files render a clean
seam. Fixing it means unwrapping in `make_sample.c` the way
`face_centers_from_nodes` does in `mesh.c`, and would change the fixture bytes.

## Architecture

Data flows one way: `main.c` merges `.unsviewrc` with argv and parses → `nc_io`
opens files and enumerates plottable variables → `mesh.c` loads the mesh →
`raster.c` paints an RGB buffer → either `png_export.c` writes it or `gui_x11.c`
blits it into an `XImage`.

- **`nc_io.{c,h}`** — `NcFile` per netCDF file; `MultiNc` concatenates N files along
  the time axis (`t_offsets`, `multinc_resolve`). Cross-file lookup is **by variable
  name**, not index, because var ids differ between files; a variable missing from a
  later file yields a NaN slab rather than an error.
- **`mesh.{c,h}`** — five readers (MPAS, ICON, CF-UGRID, cubed sphere, FVCOM) all filling one `UnsMesh`,
  which is the UGRID face/node model: face centers, node coordinates, and
  `vertices_on_cell` + `n_edges_on_cell`. Also caches the face-center lon/lat extent
  used for regional auto-fit, and holds `mesh_node_to_face`.
- **`raster.{c,h}`** — the whole renderer: scanline polygon fill, Bresenham lines,
  a built-in 5×7 bitmap font, and the colorbar. No graphics library involved.
- **`colormap.{c,h}`** — 18 palettes built by interpolating control-point quads.
- **`polylines.{c,h}`** — coast/border/state overlay loader plus the `PolyLayer` type.
- **`gui_x11.c`** — Xt/Xaw GUI, ~1100 lines, one file, one global `App g_app`.
  Compiled only under `UNSVIEW_HAVE_X11`.

### Invariants worth knowing before editing

- **Radians everywhere internally.** Polyline files are degrees on disk and converted
  at load; the GUI converts back to degrees only for display strings. UGRID meshes are
  degrees too — `read_coord_var` in `mesh.c` converts when the `units` attribute starts
  with `degree`, which is the single place that invariant can be broken. When a
  coordinate variable has **no** `units` attribute at all (FV3's `oro`/`sfc` files omit
  it on `geolon`/`geolat`), it falls back to magnitude: radians cannot exceed 2π, so
  anything larger is degrees. That sniff runs only on the missing-attribute path, so
  files that declare units behave exactly as before.
- **Connectivity is 0-based inside `UnsMesh`, with `-1` for unused slots.** Each reader
  normalizes: MPAS and ICON subtract 1, UGRID subtracts its `start_index` (which
  *defaults to 0*, unlike the other two). Don't re-subtract downstream.
- **Connectivity is read through `nc_get_var_longlong`, never `nc_get_var_int`.**
  Real files use int, uint (`_FillValue` 4294967295) and int64 (`_FillValue`
  INT64_MIN); the int reader returns `NC_ERANGE` on the latter two.
- **ICON's `vertex_of_cell` is `(nv, cell)`** — transposed relative to MPAS's
  `verticesOnCell(nCells, maxEdges)`. `load_icon` picks the face axis by matching a
  dimension length to the cell count rather than trusting position.
- **Model detection order is UGRID → MPAS → ICON → cs → fvcom** (`detect_model`). UGRID
  first because it self-describes via `cf_role="mesh_topology"`; the others are
  name heuristics. `--model` skips detection.
- **A cubed sphere has no connectivity array.** `load_cubedsphere` synthesises
  quads from the corner array by index arithmetic, flattening cells as
  (face, y, x). `nc_read_slab` reads the face-dim block whole and in file order
  so the two orderings agree — change one and you must change both.
- **Cubed-sphere corners are often absent and then reconstructed.** Operational
  GEOS (GEOS-IT) keeps geometry in a separate gridspec file and ships `lons`/`lats`
  centers only; FV3 `oro`/`sfc` ship `geolon`/`geolat`. `load_cubedsphere` picks a
  geometry source in order — `corner_lons`, `grid_lon`, else derive from centers via
  `corners_from_centers`. GCHP *history* output does carry corners, which is why
  `samples/testdata/geos_c12.nc` has both and `geos_it_c180_const.nc4` has neither.
- **`corners_from_centers` works in 3-D Cartesian, deliberately.** Averaging
  longitudes breaks at the date line and degenerates at the poles — the very thing
  `face_centers_from_nodes` unwraps to avoid. Centers are extended by one
  linearly-extrapolated ghost ring so edge corners don't collapse onto edge centers,
  then each corner is the normalized mean of its four neighbours. Error is O(h²):
  measured against real corners at c12 (the coarsest case, 778 km cells) the worst
  corner is 1.7% of a cell out; `tests/check_corners.c` is that measurement.
  Faces extrapolate independently, so a shared face edge can disagree by O(h²) —
  the reason a stitched globe is not exactly watertight.
- **GEOS and FV3 are the same geometry, spelled differently.** GEOS:
  `corner_lons/lons`, 3-D with a leading `nf`. FV3: `grid_lon/grid_lont`, 2-D,
  one file per tile (so `nf = 1` and a render covers a sixth of the globe).
  FV3's `oro`/`sfc` files name their centers `geolon` and dimension them
  `lat`/`lon`, which is why `mesh_probe_dims` tries three center-variable names
  and why a failed `--grid` dimension hint falls back to the file's own probe
  instead of being fatal. `C48_grid.tileN.nc` is the supergrid (2x model
  resolution); `C48_grid_spec.tileN.nc` is the one with model cells.
- **The face index can span several dimensions.** `NcFile.ncells_dim_ids` is a
  list, `ncells` their product, and a flat cell index is the C-order flattening
  across them. `mesh_probe_dims` reports them space-separated for this reason.
  Everything else in the codebase still sees one flat cell index.
- **Polar corners are split** (`load_cubedsphere`). A node exactly on a pole is a
  point, but equirectangular renders a pole as the whole top edge, so an
  unsplit polar cell collapses to a triangle and leaves ~4% of the map unpainted.
  Each polar corner becomes two nodes carrying its ring-neighbours' longitudes,
  which is why `max_edges` is 5 for a quad mesh. The `POLE_LAT` test is a
  **tolerance (1e-6 rad), not equality** — a corner read from a file sits exactly on
  the pole, but a reconstructed one only gets within ~1e-8 rad, and missing the test
  silently reinstates the wedge. 1e-6 rad is ~6 m, far below even a c3072 cell.
- **`--tiles` is explicit, never inferred.** A cubed sphere arrives either as one
  file with a leading `nf` (GEOS) or as six tile files (FV3). Six tile files are
  indistinguishable from six single-step time files, so guessing would silently
  mislabel a time axis; `main.c` prints a hint instead when >1 cs file is passed
  without it. Under `--tiles`, `MultiNc.tile_mode` makes files concatenate along the
  **cell** axis rather than time, `--grid` accepts one file per tile, and
  `mesh_load_tiles` rebases vertex ids per tile. Global cell index is tile-major then
  (y, x) — the same order `multinc_read_slab` assembles data in.
- **Face centers derived from nodes must unwrap first** (`face_centers_from_nodes`).
  A naive mean puts a date-line-straddling face near lon 0, and `raster_render_mesh`
  uses the face center as its unwrap reference, so the polygon tears across the map.
- **Date-line handling** (`raster_render_mesh`): each cell's vertices are unwrapped to
  within ±π of its own cell center, then the polygon is drawn three times at
  `shift ∈ {-1,0,+1} × 2π` with a bbox reject. That is what makes wrap-around and
  Pacific-centered regional meshes work; keep the triple draw if you touch it.
- **`Raster` carries an `alpha` plane** (0/1 painted flag), used so PNG export and the
  X11 blit can paint an explicit background under unpainted pixels.
- **Two different extra-dim indexing strategies, on purpose.** `nc_read_slab` maps
  non-`nCells` dims *positionally* (1st extra ← time, 2nd ← level) so `(nCells,nMonths)`
  and `(Time,nVertLevels,nCells)` both work. `multinc_read_cell_value` maps *by dim id*
  instead, because the click-to-plot popup must not misassign time vs level on
  level-only or time-only variables. Both are commented in `nc_io.c`.
- **Variable picker = numeric vars carrying the face *or* node dim, minus two filters,
  sorted A–Z.** The static `MESH_VARS` blacklist in `nc_io.c` covers MPAS; everything
  else is filtered *after the fact* by `multinc_hide_mesh_vars` using the names the
  reader reports in `UnsMesh.mesh_vars`. That indirection exists because UGRID files
  choose their own mesh variable names, so no static list can work, and because
  `nc_open_file` runs before any mesh is loaded. `tests/strip_mesh.c` keeps its own
  longer list for a different purpose; they are deliberately not shared.
- **A node dim is only reported for node-centered formats** (`mesh_probe_dims`).
  MPAS's `nVertices` and ICON's `vertex` are deliberately *not* reported: those models
  put fields on cells only, so surfacing their vertex dim would just fill the picker
  with mesh metadata.
- **`--grid` is opened before the data files** so its mesh dimension names can be
  passed to `nc_open_file` as hints. Stream-split UGRID data files carry no topology
  variable, so only the grid file knows what the mesh dims are called.
- **`.unsviewrc` is expanded into argv and parsed by the ordinary option loop**
  (`RC_KEYS` in `main.c`), before the real arguments. Nothing about option semantics is
  duplicated, and the command line wins by being parsed second. `--no-rc` is detected
  in a separate pre-scan since it must act before the file is read.
- **`gui_run`'s prototype is declared inline in `main.c`**, not in a header. Changing
  its signature means editing both files.
- **Overlay resolution order** (`resolve_data_file` in `main.c`): `$UNSVIEW_DATA_DIR` →
  compile-time `UNSVIEW_DATA_DIR` → `./samples`. The bundled coast/borders/states trio is
  loaded **by default**; an explicit `--coast/--borders/--states` suppresses the
  bundled layer *of that same type*, `--lines` does not suppress anything, and
  `--no-coast-data` disables all three.
- Fixed caps: `MAX_POLY 32` vertices per cell (raster), `MAX_LAYERS 16`, `MAX_FILES 64`,
  `MAX_RC_ARGS 64` (main), `MAX_PALETTES 24` (colormap), `MESH_MAX_MESHVARS 32` (mesh).

### Adding a mesh reader

Write `load_<model>(int ncid, UnsMesh *m)` in `mesh.c` filling the struct, add a
branch to `detect_model` and to the `mesh_load` dispatch, and call `record_mesh_var`
for every variable you consumed so the picker hides it. If the model needs its own
dimension names, extend `mesh_probe_dims`. Then add an encoding to
`tests/make_sample.c` so CI covers it, and a `--model` line to `usage()` in `main.c`.
Nothing in `raster.c` or `gui_x11.c` should need to change.

### Adding a colormap

Add one `add_lut("name", ctrl_points, n_points)` call in `colormap_init()`. The GUI's
cmap cycle picks it up automatically via `colormap_count()`/`colormap_name_at()`; the
only manual step is updating the `-c` list in `usage()` in `main.c`.

## Odds and ends

- `usage()` in `src/main.c` is the authoritative CLI surface; `README.md` currently
  matches it. Both list 18 colormaps, which is right — but only 16 come from
  `add_lut` calls, so grepping for those undercounts. `gray` and `3gauss` are
  registered by their own helpers.
- **`colormap_by_name` silently falls back to viridis for an unrecognised name**, so
  a typo in `-c` is not an error, just an unexpected picture.
- A stale `./visualize` binary and `visualize_*.png` sit in the repo root from before
  the rename. Both are gitignored, so they never reach a published tree; delete at
  will.
- `INSTALL_HPC.md` is current.
