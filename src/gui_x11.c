#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Xaw/Form.h>
#include <X11/Xaw/Paned.h>
#include <X11/Xaw/List.h>
#include <X11/Xaw/Viewport.h>
#include <X11/Xaw/Label.h>
#include <X11/Xaw/Box.h>
#include <X11/Xaw/Command.h>
#include <X11/Xaw/Simple.h>
#include <X11/Xaw/Dialog.h>
#include <X11/Shell.h>

#include "nc_io.h"
#include "mesh.h"
#include "raster.h"
#include "colormap.h"
#include "polylines.h"

/* Defined in src/png_export.c */
int png_export(const char *path, const Raster *r, uint32_t bg);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    MultiNc *mnc;
    UnsMesh *m;

    int var_idx;
    int t_idx;
    int l_idx;
    const uint32_t *palette;
    char cmap_name[32];

    Viewport vp;
    Raster *raster;
    XImage *ximage;

    Display *dpy;
    Widget top;
    Widget canvas;
    Widget status;
    Widget range_popup;
    Widget save_popup;
    GC gc;

    double *values;
    double fill;
    double vmin, vmax;
    double data_vmin, data_vmax;
    int has_user_vmin;
    int has_user_vmax;

    int canvas_w;
    int canvas_h;

    int dragging;
    int drag_x, drag_y;
    int press_x, press_y;   /* anchor of the current Button1 press */
    int moved;              /* set once the press turns into a real drag */

    const PolyLayer *layers;
    int n_layers;
    char   var_units[64];   /* "units" attribute of current variable, or "" */

    /* persistent cursor state — updated on hover, always shown in status bar */
    int    last_cell;
    double last_lon, last_lat, last_val;
    /* per-type cycle state for the typed line layers
     *   1 = white (default), 2 = black, 0 = off
     * Generic --lines layers are not affected by this and always render
     * with their per-layer color. */
    int layer_state[POLY_TYPE_COUNT];
} App;

static App g_app;

static void update_status(App *a) {
    NcFile *f0 = a->mnc->files[0];
    const char *vname = (a->var_idx >= 0 && a->var_idx < f0->n_vars_plottable)
                        ? f0->var_names[a->var_idx] : "(none)";
    char timebuf[64];
    if (a->mnc->n_files > 1) {
        int fi, lt;
        multinc_resolve(a->mnc, a->t_idx, &fi, &lt);
        snprintf(timebuf, sizeof(timebuf), "t=%d [file %d/%d  local t=%d]",
                 a->t_idx, fi + 1, a->mnc->n_files, lt);
    } else {
        snprintf(timebuf, sizeof(timebuf), "t=%d", a->t_idx);
    }
    char cursorbuf[160];
    if (a->last_cell >= 0) {
        snprintf(cursorbuf, sizeof(cursorbuf),
                 "   lon=%.3f  lat=%.3f  cell=%d  val=%g",
                 a->last_lon * 180.0 / M_PI, a->last_lat * 180.0 / M_PI,
                 a->last_cell, a->last_val);
    } else {
        snprintf(cursorbuf, sizeof(cursorbuf), "   (hover for value)");
    }
    char buf[512];
    snprintf(buf, sizeof(buf), "%s  %s  l=%d  range=[%g, %g]  cmap=%s%s",
             vname, timebuf, a->l_idx, a->vmin, a->vmax, a->cmap_name, cursorbuf);
    XtVaSetValues(a->status, XtNlabel, buf, NULL);
}

static void ensure_raster(App *a, int w, int h) {
    if (a->raster && a->raster->width == w && a->raster->height == h) return;
    if (a->raster) raster_free(a->raster);
    a->raster = raster_create(w, h);
    if (a->ximage) {
        XDestroyImage(a->ximage);
        a->ximage = NULL;
    }
}

/* Blit a Raster into a window via its XImage, creating the image on first use.
 * Pixels with alpha==0 are painted with bg. Shared by the main canvas and the
 * profile/time-series popups.
 *
 * Fast path: 32-bpp ZPixmap with the standard RGB888 mask layout (true on
 * essentially every modern X server, including XQuartz). Write the pre-packed
 * ARGB words straight into the image buffer — XPutPixel goes through
 * visual-conversion logic per pixel and is the dominant blit cost. Fallback
 * path uses XPutPixel for unusual visuals. */
static void blit_raster(Display *dpy, Window win, GC gc,
                        XImage **pim, Raster *r, uint32_t bg) {
    if (!r) return;
    if (!*pim) {
        int depth = DefaultDepth(dpy, DefaultScreen(dpy));
        Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
        char *data = malloc((size_t)r->width * (size_t)r->height * 4);
        *pim = XCreateImage(dpy, vis, depth, ZPixmap, 0,
                            data, r->width, r->height, 32, 0);
    }
    XImage *im = *pim;
    Visual *vis = DefaultVisual(dpy, DefaultScreen(dpy));
    int n = r->width * r->height;
    int fast = (im->bits_per_pixel == 32 &&
                vis->red_mask   == 0x00ff0000 &&
                vis->green_mask == 0x0000ff00 &&
                vis->blue_mask  == 0x000000ff &&
                im->byte_order  == LSBFirst);
    if (fast) {
        uint32_t *out = (uint32_t *)im->data;
        const uint32_t *src = r->pixels;
        const uint8_t  *al  = r->alpha;
        for (int i = 0; i < n; i++) {
            out[i] = al[i] ? src[i] : bg;
        }
    } else {
        for (int y = 0; y < r->height; y++) {
            for (int x = 0; x < r->width; x++) {
                size_t k = (size_t)y * (size_t)r->width + (size_t)x;
                uint32_t c = r->alpha[k] ? r->pixels[k] : bg;
                XPutPixel(im, x, y, c);
            }
        }
    }
    if (win) {
        XPutImage(dpy, win, gc, im, 0, 0, 0, 0, r->width, r->height);
    }
}

