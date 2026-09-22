#include "assets.h"

#include "gx_renderer.h"

#include "data_win.h"
#include "file_system.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_image.h"
#include "utils.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

/* sprite_add / sprite_replace / background_replace: the engine stubs or lacks them, and 1.5.5
 * loads its loose lang/ and mods/ PNGs with them (area-intro headers, palettes, trails, title art).
 * A PNG becomes a page of its own; frames sit side by side in it, as GameMaker lays them out. */

static int32_t tpag_append(DataWin *dw)
{
	int32_t i = (int32_t)dw->tpag.count;

	dw->tpag.items = (TexturePageItem *)safeRealloc(dw->tpag.items, (dw->tpag.count + 1) * sizeof(TexturePageItem));
	memset(&dw->tpag.items[i], 0, sizeof(TexturePageItem));
	dw->tpag.items[i].texturePageId = -1;
	dw->tpag.count++;
	return i;
}

/* Loads the PNG, cuts it into `frames` TPAG items, returns them (caller frees) or NULL. */
static int32_t *load_frames(VMContext *ctx, const char *fname, int frames, bool removeback, int *fw, int *fh)
{
	DataWin *dw = ctx->dataWin;
	FileSystem *fs = ctx->runner->fileSystem;
	uint8_t *data, *rgba;
	int32_t size, page, *tpags;
	int w, h, n, i;

	if (frames < 1)
		frames = 1;
	if (!fs->vtable->readFileBinary(fs, fname, &data, &size))
		return NULL;
	rgba = stbi_load_from_memory(data, size, &w, &h, &n, 4);
	free(data);
	if (!rgba) {
		logWarn("Wii: %s is not a PNG we can read\n", fname);
		return NULL;
	}
	if (removeback) { /* GameMaker: the bottom-left pixel's colour is the background */
		const uint8_t *bg = rgba + ((size_t)(h - 1) * w) * 4;

		for (i = 0; i < w * h; i++)
			if (memcmp(rgba + (size_t)i * 4, bg, 3) == 0)
				rgba[(size_t)i * 4 + 3] = 0;
	}
	page = GxRenderer_addImage(rgba, w, h);
	stbi_image_free(rgba);
	if (page < 0) {
		logWarn("Wii: no room for %s (%dx%d)\n", fname, w, h);
		return NULL;
	}
	*fw = w / frames;
	*fh = h;
	logInfo("Wii: image %s %dx%d x%d -> page %d\n", fname, w, h, frames, (int)page);
	tpags = (int32_t *)safeMalloc((size_t)frames * sizeof(int32_t));
	for (i = 0; i < frames; i++) {
		TexturePageItem *item;

		tpags[i] = tpag_append(dw);
		item = &dw->tpag.items[tpags[i]];
		item->present = true;
		item->sourceX = (uint16_t)(i * *fw);
		item->sourceWidth = item->targetWidth = item->boundingWidth = (uint16_t)*fw;
		item->sourceHeight = item->targetHeight = item->boundingHeight = (uint16_t)h;
		item->texturePageId = (int16_t)page;
	}
	return tpags;
}

static void sprite_fill(Sprite *spr, int32_t *tpags, int frames, int w, int h, int xorig, int yorig)
{
	spr->width = (uint32_t)w;
	spr->height = (uint32_t)h;
	spr->originX = xorig;
	spr->originY = yorig;
	spr->textureCount = (uint32_t)frames;
	spr->tpagIndices = tpags; /* a replaced sprite's old frame list is a few bytes; left to the game's end */
}

static const char *arg_str(RValue *args, int32_t argCount, int i)
{
	return (i < argCount && args[i].type == RVALUE_STRING) ? args[i].string : "";
}

static int arg_int(RValue *args, int32_t argCount, int i)
{
	return i < argCount ? (int)RValue_toReal(args[i]) : 0;
}

/* sprite_add(fname, imgnumb, removeback, smooth, xorig, yorig) */
static RValue wii_sprite_add(VMContext *ctx, RValue *args, int32_t argCount)
{
	DataWin *dw = ctx->dataWin;
	int frames = arg_int(args, argCount, 1), w, h;
	int32_t *tpags = load_frames(ctx, arg_str(args, argCount, 0), frames, arg_int(args, argCount, 2) != 0, &w, &h);
	uint32_t slot;

	if (!tpags)
		return RValue_makeReal(-1.0);
	slot = DataWin_allocSpriteSlot(dw, dw->sprt.parsedCount);
	sprite_fill(&dw->sprt.sprites[slot], tpags, frames < 1 ? 1 : frames, w, h, arg_int(args, argCount, 4),
		    arg_int(args, argCount, 5));
	return RValue_makeReal((GMLReal)slot);
}

