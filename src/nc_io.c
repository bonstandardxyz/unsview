#include "nc_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

static int find_dim(int ncid, const char *name, int *out) {
    int id;
    if (nc_inq_dimid(ncid, name, &id) == NC_NOERR) {
        *out = id;
        return 1;
    }
    return 0;
}

NcFile *nc_open_file(const char *path, const char *face_hint, const char *node_hint) {
    int ncid;
    int rc = nc_open(path, NC_NOWRITE, &ncid);
    if (rc != NC_NOERR) {
        fprintf(stderr, "netcdf open '%s': %s\n", path, nc_strerror(rc));
        return NULL;
    }

    NcFile *f = calloc(1, sizeof(*f));
    f->ncid = ncid;
    f->time_dim_id = -1;
    f->level_dim_id = -1;

    /* Which dimension indexes the mesh differs per model (nCells, cell, an
     * arbitrary UGRID name...), so ask mesh.c rather than hardcoding one. */
    f->n_ncells_dims = 0;
    f->nnodes_dim_id = -1;
    char face_dim[4 * (NC_MAX_NAME + 1)], node_dim[NC_MAX_NAME + 1];
    mesh_probe_dims(ncid, face_dim, node_dim, sizeof face_dim);

    /* The face index may span several dimensions, reported space-separated.
     * Try the grid file's names first, then this file's own: an FV3 grid_spec
     * says "grid_yt grid_xt" while its oro/sfc files spell the same mesh
     * "lat lon", so a hint that does not match must not be fatal. */
    char face_buf[4 * (NC_MAX_NAME + 1)];
    for (int attempt = 0; attempt < 2 && f->n_ncells_dims == 0; attempt++) {
        const char *src = (attempt == 0 && face_hint && *face_hint) ? face_hint
                        : (attempt == 0) ? NULL : face_dim;
        if (!src || !*src) continue;
        snprintf(face_buf, sizeof face_buf, "%s", src);
        for (char *tok = strtok(face_buf, " "); tok; tok = strtok(NULL, " ")) {
            int d;
            if (f->n_ncells_dims >= MESH_MAX_GRID_DIMS) break;
            if (!find_dim(ncid, tok, &d)) { f->n_ncells_dims = 0; break; }
            f->ncells_dim_ids[f->n_ncells_dims++] = d;
        }
    }

    if (node_hint && *node_hint) find_dim(ncid, node_hint, &f->nnodes_dim_id);
    if (f->nnodes_dim_id < 0 && node_dim[0]) find_dim(ncid, node_dim, &f->nnodes_dim_id);
    if (f->n_ncells_dims == 0 && f->nnodes_dim_id < 0) {
        fprintf(stderr,
                "'%s': no unstructured mesh dimension found (looked for a UGRID "
                "mesh_topology, MPAS nCells, ICON cell, cubed-sphere corner_lons)\n",
                path);
        nc_close(ncid);
        free(f);
        return NULL;
    }
    f->ncells = f->n_ncells_dims ? 1 : 0;
    for (int g = 0; g < f->n_ncells_dims; g++) {
        size_t len = 0;
        nc_inq_dimlen(ncid, f->ncells_dim_ids[g], &len);
        f->ncells *= len;
    }
    if (f->nnodes_dim_id >= 0) nc_inq_dimlen(ncid, f->nnodes_dim_id, &f->nnodes);

    if (find_dim(ncid, "Time", &f->time_dim_id) ||
        find_dim(ncid, "time", &f->time_dim_id)) {
        nc_inq_dimlen(ncid, f->time_dim_id, &f->ntime);
    }

    /* nVertLevels is MPAS; the rest cover ICON and the ocean/UGRID models. */
    static const char *LEVEL_DIMS[] = {
        "nVertLevels", "nVertLevelsP1", "height", "height_2",
        "lev", "level", "nz", "nz1", "depth", NULL
    };
    for (int i = 0; LEVEL_DIMS[i]; i++) {
        if (find_dim(ncid, LEVEL_DIMS[i], &f->level_dim_id)) {
            nc_inq_dimlen(ncid, f->level_dim_id, &f->nlevels);
            break;
        }
    }

    int nvars = 0;
    nc_inq_nvars(ncid, &nvars);

    f->var_ids = calloc(nvars, sizeof(int));
    f->var_names = calloc(nvars, sizeof(*f->var_names));
    f->var_has_time = calloc(nvars, sizeof(int));
    f->var_has_level = calloc(nvars, sizeof(int));
    f->var_on_node = calloc(nvars, sizeof(int));

    /* MPAS mesh-metadata variables that share the nCells dim but are not
     * plottable fields. Hide them from the variable picker. */
    static const char *MESH_VARS[] = {
        "lonCell", "latCell", "xCell", "yCell", "zCell",
        "areaCell", "meshDensity", "indexToCellID", "cellsOnCell",
        "edgesOnCell", "verticesOnCell", "nEdgesOnCell",
        "cellsOnEdge", "verticesOnEdge", "edgesOnVertex",
        "cellsOnVertex", "kiteAreasOnVertex",
        "fEdge", "fVertex",
        "bdyMaskCell",
        "localVerticalUnitVectors", "cellTangentPlane",
        "defc_a", "defc_b",
        "cell_gradient_coef_x", "cell_gradient_coef_y",
        "coeffs_reconstruct",
        NULL
    };

    for (int v = 0; v < nvars; v++) {
        char name[NC_MAX_NAME + 1] = {0};
        int ndims = 0;
        int dimids[NC_MAX_VAR_DIMS] = {0};
        nc_type t;
        nc_inq_var(ncid, v, name, &t, &ndims, dimids, NULL);

        int n_grid_seen = 0, has_nodes = 0, has_time = 0, has_level = 0;
        for (int d = 0; d < ndims; d++) {
            for (int g = 0; g < f->n_ncells_dims; g++)
                if (dimids[d] == f->ncells_dim_ids[g]) { n_grid_seen++; break; }
            if (f->nnodes_dim_id >= 0 && dimids[d] == f->nnodes_dim_id) has_nodes  = 1;
            if (f->time_dim_id   >= 0 && dimids[d] == f->time_dim_id)   has_time   = 1;
            if (f->level_dim_id  >= 0 && dimids[d] == f->level_dim_id)  has_level  = 1;
        }
        /* A face field must carry *every* dimension that indexes the mesh. */
        int has_ncells = (f->n_ncells_dims > 0 && n_grid_seen == f->n_ncells_dims);
        /* Faces win when a variable somehow spans both. */
        if (!has_ncells && !has_nodes) continue;
        if (t != NC_FLOAT && t != NC_DOUBLE && t != NC_INT &&
            t != NC_SHORT && t != NC_BYTE) continue;

        int is_mesh = 0;
        for (int k = 0; MESH_VARS[k]; k++) {
            if (!strcmp(name, MESH_VARS[k])) { is_mesh = 1; break; }
        }
        if (is_mesh) continue;

        int idx = f->n_vars_plottable++;
        f->var_ids[idx] = v;
        strncpy(f->var_names[idx], name, NC_MAX_NAME);
        f->var_has_time[idx] = has_time;
        f->var_has_level[idx] = has_level;
        f->var_on_node[idx] = (!has_ncells && has_nodes);
    }

    /* Sort plottable variables A-Z */
    if (f->n_vars_plottable > 1) {
        int n = f->n_vars_plottable;
        int *order = malloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = 1; i < n; i++) {
            int key = order[i];
            int j = i - 1;
            while (j >= 0 && strcmp(f->var_names[order[j]], f->var_names[key]) > 0) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = key;
        }
        int *tmp_ids    = malloc((size_t)n * sizeof(int));
        char (*tmp_names)[NC_MAX_NAME + 1] = malloc((size_t)n * sizeof(*f->var_names));
        int *tmp_time   = malloc((size_t)n * sizeof(int));
        int *tmp_level  = malloc((size_t)n * sizeof(int));
        int *tmp_node   = malloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++) {
            tmp_ids[i]   = f->var_ids[order[i]];
            memcpy(tmp_names[i], f->var_names[order[i]], NC_MAX_NAME + 1);
            tmp_time[i]  = f->var_has_time[order[i]];
            tmp_level[i] = f->var_has_level[order[i]];
            tmp_node[i]  = f->var_on_node[order[i]];
        }
        memcpy(f->var_ids,       tmp_ids,    (size_t)n * sizeof(int));
        memcpy(f->var_names,     tmp_names,  (size_t)n * sizeof(*f->var_names));
        memcpy(f->var_has_time,  tmp_time,   (size_t)n * sizeof(int));
        memcpy(f->var_has_level, tmp_level,  (size_t)n * sizeof(int));
        memcpy(f->var_on_node,   tmp_node,   (size_t)n * sizeof(int));
        free(order); free(tmp_ids); free(tmp_names); free(tmp_time);
        free(tmp_level); free(tmp_node);
    }

    return f;
}

