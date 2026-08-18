#include "raster.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_POLY 32

#define FONT_W 5
#define FONT_H 7
#define FONT_ADV 6

/* 5x7 bitmap font. Each glyph is 7 bytes; bit 4 = leftmost pixel. */
static const uint8_t FONT[128][FONT_H] = {
    [' '] = {0,0,0,0,0,0,0},
    ['!'] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    ['('] = {0x04,0x08,0x10,0x10,0x10,0x08,0x04},
    [')'] = {0x04,0x02,0x01,0x01,0x01,0x02,0x04},
    ['+'] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    [','] = {0x00,0x00,0x00,0x00,0x00,0x04,0x08},
    ['-'] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    ['.'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x04},
    ['/'] = {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    ['0'] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    ['1'] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    ['2'] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    ['3'] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    ['4'] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    ['5'] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    ['6'] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    ['7'] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    ['8'] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    ['9'] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    [':'] = {0x00,0x04,0x00,0x00,0x00,0x04,0x00},
    ['='] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    ['_'] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    ['A'] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['B'] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    ['C'] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    ['D'] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    ['E'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    ['F'] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    ['G'] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0E},
    ['H'] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    ['I'] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['J'] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
    ['K'] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    ['L'] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    ['M'] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    ['N'] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
    ['O'] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['P'] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    ['Q'] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    ['R'] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    ['S'] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    ['T'] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    ['U'] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    ['V'] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    ['W'] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    ['X'] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    ['Y'] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    ['Z'] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    ['a'] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    ['b'] = {0x10,0x10,0x16,0x19,0x11,0x11,0x1E},
    ['c'] = {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E},
    ['d'] = {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F},
    ['e'] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    ['f'] = {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    ['g'] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    ['h'] = {0x10,0x10,0x16,0x19,0x11,0x11,0x11},
    ['i'] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    ['j'] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    ['k'] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    ['l'] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    ['m'] = {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
    ['n'] = {0x00,0x00,0x16,0x19,0x11,0x11,0x11},
    ['o'] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    ['p'] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    ['q'] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},
    ['r'] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    ['s'] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    ['t'] = {0x08,0x08,0x1C,0x08,0x08,0x09,0x06},
    ['u'] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    ['v'] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    ['w'] = {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    ['x'] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    ['y'] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    ['z'] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
};

Raster *raster_create(int w, int h) {
    Raster *r = malloc(sizeof(*r));
    r->width = w;
    r->height = h;
    r->pixels = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    r->alpha  = calloc((size_t)w * (size_t)h, 1);
    return r;
}

void raster_free(Raster *r) {
    if (!r) return;
    free(r->pixels);
    free(r->alpha);
    free(r);
}

void raster_clear(Raster *r, uint32_t color) {
    size_t n = (size_t)r->width * (size_t)r->height;
    for (size_t i = 0; i < n; i++) {
        r->pixels[i] = color;
        r->alpha[i] = 0;
    }
}

static void fill_polygon(Raster *r, const double *xs, const double *ys, int n, uint32_t color) {
    if (n < 3) return;

    double ymin = ys[0], ymax = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }
    int y0 = (int)floor(ymin);
    int y1 = (int)ceil(ymax);
    if (y0 < 0) y0 = 0;
    if (y1 > r->height) y1 = r->height;

    double xints[MAX_POLY];
    for (int y = y0; y < y1; y++) {
        double yc = y + 0.5;
        int nint = 0;
        for (int i = 0; i < n; i++) {
            double y1e = ys[i];
            double y2e = ys[(i + 1) % n];
            double x1e = xs[i];
            double x2e = xs[(i + 1) % n];
            if (y1e == y2e) continue;
            int crosses = (yc >= y1e && yc < y2e) || (yc >= y2e && yc < y1e);
            if (!crosses) continue;
            double t = (yc - y1e) / (y2e - y1e);
            if (nint < MAX_POLY) {
                xints[nint++] = x1e + t * (x2e - x1e);
            }
        }
        for (int i = 1; i < nint; i++) {
            double v = xints[i];
            int j = i;
            while (j > 0 && xints[j - 1] > v) {
                xints[j] = xints[j - 1];
                j--;
            }
            xints[j] = v;
        }
        for (int p = 0; p + 1 < nint; p += 2) {
            int xa = (int)ceil(xints[p] - 0.5);
            int xb = (int)floor(xints[p + 1] - 0.5);
            if (xa < 0) xa = 0;
            if (xb >= r->width) xb = r->width - 1;
            for (int x = xa; x <= xb; x++) {
                size_t k = (size_t)y * (size_t)r->width + (size_t)x;
                r->pixels[k] = color;
                r->alpha[k] = 1;
            }
        }
    }
}

static uint32_t lookup_color(double v, double vmin, double vmax, const uint32_t *palette) {
    if (vmax <= vmin) return palette[0];
    double t = (v - vmin) / (vmax - vmin);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int idx = (int)(t * 255.0 + 0.5);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    return palette[idx];
}

static void put_pixel_clipped(Raster *r, int x, int y, uint32_t color) {
    if (x < 0 || x >= r->width || y < 0 || y >= r->height) return;
    size_t k = (size_t)y * (size_t)r->width + (size_t)x;
    r->pixels[k] = color;
    r->alpha[k] = 1;
}

void raster_fill_rect(Raster *r, int x, int y, int w, int h, uint32_t color) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 > r->width)  x1 = r->width;
    if (y1 > r->height) y1 = r->height;
    for (int j = y; j < y1; j++) {
        for (int i = x; i < x1; i++) {
            size_t k = (size_t)j * (size_t)r->width + (size_t)i;
            r->pixels[k] = color;
            r->alpha[k] = 1;
        }
    }
}

