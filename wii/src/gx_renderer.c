#include "gx_renderer.h"
#include "overscan.h"
#include "png_stream.h"
#include "sweep.h"
#include "warp.h"
#include "wii_mem.h"
#include "wii_video.h"

#include "common.h"
#include "data_win.h"
#include "image_decoder.h"
#include "log.h"
#include "matrix_math.h"
#include "renderer.h"
#include "runner.h"
#include "text_utils.h"
#include "utils.h"

#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#define PAGE_LRU_MAX 20
/* Headroom kept free for room loads, and what one page load needs: its PNG blob plus 2 MB of
 * texels (the decode itself streams, so there is no full-size temporary any more). */
#define MEM_RESERVE (4u << 20)
#define PAGE_LOAD_NEED (4u << 20)
#define PAGE_RETRY_FRAMES 300
#define HOST_TARGET (-1)
#define EFB_MAX_W 640
#define EFB_MAX_H 528
/* EFB is 640×480 (not 528). 512×512 CopyTex is taller than the EFB, so s_map is 4×4 of 512×256. */
#define SURF_TILE_W 512
#define SURF_TILE_H 256
/* RGB565 has no alpha: draw_clear_alpha(0,0) on gui_surface covered the room. */
#define SURF_TEX_FMT GX_TF_RGBA8
#define SURF_BPP 4
/* 1 = pillarbox 4:3 in a 16:9 XFB (480×480 centered). 0 = stretch (now). */
#ifndef WII_WIDE_PILLARBOX
#define WII_WIDE_PILLARBOX 0
#endif

typedef struct {
	float x, y, u, v;
	u8 r, g, b, a;
} GxVert;

typedef struct {
	bool loaded;
	bool failed;
	u64 retryAt; /* a failed load is retried after a cooldown, not every frame */
	int32_t w, h;
	int32_t texW, texH;
	u16 *texels;
	GXTexObj obj;
	u64 lastUsed;
} GxPage;

typedef struct {
	int32_t pixelW, pixelH;
	int32_t texW, texH;
	u16 *texels;
	GXTexObj obj;
	bool objReady;
} GxTile;

typedef struct {
	bool exists;
	int32_t w, h;
	int32_t texW, texH;
	u16 *texels;
	GXTexObj obj;
	bool objReady;
	int32_t tilesX, tilesY;
	GxTile *tiles;
} GxSurf;

typedef struct {
	Renderer base;
	int32_t gameW, gameH;
	int32_t windowW, windowH;

	GxPage *pages;
	uint32_t pageCount;
	u32 pagesResident;
	u64 frameCounter;

	GxSurf *surfs;
	uint32_t surfCount;
	int32_t currentTarget;
	bool targetBound;
	int32_t tileCol, tileRow;
	GXTexObj *boundTex;
	GxPage *boundPage;

	bool blendEnable;
	int32_t blendMode;
	BlendFactors blendFactors;
	bool alphaTestEnable;
	u8 alphaTestRef;
	bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
	bool texBound;

	GxVert *primVerts;
	int32_t primCount;
	int32_t primCap;
	int32_t primType;
	int32_t primTpag;
	bool primTextured;
	int32_t appSurfId;
	bool viewActive;
	bool viewSnapped;
	int32_t viewX, viewY, viewW, viewH;
	int32_t viewPortX, viewPortY, viewPortW, viewPortH;
	bool targetDirty;
} GxRenderer;

static RendererVtable gxVtable;
static GxRenderer *gx_cur;
static void gxSurfaceResize(Renderer *renderer, int32_t surfaceID, int32_t width, int32_t height);

static int pad4(int v)
{
	return (v + 3) & ~3;
}

static u32 tex_bytes(int tw, int th)
{
	return (u32)tw * (u32)th * sizeof(u16);
}

static u32 surf_bytes(int tw, int th)
{
	return (u32)tw * (u32)th * SURF_BPP;
}

