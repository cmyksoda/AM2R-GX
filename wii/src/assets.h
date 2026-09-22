#ifndef AM2R_WII_ASSETS_H
#define AM2R_WII_ASSETS_H

#include "vm.h"

/* Our sprite_add / sprite_replace / background_add / background_replace, or NULL for other names. */
BuiltinFunc WiiAssets_builtin(const char *name);

#endif
