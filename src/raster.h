#ifndef RASTER_H
#define RASTER_H

#include <stdint.h>
#include <stddef.h>
#include "mesh.h"
#include "polylines.h"

typedef struct {
    int width;
    int height;
    uint32_t *pixels;
    uint8_t  *alpha;
} Raster;

typedef struct {
    double lon_min;
    double lon_max;
    double lat_min;
    double lat_max;
} Viewport;

Raster *raster_create(int w, int h);
void raster_free(Raster *r);
void raster_clear(Raster *r, uint32_t color);

void raster_render_mesh(
    Raster *r,
    const UnsMesh *m,
    const double *values,
    double vmin, double vmax,
    double fill_value,
    const Viewport *vp,
    const uint32_t *palette);

void raster_draw_text(Raster *r, int x, int y, const char *s,
                      uint32_t color, int scale);
int  raster_text_width(const char *s, int scale);
int  raster_text_height(int scale);

void raster_fill_rect(Raster *r, int x, int y, int w, int h, uint32_t color);
void raster_draw_rect(Raster *r, int x, int y, int w, int h, uint32_t color);

void raster_draw_colorbar(
    Raster *r,
    const uint32_t *palette,
    double vmin, double vmax,
    const char *label,
    int x, int y, int w, int h,
    uint32_t panel_bg, uint32_t fg);

void raster_draw_line(Raster *r, int x0, int y0, int x1, int y1, uint32_t color);

void raster_render_polylines(
    Raster *r,
    const Polylines *p,
    const Viewport *vp,
    uint32_t color);

#endif