static u16 pack_rgb5a3(u8 r, u8 g, u8 b, u8 a)
{
	if (a >= 224)
		return (u16)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
	return (u16)(((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

static void unpack_rgb5a3(u16 p, u8 *r, u8 *g, u8 *b, u8 *a)
{
	if (p & 0x8000) {
		*a = 255;
		*r = (u8)(((p >> 10) & 31) * 255 / 31);
		*g = (u8)(((p >> 5) & 31) * 255 / 31);
		*b = (u8)((p & 31) * 255 / 31);
	} else {
		*a = (u8)(((p >> 12) & 7) * 255 / 7);
		*r = (u8)(((p >> 8) & 15) * 255 / 15);
		*g = (u8)(((p >> 4) & 15) * 255 / 15);
		*b = (u8)((p & 15) * 255 / 15);
	}
}

/* GX_TF_RGBA8 is 64 bytes per 4×4: AR plane then GB plane. */
static void gx_rgba8_pset(u8 *texels, int texW, int x, int y, u8 r, u8 g, u8 b, u8 a)
{
	int o = ((y & 3) * 4 + (x & 3)) * 2;
	u8 *tile = texels + (((y >> 2) * (texW >> 2) + (x >> 2)) * 64);

	tile[o] = a;
	tile[o + 1] = r;
	tile[32 + o] = g;
	tile[32 + o + 1] = b;
}

static void gx_rgba8_pget(const u8 *texels, int texW, int x, int y, u8 *r, u8 *g, u8 *b, u8 *a)
{
	int o = ((y & 3) * 4 + (x & 3)) * 2;
	const u8 *tile = texels + (((y >> 2) * (texW >> 2) + (x >> 2)) * 64);

	*a = tile[o];
	*r = tile[o + 1];
	*g = tile[32 + o];
	*b = tile[32 + o + 1];
}

static void gx_page_sample(GxPage *p, float u, float v, u8 *r, u8 *g, u8 *b, u8 *a)
{
	int x, y, off;

	if (!p || !p->texels) {
		*r = *g = *b = *a = 255;
		return;
	}
	x = (int)(u * (float)p->texW);
	y = (int)(v * (float)p->texH);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= p->texW)
		x = p->texW - 1;
	if (y >= p->texH)
		y = p->texH - 1;
	off = ((y >> 2) * (p->texW >> 2) + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3);
	unpack_rgb5a3(p->texels[off], r, g, b, a);
}

static void swizzle_rgb5a3(u16 *dst, const u8 *rgba, int w, int h, int tw)
{
	int x, y;

	memset(dst, 0, tex_bytes(tw, pad4(h)));
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const u8 *p = rgba + ((y * w + x) * 4);
			int tx = x >> 2;
			int ty = y >> 2;
			int off = (ty * (tw >> 2) + tx) * 16 + (y & 3) * 4 + (x & 3);

			dst[off] = pack_rgb5a3(p[0], p[1], p[2], p[3]);
		}
	}
}

static void gx_vtx_fmt(void)
{
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

static void gx_ortho(float top, float bottom, float left, float right)
{
	Mtx mv;
	Mtx44 proj;

	guOrtho(proj, top, bottom, left, right, 0.0f, 300.0f);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	guMtxIdentity(mv);
	guMtxTransApply(mv, mv, 0.0f, 0.0f, -1.0f);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
}

static void gx_viewport(int x, int y, int w, int h)
{
	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;
	GX_SetViewport((f32)x, (f32)y, (f32)w, (f32)h, 0.0f, 1.0f);
	GX_SetScissor(x, y, w, h);
}

static u8 gx_factor(int32_t gm)
{
	switch (gm) {
	case bm_zero:
		return GX_BL_ZERO;
	case bm_one:
		return GX_BL_ONE;
	case bm_src_color:
		return GX_BL_SRCCLR;
	case bm_inv_src_color:
		return GX_BL_INVSRCCLR;
	case bm_src_alpha:
		return GX_BL_SRCALPHA;
	case bm_inv_src_alpha:
		return GX_BL_INVSRCALPHA;
	case bm_dest_alpha:
		return GX_BL_DSTALPHA;
	case bm_inv_dest_alpha:
		return GX_BL_INVDSTALPHA;
	case bm_dest_color:
		return GX_BL_DSTCLR;
	case bm_inv_dest_color:
		return GX_BL_INVDSTCLR;
	default:
		return GX_BL_SRCALPHA;
	}
}

static void gx_apply_blend(GxRenderer *gx)
{
	u8 src, dst;

	if (!gx->blendEnable) {
		GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
		return;
	}
	switch (gx->blendMode) {
	case bm_add:
		src = GX_BL_SRCALPHA;
		dst = GX_BL_ONE;
		break;
	case bm_max:
		src = GX_BL_SRCALPHA;
		dst = GX_BL_INVSRCCLR;
		break;
	case bm_subtract:
		src = GX_BL_ZERO;
		dst = GX_BL_INVSRCCLR;
		break;
	case bm_complex:
		src = gx_factor(gx->blendFactors.src);
		dst = gx_factor(gx->blendFactors.dst);
		break;
	default:
		src = GX_BL_SRCALPHA;
		dst = GX_BL_INVSRCALPHA;
		break;
	}
	GX_SetBlendMode(GX_BM_BLEND, src, dst, GX_LO_CLEAR);
}

static void gx_apply_alpha_test(GxRenderer *gx)
{
	if (gx->alphaTestEnable)
		GX_SetAlphaCompare(GX_GEQUAL, gx->alphaTestRef, GX_AOP_AND, GX_ALWAYS, 0);
	else
		GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
}

/* s_map Create runs after CopyDisp, before beginFrame — vtx fmt is otherwise stale. */
static void gx_draw_state(GxRenderer *gx)
{
	gx_vtx_fmt();
	gx_apply_blend(gx);
	gx_apply_alpha_test(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
}

/* CopyDisp leaves the PE idle; a 2×2 kick makes later CopyTex see rasterized quads. */
static void gx_pe_kick(void)
{
	gx_vtx_fmt();
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetPixelFmt(GX_PF_RGBA6_Z24, GX_ZC_LINEAR);
	gx_viewport(0, 0, 2, 2);
	gx_ortho(0, 2, 0, 2);
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position2f32(0, 0);
	GX_Color4u8(0, 0, 0, 0);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(2, 0);
	GX_Color4u8(0, 0, 0, 0);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(2, 2);
	GX_Color4u8(0, 0, 0, 0);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(0, 2);
	GX_Color4u8(0, 0, 0, 0);
	GX_TexCoord2f32(0, 0);
	GX_End();
	GX_Flush();
}

static float mixf(float a, float b, float t)
{
	return a + (b - a) * t;
}

static u8 mixu8(u8 a, u8 b, float t)
{
	return (u8)((float)a + ((float)b - (float)a) * t + 0.5f);
}

static GxPage *gx_page_of(GxRenderer *gx, GXTexObj *obj)
{
	uint32_t i;

	if (!gx || !obj)
		return NULL;
	for (i = 0; i < gx->pageCount; i++) {
		if (gx->pages[i].loaded && &gx->pages[i].obj == obj)
			return &gx->pages[i];
	}
	return NULL;
}

static void gx_bind_tex(GxRenderer *gx, GXTexObj *obj)
{
	GX_LoadTexObj(obj, GX_TEXMAP0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	gx->texBound = true;
	gx->boundTex = obj;
	gx->boundPage = gx_page_of(gx, obj);
}

static void gx_unbind_tex(GxRenderer *gx)
{
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	gx->texBound = false;
	gx->boundTex = NULL;
	gx->boundPage = NULL;
}

static void gx_bind_tile(GxRenderer *gx, GxSurf *s, int col, int row, bool restore);
static void emit_quad_raw(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
			  float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
			  u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
			  u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3);

static GxSurf *gx_tiled_target(GxRenderer *gx)
{
	GxSurf *s;

	if (!gx || gx->currentTarget < 0 || (uint32_t)gx->currentTarget >= gx->surfCount)
		return NULL;
	s = &gx->surfs[gx->currentTarget];
	if (!s->exists || !s->tiles)
		return NULL;
	return s;
}

static void emit_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
		      float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
		      u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
		      u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	GxSurf *s = gx_tiled_target(gx_cur);
	float minx, maxx, miny, maxy;
	int c0, c1, tr0, tr1, c, tr;

	if (!s) {
		emit_quad_raw(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, u2, v2, u3, v3,
			      r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3);
		return;
	}
	minx = x0;
	maxx = x0;
	miny = y0;
	maxy = y0;
	if (x1 < minx)
		minx = x1;
	if (x1 > maxx)
		maxx = x1;
	if (y1 < miny)
		miny = y1;
	if (y1 > maxy)
		maxy = y1;
	if (x2 < minx)
		minx = x2;
	if (x2 > maxx)
		maxx = x2;
	if (y2 < miny)
		miny = y2;
	if (y2 > maxy)
		maxy = y2;
	if (x3 < minx)
		minx = x3;
	if (x3 > maxx)
		maxx = x3;
	if (y3 < miny)
		miny = y3;
	if (y3 > maxy)
		maxy = y3;
	if (maxx <= 0.0f || maxy <= 0.0f || minx >= (float)s->w || miny >= (float)s->h)
		return;
	if (minx < 0.0f)
		minx = 0.0f;
	if (miny < 0.0f)
		miny = 0.0f;
	if (maxx > (float)s->w)
		maxx = (float)s->w;
	if (maxy > (float)s->h)
		maxy = (float)s->h;
	c0 = (int)(minx / (float)SURF_TILE_W);
	tr0 = (int)(miny / (float)SURF_TILE_H);
	c1 = (int)((maxx - 0.001f) / (float)SURF_TILE_W);
	tr1 = (int)((maxy - 0.001f) / (float)SURF_TILE_H);
	if (c0 < 0)
		c0 = 0;
	if (tr0 < 0)
		tr0 = 0;
	if (c1 >= s->tilesX)
		c1 = s->tilesX - 1;
	if (tr1 >= s->tilesY)
		tr1 = s->tilesY - 1;
	if (c1 < c0 || tr1 < tr0)
		return;
	/* GX CopyTex of tiled EFB never wrote texels (320×240 magenta probe stayed 0xAA). */
	for (tr = tr0; tr <= tr1; tr++) {
		for (c = c0; c <= c1; c++) {
			int ox = c * SURF_TILE_W;
			int oy = tr * SURF_TILE_H;
			int tw = SURF_TILE_W;
			int th = SURF_TILE_H;
			int x, y, x0i, y0i, x1i, y1i;
			float dx, dy;
			GxTile *t;

			if (ox + tw > s->w)
				tw = s->w - ox;
			if (oy + th > s->h)
				th = s->h - oy;
			if (tw < 2 || th < 2)
				continue;
			x0i = (int)(minx < (float)ox ? (float)ox : minx);
			y0i = (int)(miny < (float)oy ? (float)oy : miny);
			x1i = (int)(maxx > (float)(ox + tw) ? (float)(ox + tw) : maxx);
			y1i = (int)(maxy > (float)(oy + th) ? (float)(oy + th) : maxy);
			if (x0i >= x1i || y0i >= y1i)
				continue;
			dx = maxx - minx;
			dy = maxy - miny;
			if (dx < 0.001f)
				dx = 1.0f;
			if (dy < 0.001f)
				dy = 1.0f;
			t = &s->tiles[tr * s->tilesX + c];
			if (!t->texels)
				continue;
			for (y = y0i; y < y1i; y++) {
				float py = ((float)y + 0.5f - miny) / dy;

				for (x = x0i; x < x1i; x++) {
					float px = ((float)x + 0.5f - minx) / dx;
					float uu, vv;
					u8 vr, vg, vb, va, sr, sg, sb, sa;
					int lx = x - ox, ly = y - oy;

					uu = mixf(mixf(u0, u1, px), mixf(u3, u2, px), py);
					vv = mixf(mixf(v0, v1, px), mixf(v3, v2, px), py);
					vr = mixu8(mixu8(r0, r1, px), mixu8(r3, r2, px), py);
					vg = mixu8(mixu8(g0, g1, px), mixu8(g3, g2, px), py);
					vb = mixu8(mixu8(b0, b1, px), mixu8(b3, b2, px), py);
					va = mixu8(mixu8(a0, a1, px), mixu8(a3, a2, px), py);
					if (gx_cur && gx_cur->boundPage) {
						gx_page_sample(gx_cur->boundPage, uu, vv, &sr, &sg, &sb, &sa);
						vr = (u8)((vr * sr) / 255);
						vg = (u8)((vg * sg) / 255);
						vb = (u8)((vb * sb) / 255);
						va = (u8)((va * sa) / 255);
					}
					if (va == 0)
						continue;
					if (va < 255) {
						u8 dr, dg, db, da;

						gx_rgba8_pget((u8 *)t->texels, t->texW, lx, ly, &dr, &dg, &db, &da);
						vr = (u8)((vr * va + dr * (255 - va)) / 255);
						vg = (u8)((vg * va + dg * (255 - va)) / 255);
						vb = (u8)((vb * va + db * (255 - va)) / 255);
						va = (u8)(va + (u16)da * (255 - va) / 255);
					}
					gx_rgba8_pset((u8 *)t->texels, t->texW, lx, ly, vr, vg, vb, va);
				}
			}
			t->objReady = false;
		}
	}
}

static int gx_quads;

static void emit_quad_raw(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
			  float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
			  u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
			  u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position2f32(x0, y0);
	GX_Color4u8(r0, g0, b0, a0);
	GX_TexCoord2f32(u0, v0);
	GX_Position2f32(x1, y1);
	GX_Color4u8(r1, g1, b1, a1);
	GX_TexCoord2f32(u1, v1);
	GX_Position2f32(x2, y2);
	GX_Color4u8(r2, g2, b2, a2);
	GX_TexCoord2f32(u2, v2);
	GX_Position2f32(x3, y3);
	GX_Color4u8(r3, g3, b3, a3);
	GX_TexCoord2f32(u3, v3);
	GX_End();
	if (gx_cur)
		gx_cur->targetDirty = true;
	if (++gx_quads >= 256) {
		GX_Flush();
		gx_quads = 0;
	}
}

static u8 unorm(float a)
{
	if (a <= 0.0f)
		return 0;
	if (a >= 1.0f)
		return 255;
	return (u8)(a * 255.0f + 0.5f);
}

static bool gx_evict_page(GxRenderer *gx)
{
	uint32_t i;
	uint32_t victim = UINT32_MAX;
	u64 oldest = UINT64_MAX;

	for (i = 0; i < gx->pageCount; i++) {
		if (!gx->pages[i].loaded || !gx->pages[i].texels)
			continue;
		if (gx->pages[i].lastUsed == gx->frameCounter)
			continue;
		if (gx->pages[i].lastUsed < oldest) {
			oldest = gx->pages[i].lastUsed;
			victim = i;
		}
	}
	if (victim == UINT32_MAX)
		return false;
	WiiMem_texFree(gx->pages[victim].texels);
	gx->pages[victim].texels = NULL;
	gx->pages[victim].loaded = false;
	gx->pagesResident--;
	logInfo("GX: evict TXTR %u (resident %u)\n", victim, gx->pagesResident);
	return true;
}

/* Pages idle since an earlier frame are safe to drop: every frame ends in GX_DrawDone. */
static void gx_make_room(GxRenderer *gx, u32 want)
{
	while (WiiMem_room() < want && gx_evict_page(gx))
		;
}

typedef struct {
	u16 *dst;
	int w, h, tw;
} PageRows;

static void gx_page_row(void *user, int y, const u8 *rgba, int width)
{
	PageRows *pr = (PageRows *)user;
	int x, n = width < pr->w ? width : pr->w;

	if (y >= pr->h)
		return;
	for (x = 0; x < n; x++) {
		const u8 *p = rgba + x * 4;
		int off = ((y >> 2) * (pr->tw >> 2) + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3);

		pr->dst[off] = pack_rgb5a3(p[0], p[1], p[2], p[3]);
	}
}

static bool gx_load_page(GxRenderer *gx, uint32_t pageId)
{
	DataWin *dw = gx->base.dataWin;
	GxPage *page;
	Texture *txtr;
	u8 *pixels = NULL;
	u16 *texels = NULL;
	int w, h, tw, th;
	bool streamed;

	if (pageId >= gx->pageCount)
		return false;
	page = &gx->pages[pageId];
	if (page->loaded)
		return page->texels != NULL;
	if (page->failed && gx->frameCounter < page->retryAt)
		return false;

	page->failed = false;
	gx_make_room(gx, PAGE_LOAD_NEED);
	DataWin_loadTxtrIfNeeded(dw, pageId);
	txtr = &dw->txtr.textures[pageId];
	if (!txtr->blobData)
		goto fail;
	streamed = WiiPng_size(txtr->blobData, (size_t)txtr->blobSize, &w, &h);
	if (!streamed) {
		pixels = ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize,
		                                   DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0), &w, &h);
		if (!pixels)
			goto fail;
	}
	if (w > 1024)
		w = 1024;
	if (h > 1024)
		h = 1024;
	tw = pad4(w);
	th = pad4(h);
	while (gx->pagesResident >= PAGE_LRU_MAX && gx_evict_page(gx))
		;
	texels = WiiMem_texAlloc(tex_bytes(tw, th));
	if (!texels && gx_evict_page(gx))
		texels = WiiMem_texAlloc(tex_bytes(tw, th));
	if (!texels)
		goto fail;
	if (streamed) {
		PageRows pr = { texels, w, h, tw };

		memset(texels, 0, tex_bytes(tw, th));
		if (!WiiPng_decodeRows(txtr->blobData, (size_t)txtr->blobSize, gx_page_row, &pr))
			goto fail;
	} else {
		swizzle_rgb5a3(texels, pixels, w, h, tw);
		free(pixels);
		pixels = NULL;
	}
	if (!txtr->mapped) {
		free(txtr->blobData);
		txtr->blobData = NULL;
	}
	DCFlushRange(texels, tex_bytes(tw, th));
	GX_InitTexObj(&page->obj, texels, (u16)tw, (u16)th, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&page->obj, GX_NEAR, GX_NEAR);
	page->texels = texels;
	page->w = w;
	page->h = h;
	page->texW = tw;
	page->texH = th;
	page->loaded = true;
	gx->pagesResident++;
	GX_InvalidateTexAll();
	logInfo("GX: TXTR %u %dx%d (padded %dx%d, resident %u)\n", pageId, w, h, tw, th, gx->pagesResident);
	return true;

fail:
	logWarn("GX: TXTR %u load failed, retry in %d frames\n", pageId, PAGE_RETRY_FRAMES);
	free(pixels);
	WiiMem_texFree(texels);
	if (txtr->blobData && !txtr->mapped) {
		free(txtr->blobData);
		txtr->blobData = NULL;
	}
	page->failed = true;
	page->retryAt = gx->frameCounter + PAGE_RETRY_FRAMES;
	return false;
}

static GxPage *gx_page(GxRenderer *gx, int16_t pageId)
{
	if (pageId < 0 || (uint32_t)pageId >= gx->pageCount)
		return NULL;
	if (!gx_load_page(gx, (uint32_t)pageId))
		return NULL;
	gx->pages[pageId].lastUsed = gx->frameCounter;
	return &gx->pages[pageId];
}

static void gx_tile_rect(const GxSurf *s, int col, int row, int *ox, int *oy, int *tw, int *th)
{
	*ox = col * SURF_TILE_W;
	*oy = row * SURF_TILE_H;
	*tw = s->w - *ox;
	*th = s->h - *oy;
	if (*tw > SURF_TILE_W)
		*tw = SURF_TILE_W;
	if (*th > SURF_TILE_H)
		*th = SURF_TILE_H;
	if (*tw < 2)
		*tw = 2;
	if (*th < 2)
		*th = 2;
}

static GxTile *gx_tile_at(GxSurf *s, int col, int row)
{
	return &s->tiles[row * s->tilesX + col];
}

static void gx_prep_tile_obj(GxTile *t)
{
	if (t->objReady || !t->texels)
		return;
	GX_InitTexObj(&t->obj, t->texels, (u16)t->texW, (u16)t->texH, SURF_TEX_FMT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&t->obj, GX_NEAR, GX_NEAR);
	t->objReady = true;
}

static void gx_flush_target(GxRenderer *gx)
{
	GxSurf *s;
	GxTile *t;
	u16 *texels;
	int copyW, copyH, texW, texH;

	if (gx->currentTarget < 0 || !gx->targetBound)
		return;
	if ((uint32_t)gx->currentTarget >= gx->surfCount)
		return;
	s = &gx->surfs[gx->currentTarget];
	if (!s->exists)
		return;
	if (s->tiles) {
		int i, n = s->tilesX * s->tilesY;

		for (i = 0; i < n; i++) {
			t = &s->tiles[i];
			if (!t->texels)
				continue;
			DCFlushRange(t->texels, surf_bytes(t->texW, t->texH));
			t->objReady = false;
		}
		{
			static int cpuFlushLog;
			u32 i, sum = 0, nbytes;
			u8 *p;

			t = &s->tiles[0];
			nbytes = t->texels ? surf_bytes(t->texW, t->texH) : 0;
			p = (u8 *)t->texels;
			for (i = 0; i < nbytes; i++)
				sum += p[i];
			if (cpuFlushLog < 4) {
				logInfo("GX: cpu-flush tiled %dx%d tile00 sum=%u\n", s->tilesX, s->tilesY, sum);
				cpuFlushLog++;
			}
		}
		GX_InvalidateTexAll();
		gx->targetDirty = false;
		return;
	} else {
		if (!s->texels)
			return;
		texels = s->texels;
		copyW = s->w;
		copyH = s->h;
		if (copyW > EFB_MAX_W)
			copyW = EFB_MAX_W;
		if (copyH > EFB_MAX_H)
			copyH = EFB_MAX_H;
		texW = s->texW;
		texH = s->texH;
		s->objReady = false;
	}
	if (copyW > EFB_MAX_W)
		copyW = EFB_MAX_W;
	if (copyH > (int)WiiVideo_efbHeight())
		copyH = (int)WiiVideo_efbHeight();
	copyW &= ~1;
	copyH &= ~1;
	if (copyW < 2 || copyH < 2 || !texels)
		return;
	GX_Flush();
	GX_DrawDone();
	DCInvalidateRange(texels, surf_bytes(texW, texH));
	GX_SetTexCopySrc(0, 0, (u16)copyW, (u16)copyH);
	GX_SetTexCopyDst((u16)texW, (u16)texH, SURF_TEX_FMT, GX_FALSE);
	GX_CopyTex(texels, GX_FALSE);
	GX_PixModeSync();
	DCInvalidateRange(texels, surf_bytes(texW, texH));
	GX_InvalidateTexAll();
	gx->targetDirty = false;
	gx_draw_state(gx);
}

static void gx_prep_surf_obj(GxSurf *s)
{
	if (s->objReady || !s->texels)
		return;
	GX_InitTexObj(&s->obj, s->texels, (u16)s->texW, (u16)s->texH, SURF_TEX_FMT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&s->obj, GX_NEAR, GX_NEAR);
	s->objReady = true;
}

static void gx_restore_surface(GxRenderer *gx, int32_t id)
{
	GxSurf *s;
	float u1, v1;

	if (id < 0 || (uint32_t)id >= gx->surfCount)
		return;
	s = &gx->surfs[id];
	if (!s->exists || !s->texels)
		return;
	gx_prep_surf_obj(s);
	gx_bind_tex(gx, &s->obj);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	u1 = (float)s->w / (float)s->texW;
	v1 = (float)s->h / (float)s->texH;
	emit_quad_raw(0, 0, (float)s->w, 0, (float)s->w, (float)s->h, 0, (float)s->h,
		      0, 0, u1, 0, u1, v1, 0, v1,
		      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);
	gx->targetDirty = false;
	gx_apply_blend(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
}

static void gx_restore_tile(GxRenderer *gx, GxSurf *s, int col, int row)
{
	GxTile *t = gx_tile_at(s, col, row);
	int ox, oy, tw, th;
	float u1, v1;
	GXTexObj *saved = gx->boundTex;
	bool wasBound = gx->texBound;

	if (!t->texels)
		return;
	gx_tile_rect(s, col, row, &ox, &oy, &tw, &th);
	gx_prep_tile_obj(t);
	GX_LoadTexObj(&t->obj, GX_TEXMAP0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	u1 = (float)t->pixelW / (float)t->texW;
	v1 = (float)t->pixelH / (float)t->texH;
	emit_quad_raw((float)ox, (float)oy, (float)(ox + tw), (float)oy, (float)(ox + tw), (float)(oy + th),
		      (float)ox, (float)(oy + th), 0, 0, u1, 0, u1, v1, 0, v1,
		      255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);
	if (wasBound && saved)
		gx_bind_tex(gx, saved);
	else
		gx_unbind_tex(gx);
	gx_apply_blend(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
	gx->targetDirty = false;
}

static void gx_setup_tile_view(const GxSurf *s, int col, int row)
{
	int ox, oy, tw, th;

	gx_tile_rect(s, col, row, &ox, &oy, &tw, &th);
	gx_viewport(0, 0, tw, th);
	gx_ortho((float)oy, (float)(oy + th), (float)ox, (float)(ox + tw));
}

static void gx_bind_tile(GxRenderer *gx, GxSurf *s, int col, int row, bool restore)
{
	if (col < 0 || row < 0 || col >= s->tilesX || row >= s->tilesY)
		return;
	gx_cur = gx;
	if (gx->targetBound && gx->currentTarget >= 0 &&
	    (uint32_t)gx->currentTarget < gx->surfCount &&
	    &gx->surfs[gx->currentTarget] == s && gx->tileCol == col && gx->tileRow == row) {
		gx_setup_tile_view(s, col, row);
		return;
	}
	if (gx->targetBound && gx->currentTarget >= 0 &&
	    (uint32_t)gx->currentTarget < gx->surfCount &&
	    &gx->surfs[gx->currentTarget] == s)
		gx_flush_target(gx);
	gx->tileCol = col;
	gx->tileRow = row;
	gx->targetBound = true;
	gx_draw_state(gx);
	gx_setup_tile_view(s, col, row);
	gx->targetDirty = false;
	if (restore)
		gx_restore_tile(gx, s, col, row);
}

static void gx_bind_host(GxRenderer *gx)
{
	/* Physical output is always the full framebuffer; the game's logical window
	 * size must not shrink the present into a corner (widescreen/death/settings). */
	int w = (int)WiiVideo_fbWidth();
	int h = (int)WiiVideo_efbHeight();

	/* Don't recopy the app surface from an EFB that set_target/clear already stomped. */
	if (gx->currentTarget != gx->appSurfId)
		gx_flush_target(gx);
	else
		gx->currentTarget = HOST_TARGET;
	gx->currentTarget = HOST_TARGET;
	gx->targetBound = true;
	gx->targetDirty = false;
	gx->tileCol = -1;
	gx->tileRow = -1;
	gx_cur = gx;
	/* Overscan: squeeze everything into the safe area; the border stays black
	 * (EFB clears full each present, and the scissor clips to the viewport). */
	int oh = Overscan_h();
	int ov = Overscan_v();
	if (WII_WIDE_PILLARBOX && WiiVideo_isWide()) {
		int pw = (w * 3) / 4;

		gx_viewport((w - pw) / 2 + oh, ov, pw - 2 * oh, h - 2 * ov);
	} else {
		gx_viewport(oh, ov, w - 2 * oh, h - 2 * ov);
	}
	gx->base.CPortX = 0;
	gx->base.CPortY = 0;
	gx->base.CPortW = w;
	gx->base.CPortH = h;
	gx_ortho(0, (float)h, 0, (float)w);
}

static bool gx_bind_surface(GxRenderer *gx, int32_t id, bool restore)
{
	GxSurf *s;
	int vw, vh;

	if (id < 0 || (uint32_t)id >= gx->surfCount)
		return false;
	s = &gx->surfs[id];
	if (!s->exists)
		return false;
	if (gx->currentTarget != id)
		gx_flush_target(gx);
	gx->currentTarget = id;
	gx->targetBound = true;
	gx->targetDirty = false;
	gx_cur = gx;
	gx_pe_kick();
	gx_draw_state(gx);
	gx->base.CPortX = 0;
	gx->base.CPortY = 0;
	gx->base.CPortW = s->w;
	gx->base.CPortH = s->h;
	if (s->tiles) {
		gx->tileCol = -1;
		gx->tileRow = -1;
		gx_bind_tile(gx, s, 0, 0, restore);
		return true;
	}
	gx->tileCol = 0;
	gx->tileRow = 0;
	vw = s->w;
	vh = s->h;
	if (vw > EFB_MAX_W)
		vw = EFB_MAX_W;
	if (vh > EFB_MAX_H)
		vh = EFB_MAX_H;
	gx_viewport(0, 0, vw, vh);
	gx_ortho(0, (float)s->h, 0, (float)s->w);
	if (restore)
		gx_restore_surface(gx, id);
	return true;
}

static void gx_surf_release(GxSurf *s)
{
	int i, n;

	if (s->tiles) {
		n = s->tilesX * s->tilesY;
		for (i = 0; i < n; i++)
			free(s->tiles[i].texels);
		free(s->tiles);
	}
	free(s->texels);
	s->tiles = NULL;
	s->texels = NULL;
	s->tilesX = 0;
	s->tilesY = 0;
	s->objReady = false;
}

static bool gx_alloc_tiles(GxSurf *s, int32_t width, int32_t height)
{
	int col, row, n, i;
	int tilesX = (width + SURF_TILE_W - 1) / SURF_TILE_W;
	int tilesY = (height + SURF_TILE_H - 1) / SURF_TILE_H;
	GxTile *tiles = (GxTile *)safeCalloc((size_t)tilesX * (size_t)tilesY, sizeof(GxTile));

	if (!tiles)
		return false;
	for (row = 0; row < tilesY; row++) {
		for (col = 0; col < tilesX; col++) {
			GxTile *t = &tiles[row * tilesX + col];
			int ox = col * SURF_TILE_W;
			int oy = row * SURF_TILE_H;
			int pw = width - ox;
			int ph = height - oy;

			if (pw > SURF_TILE_W)
				pw = SURF_TILE_W;
			if (ph > SURF_TILE_H)
				ph = SURF_TILE_H;
			t->pixelW = pw;
			t->pixelH = ph;
			t->texW = pad4(pw);
			t->texH = pad4(ph);
			t->texels = memalign(32, surf_bytes(t->texW, t->texH));
			if (!t->texels) {
				n = tilesX * tilesY;
				for (i = 0; i < n; i++)
					free(tiles[i].texels);
				free(tiles);
				return false;
			}
			memset(t->texels, 0, surf_bytes(t->texW, t->texH));
			DCFlushRange(t->texels, surf_bytes(t->texW, t->texH));
		}
	}
	s->tiles = tiles;
	s->tilesX = tilesX;
	s->tilesY = tilesY;
	s->texels = NULL;
	s->texW = 0;
	s->texH = 0;
	return true;
}

static int32_t gx_alloc_surface(GxRenderer *gx, int32_t width, int32_t height)
{
	uint32_t i;
	int tw, th;
	u16 *texels;
	int32_t id = -1;
	bool tiled;

	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	tiled = (width > EFB_MAX_W || height > EFB_MAX_H);
	/* Slot 0 is never a real surface — GML inits ids to 0 meaning "none". */
	for (i = 1; i < gx->surfCount; i++) {
		if (!gx->surfs[i].exists) {
			id = (int32_t)i;
			break;
		}
	}
	if (id < 0) {
		uint32_t n = gx->surfCount ? gx->surfCount * 2 : 8;
		gx->surfs = (GxSurf *)safeRealloc(gx->surfs, n * sizeof(GxSurf));
		memset(gx->surfs + gx->surfCount, 0, (n - gx->surfCount) * sizeof(GxSurf));
		id = gx->surfCount < 1 ? 1 : (int32_t)gx->surfCount;
		gx->surfCount = n;
		if (id == 0)
			id = 1;
	}
	gx_surf_release(&gx->surfs[id]);
	gx->surfs[id].exists = true;
	gx->surfs[id].w = width;
	gx->surfs[id].h = height;
	gx->surfs[id].objReady = false;
	if (tiled) {
		if (!gx_alloc_tiles(&gx->surfs[id], width, height)) {
			gx->surfs[id].exists = false;
			logWarn("GX: tiled surface %dx%d alloc failed\n", width, height);
			return -1;
		}
		logInfo("GX: surface %d %dx%d tiled %dx%d\n", id, width, height,
			gx->surfs[id].tilesX, gx->surfs[id].tilesY);
		return id;
	}
	tw = pad4(width);
	th = pad4(height);
	texels = memalign(32, surf_bytes(tw, th));
	if (!texels) {
		gx->surfs[id].exists = false;
		return -1;
	}
	memset(texels, 0, surf_bytes(tw, th));
	DCFlushRange(texels, surf_bytes(tw, th));
	gx->surfs[id].texW = tw;
	gx->surfs[id].texH = th;
	gx->surfs[id].texels = texels;
	if (id < 8)
		logInfo("GX: surface %d %dx%d\n", id, width, height);
	return id;
}

static void gx_draw_tpag(GxRenderer *gx, int32_t tpagIndex, float x0, float y0, float x1, float y1,
			 float x2, float y2, float x3, float y3, float srcX, float srcY, float srcW, float srcH,
			 u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
			 u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	DataWin *dw = gx->base.dataWin;
	TexturePageItem *tpag;
	GxPage *page;
	float u0, v0, u1, v1;

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	tpag = &dw->tpag.items[tpagIndex];
	page = gx_page(gx, tpag->texturePageId);
	if (!page)
		return;
	u0 = (tpag->sourceX + srcX) / (float)page->texW;
	v0 = (tpag->sourceY + srcY) / (float)page->texH;
	u1 = (tpag->sourceX + srcX + srcW) / (float)page->texW;
	v1 = (tpag->sourceY + srcY + srcH) / (float)page->texH;
	gx_bind_tex(gx, &page->obj);
	emit_quad(x0, y0, x1, y1, x2, y2, x3, y3,
		  u0, v0, u1, v0, u1, v1, u0, v1,
		  r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3);
}

static void gxInit(Renderer *renderer, DataWin *dataWin)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	Matrix4f world;

	renderer->dataWin = dataWin;
	Matrix4f_identity(&world);
	renderer->gmlMatrices[MATRIX_WORLD] = world;
	gx->pageCount = dataWin->txtr.count;
	gx->pages = (GxPage *)safeCalloc(gx->pageCount, sizeof(GxPage));
	gx->currentTarget = HOST_TARGET;
	gx->appSurfId = -1;
	gx->tileCol = -1;
	gx->tileRow = -1;
	gx->boundTex = NULL;
	gx->boundPage = NULL;
	gx->viewActive = false;
	gx->blendEnable = true;
	gx->blendMode = bm_normal;
	gx->blendFactors.src = bm_src_alpha;
	gx->blendFactors.dst = bm_inv_src_alpha;
	gx->blendFactors.srcAlpha = bm_src_alpha;
	gx->blendFactors.dstAlpha = bm_inv_src_alpha;
	gx->colorWriteR = gx->colorWriteG = gx->colorWriteB = gx->colorWriteA = true;
	gx_vtx_fmt();
	gx_apply_blend(gx);
	logInfo("GX: renderer init, %u TXTR pages\n", gx->pageCount);
}

static void gxDestroy(Renderer *renderer)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	uint32_t i;

	for (i = 0; i < gx->pageCount; i++)
		WiiMem_texFree(gx->pages[i].texels);
	for (i = 0; i < gx->surfCount; i++)
		gx_surf_release(&gx->surfs[i]);
	free(gx->pages);
	free(gx->surfs);
	free(gx->primVerts);
	free(gx);
}

static void gxBeginFrame(Renderer *renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t appId;

	gx->gameW = gameW;
	gx->gameH = gameH;
	gx->windowW = windowW;
	gx->windowH = windowH;
	gx->frameCounter++;
	gx->viewActive = false;
	gx->viewSnapped = false;
	gx_quads = 0;
	gx_cur = gx;
	gx_vtx_fmt();
	gx_apply_blend(gx);
	gx_apply_alpha_test(gx);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	if ((gx->frameCounter % 60) == 0)
		gx_make_room(gx, MEM_RESERVE);
	if (renderer->runner) {
		Wii_tryWarp(renderer->runner);
		Wii_sweepTick(renderer->runner);
		Wii_forceGamepadProfile(renderer->runner);
	}
	appId = renderer->runner->applicationSurfaceId;
	if (appId >= 0)
		gx_bind_surface(gx, appId, false);
	else {
		gx_viewport(0, 0, gameW, gameH);
		gx_ortho(0, (float)gameH, 0, (float)gameW);
	}
}

static void gxBeginView(Renderer *renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH,
			int32_t portX, int32_t portY, int32_t portW, int32_t portH, MAYBE_UNUSED float viewAngle)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t viewCurrent = 0;
	RuntimeView *view;
	GMLCamera *camera;

	if (portW < 1)
		portW = 1;
	if (portH < 1)
		portH = 1;
	gx_viewport(portX, portY, portW, portH);
	gx->base.CPortX = portX;
	gx->base.CPortY = portY;
	gx->base.CPortW = portW;
	gx->base.CPortH = portH;
	gx->viewActive = true;
	gx->viewX = viewX;
	gx->viewY = viewY;
	gx->viewW = viewW;
	gx->viewH = viewH;
	gx->viewPortX = portX;
	gx->viewPortY = portY;
	gx->viewPortW = portW;
	gx->viewPortH = portH;
	gx_ortho((float)viewY, (float)(viewY + viewH), (float)viewX, (float)(viewX + viewW));
	(void)viewW;
	if (renderer->runner->viewsEnabled)
		viewCurrent = renderer->runner->viewCurrent;
	view = &renderer->runner->views[viewCurrent];
	gx->base.cameraCurrent = view->cameraId;
	camera = Runner_getCameraById(renderer->runner, gx->base.cameraCurrent);
	if (camera) {
		renderer->gmlMatrices[MATRIX_VIEW] = camera->viewMatrix;
		renderer->gmlMatrices[MATRIX_PROJECTION] = camera->projectionMatrix;
	}
}

static void gxEndView(Renderer *renderer)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (gx->currentTarget == gx->appSurfId) {
		gx_flush_target(gx);
		gx->viewSnapped = true;
	}
}

