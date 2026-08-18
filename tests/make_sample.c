/*
 * Build a tiny synthetic unstructured netCDF file for testing unsview.
 *
 * Produces a coarse triangulated sphere: each triangular face is one cell.
 * Adds a Time dim (3 steps) and a scalar variable "wave" with a smooth
 * lon/lat pattern that varies in time.
 *
 * The same mesh and the same field are written in three encodings:
 *
 *   ./make_sample samples/synthetic.nc              # MPAS   (default)
 *   ./make_sample samples/synthetic_icon.nc  icon   # ICON
 *   ./make_sample samples/synthetic_ugrid.nc ugrid  # CF-UGRID
 *   ./make_sample samples/synthetic_fvcom.nc fvcom  # FVCOM
 *   ./make_sample samples/synthetic_cs.nc    cs     # cubed sphere
 *
 * One field in three encodings is the point: rendering all three must produce
 * the same picture, which is a much stronger check on the readers than each
 * one merely not crashing. The UGRID file additionally carries "wave_node",
 * the same field sampled on nodes, to exercise node-to-face averaging.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <netcdf.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHK(rc) do { \
    if ((rc) != NC_NOERR) { \
        fprintf(stderr, "%s:%d nc: %s\n", __FILE__, __LINE__, nc_strerror(rc)); \
        return 1; \
    } \
} while (0)

/* lat/lon grid of (NLAT-1) * NLON quad cells, each split into 2 triangles. */
#define NLAT 17    /* number of lat bands incl poles */
#define NLON 32    /* number of lon divisions */

typedef struct {
    int n_vert, n_cells, max_edges, n_time;
    double *lon_v, *lat_v;     /* nodes, radians */
    double *lon_c, *lat_c;     /* face centers, radians */
    int    *n_edges;
    int    *vcell;             /* [n_cells][max_edges], 1-based */
    double *wave;              /* [n_time][n_cells] */
    double *wave_node;         /* [n_time][n_vert] */
} Mesh;

static double wave_at(double lon, double lat, double phase) {
    return sin(2.0 * lon + phase) * cos(3.0 * lat);
}

static int write_mpas(const char *out, const Mesh *m) {
    int ncid;
    CHK(nc_create(out, NC_CLOBBER | NC_NETCDF4, &ncid));

    int d_ncells, d_nverts, d_maxedges, d_time;
    CHK(nc_def_dim(ncid, "nCells",     m->n_cells,   &d_ncells));
    CHK(nc_def_dim(ncid, "nVertices",  m->n_vert,    &d_nverts));
    CHK(nc_def_dim(ncid, "maxEdges",   m->max_edges, &d_maxedges));
    CHK(nc_def_dim(ncid, "Time",       m->n_time,    &d_time));

    int v_lonc, v_latc, v_lonv, v_latv, v_nedges, v_voc, v_wave;
    int dims_ncells[1] = { d_ncells };
    int dims_nverts[1] = { d_nverts };
    int dims_voc[2]    = { d_ncells, d_maxedges };
    int dims_wave[2]   = { d_time, d_ncells };

    CHK(nc_def_var(ncid, "lonCell",         NC_DOUBLE, 1, dims_ncells, &v_lonc));
    CHK(nc_def_var(ncid, "latCell",         NC_DOUBLE, 1, dims_ncells, &v_latc));
    CHK(nc_def_var(ncid, "lonVertex",       NC_DOUBLE, 1, dims_nverts, &v_lonv));
    CHK(nc_def_var(ncid, "latVertex",       NC_DOUBLE, 1, dims_nverts, &v_latv));
    CHK(nc_def_var(ncid, "nEdgesOnCell",    NC_INT,    1, dims_ncells, &v_nedges));
    CHK(nc_def_var(ncid, "verticesOnCell",  NC_INT,    2, dims_voc,    &v_voc));
    CHK(nc_def_var(ncid, "wave",            NC_DOUBLE, 2, dims_wave,   &v_wave));

    CHK(nc_enddef(ncid));

    CHK(nc_put_var_double(ncid, v_lonc,   m->lon_c));
    CHK(nc_put_var_double(ncid, v_latc,   m->lat_c));
    CHK(nc_put_var_double(ncid, v_lonv,   m->lon_v));
    CHK(nc_put_var_double(ncid, v_latv,   m->lat_v));
    CHK(nc_put_var_int(ncid,    v_nedges, m->n_edges));
    CHK(nc_put_var_int(ncid,    v_voc,    m->vcell));
    CHK(nc_put_var_double(ncid, v_wave,   m->wave));
    CHK(nc_close(ncid));
    return 0;
}

