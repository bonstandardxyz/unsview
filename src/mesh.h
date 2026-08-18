#ifndef MESH_H
#define MESH_H

#include <stddef.h>

/* Mesh-metadata variables a reader consumed, so the variable picker can hide
 * them. A static blacklist cannot cover UGRID, whose mesh variable names are
 * chosen by whoever wrote the file (Mesh2_face_nodes, SCHISM_hgrid_face_nodes,
 * ...), so each reader reports the names it used. */
#define MESH_MAX_MESHVARS 32
#define MESH_MAX_NAME     64

/* Most conventions index the mesh with one dimension; a cubed sphere uses
 * three, (nf, Ydim, Xdim). mesh_probe_dims reports them space-separated. */
#define MESH_MAX_GRID_DIMS 4

/* One unstructured mesh, normalized to the CF-UGRID face/node model: `ncells`
 * faces, each bounded by n_edges_on_cell[c] corner nodes indexed 0-based into
 * lon_vertex / lat_vertex.
 *
 * Every supported model is read into this one struct, so the renderer never
 * learns which model a file came from. Angles are radians throughout; UGRID
 * files, which conventionally store degrees, are converted at load. */
typedef struct {
    size_t  ncells;            /* faces */
    size_t  nvertices;         /* nodes */
    int     max_edges;         /* row stride of vertices_on_cell */
    double *lon_cell;          /* face centers */
    double *lat_cell;
    int    *n_edges_on_cell;   /* valid corner count per face */
    int    *vertices_on_cell;  /* [ncells][max_edges], 0-based, -1 = unused */
    double *lon_vertex;        /* nodes */
    double *lat_vertex;

    /* Cached lon/lat extent of face centers (radians), for regional auto-fit. */
    double lon_min, lon_max;
    double lat_min, lat_max;

    const char *model;         /* "mpas" | "icon" | "ugrid" */

    char mesh_vars[MESH_MAX_MESHVARS][MESH_MAX_NAME];
    int  n_mesh_vars;
} UnsMesh;

/* Load a mesh from an open netCDF id. `forced` is NULL or "auto" to detect the
 * model, or one of "mpas" / "icon" / "ugrid" to require that reader. Returns
 * NULL after reporting which reader failed and what it was missing. */
UnsMesh *mesh_load(int ncid, const char *forced);
/* Stitch N cubed-sphere tile files into one mesh (FV3 writes one file per
 * tile; GEOS puts all six faces in a single file). Tiles concatenate in the
 * order given, so the global cell index is tile-major then (y, x) within the
 * tile -- the same order multinc_read_slab assembles tile data in. n == 1 is
 * just mesh_load. */
UnsMesh *mesh_load_tiles(const int *ncids, int n, const char *forced);
void     mesh_free(UnsMesh *m);

/* Average each face's corner-node values into one per-face value. NaN corners
 * are skipped; a face with no valid corner yields NaN. `node_vals` has
 * nvertices entries, `face_vals` has ncells. */
void mesh_node_to_face(const UnsMesh *m, const double *node_vals, double *face_vals);

/* Discover this file's face and node dimension names without loading a mesh --
 * nc_io needs them before any mesh exists. Either output can come back empty.
 * A node dimension is reported only for files that carry node-centered data
 * (UGRID); MPAS and ICON store fields on cells only, so their vertex dimension
 * is deliberately not reported. */
void mesh_probe_dims(int ncid, char *face_dim, char *node_dim, size_t cap);

#endif
