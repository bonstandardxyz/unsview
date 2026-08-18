#!/usr/bin/env bash
# Download real model output for exercising unsview's readers.
#
# These are NOT committed and NOT part of the conda test: they are ~71 MB of
# third-party data, and CI already covers all five readers with the generated
# samples/synthetic*.nc set. Use this when you touch src/mesh.c and want to
# check the readers against files real models actually wrote.
#
#   ./tests/fetch_testdata.sh          # download into samples/testdata/
#   ./tests/fetch_testdata.sh --check  # render each one, report pass/fail
#
# Each supported convention gets one file carrying genuine geophysical fields,
# not just mesh metadata -- those are the ones worth looking at:
#
#   MPAS   x1.10242.static.nc        ter, landmask, greenfrac, soiltemp
#   ICON   icon_R02B04_ocean.nc      cell_elevation, cell_sea_land_mask
#   UGRID  fesom_sst.nc              sea surface temperature (node-centered)
#   CS     geos_c12.nc               PHIS surface geopotential, T air temperature
#   CS     fv3_oro_data.tile1.nc     orog_filt, land_frac (FV3 C48, one tile)
#   CS     geos_it_c180_const.nc4    PHIS, FRLAND, FROCEAN (GEOS-IT c180 native)
#   CS     fv3_C192_*.tile1..6.nc    orog_filt over all six tiles, for --tiles
#
# The GEOS-IT file is the one that matters for the centers-only path: NASA ships
# it with lons/lats and *no* corner coordinates, so unsview has to reconstruct
# the cell boundaries. geos_c12.nc has both, which is what makes it usable as
# ground truth by tests/check_corners.c.
#
# Note on ICON: DWD's operational ICON open data is GRIB2 only, which unsview
# cannot read. The MPI-M *ocean* grid is used here instead because, unlike the
# atmosphere grids, it ships real bathymetry and a land mask.
#
# Sources: uxarray's test corpus (Apache-2.0, github.com/UXARRAY/uxarray),
# the MPI-M ICON grid archive, and the UCAR/MMM MPAS mesh downloads.

set -u
cd "$(dirname "$0")/.."
DIR=samples/testdata
UX=https://raw.githubusercontent.com/UXARRAY/uxarray/main/test/meshfiles
ICON=http://icon-downloads.mpimet.mpg.de/grids/public/mpim
MMM=https://www2.mmm.ucar.edu/projects/mpas/atmosphere_meshes
EMC=https://www.ftp.emc.ncep.noaa.gov/static_files/public/UFS/GFS/fix/fix_fv3/C48
EMC192=https://www.ftp.emc.ncep.noaa.gov/static_files/public/UFS/GFS/fix/fix_fv3/C192
GEOSIT=https://gmao.gsfc.nasa.gov/gmaoftp/ops/GEOSIT_sample/data_products/asm_const_0hr_glo_C180x180x6_slv/Y2018/M01

# local name | url
FILES="
icon_R02B04_ocean.nc|$ICON/0036/icon_grid_0036_R02B04_O.nc
icon_R02B04.nc|$UX/icon/R02B04/icon_grid_0010_R02B04_G.nc
geos_c12.nc|$UX/geos-cs/c12/test-c12.native.nc4
fv3_grid_spec.tile1.nc|$EMC/C48_grid_spec.tile1.nc
fv3_oro_data.tile1.nc|$EMC/C48_oro_data.tile1.nc
geos_it_c180_const.nc4|$GEOSIT/GEOS.it.asm.asm_const_0hr_glo_C180x180x6_slv.GEOS5271.2018-01-01T0000.V01.nc4
fv3_C192_oro_data.tile1.nc|$EMC192/C192_oro_data.tile1.nc
fv3_C192_oro_data.tile2.nc|$EMC192/C192_oro_data.tile2.nc
fv3_C192_oro_data.tile3.nc|$EMC192/C192_oro_data.tile3.nc
fv3_C192_oro_data.tile4.nc|$EMC192/C192_oro_data.tile4.nc
fv3_C192_oro_data.tile5.nc|$EMC192/C192_oro_data.tile5.nc
fv3_C192_oro_data.tile6.nc|$EMC192/C192_oro_data.tile6.nc
mpas_QU1920.nc|$UX/mpas/QU/mesh.QU.1920km.151026.nc
ugrid_quadhex_grid.nc|$UX/ugrid/quad-hexagon/grid.nc
ugrid_quadhex_face.nc|$UX/ugrid/quad-hexagon/random-face-data.nc
ugrid_quadhex_node.nc|$UX/ugrid/quad-hexagon/random-node-data.nc
ugrid_geoflow_grid.nc|$UX/ugrid/geoflow-small/grid.nc
ugrid_geoflow_v1.nc|$UX/ugrid/geoflow-small/v1.nc
fesom_mesh_diag.nc|$UX/ugrid/fesom/fesom.mesh.diag.nc
fesom_sst.nc|$UX/ugrid/fesom/sst.fesom.1948.nc
"