/* ICON stores vertex_of_cell as (nv, cell) -- transposed relative to MPAS --
 * with 1-based indices, and keeps coordinates in radians. */
static int write_icon(const char *out, const Mesh *m) {
    int *voc_t = malloc(sizeof(int) * m->n_cells * m->max_edges);
    for (int c = 0; c < m->n_cells; c++)
        for (int e = 0; e < m->max_edges; e++)
            voc_t[e * m->n_cells + c] = m->vcell[c * m->max_edges + e];

    int ncid;
    CHK(nc_create(out, NC_CLOBBER | NC_NETCDF4, &ncid));

    int d_cell, d_vertex, d_nv, d_time;
    CHK(nc_def_dim(ncid, "cell",   m->n_cells,   &d_cell));
    CHK(nc_def_dim(ncid, "vertex", m->n_vert,    &d_vertex));
    CHK(nc_def_dim(ncid, "nv",     m->max_edges, &d_nv));
    CHK(nc_def_dim(ncid, "Time",   m->n_time,    &d_time));

    int v_clon, v_clat, v_vlon, v_vlat, v_voc, v_wave;
    int dims_cell[1] = { d_cell };
    int dims_vert[1] = { d_vertex };
    int dims_voc[2]  = { d_nv, d_cell };      /* transposed, as ICON writes it */
    int dims_wave[2] = { d_time, d_cell };

    CHK(nc_def_var(ncid, "clon", NC_DOUBLE, 1, dims_cell, &v_clon));
    CHK(nc_def_var(ncid, "clat", NC_DOUBLE, 1, dims_cell, &v_clat));
    CHK(nc_def_var(ncid, "vlon", NC_DOUBLE, 1, dims_vert, &v_vlon));
    CHK(nc_def_var(ncid, "vlat", NC_DOUBLE, 1, dims_vert, &v_vlat));
    CHK(nc_def_var(ncid, "vertex_of_cell", NC_INT, 2, dims_voc, &v_voc));
    CHK(nc_def_var(ncid, "wave", NC_DOUBLE, 2, dims_wave, &v_wave));

    CHK(nc_put_att_text(ncid, v_clon, "units", 6, "radian"));
    CHK(nc_put_att_text(ncid, v_clat, "units", 6, "radian"));
    CHK(nc_put_att_text(ncid, v_vlon, "units", 6, "radian"));
    CHK(nc_put_att_text(ncid, v_vlat, "units", 6, "radian"));
    CHK(nc_enddef(ncid));

    CHK(nc_put_var_double(ncid, v_clon, m->lon_c));
    CHK(nc_put_var_double(ncid, v_clat, m->lat_c));
    CHK(nc_put_var_double(ncid, v_vlon, m->lon_v));
    CHK(nc_put_var_double(ncid, v_vlat, m->lat_v));
    CHK(nc_put_var_int(ncid,    v_voc,  voc_t));
    CHK(nc_put_var_double(ncid, v_wave, m->wave));
    CHK(nc_close(ncid));

    free(voc_t);
    return 0;
}

/* CF-UGRID, written to exercise the awkward parts of the spec rather than the
 * easy path: coordinates in degrees, start_index = 1, an int64 connectivity
 * whose ragged rows are _FillValue-padded (a 4-wide array holding triangles),
 * and no face_coordinates -- so face centers must be derived from the nodes. */