static void gxApplyProjection(Renderer *renderer, const Matrix4f *viewMatrix, const Matrix4f *projectionMatrix)
{
	renderer->gmlMatrices[MATRIX_VIEW] = *viewMatrix;
	renderer->gmlMatrices[MATRIX_PROJECTION] = *projectionMatrix;
}

static void gxBeginGUI(Renderer *renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY,
		       int32_t portW, int32_t portH, int32_t targetSurfaceId)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GMLCamera *camera = &renderer->runner->guiCamera;

	gx->viewActive = false;
	if (targetSurfaceId == RENDER_TARGET_HOST_FRAMEBUFFER)
		gx_bind_host(gx);
	else
		gx_bind_surface(gx, targetSurfaceId, true);
	if (portW < 1)
		portW = guiW;
	if (portH < 1)
		portH = guiH;
	if (targetSurfaceId != RENDER_TARGET_HOST_FRAMEBUFFER)
		gx_viewport(portX, portY, portW, portH);
	gx_ortho(0, (float)guiH, 0, (float)guiW);
	gx->base.cameraCurrent = GUI_CAMERA;
	camera->allocated = true;
	camera->viewX = 0;
	camera->viewY = 0;
	camera->viewWidth = guiW;
	camera->viewHeight = guiH;
}

