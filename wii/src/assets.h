#ifndef AM2R_WII_ASSETS_H
#define AM2R_WII_ASSETS_H

#include "vm.h"

/* Our replacements for the asset builtins (sprite_add/replace, background_add/replace, font_replace,
 * surface_getpixel[_ext]); NULL for other names. */
BuiltinFunc WiiAssets_builtin(const char *name);

#endif
