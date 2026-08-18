#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>

#include "nc_io.h"
#include "mesh.h"
#include "raster.h"
#include "colormap.h"
#include "polylines.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Compile-time install location for the bundled coast/borders/states overlays.
 * Override at build with -DUNSVIEW_DATA_DIR="/some/path"; the install target sets
 * this automatically to $(DATADIR). */
#ifndef UNSVIEW_DATA_DIR
#define UNSVIEW_DATA_DIR "/usr/local/share/unsview"
#endif

/* Find a bundled overlay file. Search order:
 *   1. $UNSVIEW_DATA_DIR env var
 *   2. compile-time UNSVIEW_DATA_DIR
 *   3. ./samples/ relative to cwd (dev/in-tree)
 * Writes the resolved path into out (size cap) and returns out, or NULL. */
static const char *resolve_data_file(const char *basename, char *out, size_t cap) {
    const char *roots[3];
    int n = 0;
    const char *env = getenv("UNSVIEW_DATA_DIR");
    if (env && *env) roots[n++] = env;
    roots[n++] = UNSVIEW_DATA_DIR;
    roots[n++] = "samples";
    for (int i = 0; i < n; i++) {
        snprintf(out, cap, "%s/%s", roots[i], basename);
        if (access(out, R_OK) == 0) return out;
    }
    return NULL;
}

int png_export(const char *path, const Raster *r, uint32_t bg);

#ifdef UNSVIEW_HAVE_X11
int gui_run(MultiNc *mnc, UnsMesh *m, const char *initial_var, const char *cmap_name,
            int has_vmin, double user_vmin, int has_vmax, double user_vmax,
            const PolyLayer *layers, int n_layers,
            int force_global);
#endif

#define MAX_LAYERS  16
#define MAX_FILES   64
#define MAX_RC_ARGS 64

/* Settings an rc file may carry, each mapped to the option it mirrors.
 *
 * The rc file is treated purely as a source of arguments: its keys become the
 * very flags the option loop below already understands, and it is parsed first.
 * So rc and command line can never drift apart about what a setting means, and
 * the command line wins simply by being parsed second. */
static const struct { const char *key; const char *flag; int is_bool; } RC_KEYS[] = {
    { "model",         "--model",         0 },
    { "grid",          "--grid",          0 },
    { "tiles",         "--tiles",         1 },
    { "var",           "-v",              0 },
    { "time",          "-t",              0 },
    { "level",         "-l",              0 },
    { "cmap",          "-c",              0 },
    { "width",         "-W",              0 },
    { "height",        "-H",              0 },
    { "vmin",          "--vmin",          0 },
    { "vmax",          "--vmax",          0 },
    { "coast",         "--coast",         0 },
    { "borders",       "--borders",       0 },
    { "states",        "--states",        0 },
    { "lines",         "--lines",         0 },
    { "global",        "--global",        1 },
    { "coast-data",    "--coast-data",    1 },
    { "no-coast-data", "--no-coast-data", 1 },
    { NULL, NULL, 0 }
};

static char *rc_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

static int rc_truthy(const char *v) {
    return !*v || !strcmp(v, "1") || !strcasecmp(v, "true") ||
           !strcasecmp(v, "yes") || !strcasecmp(v, "on");
}

/* First readable of $UNSVIEWRC, ./.unsviewrc, ~/.unsviewrc. They are not
 * merged: whichever is found first is the one that applies. */
static int rc_find(char *buf, size_t cap) {
    const char *env = getenv("UNSVIEWRC");
    if (env && *env) {
        snprintf(buf, cap, "%s", env);
        return access(buf, R_OK) == 0;
    }
    snprintf(buf, cap, ".unsviewrc");
    if (access(buf, R_OK) == 0) return 1;
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, cap, "%s/.unsviewrc", home);
        if (access(buf, R_OK) == 0) return 1;
    }
    return 0;
}