static void gxSetGuiProjection(Renderer *renderer, int32_t guiW, int32_t guiH, MAYBE_UNUSED int32_t portW,
			       MAYBE_UNUSED int32_t portH, MAYBE_UNUSED bool renderingToUserSurface)
{
	gx_ortho(0, (float)guiH, 0, (float)guiW);
	renderer->cameraCurrent = GUI_CAMERA;
}

static void gxEndGUI(MAYBE_UNUSED Renderer *renderer) {}

static void gxClearScreen(Renderer *renderer, uint32_t color, float alpha);

static void gxEndFrameInit(Renderer *renderer)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (renderer->runner->usingAppSurface && !renderer->runner->appSurfaceAutoDraw) {
		/* Wipe the whole EFB (the app surface renders into its top-left corner) before the
		 * manual present. Use the FULL framebuffer, not the overscan-inset viewport, or that
		 * scratch corner peeks out in the black border once overscan shrinks the picture. */
		gx_bind_host(gx);
		gx_viewport(0, 0, (int)WiiVideo_fbWidth(), (int)WiiVideo_efbHeight());
		gxClearScreen(renderer, 0x000000, 1.0f);
		gx_bind_host(gx);
	}
}

static void gx_ov_rect(float x, float y, float w, float h, u8 r, u8 g, u8 b, u8 a)
{
	emit_quad(x, y, x + w, y, x + w, y + h, x, y + h,
		  0, 0, 1, 0, 1, 1, 0, 1,
		  r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
}

/* Full-screen calibration overlay (L + Down + Y): grid + a bright safe-area border to
 * line up against the CRT edge, plus H/V magnitude sliders. Drawn in the inset viewport. */
static void gx_draw_overscan_overlay(GxRenderer *gx)
{
	const float LW = (float)WiiVideo_fbWidth();
	const float LH = (float)WiiVideo_efbHeight();
	float cx = LW * 0.5f, cy = LH * 0.5f;
	float tlen = 220.0f, tthk = 14.0f;
	/* Fill shows how big the picture stays: full slider = no inset, empty = max inset. */
	float hf = 1.0f - (float)Overscan_h() / (float)OVERSCAN_MAX_H;
	float vf = 1.0f - (float)Overscan_v() / (float)OVERSCAN_MAX_V;
	float cl = 44.0f, ct = 7.0f;
	int i;

	gx_bind_host(gx);
	gx_ortho(0, LH, 0, LW);
	gx_unbind_tex(gx);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	gx_ov_rect(0, 0, LW, LH, 0, 0, 0, 160);
	for (i = 0; i <= (int)LW; i += 32)
		gx_ov_rect((float)i, 0, 1, LH, 90, 200, 220, 110);
	for (i = 0; i <= (int)LH; i += 32)
		gx_ov_rect(0, (float)i, LW, 1, 90, 200, 220, 110);

	gx_ov_rect(0, 0, LW, 3, 70, 255, 130, 255);
	gx_ov_rect(0, LH - 3, LW, 3, 70, 255, 130, 255);
	gx_ov_rect(0, 0, 3, LH, 70, 255, 130, 255);
	gx_ov_rect(LW - 3, 0, 3, LH, 70, 255, 130, 255);

	gx_ov_rect(0, 0, cl, ct, 255, 225, 40, 255);
	gx_ov_rect(0, 0, ct, cl, 255, 225, 40, 255);
	gx_ov_rect(LW - cl, 0, cl, ct, 255, 225, 40, 255);
	gx_ov_rect(LW - ct, 0, ct, cl, 255, 225, 40, 255);
	gx_ov_rect(0, LH - ct, cl, ct, 255, 225, 40, 255);
	gx_ov_rect(0, LH - cl, ct, cl, 255, 225, 40, 255);
	gx_ov_rect(LW - cl, LH - ct, cl, ct, 255, 225, 40, 255);
	gx_ov_rect(LW - ct, LH - cl, ct, cl, 255, 225, 40, 255);

	/* H slider (Left/Right) above center, V slider (Up/Down) below */
	gx_ov_rect(cx - tlen * 0.5f, cy - 40, tlen, tthk, 30, 30, 40, 230);
	gx_ov_rect(cx - tlen * 0.5f, cy - 40, tlen * hf, tthk, 90, 200, 255, 255);
	gx_ov_rect(cx - tthk * 0.5f, cy + 12, tthk, tlen * 0.55f, 30, 30, 40, 230);
	gx_ov_rect(cx - tthk * 0.5f, cy + 12, tthk, tlen * 0.55f * vf, 255, 170, 90, 255);

	gx_apply_blend(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
}

static void gxEndFrameEnd(Renderer *renderer)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t appId;
	GxSurf *s;
	float u1, v1;
	float dw, dh, ox, oy;

	if (renderer->runner->usingAppSurface && renderer->runner->appSurfaceAutoDraw) {
		appId = renderer->runner->applicationSurfaceId;
		if (gx->currentTarget == appId && !gx->viewSnapped)
			gx_flush_target(gx);
		gx_bind_host(gx);
		if (appId >= 0 && (uint32_t)appId < gx->surfCount && gx->surfs[appId].exists) {
			s = &gx->surfs[appId];
			gx_prep_surf_obj(s);
			gx_bind_tex(gx, &s->obj);
			GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
			int fbw = (int)WiiVideo_fbWidth();
			int fbh = (int)WiiVideo_efbHeight();
			/* Aspect-fit the app surface into the physical screen, not the logical window. */
			if ((gx->gameW * fbh) / gx->gameH < fbw) {
				dw = (float)((gx->gameW * fbh) / gx->gameH);
				dh = (float)fbh;
			} else {
				dw = (float)fbw;
				dh = (float)((gx->gameH * fbw) / gx->gameW);
			}
			ox = ((float)fbw - dw) * 0.5f;
			oy = ((float)fbh - dh) * 0.5f;
			u1 = (float)s->w / (float)s->texW;
			v1 = (float)s->h / (float)s->texH;
			emit_quad(ox, oy, ox + dw, oy, ox + dw, oy + dh, ox, oy + dh,
				  0, 0, u1, 0, u1, v1, 0, v1,
				  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);
			gx_apply_blend(gx);
		}
	}
	if (Overscan_active())
		gx_draw_overscan_overlay(gx);
}

static void gxDrawSprite(Renderer *renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
			 float xscale, float yscale, float angleDeg, uint32_t color, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	DataWin *dw = renderer->dataWin;
	TexturePageItem *tpag;
	float localX0, localY0, localX1, localY1;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	float angleRad;
	Matrix4f transform;
	u8 r = (u8)BGR_R(color), g = (u8)BGR_G(color), b = (u8)BGR_B(color), a = unorm(alpha);

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	tpag = &dw->tpag.items[tpagIndex];
	localX0 = (float)tpag->targetX - originX;
	localY0 = (float)tpag->targetY - originY;
	localX1 = localX0 + (float)tpag->targetWidth;
	localY1 = localY0 + (float)tpag->targetHeight;
	angleRad = -angleDeg * ((float)M_PI / 180.0f);
	Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);
	Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0);
	Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1);
	Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2);
	Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3);
	gx_draw_tpag(gx, tpagIndex, x0, y0, x1, y1, x2, y2, x3, y3,
		     0, 0, (float)tpag->sourceWidth, (float)tpag->sourceHeight,
		     r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
}