void raster_draw_rect(Raster *r, int x, int y, int w, int h, uint32_t color) {
    for (int i = 0; i < w; i++) {
        put_pixel_clipped(r, x + i, y, color);
        put_pixel_clipped(r, x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        put_pixel_clipped(r, x, y + j, color);
        put_pixel_clipped(r, x + w - 1, y + j, color);
    }
}

static void draw_glyph(Raster *r, int x, int y, const uint8_t *g,
                       uint32_t color, int scale) {
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT_W; col++) {
            if (!(bits & (1u << (FONT_W - 1 - col)))) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    put_pixel_clipped(r,
                        x + col * scale + dx,
                        y + row * scale + dy,
                        color);
                }
            }
        }
    }
}

void raster_draw_text(Raster *r, int x, int y, const char *s,
                      uint32_t color, int scale) {
    if (scale < 1) scale = 1;
    int adv = FONT_ADV * scale;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        const uint8_t *g = (c < 128) ? FONT[c] : NULL;
        if (g) draw_glyph(r, x, y, g, color, scale);
        x += adv;
    }
}

int raster_text_width(const char *s, int scale) {
    if (scale < 1) scale = 1;
    int n = 0; for (const char *p = s; *p; p++) n++;
    return n * FONT_ADV * scale;
}

int raster_text_height(int scale) {
    if (scale < 1) scale = 1;
    return FONT_H * scale;
}

void raster_draw_colorbar(Raster *r, const uint32_t *palette,
                          double vmin, double vmax,
                          const char *label,
                          int x, int y, int w, int h,
                          uint32_t panel_bg, uint32_t fg) {
    int scale = 2;
    int th = raster_text_height(scale);
    int pad = 4;

    int panel_w = w + 2 * pad;
    int panel_h = h + th * 2 + pad * 4;
    if (label && *label) panel_h += th + pad;
    int panel_x = x - pad;
    int panel_y = y - pad - (label && *label ? th + pad : 0);

    raster_fill_rect(r, panel_x, panel_y, panel_w, panel_h, panel_bg);
    raster_draw_rect(r, panel_x, panel_y, panel_w, panel_h, fg);

    if (label && *label) {
        raster_draw_text(r, x, panel_y + pad, label, fg, scale);
    }

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int idx = (w <= 1) ? 0 : (int)((double)i / (double)(w - 1) * 255.0 + 0.5);
            if (idx < 0) idx = 0;
            if (idx > 255) idx = 255;
            put_pixel_clipped(r, x + i, y + j, palette[idx]);
        }
    }
    raster_draw_rect(r, x - 1, y - 1, w + 2, h + 2, fg);

    char buf[32];
    int label_y = y + h + pad;

    snprintf(buf, sizeof(buf), "%.3g", vmin);
    raster_draw_text(r, x, label_y, buf, fg, scale);

    snprintf(buf, sizeof(buf), "%.3g", 0.5 * (vmin + vmax));
    int tw = raster_text_width(buf, scale);
    raster_draw_text(r, x + w / 2 - tw / 2, label_y, buf, fg, scale);

    snprintf(buf, sizeof(buf), "%.3g", vmax);
    tw = raster_text_width(buf, scale);
    raster_draw_text(r, x + w - tw, label_y, buf, fg, scale);
}