static void blit(App *a) {
    if (!a->raster) return;
    blit_raster(a->dpy, XtWindow(a->canvas), a->gc, &a->ximage,
                a->raster, 0xeeeeeeu);
}

static void redraw(App *a) {
    Dimension cw = 0, ch = 0;
    XtVaGetValues(a->canvas, XtNwidth, &cw, XtNheight, &ch, NULL);
    if (cw < 32) cw = 32;
    if (ch < 32) ch = 32;
    a->canvas_w = cw;
    a->canvas_h = ch;
    ensure_raster(a, cw, ch);
    raster_clear(a->raster, 0xeeeeee);
    if (a->values && a->vmax > a->vmin) {
        raster_render_mesh(a->raster, a->m, a->values,
                           a->vmin, a->vmax, a->fill,
                           &a->vp, a->palette);

        for (int li = 0; li < a->n_layers; li++) {
            const PolyLayer *L = &a->layers[li];
            if (!L->poly) continue;
            uint32_t color = L->color;
            if (L->type < POLY_TYPE_GENERIC) {
                int s = a->layer_state[L->type];
                if (s == 0) continue;
                color = (s == 1) ? 0xffffff : 0x000000;
            }
            raster_render_polylines(a->raster, L->poly, &a->vp, color);
        }

        int cb_w = 320;
        int cb_h = 14;
        int cb_x = 24;
        int cb_y = a->raster->height - 64;
        const char *vname = (a->var_idx >= 0 && a->var_idx < a->mnc->files[0]->n_vars_plottable)
                            ? a->mnc->files[0]->var_names[a->var_idx] : "";
        char cb_label[128];
        if (vname[0] && a->var_units[0])
            snprintf(cb_label, sizeof(cb_label), "%s (%s)", vname, a->var_units);
        else
            snprintf(cb_label, sizeof(cb_label), "%s", vname);
        raster_draw_colorbar(a->raster, a->palette, a->vmin, a->vmax,
                             cb_label, cb_x, cb_y, cb_w, cb_h,
                             0xf5f5f5, 0x202020);
    }
    blit(a);
}

static void load_variable(App *a) {
    free(a->values);
    a->values = NULL;
    NcFile *f0 = a->mnc->files[0];
    if (a->var_idx < 0 || a->var_idx >= f0->n_vars_plottable) return;
    a->values = multinc_read_slab(a->mnc, a->var_idx, a->t_idx, a->l_idx, &a->fill);
    nc_get_var_units(f0, a->var_idx, a->var_units, sizeof(a->var_units));
    if (!a->values) return;
    /* Node-centered fields arrive with one value per node; collapse onto faces
     * so everything downstream can assume a->values has ncells entries. */
    if (f0->var_on_node[a->var_idx]) {
        double *face_vals = malloc(sizeof(double) * a->m->ncells);
        mesh_node_to_face(a->m, a->values, face_vals);
        free(a->values);
        a->values = face_vals;
    }
    if (!a->has_user_vmin || !a->has_user_vmax) {
        double lo = INFINITY, hi = -INFINITY;
        int has_fill = !isnan(a->fill);
        for (size_t c = 0; c < a->m->ncells; c++) {
            double v = a->values[c];
            if (isnan(v)) continue;
            if (has_fill && v == a->fill) continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (!isfinite(lo)) lo = 0.0, hi = 1.0;
        if (!a->has_user_vmin) a->vmin = lo;
        if (!a->has_user_vmax) a->vmax = hi;
        if (a->vmax <= a->vmin) a->vmax = a->vmin + 1.0;
        a->data_vmin = lo;
        a->data_vmax = hi;
    }
    redraw(a);
    update_status(a);
}

static int has_layer_of_type(const App *a, PolyLayerType t) {
    for (int i = 0; i < a->n_layers; i++) {
        if (a->layers[i].poly && a->layers[i].type == t) return 1;
    }
    return 0;
}

static void cycle_layer_state(PolyLayerType t) {
    /* 1 (white) -> 2 (black) -> 0 (off) -> 1 (white) ... */
    static const int next_state[3] = { 1, 2, 0 };
    g_app.layer_state[t] = next_state[g_app.layer_state[t]];
    redraw(&g_app);
}

static void toggle_coast_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call; cycle_layer_state(POLY_TYPE_COAST);
}
static void toggle_borders_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call; cycle_layer_state(POLY_TYPE_BORDERS);
}
static void toggle_states_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call; cycle_layer_state(POLY_TYPE_STATES);
}

static void auto_range_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    g_app.has_user_vmin = 0;
    g_app.has_user_vmax = 0;
    g_app.vmin = g_app.data_vmin;
    g_app.vmax = g_app.data_vmax;
    redraw(&g_app);
}

static void range_dialog_ok_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    Widget dialog = (Widget)client;
    String s = XawDialogGetValueString(dialog);
    if (s) {
        double a, b;
        if (sscanf(s, "%lf %lf", &a, &b) == 2 && b > a) {
            g_app.vmin = a;
            g_app.vmax = b;
            g_app.has_user_vmin = 1;
            g_app.has_user_vmax = 1;
        }
    }
    if (g_app.range_popup) XtPopdown(g_app.range_popup);
    redraw(&g_app);
}

static void range_dialog_cancel_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.range_popup) XtPopdown(g_app.range_popup);
}

static void about_ok_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    XtPopdown((Widget)client);
    XtDestroyWidget((Widget)client);
}