void nc_close_file(NcFile *f) {
    if (!f) return;
    nc_close(f->ncid);
    free(f->var_ids);
    free(f->var_names);
    free(f->var_has_time);
    free(f->var_has_level);
    free(f->var_on_node);
    free(f);
}

size_t nc_var_grid_len(const NcFile *f, int var_idx) {
    if (var_idx < 0 || var_idx >= f->n_vars_plottable) return f->ncells;
    return f->var_on_node[var_idx] ? f->nnodes : f->ncells;
}

double *nc_read_slab(NcFile *f, int var_idx, int time_idx, int level_idx, double *out_fill) {
    int vid = f->var_ids[var_idx];
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS] = {0};
    nc_inq_var(f->ncid, vid, NULL, NULL, &ndims, dimids, NULL);

    /* The mesh axes are the node dim for a node-centered field, else every face
     * dim; each is read whole and the rest get sliced. Reading the face dims
     * whole and in file order is what makes the cubed sphere's (nf, Ydim, Xdim)
     * block arrive already flattened the way the mesh was built. */
    int on_node = f->var_on_node[var_idx];
    size_t grid_len = nc_var_grid_len(f, var_idx);

    /* Positional indexing of "extra" (non-nCells) dims:
     *   1st extra dim ← time_idx
     *   2nd extra dim ← level_idx
     *   any beyond    ← slice 0
     * This handles (Time,nCells), (Time,nVertLevels,nCells),
     * (nCells,nMonths), (nCells,nSoilComps), etc. uniformly. */
    size_t start[NC_MAX_VAR_DIMS] = {0};
    size_t count[NC_MAX_VAR_DIMS] = {0};
    int extra = 0;
    for (int d = 0; d < ndims; d++) {
        int is_grid = 0;
        if (on_node) {
            is_grid = (dimids[d] == f->nnodes_dim_id);
        } else {
            for (int g = 0; g < f->n_ncells_dims; g++)
                if (dimids[d] == f->ncells_dim_ids[g]) { is_grid = 1; break; }
        }
        if (is_grid) {
            size_t dlen = 0;
            nc_inq_dimlen(f->ncid, dimids[d], &dlen);
            start[d] = 0;
            count[d] = dlen;
            continue;
        }
        int chosen;
        if (extra == 0)      chosen = (time_idx  < 0) ? 0 : time_idx;
        else if (extra == 1) chosen = (level_idx < 0) ? 0 : level_idx;
        else                 chosen = 0;
        size_t dlen = 0;
        nc_inq_dimlen(f->ncid, dimids[d], &dlen);
        if (chosen < 0) chosen = 0;
        if (dlen > 0 && (size_t)chosen >= dlen) chosen = (int)(dlen - 1);
        start[d] = (size_t)chosen;
        count[d] = 1;
        extra++;
    }

    double *buf = malloc(sizeof(double) * grid_len);
    int rc = nc_get_vara_double(f->ncid, vid, start, count, buf);
    if (rc != NC_NOERR) {
        fprintf(stderr, "nc_get_vara_double: %s\n", nc_strerror(rc));
        free(buf);
        return NULL;
    }

    double fill = NAN;
    if (nc_get_att_double(f->ncid, vid, "_FillValue", &fill) != NC_NOERR &&
        nc_get_att_double(f->ncid, vid, "missing_value", &fill) != NC_NOERR) {
        fill = NAN;
    }
    if (out_fill) *out_fill = fill;
    return buf;
}