/* Expand an rc file into argument strings. Returns how many were produced. */
static int rc_load(const char *path, char **out, int cap) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    int n = 0;
    char line[1024];
    while (fgets(line, sizeof line, fp) && n + 2 <= cap) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *val = (char *)"";
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            val = rc_trim(eq + 1);
        }
        char *key = rc_trim(line);
        if (!*key) continue;

        int k = 0;
        while (RC_KEYS[k].key && strcmp(key, RC_KEYS[k].key)) k++;
        if (!RC_KEYS[k].key) {
            /* data-dir feeds the overlay search, which reads the environment.
             * An explicit export outranks the rc file, so only set it if unset. */
            if (!strcmp(key, "data-dir")) {
                if (*val && !getenv("UNSVIEW_DATA_DIR"))
                    setenv("UNSVIEW_DATA_DIR", val, 1);
            } else {
                fprintf(stderr, "%s: ignoring unknown setting '%s'\n", path, key);
            }
            continue;
        }
        if (RC_KEYS[k].is_bool) {
            if (rc_truthy(val)) out[n++] = strdup(RC_KEYS[k].flag);
        } else if (*val) {
            out[n++] = strdup(RC_KEYS[k].flag);
            out[n++] = strdup(val);
        }
    }
    fclose(fp);
    return n;
}

static void usage(void) {
    fprintf(stderr,
        "unsview - unstructured-mesh viewer (MPAS, ICON, UGRID, cubed sphere, FVCOM)\n"
        "usage: unsview FILE.nc [options]\n"
        "  -v VAR        select variable\n"
        "  -t INDEX      time index (or 1st extra dim, default 0)\n"
        "  -l INDEX      vertical level (or 2nd extra dim, default 0)\n"
        "  -c CMAP       colormap (viridis, plasma, magma, inferno, cividis,\n"
        "                  turbo, jet, rdbu, brbg, seismic, hot, cool,\n"
        "                  terrain, gray, wbr, rainbow, bwr, 3gauss)\n"
        "  -W WIDTH      output width (default 1024)\n"
        "  -H HEIGHT     output height (default 512)\n"
        "  -o PATH       headless: write PNG and exit\n"
        "  --model NAME    mesh convention: auto (default), mpas, icon,\n"
        "                  ugrid, cs (GEOS/FV3 cubed sphere), fvcom\n"
        "  --grid GRID.nc  load mesh from a separate grid file (use when\n"
        "                  FILE.nc is stream-output diag/history carrying data\n"
        "                  but no mesh). Repeat once per tile with --tiles.\n"
        "  --tiles         treat the input files as tiles of one cubed sphere\n"
        "                  and stitch them into a globe, instead of as steps\n"
        "                  of a time series (FV3 writes one file per tile)\n"
        "  --vmin VALUE    fix colorscale lower bound (default: data min)\n"
        "  --vmax VALUE    fix colorscale upper bound (default: data max)\n"
        "  --coast PATH    overlay coastline polylines (default color white;\n"
        "                  GUI button cycles white -> black -> off)\n"
        "  --borders PATH  overlay country borders (default white)\n"
        "  --states PATH   overlay state/province lines (default white)\n"
        "  --coast-data    auto-load all three bundled overlays (coast +\n"
        "                  borders + states); enabled by default\n"
        "  --no-coast-data disable the default overlay auto-load\n"
        "  --lines PATH[:HEX]  overlay any polyline file with optional color\n"
        "                  (e.g. --lines roads.txt:ff0000); repeatable\n"
        "  --global        force the global view (override regional auto-fit)\n"
        "  --no-rc         ignore .unsviewrc (for reproducible batch runs)\n"
        "  -h            this help\n"
        "\n"
        "Defaults may be set in $UNSVIEWRC, ./.unsviewrc or ~/.unsviewrc (first\n"
        "one found wins) as `key = value' lines, using the long option names\n"
        "without the dashes, e.g.\n"
        "    model  = icon\n"
        "    cmap   = rdbu\n"
        "    width  = 1920\n"
        "    global = yes\n"
        "Anything on the command line overrides them.\n");
}