static void gx_part_corners(float x, float y, int32_t srcW, int32_t srcH, float xscale, float yscale, float angleDeg,
			    float pivotX, float pivotY, float *x0, float *y0, float *x1, float *y1, float *x2, float *y2,
			    float *x3, float *y3)
{
	float qx0 = x, qy0 = y;
	float qx1 = x + (float)srcW * xscale, qy1 = y;
	float qx2 = x + (float)srcW * xscale, qy2 = y + (float)srcH * yscale;
	float qx3 = x, qy3 = y + (float)srcH * yscale;

	if (angleDeg == 0.0f) {
		*x0 = qx0;
		*y0 = qy0;
		*x1 = qx1;
		*y1 = qy1;
		*x2 = qx2;
		*y2 = qy2;
		*x3 = qx3;
		*y3 = qy3;
		return;
	}
	{
		float angleRad = -angleDeg * ((float)M_PI / 180.0f);
		float c = cosf(angleRad), s = sinf(angleRad);
		float dx, dy;

		dx = qx0 - pivotX;
		dy = qy0 - pivotY;
		*x0 = c * dx - s * dy + pivotX;
		*y0 = s * dx + c * dy + pivotY;
		dx = qx1 - pivotX;
		dy = qy1 - pivotY;
		*x1 = c * dx - s * dy + pivotX;
		*y1 = s * dx + c * dy + pivotY;
		dx = qx2 - pivotX;
		dy = qy2 - pivotY;
		*x2 = c * dx - s * dy + pivotX;
		*y2 = s * dx + c * dy + pivotY;
		dx = qx3 - pivotX;
		dy = qy3 - pivotY;
		*x3 = c * dx - s * dy + pivotX;
		*y3 = s * dx + c * dy + pivotY;
	}
}