void nc_get_var_units(NcFile *f, int var_idx, char *buf, size_t cap) {
    buf[0] = '\0';
    if (var_idx < 0 || var_idx >= f->n_vars_plottable || cap == 0) return;
    int vid = f->var_ids[var_idx];
    size_t len = 0;
    if (nc_inq_attlen(f->ncid, vid, "units", &len) != NC_NOERR || len == 0) return;
    if (len >= cap) len = cap - 1;
    nc_get_att_text(f->ncid, vid, "units", buf);
    buf[len] = '\0';
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
}

MultiNc *multinc_open(const char **paths, int n_paths, size_t expected_ncells,
                      const char *face_hint, const char *node_hint, int tile_mode) {
    if (n_paths <= 0) return NULL;
    MultiNc *mn = calloc(1, sizeof(*mn));
    mn->tile_mode = tile_mode;
    mn->files     = calloc((size_t)n_paths, sizeof(NcFile *));
    mn->t_offsets = calloc((size_t)(n_paths + 1), sizeof(int));
    int total = 0;
    for (int i = 0; i < n_paths; i++) {
        NcFile *f = nc_open_file(paths[i], face_hint, node_hint);
        if (!f) {
            for (int j = 0; j < mn->n_files; j++) nc_close_file(mn->files[j]);
            free(mn->files); free(mn->t_offsets); free(mn);
            return NULL;
        }
        if (expected_ncells > 0 && f->ncells != expected_ncells) {
            fprintf(stderr, "file '%s': nCells=%zu, first file has %zu — skipped (mesh mismatch)\n",
                    paths[i], f->ncells, expected_ncells);
            nc_close_file(f);
            continue;
        }
        mn->t_offsets[mn->n_files] = total;
        int nt = (f->ntime > 0) ? (int)f->ntime : 1;
        total += nt;
        mn->files[mn->n_files++] = f;
        /* Lock nCells to the first successfully added file */
        if (expected_ncells == 0) expected_ncells = f->ncells;
    }
    mn->t_offsets[mn->n_files] = total;
    mn->total_time = total;
    if (mn->n_files == 0) {
        free(mn->files); free(mn->t_offsets); free(mn);
        return NULL;
    }
    if (mn->tile_mode) {
        /* Tiles are not time segments: they all carry the same time steps, so
         * the global time axis is one file's, and it is the cells that
         * concatenate (see multinc_read_slab). */
        int nt = (mn->files[0]->ntime > 0) ? (int)mn->files[0]->ntime : 1;
        for (int i = 0; i <= mn->n_files; i++) mn->t_offsets[i] = 0;
        mn->total_time = nt;
    }
    return mn;
}