static void open_about_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    Widget shell = XtVaCreatePopupShell(
        "about", transientShellWidgetClass, g_app.top,
        XtNtitle, "About unsview",
        NULL);
    Widget form = XtVaCreateManagedWidget("form", formWidgetClass, shell, NULL);
    Widget lbl = XtVaCreateManagedWidget(
        "text", labelWidgetClass, form,
        XtNlabel,
            "unsview -- MPAS unstructured-mesh viewer\n"
            "\n"
            "Built with:\n"
            "  C99 / libnetcdf / libpng\n"
            "  X11 / Xt / Xaw (Athena Widgets)\n"
            "\n"
            "Overlay data:\n"
            "  Natural Earth  (naturalearthdata.com)\n"
            "  coastlines 110m / country borders 50m / state lines 50m\n"
            "\n"
            "Made by Bon Standard",
        XtNborderWidth,  0,
        XtNinternalWidth,  20,
        XtNinternalHeight, 12,
        XtNjustify, XtJustifyLeft,
        NULL);
    Widget btn = XtVaCreateManagedWidget(
        "ok", commandWidgetClass, form,
        XtNlabel,    "  OK  ",
        XtNfromVert, lbl,
        NULL);
    XtAddCallback(btn, XtNcallback, about_ok_cb, (XtPointer)shell);
    XtPopup(shell, XtGrabNone);
}

static void save_dialog_ok_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    Widget dialog = (Widget)client;
    String s = XawDialogGetValueString(dialog);
    if (s && *s && g_app.raster) {
        if (png_export(s, g_app.raster, 0xeeeeee) == 0) {
            char buf[600];
            snprintf(buf, sizeof(buf), "saved %s  (%dx%d)",
                     s, g_app.raster->width, g_app.raster->height);
            XtVaSetValues(g_app.status, XtNlabel, buf, NULL);
        } else {
            char buf[600];
            snprintf(buf, sizeof(buf), "FAILED to write %s", s);
            XtVaSetValues(g_app.status, XtNlabel, buf, NULL);
        }
    }
    if (g_app.save_popup) XtPopdown(g_app.save_popup);
}

static void save_dialog_cancel_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.save_popup) XtPopdown(g_app.save_popup);
}

static void open_save_dialog_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;

    /* Default filename: visualize_<var>_t<idx>.png in cwd */
    char init[256];
    const char *vn = (g_app.var_idx >= 0 && g_app.var_idx < g_app.mnc->files[0]->n_vars_plottable)
                     ? g_app.mnc->files[0]->var_names[g_app.var_idx] : "out";
    snprintf(init, sizeof(init), "visualize_%s_t%d.png", vn, g_app.t_idx);

    if (g_app.save_popup) {
        XtDestroyWidget(g_app.save_popup);
        g_app.save_popup = NULL;
    }
    g_app.save_popup = XtVaCreatePopupShell(
        "savePng", transientShellWidgetClass, g_app.top,
        XtNtitle, "Save current view as PNG", NULL);
    Widget dialog = XtVaCreateManagedWidget(
        "dialog", dialogWidgetClass, g_app.save_popup,
        XtNlabel, "Output PNG path:",
        XtNvalue, init, NULL);
    XawDialogAddButton(dialog, "Save",   save_dialog_ok_cb,     dialog);
    XawDialogAddButton(dialog, "Cancel", save_dialog_cancel_cb, dialog);
    XtPopup(g_app.save_popup, XtGrabExclusive);
}

static void open_range_dialog_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    char init[64];
    snprintf(init, sizeof(init), "%g %g", g_app.vmin, g_app.vmax);

    if (g_app.range_popup) {
        XtDestroyWidget(g_app.range_popup);
        g_app.range_popup = NULL;
    }

    g_app.range_popup = XtVaCreatePopupShell(
        "setRange", transientShellWidgetClass, g_app.top,
        XtNtitle, "Set color range", NULL);

    Widget dialog = XtVaCreateManagedWidget(
        "dialog", dialogWidgetClass, g_app.range_popup,
        XtNlabel, "Enter vmin vmax (space-separated):",
        XtNvalue, init, NULL);

    XawDialogAddButton(dialog, "Apply", range_dialog_ok_cb, dialog);
    XawDialogAddButton(dialog, "Cancel", range_dialog_cancel_cb, dialog);

    XtPopup(g_app.range_popup, XtGrabExclusive);
}

static void var_select_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)client;
    XawListReturnStruct *r = (XawListReturnStruct *)call;
    if (r && r->list_index >= 0) {
        g_app.var_idx = r->list_index;
        load_variable(&g_app);
    }
}

static void prev_t_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.t_idx > 0) {
        g_app.t_idx--;
        load_variable(&g_app);
    }
}

static void next_t_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.t_idx + 1 < g_app.mnc->total_time) {
        g_app.t_idx++;
        load_variable(&g_app);
    }
}

static void prev_l_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.l_idx > 0) {
        g_app.l_idx--;
        load_variable(&g_app);
    }
}

static void next_l_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    if (g_app.mnc->files[0]->nlevels > 0 && (size_t)(g_app.l_idx + 1) < g_app.mnc->files[0]->nlevels) {
        g_app.l_idx++;
        load_variable(&g_app);
    }
}

static void set_cmap_at(int idx) {
    int n = colormap_count();
    idx = ((idx % n) + n) % n;
    const char *nm = colormap_name_at(idx);
    if (nm) {
        strncpy(g_app.cmap_name, nm, sizeof(g_app.cmap_name) - 1);
        g_app.cmap_name[sizeof(g_app.cmap_name) - 1] = 0;
    }
    g_app.palette = colormap_by_name(g_app.cmap_name);
    redraw(&g_app);
    update_status(&g_app);
}

static int current_cmap_index(void) {
    int n = colormap_count();
    for (int i = 0; i < n; i++) {
        const char *nm = colormap_name_at(i);
        if (nm && !strcmp(nm, g_app.cmap_name)) return i;
    }
    return 0;
}

static void cmap_next_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    set_cmap_at(current_cmap_index() + 1);
}

static void cmap_prev_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    set_cmap_at(current_cmap_index() - 1);
}

/* Zoom buttons: same effect as scroll wheel; no extra memory cost since
 * zoom is just two doubles in the Viewport struct. */
