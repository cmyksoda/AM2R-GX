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

#include <ft2build.h>
#include FT_FREETYPE_H

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

/* Caller frees the returned TPAG index list. */
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

/* font_replace(ind, fname, size, bold, italic, first, last): FreeType rasterises the TTF into an
 * atlas laid out like GameMaker's embedded fonts, so the text path and string_width need nothing new.
 * Codepoints past `last` that the language inis use are added when the face has them (Cyrillic, Czech). */
#define FONT_MAX_GLYPHS 2048
#define FONT_ATLAS_MAX 1024
#define PX26(v) ((int)(((v) + 32) >> 6))

typedef struct {
	uint16_t cp;
	int16_t x, y, w, h, shift, offset, top; /* top: rows of the cell above the bitmap */
} FontCell;

static FT_Library ftLib;
static uint8_t langBits[65536 / 8]; /* codepoints the language ini files use */
static bool langScanned;

static void lang_mark(const uint8_t *t, int32_t n)
{
	int32_t i = 0;
	uint32_t cp;

	while (i < n) {
		uint8_t c = t[i++];

		if (c < 0x80)
			cp = c;
		else if ((c & 0xE0) == 0xC0 && i < n)
			cp = ((c & 0x1F) << 6) | (t[i++] & 0x3F);
		else if ((c & 0xF0) == 0xE0 && i + 1 < n) {
			cp = ((c & 0x0F) << 12) | ((t[i] & 0x3F) << 6) | (t[i + 1] & 0x3F);
			i += 2;
		} else
			continue;
		if (cp >= 32 && cp < 0xFFFE)
			langBits[cp >> 3] |= (uint8_t)(1u << (cp & 7));
	}
}

static void lang_scan(FileSystem *fs)
{
	uint8_t *list, *text;
	int32_t n, size, a, b;
	char path[96];

	langScanned = true;
	if (!fs->vtable->readFileBinary(fs, "lang/languages.txt", &list, &n))
		return;
	for (a = 0; a < n; a = b + 1) {
		for (b = a; b < n && list[b] != '\n' && list[b] != '\r'; b++)
			;
		if (b - a <= 0 || b - a > 60)
			continue;
		snprintf(path, sizeof path, "lang/%.*s", (int)(b - a), (const char *)list + a);
		if (fs->vtable->readFileBinary(fs, path, &text, &size)) {
			lang_mark(text, size);
			free(text);
		}
	}
	free(list);
}

static bool lang_uses(uint32_t cp)
{
	return (langBits[cp >> 3] >> (cp & 7)) & 1;
}

static int font_layout(FontCell *cells, int n, int width)
{
	int i, x = 1, y = 1, rowH = 0;

	for (i = 0; i < n; i++) {
		if (x + cells[i].w + 1 > width) {
			x = 1;
			y += rowH + 1;
			rowH = 0;
		}
		cells[i].x = (int16_t)x;
		cells[i].y = (int16_t)y;
		x += cells[i].w + 1;
		if (cells[i].h > rowH)
			rowH = cells[i].h;
	}
	y += rowH + 1;
	return y <= FONT_ATLAS_MAX ? y : 0;
}

static void font_blit(uint8_t *rgba, int atlasW, const FontCell *c, const FT_Bitmap *bm)
{
	int row, col, skip = c->top < 0 ? -c->top : 0;

	for (row = skip; row < (int)bm->rows && c->top + row < c->h; row++) {
		const uint8_t *src = bm->buffer + (size_t)row * (size_t)bm->pitch;
		uint8_t *dst = rgba + (((size_t)(c->y + c->top + row) * atlasW) + c->x) * 4;

		for (col = 0; col < c->w; col++)
			if (src[col >> 3] & (0x80 >> (col & 7)))
				dst[col * 4] = dst[col * 4 + 1] = dst[col * 4 + 2] = dst[col * 4 + 3] = 255;
	}
}