void multinc_free(MultiNc *mn) {
    if (!mn) return;
    for (int i = 0; i < mn->n_files; i++) nc_close_file(mn->files[i]);
    free(mn->files);
    free(mn->t_offsets);
    free(mn);
}

void multinc_resolve(const MultiNc *mn, int global_t, int *file_idx, int *local_t) {
    int fi = mn->n_files - 1;
    for (int i = 0; i < mn->n_files - 1; i++) {
        if (global_t < mn->t_offsets[i + 1]) { fi = i; break; }
    }
    *file_idx = fi;
    *local_t  = global_t - mn->t_offsets[fi];
}

/* Same variable in another file: ids differ between files, names do not. */
static int var_index_in(const NcFile *f, const char *name) {
    for (int v = 0; v < f->n_vars_plottable; v++)
        if (!strcmp(f->var_names[v], name)) return v;
    return -1;
}

/* One tile's contribution to a stitched slab: its own values, or NaN padding of
 * the right length when the variable is missing from that tile. */
static size_t tile_len(MultiNc *mn, int fi, const char *varname,
                       int var_idx, int *out_idx) {
    int vi = (fi == 0) ? var_idx : var_index_in(mn->files[fi], varname);
    if (out_idx) *out_idx = vi;
    return (vi >= 0) ? nc_var_grid_len(mn->files[fi], vi)
                     : nc_var_grid_len(mn->files[0], var_idx);
}