static int write_ugrid(const char *out, const Mesh *m) {
    const int max_nodes = m->max_edges + 1;   /* one padding column */
    const long long FILL = -9223372036854775807LL;

    long long *conn = malloc(sizeof(long long) * m->n_cells * max_nodes);
    for (int c = 0; c < m->n_cells; c++) {
        for (int e = 0; e < max_nodes; e++) {
            conn[c * max_nodes + e] = (e < m->n_edges[c])
                ? (long long)m->vcell[c * m->max_edges + e]   /* already 1-based */
                : FILL;
        }
    }
    double *lon_deg = malloc(sizeof(double) * m->n_vert);
    double *lat_deg = malloc(sizeof(double) * m->n_vert);
    for (int i = 0; i < m->n_vert; i++) {
        lon_deg[i] = m->lon_v[i] * 180.0 / M_PI;
        lat_deg[i] = m->lat_v[i] * 180.0 / M_PI;
    }

    int ncid;
    CHK(nc_create(out, NC_CLOBBER | NC_NETCDF4, &ncid));

    int d_node, d_face, d_maxn, d_time;
    CHK(nc_def_dim(ncid, "n_node",           m->n_vert,  &d_node));
    CHK(nc_def_dim(ncid, "n_face",           m->n_cells, &d_face));
    CHK(nc_def_dim(ncid, "n_max_face_nodes", max_nodes,  &d_maxn));
    CHK(nc_def_dim(ncid, "Time",             m->n_time,  &d_time));

    int v_mesh, v_nlon, v_nlat, v_conn, v_wave, v_wnode;
    int dims_node[1] = { d_node };
    int dims_conn[2] = { d_face, d_maxn };
    int dims_wave[2] = { d_time, d_face };
    int dims_wnod[2] = { d_time, d_node };

    CHK(nc_def_var(ncid, "mesh", NC_INT, 0, NULL, &v_mesh));
    CHK(nc_def_var(ncid, "node_lon", NC_DOUBLE, 1, dims_node, &v_nlon));
    CHK(nc_def_var(ncid, "node_lat", NC_DOUBLE, 1, dims_node, &v_nlat));
    CHK(nc_def_var(ncid, "face_node_connectivity", NC_INT64, 2, dims_conn, &v_conn));
    CHK(nc_def_var(ncid, "wave", NC_DOUBLE, 2, dims_wave, &v_wave));
    CHK(nc_def_var(ncid, "wave_node", NC_DOUBLE, 2, dims_wnod, &v_wnode));

    int topo_dim = 2;
    CHK(nc_put_att_text(ncid, v_mesh, "cf_role", 13, "mesh_topology"));
    CHK(nc_put_att_int(ncid, v_mesh, "topology_dimension", NC_INT, 1, &topo_dim));
    CHK(nc_put_att_text(ncid, v_mesh, "node_coordinates", 17, "node_lon node_lat"));
    CHK(nc_put_att_text(ncid, v_mesh, "face_node_connectivity", 22,
                        "face_node_connectivity"));
    CHK(nc_put_att_text(ncid, v_mesh, "face_dimension", 6, "n_face"));
    CHK(nc_put_att_text(ncid, v_mesh, "node_dimension", 6, "n_node"));

    CHK(nc_put_att_text(ncid, v_nlon, "units", 13, "degrees_east"));
    CHK(nc_put_att_text(ncid, v_nlat, "units", 14, "degrees_north"));

    long long start_index = 1;
    CHK(nc_put_att_text(ncid, v_conn, "cf_role", 22, "face_node_connectivity"));
    CHK(nc_put_att_longlong(ncid, v_conn, "start_index", NC_INT64, 1, &start_index));
    CHK(nc_put_att_longlong(ncid, v_conn, "_FillValue", NC_INT64, 1, &FILL));

    CHK(nc_put_att_text(ncid, v_wave,  "mesh", 4, "mesh"));
    CHK(nc_put_att_text(ncid, v_wave,  "location", 4, "face"));
    CHK(nc_put_att_text(ncid, v_wnode, "mesh", 4, "mesh"));
    CHK(nc_put_att_text(ncid, v_wnode, "location", 4, "node"));
    CHK(nc_enddef(ncid));

    int dummy = -1;
    CHK(nc_put_var_int(ncid, v_mesh, &dummy));
    CHK(nc_put_var_double(ncid, v_nlon, lon_deg));
    CHK(nc_put_var_double(ncid, v_nlat, lat_deg));
    CHK(nc_put_var_longlong(ncid, v_conn, conn));
    CHK(nc_put_var_double(ncid, v_wave, m->wave));
    CHK(nc_put_var_double(ncid, v_wnode, m->wave_node));
    CHK(nc_close(ncid));

    free(conn); free(lon_deg); free(lat_deg);
    return 0;
}

