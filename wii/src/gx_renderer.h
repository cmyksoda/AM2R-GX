#ifndef AM2R_WII_GX_RENDERER_H
#define AM2R_WII_GX_RENDERER_H

#include "renderer.h"

Renderer *GxRenderer_create(void);

/* Texel memory currently held, for the Dolphin sweep's leak hunting. */
void GxRenderer_memStats(uint32_t *surfCount, uint32_t *surfBytes, uint32_t *pageBytes);

#endif