static void gxDrawSpritePart(Renderer *renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW,
			     int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg, float pivotX,
			     float pivotY, uint32_t color, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	u8 r = (u8)BGR_R(color), g = (u8)BGR_G(color), b = (u8)BGR_B(color), a = unorm(alpha);

	gx_part_corners(x, y, srcW, srcH, xscale, yscale, angleDeg, pivotX, pivotY, &x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3);
	gx_draw_tpag(gx, tpagIndex, x0, y0, x1, y1, x2, y2, x3, y3,
		     (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH,
		     r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
}

static void gxDrawSpritePartColor(Renderer *renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW,
				  int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg,
				  float pivotX, float pivotY, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	u8 a = unorm(alpha);

	gx_part_corners(x, y, srcW, srcH, xscale, yscale, angleDeg, pivotX, pivotY, &x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3);
	gx_draw_tpag(gx, tpagIndex, x0, y0, x1, y1, x2, y2, x3, y3,
		     (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH,
		     (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		     (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
		     (u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a,
		     (u8)BGR_R(c4), (u8)BGR_G(c4), (u8)BGR_B(c4), a);
}

static void gxDrawSpritePos(Renderer *renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3,
			    float y3, float x4, float y4, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	DataWin *dw = renderer->dataWin;
	TexturePageItem *tpag;
	u8 a = unorm(alpha);

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	tpag = &dw->tpag.items[tpagIndex];
	gx_draw_tpag(gx, tpagIndex, x1, y1, x2, y2, x3, y3, x4, y4,
		     0, 0, (float)tpag->sourceWidth, (float)tpag->sourceHeight,
		     255, 255, 255, a, 255, 255, 255, a, 255, 255, 255, a, 255, 255, 255, a);
}

static void gxDrawRectangleColor(Renderer *renderer, float x1, float y1, float x2, float y2, uint32_t c1, uint32_t c2,
				 uint32_t c3, uint32_t c4, float alpha, bool outline)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	u8 a = unorm(alpha);

	gx_unbind_tex(gx);
	if (outline) {
		gxDrawRectangleColor(renderer, x1, y1, x2, y1 + 1, c1, c2, c2, c1, alpha, false);
		gxDrawRectangleColor(renderer, x1, y2, x2, y2 + 1, c4, c3, c3, c4, alpha, false);
		gxDrawRectangleColor(renderer, x1, y1, x1 + 1, y2, c1, c1, c4, c4, alpha, false);
		gxDrawRectangleColor(renderer, x2, y1, x2 + 1, y2, c2, c2, c3, c3, alpha, false);
		return;
	}
	emit_quad(x1, y1, x2 + 1, y1, x2 + 1, y2 + 1, x1, y2 + 1,
		  0, 0, 1, 0, 1, 1, 0, 1,
		  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
		  (u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a,
		  (u8)BGR_R(c4), (u8)BGR_G(c4), (u8)BGR_B(c4), a);
}

static void gxDrawRectangle(Renderer *renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha,
			    bool outline)
{
	gxDrawRectangleColor(renderer, x1, y1, x2, y2, color, color, color, color, alpha, outline);
}

static void gxDrawLineColor(Renderer *renderer, float x1, float y1, float x2, float y2, float width, uint32_t c1,
			    uint32_t c2, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float dx = x2 - x1, dy = y2 - y1;
	float len = sqrtf(dx * dx + dy * dy);
	float half, px, py;
	u8 a = unorm(alpha);

	if (len < 0.0001f)
		return;
	if (width < 1.0f)
		width = 1.0f;
	half = width * 0.5f;
	px = (-dy / len) * half;
	py = (dx / len) * half;
	gx_unbind_tex(gx);
	emit_quad(x1 + px, y1 + py, x1 - px, y1 - py, x2 - px, y2 - py, x2 + px, y2 + py,
		  0, 0, 1, 0, 1, 1, 0, 1,
		  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
		  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a);
}

static void gxDrawLine(Renderer *renderer, float x1, float y1, float x2, float y2, float width, uint32_t color,
		       float alpha)
{
	gxDrawLineColor(renderer, x1, y1, x2, y2, width, color, color, alpha);
}

static void gxDrawTriangle(Renderer *renderer, float x1, float y1, float x2, float y2, float x3, float y3,
			   uint32_t c1, uint32_t c2, uint32_t c3, float alpha, bool outline)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	u8 a = unorm(alpha);

	if (outline) {
		gxDrawLineColor(renderer, x1, y1, x2, y2, 1.0f, c1, c2, alpha);
		gxDrawLineColor(renderer, x2, y2, x3, y3, 1.0f, c2, c3, alpha);
		gxDrawLineColor(renderer, x3, y3, x1, y1, 1.0f, c3, c1, alpha);
		return;
	}
	gx_unbind_tex(gx);
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
	GX_Position2f32(x1, y1);
	GX_Color4u8((u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x2, y2);
	GX_Color4u8((u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a);
	GX_TexCoord2f32(0, 0);
	GX_Position2f32(x3, y3);
	GX_Color4u8((u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a);
	GX_TexCoord2f32(0, 0);
	GX_End();
	if (gx_cur)
		gx_cur->targetDirty = true;
}

static void gxDrawTextColor(Renderer *renderer, const char *text, float x, float y, float xscale, float yscale,
			    float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha,
			    float lineSeparation)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	DataWin *dw = renderer->dataWin;
	Font *font;
	Sprite *spriteFont;
	TexturePageItem *tpag;
	GxPage *page;
	int32_t textLen, lineCount, lineStart, lineIdx;
	float lineStride, totalHeight, valignOffset, angleRad;
	Matrix4f transform;
	bool snap;
	float cursorY;
	u8 a = unorm(alpha);

	if (!text || renderer->drawFont < 0 || (uint32_t)renderer->drawFont >= dw->font.count)
		return;
	font = &dw->font.fonts[renderer->drawFont];
	spriteFont = NULL;
	tpag = NULL;
	page = NULL;
	if (font->isSpriteFont) {
		if (font->spriteIndex < 0 || (uint32_t)font->spriteIndex >= dw->sprt.count)
			return;
		spriteFont = &dw->sprt.sprites[font->spriteIndex];
	} else {
		if (font->tpagIndex < 0 || (uint32_t)font->tpagIndex >= dw->tpag.count)
			return;
		tpag = &dw->tpag.items[font->tpagIndex];
		page = gx_page(gx, tpag->texturePageId);
		if (!page)
			return;
		gx_bind_tex(gx, &page->obj);
	}
	textLen = (int32_t)strlen(text);
	if (textLen <= 0)
		return;
	lineCount = TextUtils_countLines(text, textLen);
	lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font)
					     : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
	totalHeight = (float)lineCount * lineStride;
	valignOffset = 0;
	if (renderer->drawValign == 1)
		valignOffset = -totalHeight / 2.0f;
	else if (renderer->drawValign == 2)
		valignOffset = -totalHeight;
	angleRad = -angleDeg * ((float)M_PI / 180.0f);
	Matrix4f_setTransform2D(&transform, x, y, xscale * font->scaleX, yscale * font->scaleY, angleRad);
	/* Centred odd-width lines start on a half pixel, where GX_NEAR samples sit exactly on texel
	 * seams and real hardware garbles thin glyphs. Keep unrotated 1:1 text on the pixel grid. */
	snap = angleDeg == 0.0f && xscale * font->scaleX == 1.0f && yscale * font->scaleY == 1.0f;
	cursorY = valignOffset - (float)font->ascenderOffset;
	lineStart = 0;
	for (lineIdx = 0; lineIdx < lineCount; lineIdx++) {
		int32_t lineEnd = lineStart;
		int32_t lineLen, pos;
		float lineWidth, halignOffset, cursorX;
		uint16_t ch = 0;
		bool hasCh;

		while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd]))
			lineEnd++;
		lineLen = lineEnd - lineStart;
		lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
		halignOffset = 0;
		if (renderer->drawHalign == 1)
			halignOffset = -lineWidth / 2.0f;
		else if (renderer->drawHalign == 2)
			halignOffset = -lineWidth;
		cursorX = halignOffset;
		pos = 0;
		hasCh = lineLen > pos;
		if (hasCh)
			ch = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
		while (hasCh) {
			FontGlyph *glyph = TextUtils_findGlyph(font, ch);
			uint16_t nextCh = 0;
			bool hasNext = lineLen > pos;

			if (hasNext)
				nextCh = TextUtils_decodeUtf8(text + lineStart, lineLen, &pos);
			if (glyph && glyph->sourceWidth && glyph->sourceHeight) {
				TexturePageItem *gtpag = tpag;
				GxPage *gpage = page;
				float u0, v0, u1, v1, lx0, ly0, lx1, ly1;
				float px0, py0, px1, py1, px2, py2, px3, py3;
				float leftFrac, rightFrac;
				int32_t tc1, tc2, tc3, tc4;
				bool ready = false;

				if (spriteFont) {
					int32_t gi = (int32_t)(glyph - font->glyphs);
					int32_t tpagIdx;

					if (gi >= 0 && gi < (int32_t)spriteFont->textureCount) {
						tpagIdx = spriteFont->tpagIndices[gi];
						if (tpagIdx >= 0 && (uint32_t)tpagIdx < dw->tpag.count) {
							gtpag = &dw->tpag.items[tpagIdx];
							gpage = gx_page(gx, gtpag->texturePageId);
							if (gpage) {
								gx_bind_tex(gx, &gpage->obj);
								u0 = (float)gtpag->sourceX / (float)gpage->texW;
								v0 = (float)gtpag->sourceY / (float)gpage->texH;
								u1 = (float)(gtpag->sourceX + gtpag->sourceWidth) / (float)gpage->texW;
								v1 = (float)(gtpag->sourceY + gtpag->sourceHeight) / (float)gpage->texH;
								lx0 = cursorX + (float)glyph->offset;
								ly0 = cursorY + (float)(int32_t)gtpag->targetY - (float)font->spriteOriginYAdjust;
								lx1 = lx0 + (float)gtpag->sourceWidth;
								ly1 = ly0 + (float)gtpag->sourceHeight;
								ready = true;
							}
						}
					}
				} else if (gtpag && gpage) {
					u0 = (float)(gtpag->sourceX + glyph->sourceX) / (float)gpage->texW;
					v0 = (float)(gtpag->sourceY + glyph->sourceY) / (float)gpage->texH;
					u1 = (float)(gtpag->sourceX + glyph->sourceX + glyph->sourceWidth) / (float)gpage->texW;
					v1 = (float)(gtpag->sourceY + glyph->sourceY + glyph->sourceHeight) / (float)gpage->texH;
					lx0 = cursorX + (float)glyph->offset;
					ly0 = cursorY;
					lx1 = lx0 + (float)glyph->sourceWidth;
					ly1 = ly0 + (float)glyph->sourceHeight;
					ready = true;
				}
				if (ready) {
					leftFrac = (lineWidth > 0.0f) ? (cursorX / lineWidth) : 0.0f;
					rightFrac = (lineWidth > 0.0f) ? ((cursorX + (float)glyph->shift) / lineWidth) : 1.0f;
					tc1 = Color_lerp(c1, c2, leftFrac);
					tc2 = Color_lerp(c1, c2, rightFrac);
					tc3 = Color_lerp(c4, c3, rightFrac);
					tc4 = Color_lerp(c4, c3, leftFrac);
					Matrix4f_transformPoint(&transform, lx0, ly0, &px0, &py0);
					Matrix4f_transformPoint(&transform, lx1, ly0, &px1, &py1);
					Matrix4f_transformPoint(&transform, lx1, ly1, &px2, &py2);
					Matrix4f_transformPoint(&transform, lx0, ly1, &px3, &py3);
					if (snap) {
						px0 = px3 = floorf(px0 + 0.5f);
						px1 = px2 = floorf(px1 + 0.5f);
						py0 = py1 = floorf(py0 + 0.5f);
						py2 = py3 = floorf(py2 + 0.5f);
					}
					emit_quad(px0, py0, px1, py1, px2, py2, px3, py3,
						  u0, v0, u1, v0, u1, v1, u0, v1,
						  (u8)BGR_R(tc1), (u8)BGR_G(tc1), (u8)BGR_B(tc1), a,
						  (u8)BGR_R(tc2), (u8)BGR_G(tc2), (u8)BGR_B(tc2), a,
						  (u8)BGR_R(tc3), (u8)BGR_G(tc3), (u8)BGR_B(tc3), a,
						  (u8)BGR_R(tc4), (u8)BGR_G(tc4), (u8)BGR_B(tc4), a);
				}
			}
			if (glyph) {
				cursorX += (float)glyph->shift;
				if (hasNext)
					cursorX += TextUtils_getKerningOffset(glyph, nextCh);
			}
			ch = nextCh;
			hasCh = hasNext;
		}
		cursorY += lineStride;
		lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
	}
}

static void gxDrawText(Renderer *renderer, const char *text, float x, float y, float xscale, float yscale,
		       float angleDeg, float lineSeparation)
{
	int32_t c = (int32_t)renderer->drawColor;

	gxDrawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c, c, c, c, renderer->drawAlpha, lineSeparation);
}

static void gxFlush(MAYBE_UNUSED Renderer *renderer) {}

static void gxClearScreen(Renderer *renderer, uint32_t color, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float w = (float)gx->base.CPortW;
	float h = (float)gx->base.CPortH;
	u8 a = unorm(alpha);
	u8 r = (u8)BGR_R(color), g = (u8)BGR_G(color), b = (u8)BGR_B(color);

	if (gx->currentTarget >= 0 && (uint32_t)gx->currentTarget < gx->surfCount &&
	    gx->surfs[gx->currentTarget].exists) {
		w = (float)gx->surfs[gx->currentTarget].w;
		h = (float)gx->surfs[gx->currentTarget].h;
	}
	if (w < 1.0f)
		w = (float)gx->gameW;
	if (h < 1.0f)
		h = (float)gx->gameH;
	gx_unbind_tex(gx);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	emit_quad(0, 0, w, 0, w, h, 0, h, 0, 0, 1, 0, 1, 1, 0, 1,
		  r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
	gx_apply_blend(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
}

static int32_t gxCreateSpriteFromSurface(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID,
					 MAYBE_UNUSED int32_t x, MAYBE_UNUSED int32_t y, MAYBE_UNUSED int32_t w,
					 MAYBE_UNUSED int32_t h, MAYBE_UNUSED bool removeback, MAYBE_UNUSED bool smooth,
					 MAYBE_UNUSED int32_t xorig, MAYBE_UNUSED int32_t yorig)
{
	return -1;
}

static void gxDeleteSprite(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t spriteIndex) {}

static BlendFactors gxGpuGetBlendFactors(Renderer *renderer)
{
	return ((GxRenderer *)renderer)->blendFactors;
}

static int32_t gxGpuGetBlendMode(Renderer *renderer)
{
	return ((GxRenderer *)renderer)->blendMode;
}

static void gxGpuSetBlendMode(Renderer *renderer, int32_t mode)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gx->blendMode = mode;
	if (mode == bm_max)
		logInfo("GX: bm_max (approx SRCALPHA, INVSRCCLR)\n");
	gx_apply_blend(gx);
}

static void gxGpuSetBlendModeExt(Renderer *renderer, int32_t sfactor, int32_t dfactor, int32_t sfactor_alpha,
				 int32_t dfactor_alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gx->blendMode = bm_complex;
	gx->blendFactors.src = sfactor;
	gx->blendFactors.dst = dfactor;
	gx->blendFactors.srcAlpha = sfactor_alpha;
	gx->blendFactors.dstAlpha = dfactor_alpha;
	gx_apply_blend(gx);
}

static void gxGpuSetBlendEnable(Renderer *renderer, bool enable)
{
	((GxRenderer *)renderer)->blendEnable = enable;
	gx_apply_blend((GxRenderer *)renderer);
}

static void gxGpuSetAlphaTestEnable(Renderer *renderer, bool enable)
{
	((GxRenderer *)renderer)->alphaTestEnable = enable;
	gx_apply_alpha_test((GxRenderer *)renderer);
}

static bool gxGpuGetAlphaTestEnable(Renderer *renderer)
{
	return ((GxRenderer *)renderer)->alphaTestEnable;
}

static void gxGpuSetAlphaTestRef(Renderer *renderer, uint8_t ref)
{
	((GxRenderer *)renderer)->alphaTestRef = ref;
	gx_apply_alpha_test((GxRenderer *)renderer);
}

static void gxGpuSetColorWriteEnable(Renderer *renderer, bool red, bool green, bool blue, bool alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gx->colorWriteR = red;
	gx->colorWriteG = green;
	gx->colorWriteB = blue;
	gx->colorWriteA = alpha;
	GX_SetColorUpdate(red || green || blue);
	GX_SetAlphaUpdate(alpha);
}

static void gxGpuGetColorWriteEnable(Renderer *renderer, bool *red, bool *green, bool *blue, bool *alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (red)
		*red = gx->colorWriteR;
	if (green)
		*green = gx->colorWriteG;
	if (blue)
		*blue = gx->colorWriteB;
	if (alpha)
		*alpha = gx->colorWriteA;
}

static bool gxGpuGetBlendEnable(Renderer *renderer)
{
	return ((GxRenderer *)renderer)->blendEnable;
}

static void gxGpuSetFog(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED bool enable, MAYBE_UNUSED uint32_t color) {}

static void gxDrawSpriteTiled(Renderer *renderer, int32_t tpagIndex, float originX, float originY, float x, float y,
			      float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH,
			      uint32_t color, float alpha)
{
	DataWin *dw = renderer->dataWin;
	TexturePageItem *tpag;
	float ax = fabsf(xscale), ay = fabsf(yscale);
	float tileW, tileH, startX, endX, startY, endY, cx, cy;

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	tpag = &dw->tpag.items[tpagIndex];
	tileW = (float)tpag->boundingWidth * ax;
	tileH = (float)tpag->boundingHeight * ay;
	if (tileW <= 0 || tileH <= 0)
		return;
	if (tileX) {
		startX = fmodf(x - originX * ax, tileW);
		if (startX > 0)
			startX -= tileW;
		endX = roomW;
	} else {
		startX = x - originX * ax;
		endX = startX + tileW;
	}
	if (tileY) {
		startY = fmodf(y - originY * ay, tileH);
		if (startY > 0)
			startY -= tileH;
		endY = roomH;
	} else {
		startY = y - originY * ay;
		endY = startY + tileH;
	}
	for (cy = startY; cy < endY; cy += tileH) {
		for (cx = startX; cx < endX; cx += tileW)
			gxDrawSprite(renderer, tpagIndex, cx + originX * xscale, cy + originY * yscale, originX, originY,
				     xscale, yscale, 0, color, alpha);
	}
}

static int32_t gxCreateSurface(Renderer *renderer, int32_t width, int32_t height)
{
	return gx_alloc_surface((GxRenderer *)renderer, width, height);
}

static bool gxSurfaceExists(Renderer *renderer, int32_t surfaceID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return false;
	return gx->surfs[surfaceID].exists;
}

static bool gxSetRenderTarget(Renderer *renderer, int32_t surfaceID, bool implicitApplicationSurface)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t viewCurrent = 0;
	RuntimeView *view;
	GMLCamera *camera;

	if (surfaceID == RENDER_TARGET_HOST_FRAMEBUFFER || surfaceID == APPLICATION_SURFACE_ID) {
		gx_bind_host(gx);
		return true;
	}
	if (!gx_bind_surface(gx, surfaceID, true))
		return false;
	if (renderer->runner->viewsEnabled)
		viewCurrent = renderer->runner->viewCurrent;
	view = &renderer->runner->views[viewCurrent];
	gx->base.cameraCurrent = view->cameraId;
	camera = Runner_getCameraById(renderer->runner, gx->base.cameraCurrent);
	if (surfaceID == renderer->runner->applicationSurfaceId && implicitApplicationSurface) {
		/* oCharacter set_target/reset mid-view; keep the camera ortho, not 0..gameW. */
		if (gx->viewActive) {
			gx_viewport(gx->viewPortX, gx->viewPortY, gx->viewPortW, gx->viewPortH);
			gx->base.CPortX = gx->viewPortX;
			gx->base.CPortY = gx->viewPortY;
			gx->base.CPortW = gx->viewPortW;
			gx->base.CPortH = gx->viewPortH;
			gx_ortho((float)gx->viewY, (float)(gx->viewY + gx->viewH),
				 (float)gx->viewX, (float)(gx->viewX + gx->viewW));
		} else {
			gx_viewport(0, 0, gx->gameW, gx->gameH);
			gx_ortho(0, (float)gx->gameH, 0, (float)gx->gameW);
		}
		if (camera)
			gxApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
		return true;
	}
	if (camera)
		gxApplyProjection(renderer, &camera->viewMatrix, &camera->projectionMatrix);
	return true;
}

static int32_t gxEnsureApplicationSurface(Renderer *renderer, int32_t width, int32_t height)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t id = gx->appSurfId;

	if (id >= 0 && (uint32_t)id < gx->surfCount && gx->surfs[id].exists &&
	    gx->surfs[id].w == width && gx->surfs[id].h == height) {
		renderer->runner->applicationSurfaceId = id;
		return id;
	}
	if (id >= 0 && (uint32_t)id < gx->surfCount && gx->surfs[id].exists) {
		gxSurfaceResize(renderer, id, width, height);
		renderer->runner->applicationSurfaceId = id;
		return id;
	}
	id = gx_alloc_surface(gx, width, height);
	gx->appSurfId = id;
	renderer->runner->applicationSurfaceId = id;
	return id;
}

static float gxGetSurfaceWidth(Renderer *renderer, int32_t surfaceID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount || !gx->surfs[surfaceID].exists)
		return 0.0f;
	return (float)gx->surfs[surfaceID].w;
}