static RValue wii_font_replace(VMContext *ctx, RValue *args, int32_t argCount)
{
	DataWin *dw = ctx->dataWin;
	FileSystem *fs = ctx->runner->fileSystem;
	int ind = arg_int(args, argCount, 0), px = arg_int(args, argCount, 2);
	int first = arg_int(args, argCount, 5), last = arg_int(args, argCount, 6);
	const char *fname = arg_str(args, argCount, 1);
	FT_Face face;
	FontCell *cells;
	FontGlyph *glyphs;
	Font *font;
	uint8_t *data, *rgba;
	int32_t size, page, item;
	int asc, desc, cellH, n = 0, extra = 0, atlasW = 256, atlasH, i, maxH = 0;
	uint32_t cp;

	if (ind < 0 || (uint32_t)ind >= dw->font.count || px < 1 || px > 64 || first < 0 || last > 0xFFFF)
		return RValue_makeReal(0.0);
	if (!ftLib && FT_Init_FreeType(&ftLib)) {
		logWarn("Wii: FreeType init failed\n");
		return RValue_makeReal(0.0);
	}
	if (!fs->vtable->readFileBinary(fs, fname, &data, &size))
		return RValue_makeReal(0.0);
	if (FT_New_Memory_Face(ftLib, data, size, 0, &face) || FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px)) {
		logWarn("Wii: %s is not a font FreeType can read\n", fname);
		free(data);
		return RValue_makeReal(0.0);
	}
	if (!langScanned)
		lang_scan(fs);
	/* GameMaker rounds the ascender and descender separately; the sum is the line height. */
	asc = PX26(face->size->metrics.ascender);
	desc = PX26(-face->size->metrics.descender);
	cellH = asc + desc;
	cells = (FontCell *)safeMalloc(FONT_MAX_GLYPHS * sizeof(FontCell));
	for (cp = (uint32_t)first; cp < 0xFFFE && n < FONT_MAX_GLYPHS; cp++) {
		FT_GlyphSlot g;
		FontCell *c = &cells[n];

		if (cp > (uint32_t)last && !(lang_uses(cp) && FT_Get_Char_Index(face, cp)))
			continue;
		if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO))
			continue;
		g = face->glyph;
		c->cp = (uint16_t)cp;
		c->shift = (int16_t)PX26(g->advance.x);
		c->offset = (int16_t)g->bitmap_left;
		c->top = (int16_t)(asc - g->bitmap_top);
		c->w = (int16_t)g->bitmap.width;
		c->h = (int16_t)(c->top + (int)g->bitmap.rows);
		if (!g->bitmap.width || !g->bitmap.rows) {
			c->w = c->shift;
			c->h = (int16_t)cellH;
			c->offset = 0;
		}
		if (c->h < 0)
			c->h = 0;
		if (cp > (uint32_t)last)
			extra++;
		n++;
	}
	atlasH = font_layout(cells, n, atlasW);
	if (!atlasH)
		atlasH = font_layout(cells, n, atlasW = 512);
	if (!atlasH)
		atlasH = font_layout(cells, n, atlasW = FONT_ATLAS_MAX);
	if (!atlasH) {
		logWarn("Wii: %s at %d px does not fit a font page\n", fname, px);
		goto fail;
	}
	rgba = (uint8_t *)calloc((size_t)atlasW * atlasH, 4);
	if (!rgba)
		goto fail;
	for (i = 0; i < n; i++)
		if (cells[i].w && !FT_Load_Char(face, cells[i].cp, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) &&
		    face->glyph->bitmap.width)
			font_blit(rgba, atlasW, &cells[i], &face->glyph->bitmap);
	page = GxRenderer_addImage(rgba, atlasW, atlasH);
	free(rgba);
	if (page < 0) {
		logWarn("Wii: no room for the %s font page\n", fname);
		goto fail;
	}
	item = tpag_append(dw);
	dw->tpag.items[item].present = true;
	dw->tpag.items[item].sourceWidth = dw->tpag.items[item].targetWidth = dw->tpag.items[item].boundingWidth = (uint16_t)atlasW;
	dw->tpag.items[item].sourceHeight = dw->tpag.items[item].targetHeight = dw->tpag.items[item].boundingHeight = (uint16_t)atlasH;
	dw->tpag.items[item].texturePageId = (int16_t)page;
	glyphs = (FontGlyph *)safeMalloc((size_t)n * sizeof(FontGlyph));
	memset(glyphs, 0, (size_t)n * sizeof(FontGlyph));
	for (i = 0; i < n; i++) {
		glyphs[i].character = cells[i].cp;
		glyphs[i].sourceX = (uint16_t)cells[i].x;
		glyphs[i].sourceY = (uint16_t)cells[i].y;
		glyphs[i].sourceWidth = (uint16_t)cells[i].w;
		glyphs[i].sourceHeight = (uint16_t)cells[i].h;
		glyphs[i].shift = cells[i].shift;
		glyphs[i].offset = cells[i].offset;
		if (cells[i].h > maxH)
			maxH = cells[i].h;
	}
	font = &dw->font.fonts[ind];
	font->emSize = (float)px;
	font->bold = arg_int(args, argCount, 3) != 0;
	font->italic = arg_int(args, argCount, 4) != 0;
	font->rangeStart = (uint16_t)first;
	font->rangeEnd = (uint32_t)last;
	font->antiAliasing = 0;
	font->tpagIndex = item;
	font->scaleX = font->scaleY = 1.0f;
	font->ascenderOffset = 0;
	font->glyphCount = (uint32_t)n;
	font->glyphs = glyphs; /* the embedded glyph table is a few KB; left to the game's end */
	font->maxGlyphHeight = (uint32_t)maxH;
	font->isSpriteFont = false;
	font->spriteIndex = -1;
	Font_buildGlyphLUT(font);
	logInfo("Wii: font %d <- %s %dpx: %d glyphs (%d beyond %d) on page %d %dx%d, line %d (asc %d)\n",
	        ind, fname, px, n, extra, last, (int)page, atlasW, atlasH, cellH, asc);
	free(cells);
	FT_Done_Face(face);
	free(data);
	return RValue_makeReal(1.0);
fail:
	free(cells);
	FT_Done_Face(face);
	free(data);
	return RValue_makeReal(0.0);
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
	if (strcmp(name, "font_replace") == 0)
		return wii_font_replace;
	return NULL;
}
