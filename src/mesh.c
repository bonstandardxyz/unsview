#include "mesh.h"

#include <netcdf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESH_PI 3.14159265358979323846

/* ------------------------------------------------------------------ *
 * Low-level netCDF helpers
 * ------------------------------------------------------------------ */

static double *read_double_var(int ncid, const char *name, size_t n) {
    int vid;
    if (nc_inq_varid(ncid, name, &vid) != NC_NOERR) return NULL;
    double *buf = malloc(sizeof(double) * n);
    if (nc_get_var_double(ncid, vid, buf) != NC_NOERR) {
        free(buf);
        return NULL;
    }
    return buf;
}

static int *read_int_var(int ncid, const char *name, size_t n) {
    int vid;
    if (nc_inq_varid(ncid, name, &vid) != NC_NOERR) return NULL;
    int *buf = malloc(sizeof(int) * n);
    if (nc_get_var_int(ncid, vid, buf) != NC_NOERR) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Connectivity is read as long long because real files store it as int, uint or
 * int64 -- reading a uint _FillValue of 4294967295 or an int64 one of INT64_MIN
 * through nc_get_var_int fails with NC_ERANGE. */
static long long *read_ll_var(int ncid, const char *name, size_t n) {
    int vid;
    if (nc_inq_varid(ncid, name, &vid) != NC_NOERR) return NULL;
    long long *buf = malloc(sizeof(long long) * n);
    if (nc_get_var_longlong(ncid, vid, buf) != NC_NOERR) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Read a text attribute into buf. Returns 0 if absent, non-text, or too long. */
static int get_att_text(int ncid, int varid, const char *att,
                        char *buf, size_t cap) {
    nc_type t;
    size_t len = 0;
    if (nc_inq_att(ncid, varid, att, &t, &len) != NC_NOERR) return 0;
    if (t != NC_CHAR || len == 0 || len >= cap) return 0;
    if (nc_get_att_text(ncid, varid, att, buf) != NC_NOERR) return 0;
    buf[len] = '\0';
    return 1;
}

/* First variable carrying att == value, or -1. */
static int find_var_by_att(int ncid, const char *att, const char *value) {
    int nvars = 0;
    nc_inq_nvars(ncid, &nvars);
    for (int v = 0; v < nvars; v++) {
        char buf[128];
        if (get_att_text(ncid, v, att, buf, sizeof buf) && !strcmp(buf, value))
            return v;
    }
    return -1;
}

/* Length of a 1-D variable's only dimension. Returns 0 if not 1-D. */
static size_t var_dim1_len(int ncid, const char *name) {
    int vid, ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    if (nc_inq_varid(ncid, name, &vid) != NC_NOERR) return 0;
    if (nc_inq_var(ncid, vid, NULL, NULL, &ndims, dimids, NULL) != NC_NOERR) return 0;
    if (ndims != 1) return 0;
    size_t len = 0;
    nc_inq_dimlen(ncid, dimids[0], &len);
    return len;
}

/* Read a lon/lat variable as radians. MPAS and ICON store radians, UGRID
 * conventionally stores degrees -- the units attribute decides, so the
 * "radians everywhere internally" invariant holds for every model. */
static double *read_coord_var(int ncid, const char *name, size_t n) {
    double *buf = read_double_var(ncid, name, n);
    if (!buf) return NULL;
    int vid;
    char units[64];
    int degrees;
    if (nc_inq_varid(ncid, name, &vid) == NC_NOERR &&
        get_att_text(ncid, vid, "units", units, sizeof units)) {
        degrees = (strncmp(units, "degree", 6) == 0);
    } else {
        /* No units attribute at all -- FV3's oro and sfc files omit it on
         * geolon/geolat, and read as radians those degree values put the mesh
         * somewhere off the planet. Radians cannot exceed 2*pi, so a larger
         * magnitude is unambiguously degrees. This runs *only* when the
         * attribute is missing, so every file that declares its units keeps
         * exactly the behaviour it had. */
        double mx = 0.0;
        for (size_t i = 0; i < n; i++) {
            double a = fabs(buf[i]);
            if (a > mx && a < 1e10) mx = a;   /* 1e20 fill values are not data */
        }
        degrees = (mx > 2.0 * MESH_PI);
        if (degrees)
            fprintf(stderr, "note: %s has no units attribute; values exceed "
                            "2*pi, reading as degrees\n", name);
    }
    if (degrees)
        for (size_t i = 0; i < n; i++) buf[i] *= MESH_PI / 180.0;
    return buf;
}

static void record_mesh_var(UnsMesh *m, const char *name) {
    if (!name || !*name || m->n_mesh_vars >= MESH_MAX_MESHVARS) return;
    snprintf(m->mesh_vars[m->n_mesh_vars], MESH_MAX_NAME, "%s", name);
    m->n_mesh_vars++;
}

static int have_var(int ncid, const char *name) {
    int vid;
    return nc_inq_varid(ncid, name, &vid) == NC_NOERR;
}

static int have_dim(int ncid, const char *name) {
    int did;
    return nc_inq_dimid(ncid, name, &did) == NC_NOERR;
}

/* ------------------------------------------------------------------ *
 * Shared post-processing
 * ------------------------------------------------------------------ */

/* Cell-center lon/lat extent. Compute in two conventions and keep the narrower
 * one -- that handles regional datasets that happen to straddle lon=0 vs lon=pi
 * without choosing a span that wraps the world. */
static void compute_extent(UnsMesh *m) {
    double lon_min_a =  1e9, lon_max_a = -1e9;  /* [-pi, pi] convention */
    double lon_min_b =  1e9, lon_max_b = -1e9;  /* [ 0, 2pi] convention */
    double lat_min   =  1e9, lat_max   = -1e9;
    for (size_t i = 0; i < m->ncells; i++) {
        double la = m->lat_cell[i];
        if (la < lat_min) lat_min = la;
        if (la > lat_max) lat_max = la;
        double a = m->lon_cell[i];
        while (a >  MESH_PI) a -= 2.0 * MESH_PI;
        while (a < -MESH_PI) a += 2.0 * MESH_PI;
        if (a < lon_min_a) lon_min_a = a;
        if (a > lon_max_a) lon_max_a = a;
        double b = m->lon_cell[i];
        while (b > 2.0 * MESH_PI) b -= 2.0 * MESH_PI;
        while (b < 0.0) b += 2.0 * MESH_PI;
        if (b < lon_min_b) lon_min_b = b;
        if (b > lon_max_b) lon_max_b = b;
    }
    double span_a = lon_max_a - lon_min_a;
    double span_b = lon_max_b - lon_min_b;
    if (span_b < span_a) {
        /* Pacific-centered regional: shift back into [-pi, pi] window */
        double mid = (lon_min_b + lon_max_b) * 0.5;
        double shift = (mid > MESH_PI) ? 2.0 * MESH_PI : 0.0;
        m->lon_min = lon_min_b - shift;
        m->lon_max = lon_max_b - shift;
    } else {
        m->lon_min = lon_min_a;
        m->lon_max = lon_max_a;
    }
    m->lat_min = lat_min;
    m->lat_max = lat_max;
}

/* Face centers as the mean of each face's corner nodes, unwrapping every corner
 * to within +-pi of the first one before averaging.
 *
 * The unwrap is load-bearing, not cosmetic: a naive mean puts a face straddling
 * the date line near lon 0, and raster_render_mesh uses the face center as the
 * reference for unwrapping that face's vertices -- so the polygon would be torn
 * across the whole map. */
static void face_centers_from_nodes(UnsMesh *m) {
    m->lon_cell = malloc(sizeof(double) * m->ncells);
    m->lat_cell = malloc(sizeof(double) * m->ncells);
    for (size_t c = 0; c < m->ncells; c++) {
        int n = m->n_edges_on_cell[c];
        if (n <= 0) {
            m->lon_cell[c] = 0.0;
            m->lat_cell[c] = 0.0;
            continue;
        }
        double ref = m->lon_vertex[m->vertices_on_cell[c * (size_t)m->max_edges]];
        double slon = 0.0, slat = 0.0;
        for (int e = 0; e < n; e++) {
            int vi = m->vertices_on_cell[c * (size_t)m->max_edges + (size_t)e];
            double lon = m->lon_vertex[vi];
            while (lon - ref >  MESH_PI) lon -= 2.0 * MESH_PI;
            while (lon - ref < -MESH_PI) lon += 2.0 * MESH_PI;
            slon += lon;
            slat += m->lat_vertex[vi];
        }
        double lon = slon / n;
        while (lon >  MESH_PI) lon -= 2.0 * MESH_PI;
        while (lon < -MESH_PI) lon += 2.0 * MESH_PI;
        m->lon_cell[c] = lon;
        m->lat_cell[c] = slat / n;
    }
}

/* ------------------------------------------------------------------ *
 * MPAS
 * ------------------------------------------------------------------ */

static int load_mpas(int ncid, UnsMesh *m) {
    int d_cells, d_verts, d_maxedges;
    if (nc_inq_dimid(ncid, "nCells", &d_cells) != NC_NOERR ||
        nc_inq_dimid(ncid, "nVertices", &d_verts) != NC_NOERR ||
        nc_inq_dimid(ncid, "maxEdges", &d_maxedges) != NC_NOERR) {
        fprintf(stderr, "mpas: need dimensions nCells, nVertices and maxEdges\n");
        return 0;
    }
    nc_inq_dimlen(ncid, d_cells, &m->ncells);
    nc_inq_dimlen(ncid, d_verts, &m->nvertices);
    size_t maxedges_sz = 0;
    nc_inq_dimlen(ncid, d_maxedges, &maxedges_sz);
    m->max_edges = (int)maxedges_sz;

    m->lon_cell         = read_coord_var(ncid, "lonCell", m->ncells);
    m->lat_cell         = read_coord_var(ncid, "latCell", m->ncells);
    m->n_edges_on_cell  = read_int_var(ncid, "nEdgesOnCell", m->ncells);
    m->vertices_on_cell = read_int_var(ncid, "verticesOnCell",
                                       m->ncells * (size_t)m->max_edges);
    m->lon_vertex       = read_coord_var(ncid, "lonVertex", m->nvertices);
    m->lat_vertex       = read_coord_var(ncid, "latVertex", m->nvertices);

    if (!m->lon_cell || !m->lat_cell || !m->n_edges_on_cell ||
        !m->vertices_on_cell || !m->lon_vertex || !m->lat_vertex) {
        fprintf(stderr, "mpas: missing one of lonCell, latCell, nEdgesOnCell, "
                        "verticesOnCell, lonVertex, latVertex\n");
        return 0;
    }

    /* MPAS indices are 1-based. */
    size_t total = m->ncells * (size_t)m->max_edges;
    for (size_t i = 0; i < total; i++) m->vertices_on_cell[i] -= 1;

    static const char *consumed[] = {
        "lonCell", "latCell", "nEdgesOnCell", "verticesOnCell",
        "lonVertex", "latVertex", NULL
    };
    for (int i = 0; consumed[i]; i++) record_mesh_var(m, consumed[i]);
    return 1;
}

/* ------------------------------------------------------------------ *
 * ICON
 * ------------------------------------------------------------------ */

static int load_icon(int ncid, UnsMesh *m) {
    m->ncells    = var_dim1_len(ncid, "clon");
    m->nvertices = var_dim1_len(ncid, "vlon");
    if (m->ncells == 0 || m->nvertices == 0) {
        fprintf(stderr, "icon: need 1-D variables clon (cells) and vlon (vertices)\n");
        return 0;
    }

    int v_conn;
    if (nc_inq_varid(ncid, "vertex_of_cell", &v_conn) != NC_NOERR) {
        fprintf(stderr, "icon: missing variable 'vertex_of_cell'\n");
        return 0;
    }
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_var(ncid, v_conn, NULL, NULL, &ndims, dimids, NULL);
    if (ndims != 2) {
        fprintf(stderr, "icon: vertex_of_cell must be 2-D, has %d dims\n", ndims);
        return 0;
    }
    size_t d0 = 0, d1 = 0;
    nc_inq_dimlen(ncid, dimids[0], &d0);
    nc_inq_dimlen(ncid, dimids[1], &d1);

    /* ICON writes vertex_of_cell as (nv, cell) -- transposed relative to MPAS's
     * verticesOnCell(nCells, maxEdges). Identify the face axis by length rather
     * than by position so either layout loads. */
    int face_axis;
    if (d1 == m->ncells)      face_axis = 1;
    else if (d0 == m->ncells) face_axis = 0;
    else {
        fprintf(stderr, "icon: vertex_of_cell dims (%zu, %zu) match neither "
                        "the cell count %zu\n", d0, d1, m->ncells);
        return 0;
    }
    m->max_edges = (int)(face_axis == 1 ? d0 : d1);

    long long *raw = read_ll_var(ncid, "vertex_of_cell", d0 * d1);
    m->lon_cell   = read_coord_var(ncid, "clon", m->ncells);
    m->lat_cell   = read_coord_var(ncid, "clat", m->ncells);
    m->lon_vertex = read_coord_var(ncid, "vlon", m->nvertices);
    m->lat_vertex = read_coord_var(ncid, "vlat", m->nvertices);
    if (!raw || !m->lon_cell || !m->lat_cell || !m->lon_vertex || !m->lat_vertex) {
        fprintf(stderr, "icon: missing one of clon, clat, vlon, vlat, vertex_of_cell\n");
        free(raw);
        return 0;
    }

    size_t stride = (size_t)m->max_edges;
    m->vertices_on_cell = malloc(sizeof(int) * m->ncells * stride);
    m->n_edges_on_cell  = malloc(sizeof(int) * m->ncells);
    for (size_t c = 0; c < m->ncells; c++) {
        for (size_t e = 0; e < stride; e++) {
            /* ICON indices are 1-based. */
            long long idx = (face_axis == 1 ? raw[e * m->ncells + c]
                                            : raw[c * stride + e]) - 1;
            int vi = -1;
            if (idx >= 0 && (unsigned long long)idx < (unsigned long long)m->nvertices)
                vi = (int)idx;
            m->vertices_on_cell[c * stride + e] = vi;
        }
        m->n_edges_on_cell[c] = m->max_edges;
    }
    free(raw);

    static const char *consumed[] = {
        "clon", "clat", "vlon", "vlat", "elon", "elat",
        "clon_vertices", "clat_vertices", "vlon_vertices", "vlat_vertices",
        "elon_vertices", "elat_vertices",
        "vertex_of_cell", "edge_of_cell", "adjacent_cell_of_edge",
        "edge_vertices", "neighbor_cell_index", "cells_of_vertex",
        "edges_of_vertex", "vertex_of_vertex", NULL
    };
    for (int i = 0; consumed[i]; i++) record_mesh_var(m, consumed[i]);
    return 1;
}

/* ------------------------------------------------------------------ *
 * CF-UGRID
 * ------------------------------------------------------------------ */

static int load_ugrid(int ncid, UnsMesh *m) {
    int topo = find_var_by_att(ncid, "cf_role", "mesh_topology");
    if (topo < 0) {
        fprintf(stderr, "ugrid: no variable carries cf_role=\"mesh_topology\"\n");
        return 0;
    }
    char topo_name[NC_MAX_NAME + 1] = {0};
    nc_inq_varname(ncid, topo, topo_name);
    record_mesh_var(m, topo_name);

    char att[512];
    char node_lon_name[MESH_MAX_NAME], node_lat_name[MESH_MAX_NAME];
    if (!get_att_text(ncid, topo, "node_coordinates", att, sizeof att) ||
        sscanf(att, "%63s %63s", node_lon_name, node_lat_name) != 2) {
        fprintf(stderr, "ugrid: '%s' has no usable node_coordinates attribute\n",
                topo_name);
        return 0;
    }
    char conn_name[MESH_MAX_NAME];
    if (!get_att_text(ncid, topo, "face_node_connectivity", att, sizeof att) ||
        sscanf(att, "%63s", conn_name) != 1) {
        fprintf(stderr, "ugrid: '%s' has no face_node_connectivity attribute\n",
                topo_name);
        return 0;
    }
    record_mesh_var(m, node_lon_name);
    record_mesh_var(m, node_lat_name);
    record_mesh_var(m, conn_name);

    m->nvertices = var_dim1_len(ncid, node_lon_name);
    if (m->nvertices == 0) {
        fprintf(stderr, "ugrid: node coordinate '%s' is missing or not 1-D\n",
                node_lon_name);
        return 0;
    }

    int v_conn;
    if (nc_inq_varid(ncid, conn_name, &v_conn) != NC_NOERR) {
        fprintf(stderr, "ugrid: connectivity variable '%s' not found\n", conn_name);
        return 0;
    }
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_var(ncid, v_conn, NULL, NULL, &ndims, dimids, NULL);
    if (ndims != 2) {
        fprintf(stderr, "ugrid: '%s' must be 2-D, has %d dims\n", conn_name, ndims);
        return 0;
    }
    size_t d0 = 0, d1 = 0;
    nc_inq_dimlen(ncid, dimids[0], &d0);
    nc_inq_dimlen(ncid, dimids[1], &d1);

    /* UGRID defaults to (n_face, n_max_face_nodes); face_dimension overrides. */
    int face_axis = 0;
    char face_dim_att[MESH_MAX_NAME];
    if (get_att_text(ncid, topo, "face_dimension", att, sizeof att) &&
        sscanf(att, "%63s", face_dim_att) == 1) {
        char dn[NC_MAX_NAME + 1];
        for (int a = 0; a < 2; a++) {
            nc_inq_dimname(ncid, dimids[a], dn);
            if (!strcmp(dn, face_dim_att)) { face_axis = a; break; }
        }
    }
    m->ncells    = (face_axis == 0) ? d0 : d1;
    m->max_edges = (int)((face_axis == 0) ? d1 : d0);

    m->lon_vertex = read_coord_var(ncid, node_lon_name, m->nvertices);
    m->lat_vertex = read_coord_var(ncid, node_lat_name, m->nvertices);
    long long *raw = read_ll_var(ncid, conn_name, d0 * d1);
    if (!m->lon_vertex || !m->lat_vertex || !raw) {
        fprintf(stderr, "ugrid: could not read node coordinates or connectivity\n");
        free(raw);
        return 0;
    }

    /* start_index is 0 by default here, unlike MPAS and ICON. */
    long long start_index = 0;
    nc_get_att_longlong(ncid, v_conn, "start_index", &start_index);
    long long fill_val = 0;
    int has_fill = (nc_get_att_longlong(ncid, v_conn, "_FillValue", &fill_val) == NC_NOERR);

    size_t stride = (size_t)m->max_edges;
    m->vertices_on_cell = malloc(sizeof(int) * m->ncells * stride);
    m->n_edges_on_cell  = malloc(sizeof(int) * m->ncells);
    for (size_t c = 0; c < m->ncells; c++) {
        int n_valid = 0;
        for (size_t e = 0; e < stride; e++) {
            long long v = (face_axis == 0) ? raw[c * stride + e]
                                           : raw[e * m->ncells + c];
            /* Ragged rows are padded with _FillValue; a short face simply stops.
             * The `v < start_index` test also catches an INT64_MIN pad without
             * overflowing the subtraction below. */
            int vi = -1;
            if (!(has_fill && v == fill_val) && v >= start_index) {
                long long idx = v - start_index;
                if ((unsigned long long)idx < (unsigned long long)m->nvertices)
                    vi = (int)idx;
            }
            m->vertices_on_cell[c * stride + e] = vi;
            if (vi >= 0 && (size_t)n_valid == e) n_valid++;
        }
        m->n_edges_on_cell[c] = n_valid;
    }
    free(raw);

    /* Face centers: use the file's own if it advertises them, else derive. */
    char face_lon_name[MESH_MAX_NAME], face_lat_name[MESH_MAX_NAME];
    if (get_att_text(ncid, topo, "face_coordinates", att, sizeof att) &&
        sscanf(att, "%63s %63s", face_lon_name, face_lat_name) == 2 &&
        var_dim1_len(ncid, face_lon_name) == m->ncells) {
        m->lon_cell = read_coord_var(ncid, face_lon_name, m->ncells);
        m->lat_cell = read_coord_var(ncid, face_lat_name, m->ncells);
        record_mesh_var(m, face_lon_name);
        record_mesh_var(m, face_lat_name);
    }
    if (!m->lon_cell || !m->lat_cell) {
        free(m->lon_cell);
        free(m->lat_cell);
        m->lon_cell = m->lat_cell = NULL;
        face_centers_from_nodes(m);
    }

    /* Anything tagged with cf_role is mesh metadata by definition -- that
     * catches n_nodes_per_face and the connectivity arrays without naming them. */
    int nvars = 0;
    nc_inq_nvars(ncid, &nvars);
    for (int v = 0; v < nvars; v++) {
        char role[128], vname[NC_MAX_NAME + 1];
        if (get_att_text(ncid, v, "cf_role", role, sizeof role) &&
            nc_inq_varname(ncid, v, vname) == NC_NOERR) {
            record_mesh_var(m, vname);
        }
    }

    /* Hide the remaining connectivity variables the topology names. */
    static const char *other_conn[] = {
        "edge_node_connectivity", "face_edge_connectivity",
        "face_face_connectivity", "edge_face_connectivity",
        "boundary_node_connectivity", "edge_coordinates", NULL
    };
    for (int i = 0; other_conn[i]; i++) {
        if (get_att_text(ncid, topo, other_conn[i], att, sizeof att)) {
            char a[MESH_MAX_NAME], b[MESH_MAX_NAME];
            int got = sscanf(att, "%63s %63s", a, b);
            if (got >= 1) record_mesh_var(m, a);
            if (got >= 2) record_mesh_var(m, b);
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ *
 * FVCOM
 * ------------------------------------------------------------------ */

/* FVCOM's triangular coastal-ocean mesh. It predates UGRID and carries no
 * cf_role, so it needs its own reader even though the geometry is ordinary:
 * nodes in lon/lat, element centers in lonc/latc, and nv(three, nele) --
 * transposed like ICON's, and 1-based. Degrees throughout.
 *
 * Most FVCOM fields (zeta, temp, salinity) sit on nodes, so they come back
 * flagged var_on_node and get averaged onto elements before rendering. */
static int load_fvcom(int ncid, UnsMesh *m) {
    m->nvertices = var_dim1_len(ncid, "lon");
    m->ncells    = var_dim1_len(ncid, "lonc");
    if (m->nvertices == 0) {
        fprintf(stderr, "fvcom: need a 1-D 'lon' (node longitudes)\n");
        return 0;
    }

    int v_conn;
    if (nc_inq_varid(ncid, "nv", &v_conn) != NC_NOERR) {
        fprintf(stderr, "fvcom: missing connectivity variable 'nv'\n");
        return 0;
    }
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_var(ncid, v_conn, NULL, NULL, &ndims, dimids, NULL);
    if (ndims != 2) {
        fprintf(stderr, "fvcom: nv must be 2-D (three, nele), has %d dims\n", ndims);
        return 0;
    }
    size_t d0 = 0, d1 = 0;
    nc_inq_dimlen(ncid, dimids[0], &d0);
    nc_inq_dimlen(ncid, dimids[1], &d1);

    /* nv is written (three, nele). Without lonc to fix the element count, take
     * whichever axis is not the corner count. */
    if (m->ncells == 0) m->ncells = (d0 == 3) ? d1 : d0;
    int face_axis = (d1 == m->ncells) ? 1 : (d0 == m->ncells) ? 0 : -1;
    if (face_axis < 0) {
        fprintf(stderr, "fvcom: nv dims (%zu, %zu) match neither the element "
                        "count %zu\n", d0, d1, m->ncells);
        return 0;
    }
    m->max_edges = (int)(face_axis == 1 ? d0 : d1);

    long long *raw = read_ll_var(ncid, "nv", d0 * d1);
    m->lon_vertex  = read_coord_var(ncid, "lon", m->nvertices);
    m->lat_vertex  = read_coord_var(ncid, "lat", m->nvertices);
    if (!raw || !m->lon_vertex || !m->lat_vertex) {
        fprintf(stderr, "fvcom: could not read lon, lat or nv\n");
        free(raw);
        return 0;
    }

    size_t stride = (size_t)m->max_edges;
    m->vertices_on_cell = malloc(sizeof(int) * m->ncells * stride);
    m->n_edges_on_cell  = malloc(sizeof(int) * m->ncells);
    for (size_t c = 0; c < m->ncells; c++) {
        int n_valid = 0;
        for (size_t e = 0; e < stride; e++) {
            /* FVCOM indices are 1-based. */
            long long idx = (face_axis == 1 ? raw[e * m->ncells + c]
                                            : raw[c * stride + e]) - 1;
            int vi = -1;
            if (idx >= 0 && (unsigned long long)idx < (unsigned long long)m->nvertices)
                vi = (int)idx;
            m->vertices_on_cell[c * stride + e] = vi;
            if (vi >= 0 && (size_t)n_valid == e) n_valid++;
        }
        m->n_edges_on_cell[c] = n_valid;
    }
    free(raw);

    m->lon_cell = read_coord_var(ncid, "lonc", m->ncells);
    m->lat_cell = read_coord_var(ncid, "latc", m->ncells);
    if (!m->lon_cell || !m->lat_cell) {
        free(m->lon_cell);
        free(m->lat_cell);
        m->lon_cell = m->lat_cell = NULL;
        face_centers_from_nodes(m);
    }

    static const char *consumed[] = {
        "lon", "lat", "lonc", "latc", "x", "y", "xc", "yc", "nv",
        "nbe", "ntsn", "nbsn", "ntve", "nbve", "a1u", "a2u",
        "aw0", "awx", "awy", "art1", "art2", "siglay", "siglev",
        "partition", "nprocs", "iint", NULL
    };
    for (int i = 0; consumed[i]; i++) record_mesh_var(m, consumed[i]);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Cubed sphere (GEOS-CS, FV3)
 * ------------------------------------------------------------------ */

/* Corners of one structured face, reconstructed from its ny*nx cell centers
 * into (ny+1)*(nx+1) corners.
 *
 * Most real cubed-sphere output ships centers and nothing else: NASA's
 * operational GEOS-IT keeps geometry in a separate gridspec file, and FV3's
 * oro/sfc files carry only geolon/geolat. A polygon renderer needs boundaries
 * the same way pcolormesh does, so we rebuild them rather than refuse the file.
 *
 * The arithmetic runs on 3-D unit vectors, not on lon/lat. That is not a
 * stylistic preference: averaging longitudes breaks across the date line and
 * degenerates at the poles -- exactly what forces face_centers_from_nodes to
 * unwrap before it can take a mean. In Cartesian space neither case is special.
 *
 * A corner is the mean of the four centers around it. The outer ring has no
 * four, so the center grid is first extended by one ghost row and column on
 * each side by linear extrapolation (g = 2a - b). Without that the edge corners
 * would collapse onto the edge centers and every face would render a half-cell
 * too small. Accuracy is O(h^2); at c180 (~55 km cells) that is far below one
 * pixel of a default 1024x512 render.
 *
 * Each face extrapolates on its own, so two faces sharing an edge can place
 * that edge O(h^2) apart. It is sub-cell and invisible, and it is the only
 * reason a stitched globe is not exactly watertight. */
static void unit_vec(double lon, double lat, double *o) {
    o[0] = cos(lat) * cos(lon);
    o[1] = cos(lat) * sin(lon);
    o[2] = sin(lat);
}

static void normalize3(double *v) {
    double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 0.0) { v[0] /= n; v[1] /= n; v[2] /= n; }
}

static int corners_from_centers(const double *clon, const double *clat,
                                size_t ny, size_t nx,
                                double *olon, double *olat) {
    if (ny < 2 || nx < 2) return 0;
    const size_t ex = nx + 2, ey = ny + 2;
    double *e = malloc(sizeof(double) * ex * ey * 3);
    if (!e) return 0;
#define EV(J, I) (&e[((size_t)(J) * ex + (size_t)(I)) * 3])

    for (size_t j = 0; j < ny; j++)
        for (size_t i = 0; i < nx; i++)
            unit_vec(clon[j * nx + i], clat[j * nx + i], EV(j + 1, i + 1));

    /* Ghost columns first, then ghost rows -- the row pass reads the columns it
     * just filled, which is what puts sensible values in the four diagonal
     * ghost cells that the corner-most corners need. */
    for (size_t j = 1; j <= ny; j++) {
        for (int k = 0; k < 3; k++) {
            EV(j, 0)[k]      = 2.0 * EV(j, 1)[k]      - EV(j, 2)[k];
            EV(j, ex - 1)[k] = 2.0 * EV(j, ex - 2)[k] - EV(j, ex - 3)[k];
        }
        normalize3(EV(j, 0));
        normalize3(EV(j, ex - 1));
    }
    for (size_t i = 0; i < ex; i++) {
        for (int k = 0; k < 3; k++) {
            EV(0, i)[k]      = 2.0 * EV(1, i)[k]      - EV(2, i)[k];
            EV(ey - 1, i)[k] = 2.0 * EV(ey - 2, i)[k] - EV(ey - 3, i)[k];
        }
        normalize3(EV(0, i));
        normalize3(EV(ey - 1, i));
    }

    for (size_t j = 0; j <= ny; j++) {
        for (size_t i = 0; i <= nx; i++) {
            double s[3];
            for (int k = 0; k < 3; k++)
                s[k] = EV(j, i)[k] + EV(j, i + 1)[k]
                     + EV(j + 1, i)[k] + EV(j + 1, i + 1)[k];
            normalize3(s);
            double z = s[2] > 1.0 ? 1.0 : (s[2] < -1.0 ? -1.0 : s[2]);
            olon[j * (nx + 1) + i] = atan2(s[1], s[0]);
            olat[j * (nx + 1) + i] = asin(z);
        }
    }
#undef EV
    free(e);
    return 1;
}

/* A cubed sphere is six structured quad faces, not an unstructured mesh -- and
 * it carries no connectivity array at all. Corners live in
 * corner_lons/corner_lats(nf, YC, XC) with YC = Y+1, so each cell's four nodes
 * are implicit in the index arithmetic, and the mesh becomes an ordinary quad
 * UnsMesh once they are written out.
 *
 * Cells are flattened (face, y, x), which is the order nc_read_slab produces
 * when it reads the (nf, Ydim, Xdim) block whole. Keep the two in step. */
static int load_cubedsphere(int ncid, UnsMesh *m) {
    /* GEOS stacks the six faces in a leading nf dimension and calls the arrays
     * corner_lons/lons. FV3 writes one file per tile -- so no face dimension at
     * all -- and calls them grid_lon/grid_lont. Same geometry either way, so an
     * FV3 tile is just nf = 1, covering a sixth of the globe.
     *
     * (FV3's other file, C48_grid.tileN.nc, is the *supergrid* at twice the
     * model resolution; grid_spec.tileN.nc is the one with model cells.) */
    /* Geometry source, best first: real corners when the file carries them,
     * otherwise centers for corners_from_centers to rebuild the boundaries
     * from. Operational GEOS and FV3's oro/sfc files only ever ship centers. */
    const char *c_lon = NULL, *c_lat = NULL;   /* corners; NULL => derive them */
    const char *f_lon = NULL, *f_lat = NULL;   /* centers */
    if (have_var(ncid, "corner_lons")) {          /* GEOS / GCHP history      */
        c_lon = "corner_lons"; c_lat = "corner_lats";
        f_lon = "lons";        f_lat = "lats";
    } else if (have_var(ncid, "grid_lon")) {      /* FV3 grid_spec            */
        c_lon = "grid_lon";  c_lat = "grid_lat";
        f_lon = "grid_lont"; f_lat = "grid_latt";
    } else if (have_var(ncid, "lons")) {          /* GEOS-IT: centers only    */
        f_lon = "lons";      f_lat = "lats";
    } else if (have_var(ncid, "grid_lont")) {     /* FV3 grid_spec, no corners*/
        f_lon = "grid_lont"; f_lat = "grid_latt";
    } else if (have_var(ncid, "geolon")) {        /* FV3 oro / sfc            */
        f_lon = "geolon";    f_lat = "geolat";
    } else {
        fprintf(stderr, "cs: need corners (corner_lons, grid_lon) or centers to "
                        "derive them from (lons, grid_lont, geolon)\n");
        return 0;
    }

    /* Shape comes from whichever array we actually have: a corner array is one
     * longer than the cell count in each direction, a center array is not. */
    const char *shape_var = c_lon ? c_lon : f_lon;
    int v_shape;
    if (nc_inq_varid(ncid, shape_var, &v_shape) != NC_NOERR) return 0;
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS];
    nc_inq_var(ncid, v_shape, NULL, NULL, &ndims, dimids, NULL);
    if (ndims != 2 && ndims != 3) {
        fprintf(stderr, "cs: %s must be 2-D (y, x) or 3-D (nf, y, x), has %d dims\n",
                shape_var, ndims);
        return 0;
    }
    size_t nf = 1, d0 = 0, d1 = 0;
    if (ndims == 3) {
        nc_inq_dimlen(ncid, dimids[0], &nf);
        nc_inq_dimlen(ncid, dimids[1], &d0);
        nc_inq_dimlen(ncid, dimids[2], &d1);
    } else {
        nc_inq_dimlen(ncid, dimids[0], &d0);
        nc_inq_dimlen(ncid, dimids[1], &d1);
    }
    size_t ny = c_lon ? d0 - 1 : d0;
    size_t nx = c_lon ? d1 - 1 : d1;
    if (nf == 0 || d0 < 2 || d1 < 2 || ny < 1 || nx < 1) {
        fprintf(stderr, "cs: %s dims (%zu, %zu, %zu) are too small\n",
                shape_var, nf, d0, d1);
        return 0;
    }
    size_t ncy = ny + 1, ncx = nx + 1;

    m->nvertices = nf * ncy * ncx;
    m->ncells    = nf * ny * nx;
    m->max_edges = 5;   /* 4 corners, plus one slot for the polar split below */

    if (c_lon) {
        m->lon_vertex = read_coord_var(ncid, c_lon, m->nvertices);
        m->lat_vertex = read_coord_var(ncid, c_lat, m->nvertices);
        if (!m->lon_vertex || !m->lat_vertex) {
            fprintf(stderr, "cs: could not read %s / %s\n", c_lon, c_lat);
            return 0;
        }
    } else {
        double *clon = read_coord_var(ncid, f_lon, m->ncells);
        double *clat = read_coord_var(ncid, f_lat, m->ncells);
        m->lon_vertex = malloc(sizeof(double) * m->nvertices);
        m->lat_vertex = malloc(sizeof(double) * m->nvertices);
        int ok = clon && clat && m->lon_vertex && m->lat_vertex;
        for (size_t f = 0; ok && f < nf; f++)
            ok = corners_from_centers(clon + f * ny * nx, clat + f * ny * nx,
                                      ny, nx,
                                      m->lon_vertex + f * ncy * ncx,
                                      m->lat_vertex + f * ncy * ncx);
        if (!ok) {
            free(clon);
            free(clat);
            fprintf(stderr, "cs: could not derive corners from %s / %s\n",
                    f_lon, f_lat);
            return 0;
        }
        /* Keep them: these *are* the cell centers, so the tail below has
         * nothing left to read (and geolon/geolat would otherwise be read, and
         * unit-sniffed, a second time). */
        m->lon_cell = clon;
        m->lat_cell = clat;
        fprintf(stderr, "cs: no corner coordinates in file; derived them from "
                        "%s/%s centers\n", f_lon, f_lat);
    }

    size_t stride = (size_t)m->max_edges;
    m->vertices_on_cell = malloc(sizeof(int) * m->ncells * stride);
    m->n_edges_on_cell  = malloc(sizeof(int) * m->ncells);
    size_t c = 0;
    for (size_t f = 0; f < nf; f++) {
        size_t base = f * ncy * ncx;
        for (size_t j = 0; j < ny; j++) {
            for (size_t i = 0; i < nx; i++) {
                m->vertices_on_cell[c * stride + 0] = (int)(base +  j      * ncx + i);
                m->vertices_on_cell[c * stride + 1] = (int)(base +  j      * ncx + i + 1);
                m->vertices_on_cell[c * stride + 2] = (int)(base + (j + 1) * ncx + i + 1);
                m->vertices_on_cell[c * stride + 3] = (int)(base + (j + 1) * ncx + i);
                m->vertices_on_cell[c * stride + 4] = -1;
                m->n_edges_on_cell[c] = 4;
                c++;
            }
        }
    }

    /* A node sitting exactly on a pole is one point, but in the equirectangular
     * projection a pole is the whole top (or bottom) edge of the map. Drawn as
     * given, every cell touching a pole collapses to a triangle and leaves a
     * wedge of the map unpainted -- 4% of the image on a c12 grid. Split that
     * corner in two, each copy carrying the longitude of its neighbour in the
     * ring, so the cell spans the edge instead of converging to a point. */
    /* Tolerance, not equality. A corner stored in the file lands exactly on the
     * pole, but one reconstructed by corners_from_centers only gets within
     * ~1e-8 rad of it, and float32 corner arrays are no better. Miss the test
     * and the cell silently collapses to a triangle, leaving the polar wedge
     * this split exists to prevent. 1e-6 rad is ~6 m on Earth -- three orders
     * below even a c3072 cell, so it cannot capture a genuine non-polar corner. */
    const double POLE_LAT = MESH_PI / 2.0 - 1e-6;
    size_t n_polar = 0;
    for (size_t k = 0; k < m->ncells; k++) {
        for (size_t e = 0; e < 4; e++) {
            int vi = m->vertices_on_cell[k * stride + e];
            if (vi >= 0 && fabs(m->lat_vertex[vi]) >= POLE_LAT) { n_polar++; break; }
        }
    }
    if (n_polar) {
        size_t grown = m->nvertices + 2 * n_polar;
        double *nlon = realloc(m->lon_vertex, sizeof(double) * grown);
        double *nlat = realloc(m->lat_vertex, sizeof(double) * grown);
        if (!nlon || !nlat) {
            fprintf(stderr, "cs: out of memory splitting %zu polar corners\n", n_polar);
            m->lon_vertex = nlon ? nlon : m->lon_vertex;
            m->lat_vertex = nlat ? nlat : m->lat_vertex;
            return 0;
        }
        m->lon_vertex = nlon;
        m->lat_vertex = nlat;

        size_t nv = m->nvertices;
        for (size_t k = 0; k < m->ncells; k++) {
            int ring[4];
            int p = -1;
            for (int e = 0; e < 4; e++) {
                ring[e] = m->vertices_on_cell[k * stride + e];
                if (p < 0 && ring[e] >= 0 &&
                    fabs(m->lat_vertex[ring[e]]) >= POLE_LAT) p = e;
            }
            if (p < 0) continue;
            int prev = ring[(p + 3) % 4], next = ring[(p + 1) % 4];
            double lat_pole = m->lat_vertex[ring[p]];
            size_t a = nv++, b = nv++;
            m->lon_vertex[a] = m->lon_vertex[prev];  m->lat_vertex[a] = lat_pole;
            m->lon_vertex[b] = m->lon_vertex[next];  m->lat_vertex[b] = lat_pole;

            int out[5];
            int n = 0;
            for (int e = 0; e < 4; e++) {
                if (e == p) { out[n++] = (int)a; out[n++] = (int)b; }
                else        { out[n++] = ring[e]; }
            }
            for (int e = 0; e < 5; e++) m->vertices_on_cell[k * stride + e] = out[e];
            m->n_edges_on_cell[k] = 5;
        }
        m->nvertices = nv;
    }

    /* Cell centers: already in hand when the corners were derived from them,
     * otherwise read from the file, otherwise averaged back off the corners. */
    if (!m->lon_cell || !m->lat_cell) {
        m->lon_cell = read_coord_var(ncid, f_lon, m->ncells);
        m->lat_cell = read_coord_var(ncid, f_lat, m->ncells);
    }
    if (!m->lon_cell || !m->lat_cell) {
        free(m->lon_cell);
        free(m->lat_cell);
        m->lon_cell = m->lat_cell = NULL;
        face_centers_from_nodes(m);
    }

    static const char *consumed[] = {
        /* GEOS */
        "lons", "lats", "corner_lons", "corner_lats",
        "Xdim", "Ydim", "XCdim", "YCdim", "nf", "ncontact",
        "contacts", "anchor", "cubed_sphere", "orientation",
        /* FV3 */
        "grid_lon", "grid_lat", "grid_lont", "grid_latt",
        "grid_x", "grid_y", "grid_xt", "grid_yt", "tile",
        "geolon", "geolat", NULL
    };
    for (int i = 0; consumed[i]; i++) record_mesh_var(m, consumed[i]);
    return 1;
}

/* ------------------------------------------------------------------ *
 * Dispatch
 * ------------------------------------------------------------------ */

/* Explicit self-description beats name heuristics, so UGRID is tried first. */
static const char *detect_model(int ncid) {
    if (find_var_by_att(ncid, "cf_role", "mesh_topology") >= 0) return "ugrid";
    if (have_dim(ncid, "nCells") && have_var(ncid, "verticesOnCell")) return "mpas";
    if (have_var(ncid, "clon") && have_var(ncid, "vertex_of_cell")) return "icon";
    if (have_var(ncid, "corner_lons") && have_var(ncid, "corner_lats")) return "cs";
    if (have_var(ncid, "grid_lon") && have_var(ncid, "grid_lont")) return "cs";
    /* Centers-only cubed sphere. Operational GEOS keeps geometry in a separate
     * gridspec file, so its data files carry lons/lats and no corners at all;
     * FV3's oro/sfc files carry geolon/geolat. Insist on the nf dimension or the
     * geolon pair -- a bare 2-D lat/lon pair is a rectilinear or curvilinear
     * grid, which is ncview's job rather than ours. */
    if (find_var_by_att(ncid, "grid_mapping_name", "gnomonic cubed-sphere") >= 0)
        return "cs";
    if (have_dim(ncid, "nf") && have_var(ncid, "lons") && have_var(ncid, "lats"))
        return "cs";
    if (have_var(ncid, "geolon") && have_var(ncid, "geolat")) return "cs";
    if (have_var(ncid, "nv") && have_var(ncid, "lonc")) return "fvcom";
    return NULL;
}

UnsMesh *mesh_load(int ncid, const char *forced) {
    const char *model = forced;
    if (!model || !*model || !strcmp(model, "auto")) {
        model = detect_model(ncid);
        if (!model) {
            fprintf(stderr,
                    "no supported mesh found: expected a UGRID mesh_topology "
                    "variable, MPAS verticesOnCell, ICON vertex_of_cell, "
                    "cubed-sphere corner_lons or lons/geolon centers, or "
                    "FVCOM nv\n");
            return NULL;
        }
    }

    UnsMesh *m = calloc(1, sizeof(*m));
    int ok;
    if (!strcmp(model, "ugrid"))      ok = load_ugrid(ncid, m);
    else if (!strcmp(model, "mpas"))  ok = load_mpas(ncid, m);
    else if (!strcmp(model, "icon"))  ok = load_icon(ncid, m);
    else if (!strcmp(model, "cs"))    ok = load_cubedsphere(ncid, m);
    else if (!strcmp(model, "fvcom")) ok = load_fvcom(ncid, m);
    else {
        fprintf(stderr,
                "unknown model '%s' (want auto, mpas, icon, ugrid, cs or fvcom)\n",
                model);
        ok = 0;
    }
    if (!ok) {
        mesh_free(m);
        return NULL;
    }

    /* Keep the literal, not the caller's buffer. */
    if (!strcmp(model, "ugrid"))     m->model = "ugrid";
    else if (!strcmp(model, "mpas")) m->model = "mpas";
    else if (!strcmp(model, "cs"))   m->model = "cs";
    else if (!strcmp(model, "fvcom")) m->model = "fvcom";
    else                             m->model = "icon";

    compute_extent(m);
    return m;
}

/* Several cubed-sphere tile files stitched into a single mesh.
 *
 * A cubed sphere reaches us one of two ways and both are ordinary: GEOS puts
 * all six faces in one file behind a leading nf dimension, FV3 writes one file
 * per tile. Without this, six tile files look like six time steps and a global
 * render is impossible.
 *
 * Tiles are concatenated in the order given, so the global cell index runs
 * tile-major and then (y, x) within a tile -- exactly the order
 * multinc_read_slab assembles the data in. Change one and you must change the
 * other. */
UnsMesh *mesh_load_tiles(const int *ncids, int n, const char *forced) {
    if (n <= 0) return NULL;
    if (n == 1) return mesh_load(ncids[0], forced);

    UnsMesh **part = calloc((size_t)n, sizeof *part);
    if (!part) return NULL;
    size_t tot_c = 0, tot_v = 0;
    int max_edges = 0, ok = 1;
    for (int i = 0; i < n && ok; i++) {
        part[i] = mesh_load(ncids[i], forced);
        if (!part[i]) { ok = 0; break; }
        if (strcmp(part[i]->model, "cs")) {
            fprintf(stderr, "--tiles: file %d holds a '%s' mesh; only "
                            "cubed-sphere tiles can be stitched\n",
                    i + 1, part[i]->model);
            ok = 0;
            break;
        }
        tot_c += part[i]->ncells;
        tot_v += part[i]->nvertices;
        if (part[i]->max_edges > max_edges) max_edges = part[i]->max_edges;
    }

    UnsMesh *m = ok ? calloc(1, sizeof *m) : NULL;
    if (m) {
        m->model            = "cs";
        m->ncells           = tot_c;
        m->nvertices        = tot_v;
        m->max_edges        = max_edges;
        m->lon_cell         = malloc(sizeof(double) * tot_c);
        m->lat_cell         = malloc(sizeof(double) * tot_c);
        m->n_edges_on_cell  = malloc(sizeof(int) * tot_c);
        m->vertices_on_cell = malloc(sizeof(int) * tot_c * (size_t)max_edges);
        m->lon_vertex       = malloc(sizeof(double) * tot_v);
        m->lat_vertex       = malloc(sizeof(double) * tot_v);
        ok = m->lon_cell && m->lat_cell && m->n_edges_on_cell &&
             m->vertices_on_cell && m->lon_vertex && m->lat_vertex;
    }
    if (!ok) {
        mesh_free(m);
        for (int i = 0; i < n; i++) mesh_free(part[i]);
        free(part);
        return NULL;
    }

    size_t co = 0, vo = 0;
    for (int i = 0; i < n; i++) {
        const UnsMesh *p = part[i];
        memcpy(m->lon_cell   + co, p->lon_cell,   sizeof(double) * p->ncells);
        memcpy(m->lat_cell   + co, p->lat_cell,   sizeof(double) * p->ncells);
        memcpy(m->lon_vertex + vo, p->lon_vertex, sizeof(double) * p->nvertices);
        memcpy(m->lat_vertex + vo, p->lat_vertex, sizeof(double) * p->nvertices);
        for (size_t c = 0; c < p->ncells; c++) {
            m->n_edges_on_cell[co + c] = p->n_edges_on_cell[c];
            for (int e = 0; e < max_edges; e++) {
                int v = (e < p->max_edges)
                      ? p->vertices_on_cell[c * (size_t)p->max_edges + (size_t)e]
                      : -1;
                /* Vertex ids are file-local; rebase them onto the merged array. */
                m->vertices_on_cell[(co + c) * (size_t)max_edges + (size_t)e] =
                    (v >= 0) ? v + (int)vo : -1;
            }
        }
        co += p->ncells;
        vo += p->nvertices;
    }
    /* Every tile reports the same mesh-variable names, so take them once --
     * six identical copies would overflow MESH_MAX_MESHVARS on their own. */
    for (int k = 0; k < part[0]->n_mesh_vars; k++)
        record_mesh_var(m, part[0]->mesh_vars[k]);

    for (int i = 0; i < n; i++) mesh_free(part[i]);
    free(part);

    compute_extent(m);
    fprintf(stderr, "stitched %d cubed-sphere tiles: %zu cells, %zu vertices\n",
            n, m->ncells, m->nvertices);
    return m;
}

void mesh_free(UnsMesh *m) {
    if (!m) return;
    free(m->lon_cell);
    free(m->lat_cell);
    free(m->n_edges_on_cell);
    free(m->vertices_on_cell);
    free(m->lon_vertex);
    free(m->lat_vertex);
    free(m);
}

void mesh_node_to_face(const UnsMesh *m, const double *node_vals, double *face_vals) {
    for (size_t c = 0; c < m->ncells; c++) {
        int n = m->n_edges_on_cell[c];
        double sum = 0.0;
        int used = 0;
        for (int e = 0; e < n; e++) {
            int vi = m->vertices_on_cell[c * (size_t)m->max_edges + (size_t)e];
            if (vi < 0 || (size_t)vi >= m->nvertices) continue;
            double v = node_vals[vi];
            if (isnan(v)) continue;
            sum += v;
            used++;
        }
        face_vals[c] = used ? sum / used : NAN;
    }
}

void mesh_probe_dims(int ncid, char *face_dim, char *node_dim, size_t cap) {
    face_dim[0] = '\0';
    node_dim[0] = '\0';

    int topo = find_var_by_att(ncid, "cf_role", "mesh_topology");
    if (topo >= 0) {
        char att[512], name[MESH_MAX_NAME], dn[NC_MAX_NAME + 1];
        int vid, ndims = 0;
        int dimids[NC_MAX_VAR_DIMS];

        if (get_att_text(ncid, topo, "face_dimension", att, sizeof att) &&
            sscanf(att, "%63s", name) == 1) {
            snprintf(face_dim, cap, "%s", name);
        } else if (get_att_text(ncid, topo, "face_node_connectivity", att, sizeof att) &&
                   sscanf(att, "%63s", name) == 1 &&
                   nc_inq_varid(ncid, name, &vid) == NC_NOERR &&
                   nc_inq_var(ncid, vid, NULL, NULL, &ndims, dimids, NULL) == NC_NOERR &&
                   ndims == 2 &&
                   nc_inq_dimname(ncid, dimids[0], dn) == NC_NOERR) {
            snprintf(face_dim, cap, "%s", dn);
        }
        if (get_att_text(ncid, topo, "node_coordinates", att, sizeof att) &&
            sscanf(att, "%63s", name) == 1 &&
            nc_inq_varid(ncid, name, &vid) == NC_NOERR &&
            nc_inq_var(ncid, vid, NULL, NULL, &ndims, dimids, NULL) == NC_NOERR &&
            ndims == 1 &&
            nc_inq_dimname(ncid, dimids[0], dn) == NC_NOERR) {
            snprintf(node_dim, cap, "%s", dn);
        }
        if (face_dim[0] || node_dim[0]) return;
    }

    /* Cubed sphere: the mesh index spans every dim of the cell-center array --
     * three for GEOS's lons(nf, Y, X), two for an FV3 tile's grid_lont(y, x) --
     * so report them space-separated rather than picking one. */
    {
        /* GEOS calls the centers lons; FV3's grid_spec calls them grid_lont,
         * and its oro/sfc files carry geolon instead. */
        const char *centers = have_var(ncid, "lons")      ? "lons"
                            : have_var(ncid, "grid_lont") ? "grid_lont"
                            : have_var(ncid, "geolon")    ? "geolon" : NULL;
        int vid, ndims = 0;
        int dimids[NC_MAX_VAR_DIMS];
        if (centers &&
            nc_inq_varid(ncid, centers, &vid) == NC_NOERR &&
            nc_inq_var(ncid, vid, NULL, NULL, &ndims, dimids, NULL) == NC_NOERR &&
            (ndims == 2 || ndims == 3)) {
            char nm[NC_MAX_NAME + 1];
            size_t used = 0;
            face_dim[0] = '\0';
            for (int d = 0; d < ndims; d++) {
                if (nc_inq_dimname(ncid, dimids[d], nm) != NC_NOERR) { used = 0; break; }
                used += (size_t)snprintf(face_dim + used, cap - used,
                                         d ? " %s" : "%s", nm);
            }
            if (used) return;
        }
    }

    /* No topology variable: fall back to the conventional spellings. Node names
     * are only the ones used by node-centered formats -- MPAS's nVertices and
     * ICON's vertex are deliberately absent, since those models put fields on
     * cells only and reporting them would fill the picker with mesh metadata. */
    static const char *faces[] = {
        "nCells", "cell", "n_face", "nMesh2_face", "nele", "elem", NULL
    };
    static const char *nodes[] = {
        "n_node", "nMesh2_node", "node", "nod2", NULL
    };
    int did;
    for (int i = 0; faces[i]; i++) {
        if (nc_inq_dimid(ncid, faces[i], &did) == NC_NOERR) {
            snprintf(face_dim, cap, "%s", faces[i]);
            break;
        }
    }
    for (int i = 0; nodes[i]; i++) {
        if (nc_inq_dimid(ncid, nodes[i], &did) == NC_NOERR) {
            snprintf(node_dim, cap, "%s", nodes[i]);
            break;
        }
    }
}