static float gxGetSurfaceHeight(Renderer *renderer, int32_t surfaceID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount || !gx->surfs[surfaceID].exists)
		return 0.0f;
	return (float)gx->surfs[surfaceID].h;
}

static void gxDrawSurfaceColor(Renderer *renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
			       int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg,
			       uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GxSurf *s;
	float u0, v0, u1, v1;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	float angleRad;
	Matrix4f transform;
	u8 a = unorm(alpha);

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return;
	s = &gx->surfs[surfaceID];
	if (!s->exists)
		return;
	if (gx->currentTarget == surfaceID)
		gx_flush_target(gx);
	if (srcWidth <= 0)
		srcWidth = s->w;
	if (srcHeight <= 0)
		srcHeight = s->h;
	angleRad = -angleDeg * ((float)M_PI / 180.0f);
	Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);
	if (s->tiles) {
		int col, row;
		static int atlasBlitLog;

		if (atlasBlitLog < 4) {
			logInfo("GX: draw tiled surface %d %dx%d (%dx%d tiles) at %.0f,%.0f\n",
				surfaceID, s->w, s->h, s->tilesX, s->tilesY, x, y);
			atlasBlitLog++;
		}
		for (row = 0; row < s->tilesY; row++) {
			for (col = 0; col < s->tilesX; col++) {
				GxTile *t = gx_tile_at(s, col, row);
				int ox, oy, tw, th;
				int ix0, iy0, ix1, iy1;
				float lx0, ly0, lx1, ly1;

				if (!t->texels)
					continue;
				gx_tile_rect(s, col, row, &ox, &oy, &tw, &th);
				ix0 = ox > srcLeft ? ox : srcLeft;
				iy0 = oy > srcTop ? oy : srcTop;
				ix1 = ox + tw < srcLeft + srcWidth ? ox + tw : srcLeft + srcWidth;
				iy1 = oy + th < srcTop + srcHeight ? oy + th : srcTop + srcHeight;
				if (ix0 >= ix1 || iy0 >= iy1)
					continue;
				lx0 = (float)(ix0 - srcLeft);
				ly0 = (float)(iy0 - srcTop);
				lx1 = (float)(ix1 - srcLeft);
				ly1 = (float)(iy1 - srcTop);
				u0 = (float)(ix0 - ox) / (float)t->texW;
				v0 = (float)(iy0 - oy) / (float)t->texH;
				u1 = (float)(ix1 - ox) / (float)t->texW;
				v1 = (float)(iy1 - oy) / (float)t->texH;
				Matrix4f_transformPoint(&transform, lx0, ly0, &x0, &y0);
				Matrix4f_transformPoint(&transform, lx1, ly0, &x1, &y1);
				Matrix4f_transformPoint(&transform, lx1, ly1, &x2, &y2);
				Matrix4f_transformPoint(&transform, lx0, ly1, &x3, &y3);
				gx_prep_tile_obj(t);
				gx_bind_tex(gx, &t->obj);
				emit_quad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v0, u1, v1, u0, v1,
					  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
					  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
					  (u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a,
					  (u8)BGR_R(c4), (u8)BGR_G(c4), (u8)BGR_B(c4), a);
			}
		}
		return;
	}
	if (!s->texels)
		return;
	gx_prep_surf_obj(s);
	u0 = (float)srcLeft / (float)s->texW;
	v0 = (float)srcTop / (float)s->texH;
	u1 = (float)(srcLeft + srcWidth) / (float)s->texW;
	v1 = (float)(srcTop + srcHeight) / (float)s->texH;
	Matrix4f_transformPoint(&transform, 0, 0, &x0, &y0);
	Matrix4f_transformPoint(&transform, (float)srcWidth, 0, &x1, &y1);
	Matrix4f_transformPoint(&transform, (float)srcWidth, (float)srcHeight, &x2, &y2);
	Matrix4f_transformPoint(&transform, 0, (float)srcHeight, &x3, &y3);
	gx_bind_tex(gx, &s->obj);
	emit_quad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v0, u1, v1, u0, v1,
		  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
		  (u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a,
		  (u8)BGR_R(c4), (u8)BGR_G(c4), (u8)BGR_B(c4), a);
}

static void gxDrawSurface(Renderer *renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
			  int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg,
			  uint32_t color, float alpha)
{
	gxDrawSurfaceColor(renderer, surfaceID, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg,
			   color, color, color, color, alpha);
}

static void gxDrawSurfaceTiled(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED float x,
			       MAYBE_UNUSED float y, MAYBE_UNUSED float xscale, MAYBE_UNUSED float yscale,
			       MAYBE_UNUSED float roomW, MAYBE_UNUSED float roomH, MAYBE_UNUSED uint32_t color,
			       MAYBE_UNUSED float alpha)
{
}

static void gxSurfaceResize(Renderer *renderer, int32_t surfaceID, int32_t width, int32_t height)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GxSurf *s;
	bool tiled;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount || !gx->surfs[surfaceID].exists)
		return;
	if (gx->surfs[surfaceID].w == width && gx->surfs[surfaceID].h == height)
		return;
	if (width < 1)
		width = 1;
	if (height < 1)
		height = 1;
	s = &gx->surfs[surfaceID];
	gx_surf_release(s);
	s->w = width;
	s->h = height;
	s->objReady = false;
	tiled = (width > EFB_MAX_W || height > EFB_MAX_H);
	if (tiled) {
		if (!gx_alloc_tiles(s, width, height))
			s->exists = false;
		return;
	}
	{
		int tw = pad4(width);
		int th = pad4(height);
		u16 *texels = memalign(32, surf_bytes(tw, th));

		if (!texels) {
			s->exists = false;
			return;
		}
		memset(texels, 0, surf_bytes(tw, th));
		DCFlushRange(texels, surf_bytes(tw, th));
		s->texW = tw;
		s->texH = th;
		s->texels = texels;
	}
}

static void gxSurfaceFree(Renderer *renderer, int32_t surfaceID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return;
	if (surfaceID == renderer->runner->applicationSurfaceId)
		return;
	gx_surf_release(&gx->surfs[surfaceID]);
	memset(&gx->surfs[surfaceID], 0, sizeof(GxSurf));
}

static void gxSurfaceCopy(Renderer *renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID,
			  int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GxSurf *src, *dest;
	int32_t saved = gx->currentTarget;
	u32 nbytes;
	static int copyLog;

	if (srcSurfaceID < 0 || destSurfaceID < 0)
		return;
	if ((uint32_t)srcSurfaceID >= gx->surfCount || (uint32_t)destSurfaceID >= gx->surfCount)
		return;
	src = &gx->surfs[srcSurfaceID];
	dest = &gx->surfs[destSurfaceID];
	if (!src->exists || !dest->exists)
		return;
	if ((!src->texels && !src->tiles) || (!dest->texels && !dest->tiles))
		return;

	/* Step-time copy (water) runs after beginFrame rebound the app surface without
	 * restore. Flushing then would stamp the host 2× blit over last view's texels. */
	if (gx->currentTarget == srcSurfaceID && gx->targetDirty)
		gx_flush_target(gx);

	if (!part && destX == 0 && destY == 0 && src->w == dest->w && src->h == dest->h &&
	    src->texels && dest->texels && src->texW == dest->texW && src->texH == dest->texH) {
		nbytes = surf_bytes(src->texW, src->texH);
		memcpy(dest->texels, src->texels, nbytes);
		DCFlushRange(dest->texels, nbytes);
		dest->objReady = false;
		GX_InvalidateTexAll();
		if (copyLog < 8) {
			logInfo("GX: surface_copy memcpy %d->%d %dx%d\n", srcSurfaceID, destSurfaceID, src->w, src->h);
			copyLog++;
		}
		return;
	}

	if (!gx_bind_surface(gx, destSurfaceID, destX != 0 || destY != 0 || part))
		return;
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	gxDrawSurface(renderer, srcSurfaceID, part ? srcX : 0, part ? srcY : 0, part ? srcW : src->w,
		      part ? srcH : src->h, (float)destX, (float)destY, 1.0f, 1.0f, 0, 0xFFFFFF, 1.0f);
	gx_apply_blend(gx);
	gx_flush_target(gx);
	if (copyLog < 8) {
		logInfo("GX: surface_copy efb %d->%d\n", srcSurfaceID, destSurfaceID);
		copyLog++;
	}
	if (saved == HOST_TARGET)
		gx_bind_host(gx);
	else if (saved >= 0)
		gx_bind_surface(gx, saved, true);
}