int main(int argc, char **argv) {
    const char *file_paths[MAX_FILES];
    int n_files = 0;
    /* --grid may be repeated, one per cubed-sphere tile; one entry is the
     * ordinary "data here, mesh there" case. */
    const char *grid_paths[MAX_FILES];
    int n_grids = 0;
    int tile_mode = 0;
    const char *model_name = NULL;   /* NULL = auto-detect */
    const char *var_name = NULL;
    const char *cmap_name = "viridis";
    const char *out_png = NULL;
    int t_idx = 0, l_idx = 0;
    int width = 1024, height = 512;
    int has_vmin = 0, has_vmax = 0;
    double user_vmin = 0, user_vmax = 0;
    int force_global = 0;
    int no_coast_data = 0;

    struct {
        const char *path;
        uint32_t color;
        PolyLayerType type;
    } layer_spec[MAX_LAYERS];
    int n_layer_spec = 0;

    /* --no-rc has to be spotted before the rc file is read, so it gets its own
     * scan. Batch runs and conda_test.sh need it: a developer's ~/.unsviewrc
     * setting `cmap = jet` would otherwise silently change rendered output. */
    int use_rc = 1;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--no-rc")) use_rc = 0;

    char *rc_args[MAX_RC_ARGS];
    int n_rc = 0;
    char rc_path[1024];
    if (use_rc && rc_find(rc_path, sizeof rc_path)) {
        n_rc = rc_load(rc_path, rc_args, MAX_RC_ARGS);
        if (n_rc > 0) fprintf(stderr, "defaults from %s\n", rc_path);
    }

    int nav = argc + n_rc;
    const char **av = malloc(sizeof(*av) * (size_t)nav);
    av[0] = argv[0];
    for (int i = 0; i < n_rc; i++) av[1 + i] = rc_args[i];
    for (int i = 1; i < argc; i++) av[n_rc + i] = argv[i];

    for (int i = 1; i < nav; i++) {
        const char *a = av[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(a, "-v") && i + 1 < nav) {
            var_name = av[++i];
        } else if (!strcmp(a, "-t") && i + 1 < nav) {
            t_idx = atoi(av[++i]);
        } else if (!strcmp(a, "-l") && i + 1 < nav) {
            l_idx = atoi(av[++i]);
        } else if (!strcmp(a, "-c") && i + 1 < nav) {
            cmap_name = av[++i];
        } else if (!strcmp(a, "-W") && i + 1 < nav) {
            width = atoi(av[++i]);
        } else if (!strcmp(a, "-H") && i + 1 < nav) {
            height = atoi(av[++i]);
        } else if (!strcmp(a, "-o") && i + 1 < nav) {
            out_png = av[++i];
        } else if (!strcmp(a, "--grid") && i + 1 < nav) {
            if (n_grids < MAX_FILES) grid_paths[n_grids++] = av[++i];
            else i++;
        } else if (!strcmp(a, "--tiles")) {
            tile_mode = 1;
        } else if (!strcmp(a, "--model") && i + 1 < nav) {
            model_name = av[++i];
        } else if (!strcmp(a, "--vmin") && i + 1 < nav) {
            user_vmin = atof(av[++i]);
            has_vmin = 1;
        } else if (!strcmp(a, "--vmax") && i + 1 < nav) {
            user_vmax = atof(av[++i]);
            has_vmax = 1;
        } else if (!strcmp(a, "--coast") && i + 1 < nav) {
            if (n_layer_spec < MAX_LAYERS) {
                layer_spec[n_layer_spec].path  = av[++i];
                layer_spec[n_layer_spec].color = 0xffffff;
                layer_spec[n_layer_spec].type  = POLY_TYPE_COAST;
                n_layer_spec++;
            }
        } else if (!strcmp(a, "--borders") && i + 1 < nav) {
            if (n_layer_spec < MAX_LAYERS) {
                layer_spec[n_layer_spec].path  = av[++i];
                layer_spec[n_layer_spec].color = 0xffffff;
                layer_spec[n_layer_spec].type  = POLY_TYPE_BORDERS;
                n_layer_spec++;
            }
        } else if (!strcmp(a, "--states") && i + 1 < nav) {
            if (n_layer_spec < MAX_LAYERS) {
                layer_spec[n_layer_spec].path  = av[++i];
                layer_spec[n_layer_spec].color = 0xffffff;
                layer_spec[n_layer_spec].type  = POLY_TYPE_STATES;
                n_layer_spec++;
            }
        } else if (!strcmp(a, "--lines") && i + 1 < nav) {
            if (n_layer_spec < MAX_LAYERS) {
                const char *spec = av[++i];
                const char *colon = strchr(spec, ':');
                uint32_t color = 0x202020;
                static char path_buf[MAX_LAYERS][1024];
                int slot = n_layer_spec;
                if (colon) {
                    size_t pl = (size_t)(colon - spec);
                    if (pl >= sizeof(path_buf[slot])) pl = sizeof(path_buf[slot]) - 1;
                    memcpy(path_buf[slot], spec, pl);
                    path_buf[slot][pl] = 0;
                    unsigned int c;
                    if (sscanf(colon + 1, "%x", &c) == 1) color = c;
                    layer_spec[slot].path = path_buf[slot];
                } else {
                    layer_spec[slot].path = spec;
                }
                layer_spec[slot].color = color;
                layer_spec[slot].type  = POLY_TYPE_GENERIC;
                n_layer_spec++;
            }
        } else if (!strcmp(a, "--coast-data")) {
            const struct { const char *file; uint32_t color; PolyLayerType type; } bundle[] = {
                { "coastlines_110m.txt", 0xffffff, POLY_TYPE_COAST   },
                { "borders_50m.txt",     0xffffff, POLY_TYPE_BORDERS },
                { "states_50m.txt",      0xffffff, POLY_TYPE_STATES  },
            };
            static char coastdata_buf[3][1024];
            for (int b = 0; b < 3 && n_layer_spec < MAX_LAYERS; b++) {
                const char *p = resolve_data_file(bundle[b].file,
                                                  coastdata_buf[b],
                                                  sizeof(coastdata_buf[b]));
                if (!p) {
                    fprintf(stderr, "warning: --coast-data: cannot find %s "
                                    "(searched $UNSVIEW_DATA_DIR, %s, samples/)\n",
                                    bundle[b].file, UNSVIEW_DATA_DIR);
                    continue;
                }
                layer_spec[n_layer_spec].path  = p;
                layer_spec[n_layer_spec].color = bundle[b].color;
                layer_spec[n_layer_spec].type  = bundle[b].type;
                n_layer_spec++;
            }
        } else if (!strcmp(a, "--no-coast-data")) {
            no_coast_data = 1;
        } else if (!strcmp(a, "--global")) {
            force_global = 1;
        } else if (!strcmp(a, "--no-rc")) {
            /* Already handled before the rc file was read. */
        } else if (a[0] != '-') {
            if (n_files < MAX_FILES) file_paths[n_files++] = a;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage();
            return 1;
        }
    }

    /* Bundled overlays are on by default: load each of coast/borders/states
     * unless --no-coast-data was given or the user already supplied an explicit
     * layer of that same type (e.g. --coast PATH overrides the bundled coast).
     * Generic --lines layers don't suppress the bundled trio. */
    if (!no_coast_data) {
        static const struct {
            const char *file; uint32_t color; PolyLayerType type;
        } bundle[] = {
            { "coastlines_110m.txt", 0xffffff, POLY_TYPE_COAST   },
            { "borders_50m.txt",     0xffffff, POLY_TYPE_BORDERS },
            { "states_50m.txt",      0xffffff, POLY_TYPE_STATES  },
        };
        static char auto_buf[3][1024];
        for (int b = 0; b < 3 && n_layer_spec < MAX_LAYERS; b++) {
            int already = 0;
            for (int s = 0; s < n_layer_spec; s++) {
                if (layer_spec[s].type == bundle[b].type) { already = 1; break; }
            }
            if (already) continue;
            const char *p = resolve_data_file(bundle[b].file,
                                              auto_buf[b], sizeof(auto_buf[b]));
            if (!p) continue;
            layer_spec[n_layer_spec].path  = p;
            layer_spec[n_layer_spec].color = bundle[b].color;
            layer_spec[n_layer_spec].type  = bundle[b].type;
            n_layer_spec++;
        }
    }

    if (n_files == 0) { usage(); return 1; }

    colormap_init();

    /* Open the grid file first, if any: stream-split UGRID output names its
     * mesh dimensions arbitrarily and only the grid file declares them, so the
     * data files need those names as a hint before they can be interpreted. */
    int grid_ncids[MAX_FILES];
    int n_grid_ncids = 0;
    char gface[NC_MAX_NAME + 1] = {0}, gnode[NC_MAX_NAME + 1] = {0};
    for (int g = 0; g < n_grids; g++) {
        int id;
        int rc = nc_open(grid_paths[g], NC_NOWRITE, &id);
        if (rc != NC_NOERR) {
            fprintf(stderr, "open grid '%s': %s\n", grid_paths[g], nc_strerror(rc));
            for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
            return 1;
        }
        grid_ncids[n_grid_ncids++] = id;
    }
    if (n_grid_ncids > 0)
        mesh_probe_dims(grid_ncids[0], gface, gnode, sizeof gface);

    MultiNc *mnc = multinc_open(file_paths, n_files, 0,
                                gface[0] ? gface : NULL,
                                gnode[0] ? gnode : NULL,
                                tile_mode);
    if (!mnc) {
        for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
        return 1;
    }

    if (n_grid_ncids > 0) {
        /* A data file carrying only node-centered fields has no face dimension
         * at all (ncells == 0); there is nothing to cross-check then. Tiles are
         * skipped too -- one grid file describes one tile while the data spans
         * all of them, so the totals are compared after the stitch instead. */
        int dim_nc;
        if (!tile_mode && n_grid_ncids == 1 &&
            gface[0] && mnc->files[0]->ncells > 0 &&
            nc_inq_dimid(grid_ncids[0], gface, &dim_nc) == NC_NOERR) {
            size_t gnc = 0;
            nc_inq_dimlen(grid_ncids[0], dim_nc, &gnc);
            if (gnc != mnc->files[0]->ncells) {
                fprintf(stderr,
                        "grid file has %zu cells, data file has %zu; mismatch\n",
                        gnc, mnc->files[0]->ncells);
                for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
                multinc_free(mnc);
                return 1;
            }
        }
        fprintf(stderr, "using mesh from %d grid file%s: %s%s\n",
                n_grid_ncids, n_grid_ncids > 1 ? "s" : "", grid_paths[0],
                n_grid_ncids > 1 ? " ..." : "");
    }

    UnsMesh *m;
    if (n_grid_ncids > 0) {
        m = mesh_load_tiles(grid_ncids, n_grid_ncids, model_name);
    } else if (tile_mode) {
        int ids[MAX_FILES];
        for (int i = 0; i < mnc->n_files; i++) ids[i] = mnc->files[i]->ncid;
        m = mesh_load_tiles(ids, mnc->n_files, model_name);
    } else {
        m = mesh_load(mnc->files[0]->ncid, model_name);
    }
    if (!m) {
        /* Only suggest --grid when auto-detection came up empty. If the user
         * forced a model, the file may well hold a mesh of a different one and
         * pointing at --grid would send them the wrong way. */
        if (n_grids == 0 && !model_name) {
            fprintf(stderr,
                    "hint: this file appears to carry data but no mesh "
                    "(stream-output diag/history?). Try:\n"
                    "      unsview %s --grid YOUR_GRID.nc ...\n",
                    file_paths[0]);
        }
        for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
        multinc_free(mnc);
        return 1;
    }
    if (tile_mode) {
        size_t data_cells = 0;
        for (int i = 0; i < mnc->n_files; i++) data_cells += mnc->files[i]->ncells;
        if (data_cells > 0 && data_cells != m->ncells) {
            fprintf(stderr,
                    "--tiles: the mesh has %zu cells but the %d data files "
                    "supply %zu. Give one --grid per tile, in the same order as "
                    "the data files.\n",
                    m->ncells, mnc->n_files, data_cells);
            mesh_free(m);
            for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
            multinc_free(mnc);
            return 1;
        }
    } else if (mnc->n_files > 1 && !strcmp(m->model, "cs")) {
        /* Six tile files look exactly like six time steps, so this cannot be
         * auto-detected without risking a silently mislabelled time axis. */
        fprintf(stderr,
                "note: %d cubed-sphere files given without --tiles, so they are "
                "being read as %d time steps of a single tile.\n"
                "      Add --tiles to stitch them into one globe instead.\n",
                mnc->n_files, mnc->total_time);
    }
    multinc_hide_mesh_vars(mnc, m);

    PolyLayer layers[MAX_LAYERS];
    int n_layers = 0;
    for (int s = 0; s < n_layer_spec; s++) {
        Polylines *p = polylines_load(layer_spec[s].path);
        if (!p) continue;
        layers[n_layers].poly  = p;
        layers[n_layers].color = layer_spec[s].color;
        layers[n_layers].type  = layer_spec[s].type;
        n_layers++;
        fprintf(stderr, "loaded layer #%d (%s, color #%06x): %zu points\n",
                n_layers, layer_spec[s].path, layer_spec[s].color, p->n_pts);
    }

    fprintf(stderr, "loaded %s mesh: %zu cells, %zu vertices, max_edges=%d\n",
            m->model, m->ncells, m->nvertices, m->max_edges);
    fprintf(stderr, "plottable variables (%d):\n", mnc->files[0]->n_vars_plottable);
    for (int v = 0; v < mnc->files[0]->n_vars_plottable; v++) {
        fprintf(stderr, "  %s%s%s%s\n",
                mnc->files[0]->var_names[v],
                mnc->files[0]->var_has_time[v] ? " (time)" : "",
                mnc->files[0]->var_has_level[v] ? " (level)" : "",
                mnc->files[0]->var_on_node[v] ? " (on nodes)" : "");
    }

    int rc_main = 0;

    if (out_png) {
        if (!var_name) {
            if (mnc->files[0]->n_vars_plottable == 0) {
                fprintf(stderr, "no plottable variables found\n");
                rc_main = 1;
                goto done;
            }
            var_name = mnc->files[0]->var_names[0];
        }
        int var_idx = -1;
        for (int v = 0; v < mnc->files[0]->n_vars_plottable; v++) {
            if (!strcmp(mnc->files[0]->var_names[v], var_name)) {
                var_idx = v;
                break;
            }
        }
        if (var_idx < 0) {
            fprintf(stderr, "variable not found: %s\n", var_name);
            rc_main = 1;
            goto done;
        }

        double fill = NAN;
        double *values = multinc_read_slab(mnc, var_idx, t_idx, l_idx, &fill);
        if (!values) { rc_main = 1; goto done; }

        /* Node-centered fields (SCHISM, FESOM, ...) carry one value per mesh
         * node; collapse them onto faces so the flat polygon fill applies. */
        if (mnc->files[0]->var_on_node[var_idx]) {
            double *face_vals = malloc(sizeof(double) * m->ncells);
            mesh_node_to_face(m, values, face_vals);
            free(values);
            values = face_vals;
        }

        double vmin = INFINITY, vmax = -INFINITY;
        int has_fill = !isnan(fill);
        for (size_t c = 0; c < m->ncells; c++) {
            double v = values[c];
            if (isnan(v)) continue;
            if (has_fill && v == fill) continue;
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }
        if (!isfinite(vmin)) { vmin = 0; vmax = 1; }
        if (has_vmin) vmin = user_vmin;
        if (has_vmax) vmax = user_vmax;
        if (vmax <= vmin) vmax = vmin + 1.0;
        fprintf(stderr, "rendering var=%s t=%d l=%d range=[%g, %g]%s\n",
                var_name, t_idx, l_idx, vmin, vmax,
                (has_vmin || has_vmax) ? " (user-fixed)" : "");

        Raster *r = raster_create(width, height);
        raster_clear(r, 0xeeeeee);
        Viewport vp = { -M_PI, M_PI, -M_PI / 2.0, M_PI / 2.0 };
        if (!force_global && m->lon_max > m->lon_min && m->lat_max > m->lat_min) {
            double lon_span = m->lon_max - m->lon_min;
            double lat_span = m->lat_max - m->lat_min;
            if (lon_span < 2.0 * M_PI * 0.85 || lat_span < M_PI * 0.85) {
                double pad_lon = lon_span * 0.05;
                double pad_lat = lat_span * 0.05;
                vp.lon_min = m->lon_min - pad_lon;
                vp.lon_max = m->lon_max + pad_lon;
                vp.lat_min = m->lat_min - pad_lat;
                vp.lat_max = m->lat_max + pad_lat;
                if (vp.lat_min < -M_PI / 2.0) vp.lat_min = -M_PI / 2.0;
                if (vp.lat_max >  M_PI / 2.0) vp.lat_max =  M_PI / 2.0;
                fprintf(stderr, "regional auto-fit: lon=[%.3f, %.3f] lat=[%.3f, %.3f]\n",
                        vp.lon_min, vp.lon_max, vp.lat_min, vp.lat_max);
            }
        }
        raster_render_mesh(r, m, values, vmin, vmax, fill, &vp,
                           colormap_by_name(cmap_name));

        for (int li = 0; li < n_layers; li++) {
            if (layers[li].poly) {
                raster_render_polylines(r, layers[li].poly, &vp, layers[li].color);
            }
        }

        int cb_w = 320;
        int cb_h = 14;
        int cb_x = 24;
        int cb_y = height - 64;
        char cb_label[128];
        char var_units[64] = "";
        nc_get_var_units(mnc->files[0], var_idx, var_units, sizeof(var_units));
        if (var_units[0])
            snprintf(cb_label, sizeof(cb_label), "%s (%s)", var_name, var_units);
        else
            snprintf(cb_label, sizeof(cb_label), "%s", var_name);
        raster_draw_colorbar(r, colormap_by_name(cmap_name), vmin, vmax,
                             cb_label, cb_x, cb_y, cb_w, cb_h,
                             0xf5f5f5, 0x202020);

        if (png_export(out_png, r, 0xeeeeee) == 0) {
            fprintf(stderr, "wrote %s\n", out_png);
        } else {
            rc_main = 1;
        }
        raster_free(r);
        free(values);
    } else {
#ifdef UNSVIEW_HAVE_X11
        rc_main = gui_run(mnc, m, var_name, cmap_name,
                          has_vmin, user_vmin, has_vmax, user_vmax,
                          layers, n_layers,
                          force_global);
#else
        fprintf(stderr,
                "GUI not compiled in (no X11 detected at build time).\n"
                "Use -o PATH for headless PNG mode.\n");
        rc_main = 1;
#endif
    }

done:
    for (int li = 0; li < n_layers; li++) polylines_free(layers[li].poly);
    mesh_free(m);
    for (int k = 0; k < n_grid_ncids; k++) nc_close(grid_ncids[k]);
    multinc_free(mnc);
    return rc_main;
}