static void zoom_apply(double factor) {
    double cx = (g_app.vp.lon_min + g_app.vp.lon_max) / 2.0;
    double cy = (g_app.vp.lat_min + g_app.vp.lat_max) / 2.0;
    double zw = (g_app.vp.lon_max - g_app.vp.lon_min) * factor;
    double zh = (g_app.vp.lat_max - g_app.vp.lat_min) * factor;
    if (zw > 2.5 * M_PI) zw = 2.5 * M_PI;
    if (zh > 1.25 * M_PI) zh = 1.25 * M_PI;
    g_app.vp.lon_min = cx - zw / 2.0;
    g_app.vp.lon_max = cx + zw / 2.0;
    g_app.vp.lat_min = cy - zh / 2.0;
    g_app.vp.lat_max = cy + zh / 2.0;
    redraw(&g_app);
}
static void zoom_in_cb(Widget w, XtPointer c, XtPointer call)  {
    (void)w; (void)c; (void)call; zoom_apply(0.7);
}
static void zoom_out_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call; zoom_apply(1.4);
}

static void reset_view_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    g_app.vp.lon_min = -M_PI;
    g_app.vp.lon_max =  M_PI;
    g_app.vp.lat_min = -M_PI / 2.0;
    g_app.vp.lat_max =  M_PI / 2.0;
    redraw(&g_app);
}

/* Fit viewport to the mesh's data extent (cell-center bounding box, padded).
 * Useful for regional MPAS grids that you don't want to start at global view. */
static void fit_data_cb(Widget w, XtPointer c, XtPointer call) {
    (void)w; (void)c; (void)call;
    const UnsMesh *m = g_app.m;
    if (!m || m->lon_max <= m->lon_min || m->lat_max <= m->lat_min) return;
    double pad_lon = (m->lon_max - m->lon_min) * 0.05;
    double pad_lat = (m->lat_max - m->lat_min) * 0.05;
    g_app.vp.lon_min = m->lon_min - pad_lon;
    g_app.vp.lon_max = m->lon_max + pad_lon;
    g_app.vp.lat_min = m->lat_min - pad_lat;
    g_app.vp.lat_max = m->lat_max + pad_lat;
    if (g_app.vp.lat_min < -M_PI / 2.0) g_app.vp.lat_min = -M_PI / 2.0;
    if (g_app.vp.lat_max >  M_PI / 2.0) g_app.vp.lat_max =  M_PI / 2.0;
    redraw(&g_app);
}

static int pick_cell_naive(const App *a, double lon, double lat, double *out_val) {
    int best = -1;
    double bestd = INFINITY;
    for (size_t c = 0; c < a->m->ncells; c++) {
        double dlon = lon - a->m->lon_cell[c];
        while (dlon >  M_PI) dlon -= 2.0 * M_PI;
        while (dlon < -M_PI) dlon += 2.0 * M_PI;
        double dlat = lat - a->m->lat_cell[c];
        double d = dlon * dlon + dlat * dlat;
        if (d < bestd) { bestd = d; best = (int)c; }
    }
    if (best >= 0 && a->values) *out_val = a->values[best];
    return best;
}

/* ---- click-to-plot: per-cell vertical profile / time series popup ---- */

typedef struct {
    Widget   shell;
    Widget   canvas;
    Widget   toggle_btn;     /* NULL unless both profile and series exist */
    Display *dpy;
    GC       gc;
    XImage  *ximage;
    Raster  *raster;

    int    cell;
    double lon, lat;         /* radians */
    int    t_idx, l_idx;     /* fixed context at click time */
    char   var_name[128];
    char   var_units[64];

    int    has_profile, has_series;
    int    mode;             /* 0 = profile, 1 = time series */

    double *prof; int nprof; /* value per level, at fixed time */
    double *ser;  int nser;  /* value per global time, at fixed level */
} PlotWin;