double *multinc_read_slab(MultiNc *mn, int var_idx, int global_t, int level, double *fill) {
    if (mn->tile_mode) {
        /* Concatenate the tiles along the cell axis, in file order -- the same
         * order mesh_load_tiles concatenated the mesh in. */
        const char *varname = mn->files[0]->var_names[var_idx];
        size_t tot = 0;
        for (int i = 0; i < mn->n_files; i++)
            tot += tile_len(mn, i, varname, var_idx, NULL);
        double *out = malloc(sizeof(double) * tot);
        if (!out) return NULL;
        if (fill) *fill = NAN;
        size_t off = 0;
        for (int i = 0; i < mn->n_files; i++) {
            int vi;
            size_t len = tile_len(mn, i, varname, var_idx, &vi);
            double *src = NULL;
            double f2 = NAN;
            if (vi >= 0) src = nc_read_slab(mn->files[i], vi, global_t, level, &f2);
            if (src) {
                memcpy(out + off, src, sizeof(double) * len);
                free(src);
                if (i == 0 && fill) *fill = f2;
            } else {
                for (size_t k = 0; k < len; k++) out[off + k] = NAN;
            }
            off += len;
        }
        return out;
    }
    int fi, lt;
    multinc_resolve(mn, global_t, &fi, &lt);
    NcFile *f  = mn->files[fi];
    NcFile *f0 = mn->files[0];
    /* Look up variable by name in the target file (IDs may differ across files) */
    const char *varname = f0->var_names[var_idx];
    int actual_idx = var_idx; /* same position is common */
    if (fi != 0) {
        actual_idx = -1;
        for (int v = 0; v < f->n_vars_plottable; v++) {
            if (!strcmp(f->var_names[v], varname)) { actual_idx = v; break; }
        }
    }
    if (actual_idx < 0) {
        /* Variable absent in this file — return NaN array. Size it from the
         * first file, which is where the caller's expectations come from. */
        size_t n = nc_var_grid_len(f0, var_idx);
        double *buf = malloc(sizeof(double) * n);
        for (size_t i = 0; i < n; i++) buf[i] = NAN;
        if (fill) *fill = NAN;
        return buf;
    }
    return nc_read_slab(f, actual_idx, lt, level, fill);
}

