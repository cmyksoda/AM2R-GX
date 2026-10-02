#ifndef AM2R_WII_GX_RENDERER_H
#define AM2R_WII_GX_RENDERER_H

#include "renderer.h"

#define GX_MAX_TEX_DIM 1024

Renderer *GxRenderer_create(void);

/* Texel memory currently held, for the Dolphin sweep's leak hunting. */
void GxRenderer_memStats(uint32_t *surfCount, uint32_t *surfBytes, uint32_t *pageBytes);
uint8_t *GxRenderer_tpagUsage(uint32_t *bytes);
/* display_reset(): every surface is gone a few frames later, as on the real runner. */
void GxRenderer_loseSurfaces(void);

/* Set when a texture page is too big for the GPU: the game quits and the message is shown. */
const char *GxRenderer_fatal(void);
/* Wide mode: the composed 426×480 frame goes to the screen here, just before the display copy. */
void GxRenderer_present(void);
/* A loose RGBA image as a texture page of its own; returns the page id (past the TXTR pages) or -1. */
int32_t GxRenderer_addImage(const uint8_t *rgba, int w, int h);
bool GxRenderer_surfacePixel(int32_t surfaceID, int x, int y, uint8_t rgba[4]);
void GxRenderer_setWideFilter(bool on);
/* The game's Widescreen is off on a 16:9 console: draw 4:3 centred with black sides. */
void GxRenderer_setPillarbox(bool on);
/* Instrumentation: called with the fill of every drawn page item, and with tpagIndex -1 every 200 frames. */
struct DataWin;
typedef void (*GxFillProbe)(struct DataWin *dw, int32_t tpagIndex, double area);
void GxRenderer_setFillProbe(GxFillProbe probe);
/* Answer "no shaders" to the game, to see what it does without them. */
void GxRenderer_setShadersOff(bool off);
/* The pages these texture items sit on, to load in the background in this order and keep longer; none clears it. */
void GxRenderer_wish(struct DataWin *dw, const int32_t *tpags, uint32_t count);
/* Text over a one-pixel outline in all eight directions (AM2R's draw_cool_text), laid out once for all nine draws. */
void GxRenderer_drawOutlinedText(Renderer *renderer, const char *text, double x, double y, int32_t outline, int32_t top,
				 int32_t bottom, float alpha);

#endif