static void plot_render(PlotWin *p) {
    Dimension cw = 0, ch = 0;
    XtVaGetValues(p->canvas, XtNwidth, &cw, XtNheight, &ch, NULL);
    if (cw < 80) cw = 80;
    if (ch < 80) ch = 80;
    if (p->raster && (p->raster->width != (int)cw || p->raster->height != (int)ch)) {
        raster_free(p->raster);
        p->raster = NULL;
        if (p->ximage) { XDestroyImage(p->ximage); p->ximage = NULL; }
    }
    if (!p->raster) p->raster = raster_create(cw, ch);
    Raster *r = p->raster;
    raster_clear(r, 0xffffff);

    int profile = (p->mode == 0);
    int n        = profile ? p->nprof : p->nser;
    const double *ys = profile ? p->prof : p->ser;

    const uint32_t fg = 0x202020, axc = 0x808080, dim = 0x707070;
    const uint32_t line_col = 0x1f77b4;

    /* header lines */
    char hdr[256];
    if (p->var_units[0])
        snprintf(hdr, sizeof(hdr), "%s (%s)   cell=%d  lon=%.3f  lat=%.3f",
                 p->var_name, p->var_units, p->cell,
                 p->lon * 180.0 / M_PI, p->lat * 180.0 / M_PI);
    else
        snprintf(hdr, sizeof(hdr), "%s   cell=%d  lon=%.3f  lat=%.3f",
                 p->var_name, p->cell,
                 p->lon * 180.0 / M_PI, p->lat * 180.0 / M_PI);
    char ctx[160];
    if (profile)
        snprintf(ctx, sizeof(ctx), "vertical profile   (t=%d)   level 0..%d",
                 p->t_idx, n - 1);
    else
        snprintf(ctx, sizeof(ctx), "time series   (level=%d)   t 0..%d",
                 p->l_idx, n - 1);
    int th = raster_text_height(1);
    raster_draw_text(r, 8, 6, hdr, fg, 1);
    raster_draw_text(r, 8, 6 + th + 2, ctx, dim, 1);

    int ml = 70, mr = 18;
    int mt = 6 + 2 * (th + 2) + 10;
    int mb = 26;
    int px0 = ml, py0 = mt;
    int pw = r->width  - ml - mr;
    int ph = r->height - mt - mb;
    if (pw < 16 || ph < 16) { blit_raster(p->dpy, XtWindow(p->canvas), p->gc, &p->ximage, r, 0xffffffu); return; }
    raster_draw_rect(r, px0, py0, pw, ph, axc);

    double vmin = INFINITY, vmax = -INFINITY;
    int finite = 0;
    for (int i = 0; i < n; i++) {
        double v = ys[i];
        if (isnan(v)) continue;
        finite++;
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    if (!finite) {
        raster_draw_text(r, px0 + 8, py0 + ph / 2 - th / 2, "no valid data", dim, 1);
        blit_raster(p->dpy, XtWindow(p->canvas), p->gc, &p->ximage, r, 0xffffffu);
        return;
    }
    if (vmax <= vmin) { double e = (vmin != 0.0) ? fabs(vmin) * 0.05 : 1.0; vmin -= e; vmax += e; }

    /* connect consecutive finite samples; break the line on NaN gaps */
    int have_prev = 0, prevx = 0, prevy = 0;
    for (int i = 0; i < n; i++) {
        double v = ys[i];
        if (isnan(v)) { have_prev = 0; continue; }
        double fidx = (n > 1) ? (double)i / (double)(n - 1) : 0.5;
        double fval = (v - vmin) / (vmax - vmin);
        int x, y;
        if (profile) {            /* X = value, Y = level (0 at top) */
            x = px0 + (int)(fval * pw);
            y = py0 + (int)(fidx * ph);
        } else {                  /* X = time, Y = value (min at bottom) */
            x = px0 + (int)(fidx * pw);
            y = py0 + ph - (int)(fval * ph);
        }
        raster_fill_rect(r, x - 1, y - 1, 3, 3, line_col);
        if (have_prev) raster_draw_line(r, prevx, prevy, x, y, line_col);
        prevx = x; prevy = y; have_prev = 1;
    }

    /* axis labels */
    char lo[40], hi[40];
    snprintf(lo, sizeof(lo), "%g", vmin);
    snprintf(hi, sizeof(hi), "%g", vmax);
    if (profile) {
        /* value axis along the bottom, level axis down the left */
        raster_draw_text(r, px0, py0 + ph + 6, lo, dim, 1);
        raster_draw_text(r, px0 + pw - raster_text_width(hi, 1), py0 + ph + 6, hi, dim, 1);
        raster_draw_text(r, 8, py0 - th - 2, "lvl 0", dim, 1);
        char lvb[24]; snprintf(lvb, sizeof(lvb), "lvl %d", n - 1);
        raster_draw_text(r, 8, py0 + ph - th, lvb, dim, 1);
    } else {
        /* value axis up the left, time axis along the bottom */
        raster_draw_text(r, 8, py0, hi, dim, 1);
        raster_draw_text(r, 8, py0 + ph - th, lo, dim, 1);
        raster_draw_text(r, px0, py0 + ph + 6, "t=0", dim, 1);
        char tb[24]; snprintf(tb, sizeof(tb), "t=%d", n - 1);
        raster_draw_text(r, px0 + pw - raster_text_width(tb, 1), py0 + ph + 6, tb, dim, 1);
    }

    blit_raster(p->dpy, XtWindow(p->canvas), p->gc, &p->ximage, r, 0xffffffu);
}

static void plot_event_cb(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)w; (void)cont;
    PlotWin *p = (PlotWin *)client;
    if (ev->type == Expose) {
        if (ev->xexpose.count == 0) plot_render(p);
    } else if (ev->type == ConfigureNotify) {
        plot_render(p);
    }
}

static void plot_toggle_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    PlotWin *p = (PlotWin *)client;
    if (!(p->has_profile && p->has_series)) return;
    p->mode = !p->mode;
    XtVaSetValues(p->toggle_btn, XtNlabel,
                  p->mode ? "show profile" : "show series", NULL);
    plot_render(p);
}

static void plot_close_cb(Widget w, XtPointer client, XtPointer call) {
    (void)w; (void)call;
    PlotWin *p = (PlotWin *)client;
    XtPopdown(p->shell);
    if (p->ximage) { XDestroyImage(p->ximage); p->ximage = NULL; }
    if (p->raster) { raster_free(p->raster); p->raster = NULL; }
    if (p->gc) XFreeGC(p->dpy, p->gc);
    free(p->prof);
    free(p->ser);
    XtDestroyWidget(p->shell);
    free(p);
}