/* FVCOM: nodes in lon/lat, element centers in lonc/latc, and nv(three, nele)
 * transposed and 1-based. Degrees, and node-centered fields are the norm, so
 * this writes "wave" on elements and "zeta" on nodes the way FVCOM output does. */
static int write_fvcom(const char *out, const Mesh *m) {
    int *nv_t = malloc(sizeof(int) * m->n_cells * m->max_edges);
    for (int c = 0; c < m->n_cells; c++)
        for (int e = 0; e < m->max_edges; e++)
            nv_t[e * m->n_cells + c] = m->vcell[c * m->max_edges + e];

    double *lon_d = malloc(sizeof(double) * m->n_vert);
    double *lat_d = malloc(sizeof(double) * m->n_vert);
    double *lonc_d = malloc(sizeof(double) * m->n_cells);
    double *latc_d = malloc(sizeof(double) * m->n_cells);
    for (int i = 0; i < m->n_vert; i++) {
        lon_d[i] = m->lon_v[i] * 180.0 / M_PI;
        lat_d[i] = m->lat_v[i] * 180.0 / M_PI;
    }
    for (int i = 0; i < m->n_cells; i++) {
        lonc_d[i] = m->lon_c[i] * 180.0 / M_PI;
        latc_d[i] = m->lat_c[i] * 180.0 / M_PI;
    }

    int ncid;
    CHK(nc_create(out, NC_CLOBBER | NC_NETCDF4, &ncid));

    int d_node, d_nele, d_three, d_time;
    CHK(nc_def_dim(ncid, "node",  m->n_vert,    &d_node));
    CHK(nc_def_dim(ncid, "nele",  m->n_cells,   &d_nele));
    CHK(nc_def_dim(ncid, "three", m->max_edges, &d_three));
    CHK(nc_def_dim(ncid, "time",  m->n_time,    &d_time));

    int v_lon, v_lat, v_lonc, v_latc, v_nv, v_wave, v_zeta;
    int dims_node[1] = { d_node };
    int dims_nele[1] = { d_nele };
    int dims_nv[2]   = { d_three, d_nele };   /* transposed, as FVCOM writes it */
    int dims_wave[2] = { d_time, d_nele };
    int dims_zeta[2] = { d_time, d_node };

    CHK(nc_def_var(ncid, "lon",  NC_DOUBLE, 1, dims_node, &v_lon));
    CHK(nc_def_var(ncid, "lat",  NC_DOUBLE, 1, dims_node, &v_lat));
    CHK(nc_def_var(ncid, "lonc", NC_DOUBLE, 1, dims_nele, &v_lonc));
    CHK(nc_def_var(ncid, "latc", NC_DOUBLE, 1, dims_nele, &v_latc));
    CHK(nc_def_var(ncid, "nv",   NC_INT,    2, dims_nv,   &v_nv));
    CHK(nc_def_var(ncid, "wave", NC_DOUBLE, 2, dims_wave, &v_wave));
    CHK(nc_def_var(ncid, "zeta", NC_DOUBLE, 2, dims_zeta, &v_zeta));

    CHK(nc_put_att_text(ncid, v_lon,  "units", 13, "degrees_east"));
    CHK(nc_put_att_text(ncid, v_lat,  "units", 14, "degrees_north"));
    CHK(nc_put_att_text(ncid, v_lonc, "units", 13, "degrees_east"));
    CHK(nc_put_att_text(ncid, v_latc, "units", 14, "degrees_north"));
    CHK(nc_put_att_text(ncid, v_zeta, "units", 7, "meters"));
    CHK(nc_enddef(ncid));

    CHK(nc_put_var_double(ncid, v_lon,  lon_d));
    CHK(nc_put_var_double(ncid, v_lat,  lat_d));
    CHK(nc_put_var_double(ncid, v_lonc, lonc_d));
    CHK(nc_put_var_double(ncid, v_latc, latc_d));
    CHK(nc_put_var_int(ncid,    v_nv,   nv_t));
    CHK(nc_put_var_double(ncid, v_wave, m->wave));
    CHK(nc_put_var_double(ncid, v_zeta, m->wave_node));
    CHK(nc_close(ncid));

    free(nv_t); free(lon_d); free(lat_d); free(lonc_d); free(latc_d);
    return 0;
}

