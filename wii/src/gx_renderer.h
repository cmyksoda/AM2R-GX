#ifndef AM2R_WII_GX_RENDERER_H
#define AM2R_WII_GX_RENDERER_H

#include "renderer.h"

Renderer *GxRenderer_create(void);

/* Texel memory currently held, for the Dolphin sweep's leak hunting. */
void GxRenderer_memStats(uint32_t *surfCount, uint32_t *surfBytes, uint32_t *pageBytes);
uint8_t *GxRenderer_tpagUsage(uint32_t *bytes);
/* display_reset(): every surface is gone a few frames later, as on the real runner. */
void GxRenderer_loseSurfaces(void);
/* Wide mode: the composed 426×480 frame goes to the screen here, just before the display copy. */
void GxRenderer_present(void);
/* A loose RGBA image as a texture page of its own; returns the page id (past the TXTR pages) or -1. */
int32_t GxRenderer_addImage(const uint8_t *rgba, int w, int h);
bool GxRenderer_surfacePixel(int32_t surfaceID, int x, int y, uint8_t rgba[4]);
void GxRenderer_setWideFilter(bool on);
/* The game's Widescreen is off on a 16:9 console: draw 4:3 centred with black sides. */
void GxRenderer_setPillarbox(bool on);

#endif