static void open_plot_popup(App *a, int cell, double lon, double lat) {
    NcFile *f0 = a->mnc->files[0];
    int vidx = a->var_idx;
    if (vidx < 0 || vidx >= f0->n_vars_plottable) return;

    int has_level = f0->var_has_level[vidx] && f0->nlevels > 1;
    int has_time  = f0->var_has_time[vidx]  && a->mnc->total_time > 1;
    if (!has_level && !has_time) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "cell=%d: '%s' has no level/time dimension to plot",
                 cell, f0->var_names[vidx]);
        XtVaSetValues(a->status, XtNlabel, buf, NULL);
        return;
    }

    PlotWin *p = calloc(1, sizeof(*p));
    p->cell = cell;
    p->lon = lon; p->lat = lat;
    p->t_idx = a->t_idx; p->l_idx = a->l_idx;
    strncpy(p->var_name, f0->var_names[vidx], sizeof(p->var_name) - 1);
    strncpy(p->var_units, a->var_units, sizeof(p->var_units) - 1);
    p->has_profile = has_level;
    p->has_series  = has_time;
    p->mode = has_level ? 0 : 1;

    /* multinc_read_cell_value indexes the variable's own mesh axis. For a
     * node-centered field that is a node, so sample the clicked cell's first
     * corner rather than pretending a cell index means anything there. */
    int probe = cell;
    if (f0->var_on_node[vidx]) {
        probe = -1;
        for (int e = 0; e < a->m->n_edges_on_cell[cell]; e++) {
            int vi = a->m->vertices_on_cell[(size_t)cell * (size_t)a->m->max_edges + (size_t)e];
            if (vi >= 0) { probe = vi; break; }
        }
    }

    if (has_level && probe >= 0) {
        int nl = (int)f0->nlevels;
        p->prof = malloc(sizeof(double) * nl);
        p->nprof = nl;
        for (int l = 0; l < nl; l++)
            p->prof[l] = multinc_read_cell_value(a->mnc, vidx, a->t_idx, l, probe, NULL);
    }
    if (has_time && probe >= 0) {
        int nt = a->mnc->total_time;
        p->ser = malloc(sizeof(double) * nt);
        p->nser = nt;
        for (int t = 0; t < nt; t++)
            p->ser[t] = multinc_read_cell_value(a->mnc, vidx, t, a->l_idx, probe, NULL);
    }

    char title[200];
    snprintf(title, sizeof(title), "%s @ cell %d  (lon=%.2f lat=%.2f)",
             p->var_name, cell, lon * 180.0 / M_PI, lat * 180.0 / M_PI);
    p->shell = XtVaCreatePopupShell("plot", transientShellWidgetClass, a->top,
                                    XtNtitle, title, NULL);
    Widget form = XtVaCreateManagedWidget("form", formWidgetClass, p->shell, NULL);
    p->canvas = XtVaCreateManagedWidget("plotcanvas", simpleWidgetClass, form,
                                        XtNwidth, 480, XtNheight, 340, NULL);
    Widget close = XtVaCreateManagedWidget("close", commandWidgetClass, form,
                                           XtNlabel, " close ",
                                           XtNfromVert, p->canvas, NULL);
    XtAddCallback(close, XtNcallback, plot_close_cb, (XtPointer)p);
    if (has_level && has_time) {
        p->toggle_btn = XtVaCreateManagedWidget("toggle", commandWidgetClass, form,
                                                XtNlabel, "show series",
                                                XtNfromVert, p->canvas,
                                                XtNfromHoriz, close, NULL);
        XtAddCallback(p->toggle_btn, XtNcallback, plot_toggle_cb, (XtPointer)p);
    }
    XtAddEventHandler(p->canvas, ExposureMask | StructureNotifyMask,
                      False, plot_event_cb, (XtPointer)p);

    XtPopup(p->shell, XtGrabNone);
    p->dpy = XtDisplay(p->shell);
    p->gc = XCreateGC(p->dpy, XtWindow(p->canvas), 0, NULL);
    plot_render(p);
}

static void canvas_event_cb(Widget w, XtPointer client, XEvent *ev, Boolean *cont) {
    (void)w; (void)client; (void)cont;
    App *a = &g_app;

    if (ev->type == Expose) {
        if (ev->xexpose.count == 0) blit(a);
    } else if (ev->type == ConfigureNotify) {
        redraw(a);
    } else if (ev->type == ButtonPress) {
        if (ev->xbutton.button == Button1) {
            a->dragging = 1;
            a->drag_x = ev->xbutton.x;
            a->drag_y = ev->xbutton.y;
            a->press_x = ev->xbutton.x;
            a->press_y = ev->xbutton.y;
            a->moved = 0;
        } else if (ev->xbutton.button == Button4 || ev->xbutton.button == Button5) {
            double cx = (a->vp.lon_min + a->vp.lon_max) / 2.0;
            double cy = (a->vp.lat_min + a->vp.lat_max) / 2.0;
            double zw = a->vp.lon_max - a->vp.lon_min;
            double zh = a->vp.lat_max - a->vp.lat_min;
            double f = (ev->xbutton.button == Button4) ? 0.8 : 1.25;
            zw *= f;
            zh *= f;
            if (zw > 2.5 * M_PI) zw = 2.5 * M_PI;
            if (zh > 1.25 * M_PI) zh = 1.25 * M_PI;
            a->vp.lon_min = cx - zw / 2.0;
            a->vp.lon_max = cx + zw / 2.0;
            a->vp.lat_min = cy - zh / 2.0;
            a->vp.lat_max = cy + zh / 2.0;
            redraw(a);
        }
    } else if (ev->type == ButtonRelease) {
        if (ev->xbutton.button == Button1) {
            a->dragging = 0;
            /* A press+release that didn't turn into a drag is a pick: open the
             * profile/time-series popup for the cell under the cursor. */
            if (!a->moved && a->values) {
                int px = ev->xbutton.x;
                int py = ev->xbutton.y;
                double lon = a->vp.lon_min + (double)px / a->canvas_w * (a->vp.lon_max - a->vp.lon_min);
                double lat = a->vp.lat_min + (1.0 - (double)py / a->canvas_h) * (a->vp.lat_max - a->vp.lat_min);
                double val = NAN;
                int cell = pick_cell_naive(a, lon, lat, &val);
                if (cell >= 0) {
                    a->last_lon = lon;
                    a->last_lat = lat;
                    a->last_cell = cell;
                    a->last_val = val;
                    update_status(a);
                    open_plot_popup(a, cell, lon, lat);
                }
            }
        }
    } else if (ev->type == MotionNotify) {
        int px = ev->xmotion.x;
        int py = ev->xmotion.y;
        if (a->dragging) {
            if (!a->moved &&
                (abs(px - a->press_x) > 3 || abs(py - a->press_y) > 3))
                a->moved = 1;
            int dx = px - a->drag_x;
            int dy = py - a->drag_y;
            a->drag_x = px;
            a->drag_y = py;
            double sx = (a->vp.lon_max - a->vp.lon_min) / a->canvas_w;
            double sy = (a->vp.lat_max - a->vp.lat_min) / a->canvas_h;
            a->vp.lon_min -= dx * sx;
            a->vp.lon_max -= dx * sx;
            a->vp.lat_min += dy * sy;
            a->vp.lat_max += dy * sy;
            redraw(a);
        } else {
            double lon = a->vp.lon_min + (double)px / a->canvas_w * (a->vp.lon_max - a->vp.lon_min);
            double lat = a->vp.lat_min + (1.0 - (double)py / a->canvas_h) * (a->vp.lat_max - a->vp.lat_min);
            double val = NAN;
            int cell = -1;
            /* Throttle the O(ncells) hover lookup: only run on every Nth motion
             * event so big meshes don't lock the UI. The cursor lon/lat still
             * updates every frame; the cell/value just lags slightly. */
            static int motion_skip = 0;
            int skip_n = (a->m->ncells <=  20000) ? 1
                       : (a->m->ncells <=  80000) ? 3
                       : (a->m->ncells <= 250000) ? 8
                       : 0;  /* skip entirely above 250k */
            if (skip_n > 0 && a->values && (motion_skip++ % skip_n) == 0) {
                cell = pick_cell_naive(a, lon, lat, &val);
            }
            if (cell >= 0) {
                a->last_lon  = lon;
                a->last_lat  = lat;
                a->last_cell = cell;
                a->last_val  = val;
            }
            update_status(a);
        }
    }
}

