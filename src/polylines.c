#include "polylines.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

Polylines *polylines_load(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "polylines: cannot open '%s'\n", path);
        return NULL;
    }

    size_t cap = 1024;
    size_t n = 0;
    double *lon = malloc(sizeof(double) * cap);
    double *lat = malloc(sizeof(double) * cap);

    char line[1024];
    int prev_break = 1;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '\n' || *p == '#') {
            if (!prev_break && n > 0) {
                if (n + 1 > cap) {
                    cap *= 2;
                    lon = realloc(lon, sizeof(double) * cap);
                    lat = realloc(lat, sizeof(double) * cap);
                }
                lon[n] = NAN;
                lat[n] = NAN;
                n++;
                prev_break = 1;
            }
            continue;
        }

        double a, b;
        if (sscanf(p, "%lf %lf", &a, &b) != 2) continue;
        if (isnan(a) || isnan(b)) {
            if (!prev_break && n > 0) {
                if (n + 1 > cap) {
                    cap *= 2;
                    lon = realloc(lon, sizeof(double) * cap);
                    lat = realloc(lat, sizeof(double) * cap);
                }
                lon[n] = NAN;
                lat[n] = NAN;
                n++;
                prev_break = 1;
            }
            continue;
        }

        if (n + 1 > cap) {
            cap *= 2;
            lon = realloc(lon, sizeof(double) * cap);
            lat = realloc(lat, sizeof(double) * cap);
        }
        lon[n] = a * M_PI / 180.0;
        lat[n] = b * M_PI / 180.0;
        n++;
        prev_break = 0;
    }
    fclose(fp);

    if (n == 0) {
        free(lon);
        free(lat);
        fprintf(stderr, "polylines: '%s' had no parseable points\n", path);
        return NULL;
    }

    Polylines *pl = malloc(sizeof(*pl));
    pl->n_pts = n;
    pl->lon = lon;
    pl->lat = lat;
    return pl;
}

void polylines_free(Polylines *p) {
    if (!p) return;
    free(p->lon);
    free(p->lat);
    free(p);
}
