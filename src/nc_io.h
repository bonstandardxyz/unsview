#ifndef NC_IO_H
#define NC_IO_H

#include <stddef.h>
#include <netcdf.h>

#include "mesh.h"

typedef struct {
    int ncid;
    int n_vars_plottable;
    int *var_ids;
    char (*var_names)[NC_MAX_NAME + 1];
    int *var_has_time;
    int *var_has_level;
    /* Field sits on mesh nodes rather than faces. Its slab has nnodes entries,
     * not ncells, and needs mesh_node_to_face() before it can be rendered. */
    int *var_on_node;
    /* Dimensions that jointly index the faces, outermost first. Usually one
     * (MPAS nCells, ICON cell, a UGRID face dim), but a cubed sphere spreads
     * the mesh over three -- (nf, Ydim, Xdim) -- with no connectivity array.
     * `ncells` is the product of their lengths, and a flat cell index is the
     * C-order flattening across them. */
    int ncells_dim_ids[MESH_MAX_GRID_DIMS];
    int n_ncells_dims;
    int nnodes_dim_id;
    int time_dim_id;
    int level_dim_id;
    size_t ncells;
    size_t nnodes;
    size_t ntime;
    size_t nlevels;
} NcFile;

/* Number of values nc_read_slab returns for this variable: nnodes for a
 * node-centered field, ncells otherwise. */
size_t nc_var_grid_len(const NcFile *f, int var_idx);

/* face_hint / node_hint name the mesh dimensions to look for first; pass NULL
 * to rely on detection. They matter for stream-split UGRID output, where the
 * data file carries no topology variable and only the grid file knows what its
 * mesh dimensions are called. */
NcFile *nc_open_file(const char *path, const char *face_hint, const char *node_hint);
void nc_close_file(NcFile *f);
double *nc_read_slab(NcFile *f, int var_idx, int time_idx, int level_idx, double *out_fill);
/* Reads the "units" attribute of var_idx into buf (null-terminated, empty if absent). */
void nc_get_var_units(NcFile *f, int var_idx, char *buf, size_t cap);

/* Multi-file wrapper: N netCDF files concatenated along the time axis -- or,
 * in tile_mode, along the *cell* axis instead. */
typedef struct {
    NcFile **files;
    int      n_files;
    int     *t_offsets;  /* t_offsets[i] = global t-index of first step in files[i] */
    int      total_time; /* total steps across all files (each file contributes max(ntime,1)) */
    /* Files are tiles of one mesh (FV3 writes one file per cubed-sphere tile),
     * not segments of a time axis. They then share one time axis and their
     * cells concatenate, tile-major, matching mesh_load_tiles. */
    int      tile_mode;
} MultiNc;

MultiNc *multinc_open(const char **paths, int n_paths, size_t expected_ncells,
                      const char *face_hint, const char *node_hint, int tile_mode);
void     multinc_free(MultiNc *mn);
void     multinc_resolve(const MultiNc *mn, int global_t, int *file_idx, int *local_t);
double  *multinc_read_slab(MultiNc *mn, int var_idx, int global_t, int level, double *fill);
/* Read a single value for one cell at a given time/level.
 * Returns NAN and sets *ok=0 (if ok != NULL) when the value is unavailable. */
double   multinc_read_cell_value(MultiNc *mn, int var_idx, int global_t,
                                 int level, int cell, int *ok);
/* Hide the mesh-metadata variables the reader consumed from the picker. Called
 * after mesh_load, because the data files are opened before any mesh exists. */
void     multinc_hide_mesh_vars(MultiNc *mn, const UnsMesh *m);

#endif
