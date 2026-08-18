/*
 * strip_mesh: copy a netCDF file but drop MPAS mesh-metadata variables.
 * Used to simulate stream-split MPAS output (data file without mesh) so we
 * can test visualize's --grid GRIDFILE.nc option.
 *
 * usage: strip_mesh INPUT.nc OUTPUT.nc
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netcdf.h>

#define CHK(rc) do { \
    if ((rc) != NC_NOERR) { \
        fprintf(stderr, "%s:%d nc: %s\n", __FILE__, __LINE__, nc_strerror(rc)); \
        return 1; \
    } \
} while (0)

static const char *MESH_VARS[] = {
    "lonCell", "latCell", "xCell", "yCell", "zCell",
    "areaCell", "meshDensity", "indexToCellID", "cellsOnCell",
    "edgesOnCell", "verticesOnCell", "nEdgesOnCell",
    "lonEdge", "latEdge", "xEdge", "yEdge", "zEdge",
    "indexToEdgeID", "cellsOnEdge", "verticesOnEdge",
    "lonVertex", "latVertex", "xVertex", "yVertex", "zVertex",
    "indexToVertexID", "edgesOnVertex", "cellsOnVertex",
    "kiteAreasOnVertex",
    "fEdge", "fVertex", "bdyMaskCell",
    "localVerticalUnitVectors", "cellTangentPlane",
    "defc_a", "defc_b",
    "cell_gradient_coef_x", "cell_gradient_coef_y",
    "coeffs_reconstruct",
    NULL
};

static int is_mesh(const char *name) {
    for (int i = 0; MESH_VARS[i]; i++) {
        if (!strcmp(name, MESH_VARS[i])) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s IN.nc OUT.nc\n", argv[0]);
        return 1;
    }
    int src, dst;
    CHK(nc_open(argv[1], NC_NOWRITE, &src));
    CHK(nc_create(argv[2], NC_CLOBBER | NC_NETCDF4, &dst));

    int ndims, nvars;
    CHK(nc_inq_ndims(src, &ndims));
    CHK(nc_inq_nvars(src, &nvars));

    int *dim_map = calloc(ndims, sizeof(int));
    for (int d = 0; d < ndims; d++) {
        char name[NC_MAX_NAME + 1];
        size_t len;
        CHK(nc_inq_dim(src, d, name, &len));
        int unlimid = -1;
        nc_inq_unlimdim(src, &unlimid);
        size_t use = (d == unlimid) ? NC_UNLIMITED : len;
        int new_d;
        CHK(nc_def_dim(dst, name, use, &new_d));
        dim_map[d] = new_d;
    }

    int *var_map = calloc(nvars, sizeof(int));
    for (int v = 0; v < nvars; v++) {
        char name[NC_MAX_NAME + 1] = {0};
        nc_type t;
        int vd, dimids[NC_MAX_VAR_DIMS];
        CHK(nc_inq_var(src, v, name, &t, &vd, dimids, NULL));
        var_map[v] = -1;
        if (is_mesh(name)) continue;
        int new_dimids[NC_MAX_VAR_DIMS];
        for (int k = 0; k < vd; k++) new_dimids[k] = dim_map[dimids[k]];
        int new_v;
        CHK(nc_def_var(dst, name, t, vd, new_dimids, &new_v));
        var_map[v] = new_v;

        int natts;
        CHK(nc_inq_varnatts(src, v, &natts));
        for (int a = 0; a < natts; a++) {
            char aname[NC_MAX_NAME + 1];
            CHK(nc_inq_attname(src, v, a, aname));
            CHK(nc_copy_att(src, v, aname, dst, new_v));
        }
    }

    CHK(nc_enddef(dst));

    for (int v = 0; v < nvars; v++) {
        if (var_map[v] < 0) continue;
        nc_type t;
        int vd, dimids[NC_MAX_VAR_DIMS];
        CHK(nc_inq_var(src, v, NULL, &t, &vd, dimids, NULL));
        size_t total = 1;
        size_t dlen[NC_MAX_VAR_DIMS] = {0};
        for (int k = 0; k < vd; k++) {
            CHK(nc_inq_dimlen(src, dimids[k], &dlen[k]));
            total *= dlen[k];
        }
        if (vd == 0) total = 1;
        size_t elem = (t == NC_DOUBLE) ? 8 : (t == NC_FLOAT || t == NC_INT) ? 4 :
                      (t == NC_SHORT) ? 2 : 1;
        void *buf = malloc(total * elem);
        CHK(nc_get_var(src, v, buf));
        CHK(nc_put_var(dst, var_map[v], buf));
        free(buf);
    }

    free(dim_map); free(var_map);
    CHK(nc_close(src)); CHK(nc_close(dst));
    fprintf(stderr, "wrote %s (mesh vars stripped)\n", argv[2]);
    return 0;
}
