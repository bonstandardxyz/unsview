#ifndef POLYLINES_H
#define POLYLINES_H

#include <stddef.h>

/* Simple polyline collection. All coordinates are in degrees on disk and
 * converted to radians at load time so render code matches mesh convention. */
typedef struct {
    size_t n_pts;
    double *lon;        /* radians, NaN marks polyline break */
    double *lat;        /* radians, NaN marks polyline break */
} Polylines;

/* Load a whitespace-separated lon/lat polyline file.
 *
 *   # comment
 *   <lon> <lat>            -> point in degrees
 *   <blank line>           -> polyline break
 *   NaN NaN                -> polyline break (alt form)
 *
 * Returns NULL on open failure or if the file has no parseable points.
 */
Polylines *polylines_load(const char *path);
void polylines_free(Polylines *p);

#include <stdint.h>

typedef enum {
    POLY_TYPE_COAST = 0,
    POLY_TYPE_BORDERS,
    POLY_TYPE_STATES,
    POLY_TYPE_GENERIC,
    POLY_TYPE_COUNT
} PolyLayerType;

typedef struct {
    Polylines *poly;
    uint32_t color;       /* default render color for this layer */
    PolyLayerType type;   /* which CLI flag created it */
} PolyLayer;

#endif
