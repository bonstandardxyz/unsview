/* Quantitative accuracy check for corners_from_centers().
 *
 * Most cubed-sphere output ships cell centers only, so unsview reconstructs the
 * corners it needs to fill polygons. "It renders and looks right" is a weak
 * check on geometry, so this measures the reconstruction against ground truth:
 * a file carrying BOTH centers and real corners (any GEOS/GCHP history file,
 * e.g. samples/testdata/geos_c12.nc) lets us derive corners from its centers
 * and compare them to the corners the model itself wrote.
 *
 * mesh.c is #included rather than linked because corners_from_centers is
 * static -- it has no business being public just to be tested.
 *
 *   cc -O2 -I$(brew --prefix netcdf)/include -L$(brew --prefix netcdf)/lib \
 *      -o tests/check_corners tests/check_corners.c -lnetcdf -lm
 *   ./tests/check_corners samples/testdata/geos_c12.nc
 *
 * Exit status is 0 when the worst corner is inside 5% of a cell, 1 otherwise.
 */
#include "../src/mesh.c"

static double sep_deg(double lon_a, double lat_a, double lon_b, double lat_b) {
    double a[3], b[3];
    unit_vec(lon_a, lat_a, a);
    unit_vec(lon_b, lat_b, b);
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (d >  1.0) d =  1.0;
    if (d < -1.0) d = -1.0;
    return acos(d) * 180.0 / MESH_PI;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE.nc   (needs both centers and corners)\n",
                argv[0]);
        return 2;
    }
    int ncid;
    if (nc_open(argv[1], NC_NOWRITE, &ncid) != NC_NOERR) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    if (!have_var(ncid, "corner_lons") || !have_var(ncid, "lons")) {
        fprintf(stderr, "%s has no corner_lons/lons pair; nothing to compare\n",
                argv[1]);
        return 2;
    }

    int vid, ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_varid(ncid, "corner_lons", &vid);
    nc_inq_var(ncid, vid, NULL, NULL, &ndims, dimids, NULL);
    size_t nf = 1, ncy, ncx;
    if (ndims == 3) {
        nc_inq_dimlen(ncid, dimids[0], &nf);
        nc_inq_dimlen(ncid, dimids[1], &ncy);
        nc_inq_dimlen(ncid, dimids[2], &ncx);
    } else {
        nc_inq_dimlen(ncid, dimids[0], &ncy);
        nc_inq_dimlen(ncid, dimids[1], &ncx);
    }
    size_t ny = ncy - 1, nx = ncx - 1;

    double *clon = read_coord_var(ncid, "lons", nf * ny * nx);
    double *clat = read_coord_var(ncid, "lats", nf * ny * nx);
    double *rlon = read_coord_var(ncid, "corner_lons", nf * ncy * ncx);
    double *rlat = read_coord_var(ncid, "corner_lats", nf * ncy * ncx);
    double *dlon = malloc(sizeof(double) * nf * ncy * ncx);
    double *dlat = malloc(sizeof(double) * nf * ncy * ncx);
    if (!clon || !clat || !rlon || !rlat || !dlon || !dlat) return 2;

    for (size_t f = 0; f < nf; f++) {
        if (!corners_from_centers(clon + f * ny * nx, clat + f * ny * nx, ny, nx,
                                  dlon + f * ncy * ncx, dlat + f * ncy * ncx)) {
            fprintf(stderr, "derivation failed on face %zu\n", f);
            return 2;
        }
    }

    /* Mean cell size, so the error can be reported in units that matter. */
    double cell = 0.0;
    size_t ncell = 0;
    for (size_t f = 0; f < nf; f++)
        for (size_t j = 0; j < ny; j++)
            for (size_t i = 0; i + 1 < nx; i++) {
                size_t o = f * ny * nx + j * nx + i;
                cell += sep_deg(clon[o], clat[o], clon[o + 1], clat[o + 1]);
                ncell++;
            }
    cell /= (double)ncell;

    double worst = 0.0, sum = 0.0;
    size_t n = nf * ncy * ncx, worst_at = 0;
    for (size_t k = 0; k < n; k++) {
        double d = sep_deg(dlon[k], dlat[k], rlon[k], rlat[k]);
        sum += d;
        if (d > worst) { worst = d; worst_at = k; }
    }

    printf("%s\n", argv[1]);
    printf("  faces %zu, cells %zux%zu, corners %zu\n", nf, ny, nx, n);
    printf("  mean cell size    : %.4f deg (%.1f km)\n", cell, cell * 111.195);
    printf("  mean corner error : %.6f deg (%.3f km, %.3f%% of a cell)\n",
           sum / n, sum / n * 111.195, 100.0 * (sum / n) / cell);
    printf("  worst corner error: %.6f deg (%.3f km, %.3f%% of a cell) at index %zu\n",
           worst, worst * 111.195, 100.0 * worst / cell, worst_at);

    int ok = (worst < 0.05 * cell);
    printf("  %s\n", ok ? "PASS (worst corner inside 5% of a cell)"
                        : "FAIL (worst corner beyond 5% of a cell)");
    nc_close(ncid);
    return ok ? 0 : 1;
}