/* sprite_replace(ind, fname, imgnumb, removeback, smooth, xorig, yorig) */
static RValue wii_sprite_replace(VMContext *ctx, RValue *args, int32_t argCount)
{
	DataWin *dw = ctx->dataWin;
	int ind = arg_int(args, argCount, 0), frames = arg_int(args, argCount, 2), w, h;
	int32_t *tpags;

	if (ind < 0 || (uint32_t)ind >= dw->sprt.count)
		return RValue_makeReal(0.0);
	tpags = load_frames(ctx, arg_str(args, argCount, 1), frames, arg_int(args, argCount, 3) != 0, &w, &h);
	if (!tpags)
		return RValue_makeReal(0.0);
	sprite_fill(&dw->sprt.sprites[ind], tpags, frames < 1 ? 1 : frames, w, h, arg_int(args, argCount, 5),
		    arg_int(args, argCount, 6));
	return RValue_makeReal(1.0);
}

/* background_replace(ind, fname, removeback, smooth) */
static RValue wii_background_replace(VMContext *ctx, RValue *args, int32_t argCount)
{
	DataWin *dw = ctx->dataWin;
	int ind = arg_int(args, argCount, 0), w, h;
	int32_t *tpags;

	if (ind < 0 || (uint32_t)ind >= dw->bgnd.count)
		return RValue_makeReal(0.0);
	tpags = load_frames(ctx, arg_str(args, argCount, 1), 1, arg_int(args, argCount, 2) != 0, &w, &h);
	if (!tpags)
		return RValue_makeReal(0.0);
	dw->bgnd.backgrounds[ind].tpagIndex = tpags[0];
	free(tpags);
	return RValue_makeReal(1.0);
}

/* background_add(fname, removeback, smooth) */
static RValue wii_background_add(VMContext *ctx, RValue *args, int32_t argCount)
{
	DataWin *dw = ctx->dataWin;
	int w, h;
	int32_t *tpags = load_frames(ctx, arg_str(args, argCount, 0), 1, arg_int(args, argCount, 1) != 0, &w, &h);
	uint32_t i;

	if (!tpags)
		return RValue_makeReal(-1.0);
	i = dw->bgnd.count;
	dw->bgnd.backgrounds = (Background *)safeRealloc(dw->bgnd.backgrounds, (i + 1) * sizeof(Background));
	memset(&dw->bgnd.backgrounds[i], 0, sizeof(Background));
	dw->bgnd.backgrounds[i].present = true;
	dw->bgnd.backgrounds[i].name = "__newbackground";
	dw->bgnd.backgrounds[i].tpagIndex = tpags[0];
	dw->bgnd.count++;
	free(tpags);
	return RValue_makeReal((GMLReal)i);
}

/* surface_getpixel(surf, x, y) / surface_getpixel_ext: GameMaker's colour packing, alpha on top. */
static RValue wii_surface_getpixel(MAYBE_UNUSED VMContext *ctx, RValue *args, int32_t argCount)
{
	uint8_t px[4];

	if (argCount < 3 || !GxRenderer_surfacePixel(arg_int(args, argCount, 0), arg_int(args, argCount, 1),
						      arg_int(args, argCount, 2), px))
		return RValue_makeReal(0.0);
	return RValue_makeReal((GMLReal)(((uint32_t)px[3] << 24) | ((uint32_t)px[2] << 16) | ((uint32_t)px[1] << 8) | px[0]));
}

BuiltinFunc WiiAssets_builtin(const char *name)
{
	if (strcmp(name, "surface_getpixel") == 0 || strcmp(name, "surface_getpixel_ext") == 0)
		return wii_surface_getpixel;
	if (strcmp(name, "sprite_add") == 0)
		return wii_sprite_add;
	if (strcmp(name, "sprite_replace") == 0)
		return wii_sprite_replace;
	if (strcmp(name, "background_replace") == 0)
		return wii_background_replace;
	if (strcmp(name, "background_add") == 0)
		return wii_background_add;
	return NULL;
}