if [ "${1:-}" != "--check" ]; then
    mkdir -p "$DIR"
    for entry in $FILES; do
        out="$DIR/${entry%%|*}"
        url="${entry#*|}"
        if [ -s "$out" ]; then
            echo "have    $out"
            continue
        fi
        echo "fetch   $out"
        curl -sSL --fail --max-time 600 -o "$out" "$url" \
            || { echo "  FAILED: $url" >&2; rm -f "$out"; }
    done

    # MPAS ships its realistic mesh as a tarball, so it needs unpacking.
    if [ -s "$DIR/x1.10242.static.nc" ]; then
        echo "have    $DIR/x1.10242.static.nc"
    else
        echo "fetch   $DIR/x1.10242.static.nc (via tarball)"
        if curl -sSL --fail --max-time 600 -o "$DIR/x1.10242_static.tar.gz" \
               "$MMM/x1.10242_static.tar.gz"; then
            tar xzf "$DIR/x1.10242_static.tar.gz" -C "$DIR" \
                && rm -f "$DIR/x1.10242_static.tar.gz"
        else
            echo "  FAILED: $MMM/x1.10242_static.tar.gz" >&2
        fi
    fi

    echo
    echo "Downloaded to $DIR/ (gitignored). Now run:"
    echo "    $0 --check"
    exit 0
fi

# ---- --check: render each and report -----------------------------------------
[ -x ./unsview ] || { echo "build first: make" >&2; exit 1; }
fails=0 skips=0
try() {
    desc=$1; file=$2; shift 2
    if [ ! -s "$file" ]; then
        printf '  skip %s (not downloaded)\n' "$desc"; skips=$((skips + 1)); return
    fi
    if ./unsview --no-rc "$file" "$@" >/dev/null 2>&1; then
        printf '  ok   %s\n' "$desc"
    else
        printf '  FAIL %s\n' "$desc"; fails=$((fails + 1))
    fi
}

echo "real geophysical fields:"
try "mpas   ter, 10242 cells"        $DIR/x1.10242.static.nc -v ter -c terrain -o /tmp/uv_mpas_ter.png
try "icon   bathymetry + land mask"  $DIR/icon_R02B04_ocean.nc -v cell_elevation -c terrain -o /tmp/uv_icon_elev.png
try "ugrid  fesom sst (node data)"   $DIR/fesom_sst.nc --grid $DIR/fesom_mesh_diag.nc -v sst -o /tmp/uv_fesom.png
try "cs     fv3 orography (tile 1)" $DIR/fv3_oro_data.tile1.nc --grid $DIR/fv3_grid_spec.tile1.nc -v orog_filt -c terrain -o /tmp/uv_fv3_oro.png

echo "mesh-only / structural cases:"
try "mpas   QU1920 hexagons"         $DIR/mpas_QU1920.nc -o /tmp/uv_mpas.png
try "icon   R02B04, 20480 triangles" $DIR/icon_R02B04.nc -v cell_area -o /tmp/uv_icon.png
try "ugrid  quad-hexagon, face data" $DIR/ugrid_quadhex_face.nc --grid $DIR/ugrid_quadhex_grid.nc -o /tmp/uv_qf.png
try "ugrid  quad-hexagon, node data" $DIR/ugrid_quadhex_node.nc --grid $DIR/ugrid_quadhex_grid.nc -o /tmp/uv_qn.png
try "ugrid  geoflow, derived centers" $DIR/ugrid_geoflow_v1.nc --grid $DIR/ugrid_geoflow_grid.nc -o /tmp/uv_gf.png
try "cs     GEOS c12, 6 faces"       $DIR/geos_c12.nc -v PHIS -c terrain -o /tmp/uv_cs.png
try "cs     FV3 C48 tile 1 grid"    $DIR/fv3_grid_spec.tile1.nc -v area -o /tmp/uv_fv3.png

echo "corners reconstructed from centers:"
try "cs     GEOS-IT c180, no corners" $DIR/geos_it_c180_const.nc4 -v PHIS -c terrain -o /tmp/uv_geos180.png
try "cs     GEOS-IT c180 land mask"   $DIR/geos_it_c180_const.nc4 -v FRLAND -o /tmp/uv_geos180_land.png
try "cs     FV3 oro_data, no --grid"  $DIR/fv3_oro_data.tile1.nc -v orog_filt -c terrain -o /tmp/uv_fv3_nogrid.png
try "cs     FV3 C192, 6 tiles -> globe" \
    $DIR/fv3_C192_oro_data.tile1.nc $DIR/fv3_C192_oro_data.tile2.nc \
    $DIR/fv3_C192_oro_data.tile3.nc $DIR/fv3_C192_oro_data.tile4.nc \
    $DIR/fv3_C192_oro_data.tile5.nc $DIR/fv3_C192_oro_data.tile6.nc \
    --tiles -v orog_filt -c terrain -o /tmp/uv_fv3_globe.png

echo
[ "$skips" -gt 0 ] && echo "$skips skipped -- run '$0' without --check first"
if [ "$fails" -eq 0 ]; then
    echo "all rendered. PNGs are in /tmp/uv_*.png -- open them: a correct render"
    echo "has no polygons stretched across the map at +-180 degrees."
else
    echo "$fails failed" >&2
    exit 1
fi