double multinc_read_cell_value(MultiNc *mn, int var_idx, int global_t,
                               int level, int cell, int *ok) {
    if (ok) *ok = 0;
    int fi, lt;
    if (mn->tile_mode) {
        /* Tiles share a time axis and split the cells, so the file is picked by
         * cell index rather than by time, and `cell` becomes tile-local. */
        size_t base = 0;
        for (fi = 0; fi < mn->n_files; fi++) {
            size_t nc = mn->files[fi]->ncells;
            if (cell >= 0 && (size_t)cell < base + nc) break;
            base += nc;
        }
        if (fi >= mn->n_files) return NAN;
        cell -= (int)base;
        lt = global_t;
    } else {
        multinc_resolve(mn, global_t, &fi, &lt);
    }
    NcFile *f  = mn->files[fi];
    NcFile *f0 = mn->files[0];
    const char *varname = f0->var_names[var_idx];
    int actual_idx = var_idx;
    if (fi != 0) {
        actual_idx = -1;
        for (int v = 0; v < f->n_vars_plottable; v++) {
            if (!strcmp(f->var_names[v], varname)) { actual_idx = v; break; }
        }
    }
    if (actual_idx < 0) return NAN;
    /* `cell` indexes the variable's own mesh axis: a node index for a
     * node-centered field, a face index otherwise. The GUI does the mapping,
     * because it is the side that holds the connectivity. */
    int on_node = f->var_on_node[actual_idx];
    if (cell < 0 || (size_t)cell >= nc_var_grid_len(f, actual_idx)) return NAN;

    /* Undo the C-order flattening so a flat cell index picks the right slot on
     * each face dimension. With one face dim this is just cell itself. */
    size_t grid_idx[MESH_MAX_GRID_DIMS] = {0};
    if (!on_node) {
        size_t rem = (size_t)cell;
        for (int g = f->n_ncells_dims - 1; g >= 0; g--) {
            size_t dlen = 0;
            nc_inq_dimlen(f->ncid, f->ncells_dim_ids[g], &dlen);
            if (dlen == 0) dlen = 1;
            grid_idx[g] = rem % dlen;
            rem /= dlen;
        }
    }

    int vid = f->var_ids[actual_idx];
    int ndims = 0;
    int dimids[NC_MAX_VAR_DIMS] = {0};
    nc_inq_var(f->ncid, vid, NULL, NULL, &ndims, dimids, NULL);

    /* Map by dimension id (not position): nCells -> the picked cell, the Time
     * dim -> lt, the level dim -> level, anything else -> slice 0. This is
     * robust for level-only or time-only variables where positional ordering
     * would otherwise misassign the time/level index. */
    size_t start[NC_MAX_VAR_DIMS] = {0};
    size_t count[NC_MAX_VAR_DIMS] = {0};
    for (int d = 0; d < ndims; d++) {
        int chosen = -1;
        if (on_node && dimids[d] == f->nnodes_dim_id) chosen = cell;
        if (chosen < 0 && !on_node) {
            for (int g = 0; g < f->n_ncells_dims; g++)
                if (dimids[d] == f->ncells_dim_ids[g]) { chosen = (int)grid_idx[g]; break; }
        }
        if (chosen >= 0)                                { /* already set */ }
        else if (f->time_dim_id  >= 0 && dimids[d] == f->time_dim_id)  chosen = (lt    < 0) ? 0 : lt;
        else if (f->level_dim_id >= 0 && dimids[d] == f->level_dim_id) chosen = (level < 0) ? 0 : level;
        else                                            chosen = 0;
        size_t dlen = 0;
        nc_inq_dimlen(f->ncid, dimids[d], &dlen);
        if (chosen < 0) chosen = 0;
        if (dlen > 0 && (size_t)chosen >= dlen) chosen = (int)(dlen - 1);
        start[d] = (size_t)chosen;
        count[d] = 1;
    }

    double v = NAN;
    int rc = nc_get_vara_double(f->ncid, vid, start, count, &v);
    if (rc != NC_NOERR) return NAN;

    /* Treat the variable's fill/missing sentinel as no-data. */
    double fillv = NAN;
    if (nc_get_att_double(f->ncid, vid, "_FillValue", &fillv) == NC_NOERR ||
        nc_get_att_double(f->ncid, vid, "missing_value", &fillv) == NC_NOERR) {
        if (!isnan(fillv) && v == fillv) return NAN;
    }
    if (ok) *ok = 1;
    return v;
}

void multinc_hide_mesh_vars(MultiNc *mn, const UnsMesh *m) {
    if (!mn || !m || m->n_mesh_vars == 0) return;
    for (int i = 0; i < mn->n_files; i++) {
        NcFile *f = mn->files[i];
        int kept = 0;
        for (int v = 0; v < f->n_vars_plottable; v++) {
            int hide = 0;
            for (int k = 0; k < m->n_mesh_vars; k++) {
                if (!strcmp(f->var_names[v], m->mesh_vars[k])) { hide = 1; break; }
            }
            if (hide) continue;
            if (kept != v) {
                f->var_ids[kept]       = f->var_ids[v];
                memcpy(f->var_names[kept], f->var_names[v], NC_MAX_NAME + 1);
                f->var_has_time[kept]  = f->var_has_time[v];
                f->var_has_level[kept] = f->var_has_level[v];
                f->var_on_node[kept]   = f->var_on_node[v];
            }
            kept++;
        }
        f->n_vars_plottable = kept;
    }
}
