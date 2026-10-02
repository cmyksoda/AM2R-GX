#ifndef AM2R_WII_ASSETS_H
#define AM2R_WII_ASSETS_H

#include "vm.h"

/* Our replacements for the asset builtins (sprite_add/replace, background_add/replace, font_replace,
 * surface_getpixel_ext); NULL for other names. */
BuiltinFunc WiiAssets_builtin(const char *name);

/* One page from an RGBA strip and a single-frame sprite per cell (x, width; full height); -1 where it failed. */
void WiiAssets_addSprites(DataWin *dw, const uint8_t *rgba, int w, int h, const int *cellX, const int *cellW, int n,
			  int32_t *sprites);

#endif
