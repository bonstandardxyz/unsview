#include "colormap.h"

#include <math.h>
#include <strings.h>
#include <string.h>

/* Each palette is 256 uint32 entries packed as 0x00RRGGBB.
 * Sequential and diverging palettes are interpolated from a small set of
 * matplotlib/CET control points. */

typedef struct {
    const char *name;
    uint32_t lut[256];
} Palette;

static uint32_t pack(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* `cps` is a flat array of {t, r, g, b} quads (t in 0..255). */
static void interp_lut(uint32_t *out, const int *cps, int ncp) {
    for (int i = 0; i < 256; i++) {
        int seg = 0;
        while (seg < ncp - 2 && cps[(seg + 1) * 4] < i) seg++;
        int t0 = cps[seg * 4];
        int t1 = cps[(seg + 1) * 4];
        double t = (t1 == t0) ? 0.0 : (double)(i - t0) / (double)(t1 - t0);
        int r = cps[seg * 4 + 1] + (int)(t * (cps[(seg + 1) * 4 + 1] - cps[seg * 4 + 1]));
        int g = cps[seg * 4 + 2] + (int)(t * (cps[(seg + 1) * 4 + 2] - cps[seg * 4 + 2]));
        int b = cps[seg * 4 + 3] + (int)(t * (cps[(seg + 1) * 4 + 3] - cps[seg * 4 + 3]));
        out[i] = pack(r, g, b);
    }
}

#define MAX_PALETTES 24
static Palette g_palettes[MAX_PALETTES];
static int     g_npalettes = 0;

static void add_lut(const char *name, const int *cps, int ncp) {
    if (g_npalettes >= MAX_PALETTES) return;
    g_palettes[g_npalettes].name = name;
    interp_lut(g_palettes[g_npalettes].lut, cps, ncp);
    g_npalettes++;
}

static void add_3gauss(void) {
    if (g_npalettes >= MAX_PALETTES) return;
    g_palettes[g_npalettes].name = "3gauss";
    for (int i = 0; i < 256; i++) {
        double x = i / 255.0;
        double sigma = 0.23;
        double r = exp(-0.5 * ((x - 0.75) / sigma) * ((x - 0.75) / sigma));
        double g = exp(-0.5 * ((x - 0.50) / sigma) * ((x - 0.50) / sigma));
        double b = exp(-0.5 * ((x - 0.25) / sigma) * ((x - 0.25) / sigma));
        g_palettes[g_npalettes].lut[i] = pack(
            (int)(r * 255 + 0.5),
            (int)(g * 255 + 0.5),
            (int)(b * 255 + 0.5));
    }
    g_npalettes++;
}

static void add_gray(void) {
    if (g_npalettes >= MAX_PALETTES) return;
    g_palettes[g_npalettes].name = "gray";
    for (int i = 0; i < 256; i++) g_palettes[g_npalettes].lut[i] = pack(i, i, i);
    g_npalettes++;
}

void colormap_init(void) {
    g_npalettes = 0;

    /* perceptually-uniform sequential (matplotlib defaults) */
    int viridis[] = {
          0,  68,   1,  84,   64,  59,  81, 139,  128,  33, 144, 140,
        191,  94, 201,  97,  255, 253, 231,  36,
    };
    add_lut("viridis", viridis, 5);

    int plasma[] = {
          0,  13,   8, 135,   64, 126,   3, 168,  128, 204,  71, 120,
        191, 248, 149,  64,  255, 240, 249,  33,
    };
    add_lut("plasma", plasma, 5);

    int magma[] = {
          0,   0,   0,   3,   64,  80,  18, 123,  128, 183,  55, 121,
        191, 251, 135,  97,  255, 252, 253, 191,
    };
    add_lut("magma", magma, 5);

    int inferno[] = {
          0,   0,   0,   3,   64,  87,  15, 109,  128, 187,  55,  84,
        191, 249, 142,   8,  255, 252, 254, 164,
    };
    add_lut("inferno", inferno, 5);

    int cividis[] = {
          0,   0,  32,  76,   64,  44,  64, 117,  128, 121, 122, 124,
        191, 184, 174, 121,  255, 253, 231, 137,
    };
    add_lut("cividis", cividis, 5);

    /* google "turbo" — high-contrast rainbow without jet's banding */
    int turbo[] = {
          0,  48,  18,  59,   32,  58, 120, 246,   64,  39, 199, 224,
         96,  65, 247, 158,  128, 168, 251,  77,  160, 245, 211,  46,
        192, 247, 144,  43,  224, 215,  60,  41,  255, 122,  17,  19,
    };
    add_lut("turbo", turbo, 9);

    /* classic ncview-style rainbow */
    int jet[] = {
          0,   0,   0, 127,   64,   0, 127, 255,  128, 127, 255, 127,
        191, 255, 127,   0,  255, 127,   0,   0,
    };
    add_lut("jet", jet, 5);

    /* ColorBrewer diverging — RdBu */
    int rdbu[] = {
          0,   5,  48,  97,  128, 247, 247, 247,  255, 103,   0,  31,
    };
    add_lut("rdbu", rdbu, 3);

    /* ColorBrewer diverging — BrBG (brown ↔ teal) */
    int brbg[] = {
          0,  84,  48,   5,  128, 245, 245, 245,  255,   0,  60,  48,
    };
    add_lut("brbg", brbg, 3);

    /* matplotlib seismic (red/white/blue, anomaly maps) */
    int seismic[] = {
          0,   0,   0,  76,   64,   0,   0, 255,  128, 255, 255, 255,
        191, 255,   0,   0,  255,  77,   0,   0,
    };
    add_lut("seismic", seismic, 5);

    /* matplotlib hot */
    int hot[] = {
          0,  10,   0,   0,   85, 255,   0,   0,  170, 255, 255,   0,
        255, 255, 255, 255,
    };
    add_lut("hot", hot, 4);

    /* matplotlib cool */
    int cool[] = {
          0,   0, 255, 255,  255, 255,   0, 255,
    };
    add_lut("cool", cool, 2);

    /* matplotlib terrain (topographic) */
    int terrain[] = {
          0,  51,  51, 153,   38,   0, 153, 204,   77,   0, 204, 102,
        128, 230, 230, 153,  178, 102,  77,  51,  255, 255, 255, 255,
    };
    add_lut("terrain", terrain, 6);

    add_gray();

    /* wbr: white -> sky-blue -> deep-blue -> violet -> red */
    int wbr[] = {
          0, 255, 255, 255,
         64, 100, 185, 255,
        128,   0,  30, 200,
        192, 180,   0, 160,
        255, 200,   0,   0,
    };
    add_lut("wbr", wbr, 5);

    /* rainbow: full-spectrum ROYGBIV (ncview-style, cleaner than jet) */
    int rainbow[] = {
          0, 120,   0, 200,
         40,   0,   0, 255,
         85,   0, 200, 230,
        128,   0, 180,   0,
        170, 220, 220,   0,
        212, 255, 100,   0,
        255, 220,   0,   0,
    };
    add_lut("rainbow", rainbow, 7);

    /* bwr: blue-white-red diverging (ncview bwr) */
    int bwr[] = {
          0,   0,   0, 180,
         64,  30, 120, 255,
        128, 255, 255, 255,
        191, 255, 110,  30,
        255, 180,   0,   0,
    };
    add_lut("bwr", bwr, 5);

    add_3gauss();
}

const uint32_t *colormap_by_name(const char *name) {
    if (!name) return g_palettes[0].lut;
    for (int i = 0; i < g_npalettes; i++) {
        if (!strcasecmp(name, g_palettes[i].name)) return g_palettes[i].lut;
    }
    /* legacy aliases */
    if (!strcasecmp(name, "RdBu")) return colormap_by_name("rdbu");
    if (!strcasecmp(name, "grey")) return colormap_by_name("gray");
    if (!strcasecmp(name, "BrBG")) return colormap_by_name("brbg");
    return g_palettes[0].lut;
}

const char *colormap_name_at(int index) {
    if (index < 0 || index >= g_npalettes) return NULL;
    return g_palettes[index].name;
}

int colormap_count(void) { return g_npalettes; }