static bool gxSurfaceGetPixels(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID, MAYBE_UNUSED uint8_t *outRGBA)
{
	return false;
}

static void gxDrawTiledPart(Renderer *renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
			    float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha)
{
	float x, y;

	if (srcW <= 0 || srcH <= 0)
		return;
	for (y = dstY; y < dstY + dstH; y += (float)srcH) {
		for (x = dstX; x < dstX + dstW; x += (float)srcW) {
			int32_t w = srcW;
			int32_t h = srcH;

			if (x + w > dstX + dstW)
				w = (int32_t)(dstX + dstW - x);
			if (y + h > dstY + dstH)
				h = (int32_t)(dstY + dstH - y);
			if (w > 0 && h > 0)
				gxDrawSpritePart(renderer, tpagIndex, srcX, srcY, w, h, x, y, 1, 1, 0, x, y, color, alpha);
		}
	}
}

static void gxGpuSetShader(Renderer *renderer, int32_t shaderIndex)
{
	renderer->currentShader = shaderIndex;
}

static void gxGpuResetShader(Renderer *renderer)
{
	renderer->currentShader = -1;
}

static int32_t gxShaderGetUniform(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shaderIndex, MAYBE_UNUSED char *uniform)
{
	return -1;
}

static int32_t gxShaderGetSamplerIndex(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shaderIndex,
				       MAYBE_UNUSED char *uniform)
{
	return -1;
}

static void gxShaderSetUniformF(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count,
				MAYBE_UNUSED float v1, MAYBE_UNUSED float v2, MAYBE_UNUSED float v3, MAYBE_UNUSED float v4)
{
}

static void gxShaderSetUniformFArray(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle,
				     MAYBE_UNUSED float *values, MAYBE_UNUSED uint32_t count)
{
}

static void gxShaderSetUniformI(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle, MAYBE_UNUSED int32_t count,
				MAYBE_UNUSED int32_t v1, MAYBE_UNUSED int32_t v2, MAYBE_UNUSED int32_t v3,
				MAYBE_UNUSED int32_t v4)
{
}

static uint32_t gxSpriteGetTexture(Renderer *renderer, int32_t tpagIndex)
{
	DataWin *dw = renderer->dataWin;

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return 0;
	return (uint32_t)(tpagIndex + 1);
}

static uint32_t gxSurfaceGetTexture(MAYBE_UNUSED Renderer *renderer, int32_t surfaceID)
{
	if (surfaceID < 0)
		return 0;
	return (uint32_t)(surfaceID + 0x10000);
}

static float gxTextureGetTexelWidth(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID)
{
	return 1.0f;
}

static float gxTextureGetTexelHeight(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID)
{
	return 1.0f;
}

static bool gxTextureGetUVs(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED uint32_t texID, MAYBE_UNUSED float *outUVs)
{
	return false;
}

static void gxTextureSetStage(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t slot, MAYBE_UNUSED uint32_t texID) {}

static bool gxShaderIsCompiled(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shader)
{
	return false;
}

static bool gxShadersSupported(void)
{
	return false;
}

static void gxSetMatrix(Renderer *renderer, int32_t matrixType, Matrix4f matrix)
{
	if (matrixType >= 0 && matrixType < MATRICES_MAX)
		renderer->gmlMatrices[matrixType] = matrix;
}

static void gxPrimEnsure(GxRenderer *gx, int32_t needed)
{
	if (needed <= gx->primCap)
		return;
	gx->primCap = gx->primCap ? gx->primCap * 2 : 32;
	while (gx->primCap < needed)
		gx->primCap *= 2;
	gx->primVerts = (GxVert *)safeRealloc(gx->primVerts, (size_t)gx->primCap * sizeof(GxVert));
}

static void gxPrimitiveBegin(Renderer *renderer, int32_t primitiveType)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gx->primType = primitiveType;
	gx->primCount = 0;
	gx->primTextured = false;
	gx->primTpag = -1;
}

static void gxPrimitiveBeginTexture(Renderer *renderer, int32_t primitiveType, int32_t texture)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gxPrimitiveBegin(renderer, primitiveType);
	gx->primTextured = true;
	gx->primTpag = texture - 1;
}

static void gxDrawVertex(Renderer *renderer, float x, float y, MAYBE_UNUSED float z, uint32_t color, float alpha,
			 float u, float v)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GxVert *vt;

	gxPrimEnsure(gx, gx->primCount + 1);
	vt = &gx->primVerts[gx->primCount++];
	vt->x = x;
	vt->y = y;
	vt->u = u;
	vt->v = v;
	vt->r = (u8)BGR_R(color);
	vt->g = (u8)BGR_G(color);
	vt->b = (u8)BGR_B(color);
	vt->a = unorm(alpha);
}

static u8 gx_prim_kind(int32_t t)
{
	switch (t) {
	case PRIMITIVE_POINTS:
		return GX_POINTS;
	case PRIMITIVE_LINES:
		return GX_LINES;
	case PRIMITIVE_LINE_STRIP:
		return GX_LINESTRIP;
	case PRIMITIVE_TRIANGLE_STRIP:
		return GX_TRIANGLESTRIP;
	case PRIMITIVE_TRIANGLE_FAN:
		return GX_TRIANGLEFAN;
	default:
		return GX_TRIANGLES;
	}
}

static void gxPrimitiveEnd(Renderer *renderer)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t i;

	if (gx->primCount <= 0)
		return;
	if (gx->primTextured && gx->primTpag >= 0) {
		DataWin *dw = renderer->dataWin;
		if ((uint32_t)gx->primTpag < dw->tpag.count) {
			GxPage *page = gx_page(gx, dw->tpag.items[gx->primTpag].texturePageId);
			if (page)
				gx_bind_tex(gx, &page->obj);
			else
				gx_unbind_tex(gx);
		}
	} else {
		gx_unbind_tex(gx);
	}
	GX_Begin(gx_prim_kind(gx->primType), GX_VTXFMT0, (u16)gx->primCount);
	for (i = 0; i < gx->primCount; i++) {
		GxVert *vt = &gx->primVerts[i];
		GX_Position2f32(vt->x, vt->y);
		GX_Color4u8(vt->r, vt->g, vt->b, vt->a);
		GX_TexCoord2f32(vt->u, vt->v);
	}
	GX_End();
	gx->primCount = 0;
}

static void gxDrawVertexBuffer(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED VertexBuffer *buffer,
			       MAYBE_UNUSED int32_t primitive, MAYBE_UNUSED int32_t texture, MAYBE_UNUSED int32_t offset,
			       MAYBE_UNUSED int32_t count)
{
}

Renderer *GxRenderer_create(void)
{
	GxRenderer *gx = (GxRenderer *)safeCalloc(1, sizeof(GxRenderer));

	gx->base.vtable = &gxVtable;
	gxVtable.init = gxInit;
	gxVtable.destroy = gxDestroy;
	gxVtable.beginFrame = gxBeginFrame;
	gxVtable.endFrameInit = gxEndFrameInit;
	gxVtable.endFrameEnd = gxEndFrameEnd;
	gxVtable.beginView = gxBeginView;
	gxVtable.endView = gxEndView;
	gxVtable.applyProjection = gxApplyProjection;
	gxVtable.beginGUI = gxBeginGUI;
	gxVtable.setGuiProjection = gxSetGuiProjection;
	gxVtable.endGUI = gxEndGUI;
	gxVtable.drawSprite = gxDrawSprite;
	gxVtable.drawSpritePart = gxDrawSpritePart;
	gxVtable.drawSpritePartColor = gxDrawSpritePartColor;
	gxVtable.drawSpritePos = gxDrawSpritePos;
	gxVtable.drawRectangle = gxDrawRectangle;
	gxVtable.drawRectangleColor = gxDrawRectangleColor;
	gxVtable.drawLine = gxDrawLine;
	gxVtable.drawTriangle = gxDrawTriangle;
	gxVtable.drawLineColor = gxDrawLineColor;
	gxVtable.drawText = gxDrawText;
	gxVtable.drawTextColor = gxDrawTextColor;
	gxVtable.primitiveBegin = gxPrimitiveBegin;
	gxVtable.primitiveBeginTexture = gxPrimitiveBeginTexture;
	gxVtable.primitiveEnd = gxPrimitiveEnd;
	gxVtable.drawVertex = gxDrawVertex;
	gxVtable.drawVertexBuffer = gxDrawVertexBuffer;
	gxVtable.flush = gxFlush;
	gxVtable.clearScreen = gxClearScreen;
	gxVtable.createSpriteFromSurface = gxCreateSpriteFromSurface;
	gxVtable.deleteSprite = gxDeleteSprite;
	gxVtable.gpuGetBlendFactors = gxGpuGetBlendFactors;
	gxVtable.gpuGetBlendMode = gxGpuGetBlendMode;
	gxVtable.gpuSetBlendMode = gxGpuSetBlendMode;
	gxVtable.gpuSetBlendModeExt = gxGpuSetBlendModeExt;
	gxVtable.gpuSetBlendEnable = gxGpuSetBlendEnable;
	gxVtable.gpuSetAlphaTestEnable = gxGpuSetAlphaTestEnable;
	gxVtable.gpuGetAlphaTestEnable = gxGpuGetAlphaTestEnable;
	gxVtable.gpuSetAlphaTestRef = gxGpuSetAlphaTestRef;
	gxVtable.gpuSetColorWriteEnable = gxGpuSetColorWriteEnable;
	gxVtable.gpuGetColorWriteEnable = gxGpuGetColorWriteEnable;
	gxVtable.gpuGetBlendEnable = gxGpuGetBlendEnable;
	gxVtable.gpuSetFog = gxGpuSetFog;
	gxVtable.drawTile = NULL;
	gxVtable.drawSpriteTiled = gxDrawSpriteTiled;
	gxVtable.createSurface = gxCreateSurface;
	gxVtable.surfaceExists = gxSurfaceExists;
	gxVtable.setRenderTarget = gxSetRenderTarget;
	gxVtable.ensureApplicationSurface = gxEnsureApplicationSurface;
	gxVtable.getSurfaceWidth = gxGetSurfaceWidth;
	gxVtable.getSurfaceHeight = gxGetSurfaceHeight;
	gxVtable.drawSurface = gxDrawSurface;
	gxVtable.drawSurfaceColor = gxDrawSurfaceColor;
	gxVtable.drawSurfaceTiled = gxDrawSurfaceTiled;
	gxVtable.surfaceResize = gxSurfaceResize;
	gxVtable.surfaceFree = gxSurfaceFree;
	gxVtable.surfaceCopy = gxSurfaceCopy;
	gxVtable.surfaceGetPixels = gxSurfaceGetPixels;
	gxVtable.drawTiledPart = gxDrawTiledPart;
	gxVtable.gpuSetShader = gxGpuSetShader;
	gxVtable.gpuResetShader = gxGpuResetShader;
	gxVtable.shaderGetUniform = gxShaderGetUniform;
	gxVtable.shaderGetSamplerIndex = gxShaderGetSamplerIndex;
	gxVtable.shaderSetUniformF = gxShaderSetUniformF;
	gxVtable.shaderSetUniformFArray = gxShaderSetUniformFArray;
	gxVtable.shaderSetUniformI = gxShaderSetUniformI;
	gxVtable.spriteGetTexture = gxSpriteGetTexture;
	gxVtable.surfaceGetTexture = gxSurfaceGetTexture;
	gxVtable.textureGetTexelWidth = gxTextureGetTexelWidth;
	gxVtable.textureGetTexelHeight = gxTextureGetTexelHeight;
	gxVtable.textureGetUVs = gxTextureGetUVs;
	gxVtable.textureSetStage = gxTextureSetStage;
	gxVtable.shaderIsCompiled = gxShaderIsCompiled;
	gxVtable.shadersSupported = gxShadersSupported;
	gxVtable.setMatrix = gxSetMatrix;
	return (Renderer *)gx;
}
