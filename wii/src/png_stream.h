#ifndef AM2R_WII_PNG_STREAM_H
#define AM2R_WII_PNG_STREAM_H

#include <gctypes.h>
#include <stdbool.h>
#include <stddef.h>

typedef void (*WiiPngRowFn)(void *user, int y, const u8 *rgba, int width);

/* Only the PNG flavour GameMaker writes for texture pages: 8-bit RGBA, non-interlaced.
 * Anything else reports false so the caller can fall back to stb_image. */
bool WiiPng_size(const u8 *png, size_t len, int *width, int *height);

/* Inflates and un-filters one scanline at a time: tens of KB of working memory instead of
 * stb_image's two full-size copies (~8 MB for a 1024x1024 page). */
bool WiiPng_decodeRows(const u8 *png, size_t len, WiiPngRowFn emit, void *user);

#endif
