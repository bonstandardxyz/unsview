#ifndef COLORMAP_H
#define COLORMAP_H

#include <stdint.h>

void colormap_init(void);

/* Look up a palette by name (case-insensitive on the standard names).
 * Returns the viridis palette if name is unknown. */
const uint32_t *colormap_by_name(const char *name);

/* Iterate the registered palette names. Returns NULL past the end.
 * Useful for the GUI cmap-cycle button. */
const char *colormap_name_at(int index);
int colormap_count(void);

#endif