int gui_run(MultiNc *mnc, UnsMesh *m, const char *initial_var, const char *cmap_name,
            int has_vmin, double user_vmin, int has_vmax, double user_vmax,
            const PolyLayer *layers, int n_layers,
            int force_global) {
    memset(&g_app, 0, sizeof(g_app));
    g_app.mnc = mnc;
    g_app.last_cell = -1;
    g_app.m = m;
    g_app.layers = layers;
    g_app.n_layers = n_layers;
    for (int t = 0; t < POLY_TYPE_COUNT; t++) g_app.layer_state[t] = 1; /* white */
    g_app.var_idx = 0;
    strncpy(g_app.cmap_name, cmap_name ? cmap_name : "viridis",
            sizeof(g_app.cmap_name) - 1);
    g_app.palette = colormap_by_name(g_app.cmap_name);
    g_app.vp.lon_min = -M_PI;
    g_app.vp.lon_max =  M_PI;
    g_app.vp.lat_min = -M_PI / 2.0;
    g_app.vp.lat_max =  M_PI / 2.0;
    /* If the mesh's cell-center extent is clearly regional (covers <85% of
     * the globe in either axis), start zoomed in on it. Otherwise stick with
     * the global view. --global forces the global view regardless. */
    if (!force_global && m && m->lon_max > m->lon_min && m->lat_max > m->lat_min) {
        double lon_span = m->lon_max - m->lon_min;
        double lat_span = m->lat_max - m->lat_min;
        if (lon_span < 2.0 * M_PI * 0.85 || lat_span < M_PI * 0.85) {
            double pad_lon = lon_span * 0.05;
            double pad_lat = lat_span * 0.05;
            g_app.vp.lon_min = m->lon_min - pad_lon;
            g_app.vp.lon_max = m->lon_max + pad_lon;
            g_app.vp.lat_min = m->lat_min - pad_lat;
            g_app.vp.lat_max = m->lat_max + pad_lat;
            if (g_app.vp.lat_min < -M_PI / 2.0) g_app.vp.lat_min = -M_PI / 2.0;
            if (g_app.vp.lat_max >  M_PI / 2.0) g_app.vp.lat_max =  M_PI / 2.0;
        }
    }
    g_app.has_user_vmin = has_vmin;
    g_app.has_user_vmax = has_vmax;
    if (has_vmin) g_app.vmin = user_vmin;
    if (has_vmax) g_app.vmax = user_vmax;

    NcFile *f0 = mnc->files[0];
    if (initial_var) {
        for (int v = 0; v < f0->n_vars_plottable; v++) {
            if (!strcmp(f0->var_names[v], initial_var)) {
                g_app.var_idx = v;
                break;
            }
        }
    }

    int argc_dummy = 1;
    char *argv_dummy[2] = { (char*)"unsview", NULL };
    XtAppContext app_ctx;
    Widget top = XtVaAppInitialize(&app_ctx, "Visualize", NULL, 0,
                                   &argc_dummy, argv_dummy, NULL,
                                   XtNwidth, 1100, XtNheight, 640, NULL);

    Widget paned = XtVaCreateManagedWidget("paned", panedWidgetClass, top,
                                           XtNorientation, XtorientHorizontal, NULL);

    Widget left = XtVaCreateManagedWidget("left", formWidgetClass, paned,
                                          XtNwidth, 220, NULL);

    String *names = calloc((size_t)f0->n_vars_plottable + 1, sizeof(String));
    for (int v = 0; v < f0->n_vars_plottable; v++) names[v] = f0->var_names[v];
    names[f0->n_vars_plottable] = NULL;

    Widget vp_w = XtVaCreateManagedWidget("vp", viewportWidgetClass, left,
                                          XtNallowVert, True,
                                          XtNwidth, 220, XtNheight, 600, NULL);
    Widget vlist = XtVaCreateManagedWidget("varlist", listWidgetClass, vp_w,
                                           XtNlist, names,
                                           XtNdefaultColumns, 1,
                                           XtNforceColumns, True, NULL);
    XtAddCallback(vlist, XtNcallback, var_select_cb, NULL);

    Widget right = XtVaCreateManagedWidget("right", formWidgetClass, paned, NULL);

    Widget btn_pt = XtVaCreateManagedWidget("prev_t", commandWidgetClass, right,
                                            XtNlabel, "<< t", NULL);
    XtAddCallback(btn_pt, XtNcallback, prev_t_cb, NULL);
    Widget btn_nt = XtVaCreateManagedWidget("next_t", commandWidgetClass, right,
                                            XtNlabel, "t >>",
                                            XtNfromHoriz, btn_pt, NULL);
    XtAddCallback(btn_nt, XtNcallback, next_t_cb, NULL);

    Widget btn_pl = XtVaCreateManagedWidget("prev_l", commandWidgetClass, right,
                                            XtNlabel, "<< lvl",
                                            XtNfromHoriz, btn_nt, NULL);
    XtAddCallback(btn_pl, XtNcallback, prev_l_cb, NULL);
    Widget btn_nl = XtVaCreateManagedWidget("next_l", commandWidgetClass, right,
                                            XtNlabel, "lvl >>",
                                            XtNfromHoriz, btn_pl, NULL);
    XtAddCallback(btn_nl, XtNcallback, next_l_cb, NULL);

    Widget btn_cp = XtVaCreateManagedWidget("cmap_prev", commandWidgetClass, right,
                                            XtNlabel, "< cmap",
                                            XtNfromHoriz, btn_nl, NULL);
    XtAddCallback(btn_cp, XtNcallback, cmap_prev_cb, NULL);
    Widget btn_cn = XtVaCreateManagedWidget("cmap_next", commandWidgetClass, right,
                                            XtNlabel, "cmap >",
                                            XtNfromHoriz, btn_cp, NULL);
    XtAddCallback(btn_cn, XtNcallback, cmap_next_cb, NULL);

    Widget btn_rv = XtVaCreateManagedWidget("reset", commandWidgetClass, right,
                                            XtNlabel, "reset view",
                                            XtNfromHoriz, btn_cn, NULL);
    XtAddCallback(btn_rv, XtNcallback, reset_view_cb, NULL);

    Widget btn_fit = XtVaCreateManagedWidget("fit", commandWidgetClass, right,
                                             XtNlabel, "fit data",
                                             XtNfromHoriz, btn_rv, NULL);
    XtAddCallback(btn_fit, XtNcallback, fit_data_cb, NULL);

    Widget btn_zi = XtVaCreateManagedWidget("zoomin", commandWidgetClass, right,
                                            XtNlabel, "zoom +",
                                            XtNfromHoriz, btn_fit, NULL);
    XtAddCallback(btn_zi, XtNcallback, zoom_in_cb, NULL);

    Widget btn_zo = XtVaCreateManagedWidget("zoomout", commandWidgetClass, right,
                                            XtNlabel, "zoom -",
                                            XtNfromHoriz, btn_zi, NULL);
    XtAddCallback(btn_zo, XtNcallback, zoom_out_cb, NULL);

    Widget btn_about = XtVaCreateManagedWidget("about", commandWidgetClass, right,
                                               XtNlabel, "about",
                                               XtNfromHoriz, btn_zo, NULL);
    XtAddCallback(btn_about, XtNcallback, open_about_cb, NULL);
    (void)btn_about;

    Widget btn_setr = XtVaCreateManagedWidget("setRange", commandWidgetClass, right,
                                              XtNlabel, "set range...",
                                              XtNfromVert, btn_pt, NULL);
    XtAddCallback(btn_setr, XtNcallback, open_range_dialog_cb, NULL);

    Widget btn_auto = XtVaCreateManagedWidget("auto", commandWidgetClass, right,
                                              XtNlabel, "auto range",
                                              XtNfromVert, btn_pt,
                                              XtNfromHoriz, btn_setr, NULL);
    XtAddCallback(btn_auto, XtNcallback, auto_range_cb, NULL);

    Widget btn_save = XtVaCreateManagedWidget("savepng", commandWidgetClass, right,
                                              XtNlabel, "save png...",
                                              XtNfromVert, btn_pt,
                                              XtNfromHoriz, btn_auto, NULL);
    XtAddCallback(btn_save, XtNcallback, open_save_dialog_cb, NULL);

    Widget last_row2 = btn_save;
    if (has_layer_of_type(&g_app, POLY_TYPE_COAST)) {
        Widget b = XtVaCreateManagedWidget("coast", commandWidgetClass, right,
                                           XtNlabel, "coast",
                                           XtNfromVert, btn_pt,
                                           XtNfromHoriz, last_row2, NULL);
        XtAddCallback(b, XtNcallback, toggle_coast_cb, NULL);
        last_row2 = b;
    }
    if (has_layer_of_type(&g_app, POLY_TYPE_BORDERS)) {
        Widget b = XtVaCreateManagedWidget("borders", commandWidgetClass, right,
                                           XtNlabel, "borders",
                                           XtNfromVert, btn_pt,
                                           XtNfromHoriz, last_row2, NULL);
        XtAddCallback(b, XtNcallback, toggle_borders_cb, NULL);
        last_row2 = b;
    }
    if (has_layer_of_type(&g_app, POLY_TYPE_STATES)) {
        Widget b = XtVaCreateManagedWidget("states", commandWidgetClass, right,
                                           XtNlabel, "states",
                                           XtNfromVert, btn_pt,
                                           XtNfromHoriz, last_row2, NULL);
        XtAddCallback(b, XtNcallback, toggle_states_cb, NULL);
        last_row2 = b;
    }

    g_app.canvas = XtVaCreateManagedWidget("canvas", simpleWidgetClass, right,
                                           XtNwidth, 850, XtNheight, 540,
                                           XtNfromVert, btn_setr, NULL);
    XtAddEventHandler(g_app.canvas,
                      ExposureMask | StructureNotifyMask |
                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                      False, canvas_event_cb, NULL);

    g_app.status = XtVaCreateManagedWidget("status", labelWidgetClass, right,
                                            XtNlabel, "ready",
                                            XtNfromVert, g_app.canvas,
                                            XtNwidth, 850,
                                            XtNjustify, XtJustifyLeft, NULL);

    XtRealizeWidget(top);
    g_app.top = top;
    g_app.dpy = XtDisplay(top);
    g_app.gc = XCreateGC(g_app.dpy, XtWindow(g_app.canvas), 0, NULL);

    XawListHighlight(vlist, g_app.var_idx);
    load_variable(&g_app);

    XtAppMainLoop(app_ctx);
    return 0;
}