void raster_draw_line(Raster *r, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        put_pixel_clipped(r, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void raster_render_polylines(Raster *r, const Polylines *p,
                             const Viewport *vp, uint32_t color) {
    if (!p || p->n_pts < 2) return;
    double lon_span = vp->lon_max - vp->lon_min;
    double lat_span = vp->lat_max - vp->lat_min;
    if (lon_span <= 0 || lat_span <= 0) return;
    double sx = (double)r->width  / lon_span;
    double sy = (double)r->height / lat_span;

    int have_prev = 0;
    double prev_lon = 0, prev_lat = 0;

    for (size_t i = 0; i < p->n_pts; i++) {
        double lon = p->lon[i];
        double lat = p->lat[i];

        if (isnan(lon) || isnan(lat)) {
            have_prev = 0;
            continue;
        }

        if (!have_prev) {
            prev_lon = lon;
            prev_lat = lat;
            have_prev = 1;
            continue;
        }

        /* Skip the segment if it crosses the dateline (huge lon jump). */
        double dlon = lon - prev_lon;
        if (fabs(dlon) > M_PI) {
            prev_lon = lon;
            prev_lat = lat;
            continue;
        }

        for (int shift = -1; shift <= 1; shift++) {
            double off = shift * 2.0 * M_PI;
            double a = prev_lon + off, b = lon + off;
            double lo = a < b ? a : b;
            double hi = a < b ? b : a;
            if (hi < vp->lon_min || lo > vp->lon_max) continue;

            int x0 = (int)((prev_lon + off - vp->lon_min) * sx);
            int y0 = (int)((double)r->height - (prev_lat - vp->lat_min) * sy);
            int x1 = (int)((lon + off - vp->lon_min) * sx);
            int y1 = (int)((double)r->height - (lat - vp->lat_min) * sy);
            raster_draw_line(r, x0, y0, x1, y1, color);
        }

        prev_lon = lon;
        prev_lat = lat;
    }
}

void raster_render_mesh(Raster *r, const UnsMesh *m, const double *values,
                        double vmin, double vmax, double fill_value,
                        const Viewport *vp, const uint32_t *palette) {
    double xs[MAX_POLY], ys[MAX_POLY];
    double lon_span = vp->lon_max - vp->lon_min;
    double lat_span = vp->lat_max - vp->lat_min;
    if (lon_span <= 0 || lat_span <= 0) return;
    double sx = (double)r->width / lon_span;
    double sy = (double)r->height / lat_span;
    int has_fill = !isnan(fill_value);

    for (size_t c = 0; c < m->ncells; c++) {
        double v = values[c];
        if (isnan(v)) continue;
        if (has_fill && v == fill_value) continue;

        int n = m->n_edges_on_cell[c];
        if (n < 3) continue;
        if (n > MAX_POLY) n = MAX_POLY;

        double clon = m->lon_cell[c];
        double vlon[MAX_POLY], vlat[MAX_POLY];
        int ok = 1;
        for (int e = 0; e < n; e++) {
            int vi = m->vertices_on_cell[c * (size_t)m->max_edges + (size_t)e];
            if (vi < 0 || (size_t)vi >= m->nvertices) { ok = 0; break; }
            double lon = m->lon_vertex[vi];
            double lat = m->lat_vertex[vi];
            while (lon - clon > M_PI)  lon -= 2.0 * M_PI;
            while (lon - clon < -M_PI) lon += 2.0 * M_PI;
            vlon[e] = lon;
            vlat[e] = lat;
        }
        if (!ok) continue;

        uint32_t color = lookup_color(v, vmin, vmax, palette);

        for (int shift = -1; shift <= 1; shift++) {
            double off = shift * 2.0 * M_PI;
            double pmin = vlon[0] + off, pmax = pmin;
            for (int e = 1; e < n; e++) {
                double L = vlon[e] + off;
                if (L < pmin) pmin = L;
                if (L > pmax) pmax = L;
            }
            if (pmax < vp->lon_min || pmin > vp->lon_max) continue;

            for (int e = 0; e < n; e++) {
                xs[e] = (vlon[e] + off - vp->lon_min) * sx;
                ys[e] = (double)r->height - (vlat[e] - vp->lat_min) * sy;
            }
            fill_polygon(r, xs, ys, n, color);
        }
    }
}
