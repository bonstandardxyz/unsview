#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>

#include "raster.h"

int png_export(const char *path, const Raster *r, uint32_t bg) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return 1;
    }

    png_structp p = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info = png_create_info_struct(p);
    if (setjmp(png_jmpbuf(p))) {
        png_destroy_write_struct(&p, &info);
        fclose(fp);
        return 1;
    }

    png_init_io(p, fp);
    png_set_IHDR(p, info, r->width, r->height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(p, info);

    uint8_t *row = malloc((size_t)3 * (size_t)r->width);
    for (int y = 0; y < r->height; y++) {
        for (int x = 0; x < r->width; x++) {
            size_t k = (size_t)y * (size_t)r->width + (size_t)x;
            uint32_t c = r->alpha[k] ? r->pixels[k] : bg;
            row[3 * x]     = (c >> 16) & 0xff;
            row[3 * x + 1] = (c >> 8) & 0xff;
            row[3 * x + 2] = c & 0xff;
        }
        png_write_row(p, row);
    }
    free(row);

    png_write_end(p, NULL);
    png_destroy_write_struct(&p, &info);
    fclose(fp);
    return 0;
}