/* An equiangular gnomonic cubed sphere, written the way GEOS and FV3 do: six
 * faces of cell centers in lons/lats(nf, Y, X) and corners in
 * corner_lons/corner_lats(nf, Y+1, X+1), with no connectivity array.
 *
 * This is a different mesh from the triangulated sphere above, so it cannot be
 * compared pixel-for-pixel with the other three -- it is here so CI exercises
 * the cubed-sphere reader at all. With an even face size the poles land exactly
 * on a corner node, which is what makes the reader's polar-corner split matter.
 */
/* with_corners = 0 writes the same mesh with cell centers only, no
 * corner_lons/corner_lats -- the shape operational GEOS actually ships, where
 * unsview has to reconstruct the boundaries before it can fill polygons. The
 * two files describe an identical mesh, so their renders should agree closely. */
static int write_cs(const char *out, int n, int n_time, int with_corners) {
    const int nf = 6, ncorner = n + 1;
    size_t n_node = (size_t)nf * ncorner * ncorner;
    size_t n_cell = (size_t)nf * n * n;

    double *clon = malloc(sizeof(double) * n_node), *clat = malloc(sizeof(double) * n_node);
    double *flon = malloc(sizeof(double) * n_cell), *flat = malloc(sizeof(double) * n_cell);

    /* Map an equiangular (alpha, beta) on face f to a point on the sphere. */
    #define CS_POINT(f, alpha, beta, LON, LAT) do {                            \
        double a_ = tan(alpha), b_ = tan(beta), X, Y, Z;                       \
        switch (f) {                                                           \
            case 0: X =  1;  Y =  a_; Z =  b_; break;  /* +X */                \
            case 1: X = -a_; Y =  1;  Z =  b_; break;  /* +Y */                \
            case 2: X = -1;  Y = -a_; Z =  b_; break;  /* -X */                \
            case 3: X =  a_; Y = -1;  Z =  b_; break;  /* -Y */                \
            case 4: X = -b_; Y =  a_; Z =  1;  break;  /* +Z, north pole */    \
            default:X =  b_; Y =  a_; Z = -1;  break;  /* -Z, south pole */    \
        }                                                                      \
        double r_ = sqrt(X*X + Y*Y + Z*Z);                                     \
        (LON) = atan2(Y, X) * 180.0 / M_PI;                                    \
        (LAT) = asin(Z / r_) * 180.0 / M_PI;                                   \
    } while (0)

    for (int f = 0; f < nf; f++) {
        for (int j = 0; j < ncorner; j++) {
            double beta = -M_PI / 4.0 + (M_PI / 2.0) * j / n;
            for (int i = 0; i < ncorner; i++) {
                double alpha = -M_PI / 4.0 + (M_PI / 2.0) * i / n;
                size_t k = ((size_t)f * ncorner + j) * ncorner + i;
                CS_POINT(f, alpha, beta, clon[k], clat[k]);
            }
        }
        for (int j = 0; j < n; j++) {
            double beta = -M_PI / 4.0 + (M_PI / 2.0) * (j + 0.5) / n;
            for (int i = 0; i < n; i++) {
                double alpha = -M_PI / 4.0 + (M_PI / 2.0) * (i + 0.5) / n;
                size_t k = ((size_t)f * n + j) * n + i;
                CS_POINT(f, alpha, beta, flon[k], flat[k]);
            }
        }
    }
    #undef CS_POINT

    double *wave = malloc(sizeof(double) * (size_t)n_time * n_cell);
    for (int t = 0; t < n_time; t++) {
        double phase = 2.0 * M_PI * t / n_time;
        for (size_t k = 0; k < n_cell; k++)
            wave[t * n_cell + k] = wave_at(flon[k] * M_PI / 180.0,
                                           flat[k] * M_PI / 180.0, phase);
    }

    int ncid;
    CHK(nc_create(out, NC_CLOBBER | NC_NETCDF4, &ncid));

    int d_nf, d_x, d_y, d_xc = -1, d_yc = -1, d_time;
    CHK(nc_def_dim(ncid, "nf",    nf,      &d_nf));
    CHK(nc_def_dim(ncid, "Ydim",  n,       &d_y));
    CHK(nc_def_dim(ncid, "Xdim",  n,       &d_x));
    if (with_corners) {
        CHK(nc_def_dim(ncid, "YCdim", ncorner, &d_yc));
        CHK(nc_def_dim(ncid, "XCdim", ncorner, &d_xc));
    }
    CHK(nc_def_dim(ncid, "Time",  n_time,  &d_time));

    int v_lons, v_lats, v_clon = -1, v_clat = -1, v_wave;
    int dims_c[3] = { d_nf, d_y,  d_x  };
    int dims_k[3] = { d_nf, d_yc, d_xc };
    int dims_w[4] = { d_time, d_nf, d_y, d_x };

    CHK(nc_def_var(ncid, "lons",        NC_DOUBLE, 3, dims_c, &v_lons));
    CHK(nc_def_var(ncid, "lats",        NC_DOUBLE, 3, dims_c, &v_lats));
    if (with_corners) {
        CHK(nc_def_var(ncid, "corner_lons", NC_DOUBLE, 3, dims_k, &v_clon));
        CHK(nc_def_var(ncid, "corner_lats", NC_DOUBLE, 3, dims_k, &v_clat));
    }
    CHK(nc_def_var(ncid, "wave",        NC_DOUBLE, 4, dims_w, &v_wave));

    CHK(nc_put_att_text(ncid, v_lons, "units", 13, "degrees_east"));
    CHK(nc_put_att_text(ncid, v_lats, "units", 14, "degrees_north"));
    if (with_corners) {
        CHK(nc_put_att_text(ncid, v_clon, "units", 13, "degrees_east"));
        CHK(nc_put_att_text(ncid, v_clat, "units", 14, "degrees_north"));
    }
    CHK(nc_put_att_text(ncid, v_wave, "coordinates", 9, "lons lats"));
    CHK(nc_enddef(ncid));

    CHK(nc_put_var_double(ncid, v_lons, flon));
    CHK(nc_put_var_double(ncid, v_lats, flat));
    if (with_corners) {
        CHK(nc_put_var_double(ncid, v_clon, clon));
        CHK(nc_put_var_double(ncid, v_clat, clat));
    }
    CHK(nc_put_var_double(ncid, v_wave, wave));
    CHK(nc_close(ncid));

    free(clon); free(clat); free(flon); free(flat); free(wave);
    fprintf(stderr, "wrote %s (cs%s): %zu cells, %zu nodes, %d time steps\n",
            out, with_corners ? "" : ", centers only", n_cell,
            with_corners ? n_node : n_cell, n_time);
    return 0;
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "samples/synthetic.nc";
    const char *fmt = (argc > 2) ? argv[2] : "mpas";

    if (!strcmp(fmt, "cs")) return write_cs(out, 12, 3, 1);
    if (!strcmp(fmt, "cs_centers")) return write_cs(out, 12, 3, 0);

    Mesh m;
    m.n_vert    = NLAT * NLON;
    m.n_cells   = (NLAT - 1) * NLON * 2;
    m.max_edges = 3;
    m.n_time    = 3;

    m.lon_v = malloc(sizeof(double) * m.n_vert);
    m.lat_v = malloc(sizeof(double) * m.n_vert);
    for (int i = 0; i < NLAT; i++) {
        double lat = -M_PI / 2.0 + M_PI * i / (NLAT - 1);
        for (int j = 0; j < NLON; j++) {
            double lon = -M_PI + 2.0 * M_PI * j / NLON;
            m.lon_v[i * NLON + j] = lon;
            m.lat_v[i * NLON + j] = lat;
        }
    }

    m.lon_c   = malloc(sizeof(double) * m.n_cells);
    m.lat_c   = malloc(sizeof(double) * m.n_cells);
    m.n_edges = malloc(sizeof(int) * m.n_cells);
    m.vcell   = malloc(sizeof(int) * m.n_cells * m.max_edges);

    int c = 0;
    for (int i = 0; i < NLAT - 1; i++) {
        for (int j = 0; j < NLON; j++) {
            int j2 = (j + 1) % NLON;
            int v00 = i       * NLON + j;
            int v01 = i       * NLON + j2;
            int v10 = (i + 1) * NLON + j;
            int v11 = (i + 1) * NLON + j2;

            m.n_edges[c] = 3;
            m.vcell[c * m.max_edges + 0] = v00 + 1;
            m.vcell[c * m.max_edges + 1] = v01 + 1;
            m.vcell[c * m.max_edges + 2] = v11 + 1;
            m.lon_c[c] = (m.lon_v[v00] + m.lon_v[v01] + m.lon_v[v11]) / 3.0;
            while (m.lon_c[c] >  M_PI) m.lon_c[c] -= 2.0 * M_PI;
            while (m.lon_c[c] < -M_PI) m.lon_c[c] += 2.0 * M_PI;
            m.lat_c[c] = (m.lat_v[v00] + m.lat_v[v01] + m.lat_v[v11]) / 3.0;
            c++;

            m.n_edges[c] = 3;
            m.vcell[c * m.max_edges + 0] = v00 + 1;
            m.vcell[c * m.max_edges + 1] = v11 + 1;
            m.vcell[c * m.max_edges + 2] = v10 + 1;
            m.lon_c[c] = (m.lon_v[v00] + m.lon_v[v11] + m.lon_v[v10]) / 3.0;
            while (m.lon_c[c] >  M_PI) m.lon_c[c] -= 2.0 * M_PI;
            while (m.lon_c[c] < -M_PI) m.lon_c[c] += 2.0 * M_PI;
            m.lat_c[c] = (m.lat_v[v00] + m.lat_v[v11] + m.lat_v[v10]) / 3.0;
            c++;
        }
    }

    m.wave      = malloc(sizeof(double) * m.n_time * m.n_cells);
    m.wave_node = malloc(sizeof(double) * m.n_time * m.n_vert);
    for (int t = 0; t < m.n_time; t++) {
        double phase = 2.0 * M_PI * t / m.n_time;
        for (int k = 0; k < m.n_cells; k++)
            m.wave[t * m.n_cells + k] = wave_at(m.lon_c[k], m.lat_c[k], phase);
        for (int k = 0; k < m.n_vert; k++)
            m.wave_node[t * m.n_vert + k] = wave_at(m.lon_v[k], m.lat_v[k], phase);
    }

    int rc;
    if      (!strcmp(fmt, "mpas"))  rc = write_mpas(out, &m);
    else if (!strcmp(fmt, "icon"))  rc = write_icon(out, &m);
    else if (!strcmp(fmt, "ugrid")) rc = write_ugrid(out, &m);
    else if (!strcmp(fmt, "fvcom")) rc = write_fvcom(out, &m);
    else {
        fprintf(stderr,
                "usage: %s OUT.nc [mpas|icon|ugrid|fvcom|cs|cs_centers]\n",
                argv[0]);
        return 1;
    }

    free(m.lon_v); free(m.lat_v); free(m.lon_c); free(m.lat_c);
    free(m.n_edges); free(m.vcell); free(m.wave); free(m.wave_node);

    if (rc == 0)
        fprintf(stderr, "wrote %s (%s): %d cells, %d vertices, %d time steps\n",
                out, fmt, m.n_cells, m.n_vert, m.n_time);
    return rc;
}
