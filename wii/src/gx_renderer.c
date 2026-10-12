#include "gx_renderer.h"
#include "overscan.h"
#include "page_loader.h"
#include "png_stream.h"
#include "am2r.h"
#include "controls.h"
#include "hooks.h"
#include "input.h"
#include "safearea.h"
#include "wii_mem.h"
#include "wii_paths.h"
#include "wii_log.h"
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
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <ogc/lwp_watchdog.h>

#define MEM_RESERVE (4u << 20) /* headroom kept free for room loads */
#define LOSE_SURFACES_FRAMES 5 /* comfortably past the game's 2-frame init */
#define HEAP_RESERVE (14u << 20) /* what malloc keeps of MEM2 once the game data is loaded */
/* What one page load needs: its PNG blob plus 2 MB of texels (the decode streams row by row). */
#define PAGE_LOAD_NEED (4u << 20)
#define PAGE_RETRY_FRAMES 300
/* AM2R orphans a surface on nearly every room change (water never frees; others free only in
 * Destroy, which room end doesn't fire). GM surfaces are volatile, so idle ones are fair game. */
#define SURF_IDLE_FRAMES 600
#define SURF_RECLAIM_BELOW (12u << 20)
#define HOST_TARGET (-1)
/* Surfaces larger than the EFB are rasterized on the CPU in 512×256 tiles (s_map: 4×4). */
#define SURF_TILE_W 512
#define SURF_TILE_H 256
/* RGB565 has no alpha; gui_surface needs draw_clear_alpha(0,0) to stay transparent. */
#define SURF_TEX_FMT GX_TF_RGBA8
#define SURF_BPP 4
/* A 4:3 game frame on a 16:9 console: 480 wide, centered, instead of stretched. */
static bool gx_pillarbox;

typedef struct {
	float x, y, u, v;
	u8 r, g, b, a;
} GxVert;

typedef struct {
	bool loaded;
	bool failed;
	bool pinned; /* a runtime image (sprite_add), not a TXTR page: never evicted or reloaded */
	u64 retryAt; /* a failed load is retried after a cooldown, not every frame */
	int32_t texW, texH;
	u16 *texels;
	GXTexObj obj;
	u64 lastUsed;
	bool loading; /* out on the loader thread */
	int32_t pngW, pngH; /* known once read; pngW < 0: never prefetched */
} GxPage;

typedef struct {
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
	u64 lastUsed;
	bool retired; /* reclaimed by us: the id is never handed out again, a stale holder draws nothing */
	int32_t sharer; /* the surface these texels are also lent to or borrowed from;
			   0 = none (slot 0 is never used) */
	u16 *spare;     /* a borrower's own buffer, idle until it or its lender is written */
} GxSurf;

typedef struct {
	Renderer base;
	int32_t gameW, gameH;

	GxPage *pages;
	uint32_t pageCount;
	u32 pagesResident;
	u64 frameCounter;

	GxSurf *surfs;
	uint32_t surfCount;
	int32_t currentTarget;
	bool targetBound;
	GXTexObj *boundTex;
	GxPage *boundPage;

	bool blendEnable;
	int32_t blendMode;
	BlendFactors blendFactors;
	bool alphaTestEnable;
	u8 alphaTestRef;
	bool colorWriteR, colorWriteG, colorWriteB, colorWriteA;
	bool texBound;
	bool skipView; /* wide mode: view 0 lands on the app surface, which the game never shows */

	GxVert *primVerts;
	int32_t primCount;
	int32_t primCap;
	int32_t primType;
	int32_t primTpag;
	bool primTextured;
	int32_t appSurfId;
	bool viewActive;
	int32_t viewSurf; /* surface the current view draws into (1.5.5 widescreen), -1 = the usual target */
	bool viewSnapped;
	int32_t viewX, viewY, viewW, viewH;
	int32_t viewPortX, viewPortY, viewPortW, viewPortH;
	bool targetDirty;
	/* Wide mode composes the frame pixel-exact at 426×480 and presents it 3:2 through a
	 * box filter, instead of the game's blit landing at 1.5 screen pixels per game pixel. */
	bool compose;
	int32_t composeId;
	int32_t guiSurf; /* oControl's HUD layer, drawn inside the HUD margins */
	int32_t shPal, shGB; /* SHDR indices of the two shaders we do natively, -1 if absent */
	int32_t palTpag; /* palette sprite bound with texture_set_stage while shPal is set */
	int palIndex; /* column of the palette to replace with (the game always passes 1) */
	u8 gbColor[4][3]; /* shGameBoy ramp, darkest first, as the game's u_color1..4 (0..255) */
	u16 *gbTexels; /* 4×4 RGB5A3 holding ramp color 2: TEV needs it as a texture input */
} GxRenderer;

/* shPaletteSwap: the GLSL searches the palette's first column for the source color and takes the
 * same row from column palette_index. TEV can't search, so each swapped sprite is recolored once
 * on the CPU (exact match on the 16-bit texel, as both are quantised alike) and cached. */
#define SWAP_MAX 256
#define SWAP_BYTES_MAX (2u << 20)
typedef struct {
	int32_t tpag, palTpag;
	int palIndex;
	int32_t texW, texH;
	u16 *texels;
	GXTexObj obj;
	u64 lastUsed;
} GxSwap;
static GxSwap gx_swaps[SWAP_MAX];
static int gx_swapCount;
static u32 gx_swapBytes;

static RendererVtable gxVtable;
static GxRenderer *gx_cur;
static bool gx_wide_filter = true; /* [Screen] WiiWideFilter; 0 = the plain 2,1,2,1 stretch */

/* Per-frame accounting for the hardware profile: what the draw phase costs outside GML scripts. */
static struct {
	u32 quads, tiles, sprites, copies, binds, targets, skipped, culled, n;
	double area; /* target pixels covered by the quads: fill, the thing a GPU-bound room pays for */
	u64 emitUs, copyUs, drawUs, otherUs, frameStart, lastEnd;
} gx_stat;

void GxRenderer_setWideFilter(bool on)
{
	gx_wide_filter = on;
}

void GxRenderer_setPillarbox(bool on)
{
	if (on != gx_pillarbox)
		logInfo("GX: pillarbox %s\n", on ? "on" : "off");
	gx_pillarbox = on;
}
static void gxSurfaceResize(Renderer *renderer, int32_t surfaceID, int32_t width, int32_t height);

static int pad4(int v)
{
	return (v + 3) & ~3;
}

static u32 tex_bytes(int tw, int th)
{
	return (u32)tw * (u32)th * sizeof(u16);
}

/* RGB5A3 pages are 4x4-texel tiles of 32 bytes. */
static int gx_texel_off(int texW, int x, int y)
{
	return ((y >> 2) * (texW >> 2) + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3);
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
	off = gx_texel_off(p->texW, x, y);
	unpack_rgb5a3(p->texels[off], r, g, b, a);
}

static void swizzle_rgb5a3(u16 *dst, const u8 *rgba, int w, int h, int tw)
{
	int x, y;

	memset(dst, 0, tex_bytes(tw, pad4(h)));
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			const u8 *p = rgba + ((y * w + x) * 4);
			int off = gx_texel_off(tw, x, y);

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

/* The projection in force, in the units quads are drawn in: what the rasterizer keeps. */
static float gx_clipT = -1e9f, gx_clipB = 1e9f, gx_clipL = -1e9f, gx_clipR = 1e9f;

/* The Display menu's adjust pages draw over everything: none of the game's safe-area moves apply to them. */
static bool gx_ov_drawing;

static void gx_ortho(float top, float bottom, float left, float right)
{
	Mtx mv;
	Mtx44 proj;

	gx_clipT = top;
	gx_clipB = bottom;
	gx_clipL = left;
	gx_clipR = right;
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
	if (gx_cur)
		gx_cur->texBound = false;
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
	if (gx->texBound && gx->boundTex == obj)
		return;
	gx_stat.binds++;
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

/* emit_quad's CPU raster for an axis-aligned quad of one color: u then depends on x alone and v on y
 * alone, so the same interpolation runs once per column and once per row instead of per pixel. */
static void raster_flat(GxTile *t, GxPage *pg, int ox, int oy, int x0i, int y0i, int x1i, int y1i,
			float minx, float miny, float dx, float dy, float u0, float u1, float v0, float v3,
			u8 cr, u8 cg, u8 cb, u8 ca)
{
	static int colSrc[SURF_TILE_W], colDst[SURF_TILE_W];
	u8 *dst = (u8 *)t->texels;
	int y, i, n = x1i - x0i;
	bool white = cr == 255 && cg == 255 && cb == 255 && ca == 255;

	if (pg && !pg->texels)
		pg = NULL;
	if (!pg && ca == 0)
		return;
	for (i = 0; i < n; i++) {
		int lx = x0i + i - ox;

		colDst[i] = (lx >> 2) * 64 + (lx & 3) * 2;
		if (pg) {
			float px = ((float)(x0i + i) + 0.5f - minx) / dx;
			int tx = (int)(mixf(u0, u1, px) * (float)pg->texW);

			if (tx < 0)
				tx = 0;
			if (tx >= pg->texW)
				tx = pg->texW - 1;
			colSrc[i] = (tx >> 2) * 16 + (tx & 3);
		}
	}
	for (y = y0i; y < y1i; y++) {
		int ly = y - oy;
		u8 *row = dst + (ly >> 2) * (t->texW >> 2) * 64 + (ly & 3) * 8;
		const u16 *src = NULL;

		if (pg) {
			float py = ((float)y + 0.5f - miny) / dy;
			int ty = (int)(mixf(v0, v3, py) * (float)pg->texH);

			if (ty < 0)
				ty = 0;
			if (ty >= pg->texH)
				ty = pg->texH - 1;
			src = pg->texels + (ty >> 2) * (pg->texW >> 2) * 16 + (ty & 3) * 4;
		}
		for (i = 0; i < n; i++) {
			u8 *p = row + colDst[i];
			u8 vr = cr, vg = cg, vb = cb, va = ca;

			if (src) {
				u8 sr, sg, sb, sa;

				unpack_rgb5a3(src[colSrc[i]], &sr, &sg, &sb, &sa);
				if (white) {
					vr = sr;
					vg = sg;
					vb = sb;
					va = sa;
				} else {
					vr = (u8)((vr * sr) / 255);
					vg = (u8)((vg * sg) / 255);
					vb = (u8)((vb * sb) / 255);
					va = (u8)((va * sa) / 255);
				}
			}
			if (va == 0)
				continue;
			if (va < 255) {
				vr = (u8)((vr * va + p[1] * (255 - va)) / 255);
				vg = (u8)((vg * va + p[32] * (255 - va)) / 255);
				vb = (u8)((vb * va + p[33] * (255 - va)) / 255);
				va = (u8)(va + (u16)p[0] * (255 - va) / 255);
			}
			p[0] = va;
			p[1] = vr;
			p[32] = vg;
			p[33] = vb;
		}
	}
}

static void emit_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
		      float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
		      u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
		      u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	GxSurf *s = gx_tiled_target(gx_cur);
	float minx, maxx, miny, maxy, lift, shift, qx, qy, dx, dy;
	int c0, c1, tr0, tr1, c, tr;
	bool flat;

	if (gx_cur && !gx_ov_drawing && (lift = WiiSafe_lift(gx_cur->base.runner)) != 0.0f) {
		y0 += lift;
		y1 += lift;
		y2 += lift;
		y3 += lift;
	}
	if (gx_cur && !gx_ov_drawing && (shift = WiiSafe_shift(gx_cur->base.runner)) != 0.0f) {
		x0 += shift;
		x1 += shift;
		x2 += shift;
		x3 += shift;
	}
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
	/* The texture spans the whole quad: a quad over the surface's edge is cut there,
	 * not squeezed into what is left. */
	qx = minx;
	qy = miny;
	dx = maxx - minx;
	dy = maxy - miny;
	if (dx < 0.001f)
		dx = 1.0f;
	if (dy < 0.001f)
		dy = 1.0f;
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
	flat = u0 == u3 && u1 == u2 && v0 == v1 && v3 == v2 && r0 == r1 && r0 == r2 && r0 == r3 &&
	       g0 == g1 && g0 == g2 && g0 == g3 && b0 == b1 && b0 == b2 && b0 == b3 && a0 == a1 && a0 == a2 && a0 == a3;
	/* Tiled surfaces are rasterized on the CPU: GX CopyTex of a tiled EFB never wrote texels. */
	for (tr = tr0; tr <= tr1; tr++) {
		for (c = c0; c <= c1; c++) {
			int ox = c * SURF_TILE_W;
			int oy = tr * SURF_TILE_H;
			int tw = SURF_TILE_W;
			int th = SURF_TILE_H;
			int x, y, x0i, y0i, x1i, y1i;
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
			t = &s->tiles[tr * s->tilesX + c];
			if (!t->texels)
				continue;
			if (flat) {
				raster_flat(t, gx_cur ? gx_cur->boundPage : NULL, ox, oy, x0i, y0i, x1i, y1i,
					    qx, qy, dx, dy, u0, u1, v0, v3, r0, g0, b0, a0);
				t->objReady = false;
				continue;
			}
			for (y = y0i; y < y1i; y++) {
				float py = ((float)y + 0.5f - qy) / dy;

				for (x = x0i; x < x1i; x++) {
					float px = ((float)x + 0.5f - qx) / dx;
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

/* The part of a quad's box inside the current view: what the rasterizer fills after the scissor.
 * Backgrounds are tiled across the whole room, so the geometric area would count mostly off-screen quads. */
static double gx_visible_area(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
	float l = fminf(fminf(x0, x1), fminf(x2, x3)), r = fmaxf(fmaxf(x0, x1), fmaxf(x2, x3));
	float t = fminf(fminf(y0, y1), fminf(y2, y3)), b = fmaxf(fmaxf(y0, y1), fmaxf(y2, y3));

	if (gx_cur) {
		l = fmaxf(l, (float)gx_cur->viewX);
		t = fmaxf(t, (float)gx_cur->viewY);
		r = fminf(r, (float)(gx_cur->viewX + gx_cur->viewW));
		b = fminf(b, (float)(gx_cur->viewY + gx_cur->viewH));
	}
	return (r > l && b > t) ? (double)(r - l) * (b - t) : 0.0;
}

static void emit_quad_raw(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3,
			  float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3,
			  u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
			  u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	u64 t;

	if (gx_cur && gx_cur->skipView && gx_cur->currentTarget == gx_cur->appSurfId) {
		gx_stat.skipped++;
		return;
	}
	t = gettime();
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
	gx_stat.quads++;
	gx_stat.area += gx_visible_area(x0, y0, x1, y1, x2, y2, x3, y3);
	gx_stat.emitUs += ticks_to_microsecs(gettime() - t);
}

static GxFillProbe gx_fill_probe; /* NULL unless a test asks: fill per page item, to name a GPU-bound room's filler */

void GxRenderer_setFillProbe(GxFillProbe probe)
{
	gx_fill_probe = probe;
}

static u8 unorm(float a)
{
	if (a <= 0.0f)
		return 0;
	if (a >= 1.0f)
		return 255;
	return (u8)(a * 255.0f + 0.5f);
}

#define PRELOAD_MAX_PAGES 2048

/* Pages the game will soon draw (AM2R: Samus's suit, the pause screen), most urgent first: they load on the
 * loader thread whenever it is idle, and count as drawn ten seconds later than they were, so a room's own pages
 * outlast idle ones. */
#define GX_WISH_MAX 32
#define GX_WISH_AGE 600
static int16_t gx_wish[GX_WISH_MAX];
static uint32_t gx_wishCount;
/* Pages of a stream animation (the title): dead weight once their room is left. */
static u8 gx_streamBits[PRELOAD_MAX_PAGES / 8];

static bool gx_wished(uint32_t page)
{
	uint32_t i;

	for (i = 0; i < gx_wishCount; i++)
		if ((uint32_t)gx_wish[i] == page)
			return true;
	return false;
}

/* The least recently drawn resident page that this frame has not touched, wished pages younger, or UINT32_MAX. */
static uint32_t gx_evict_victim(GxRenderer *gx)
{
	uint32_t i;
	uint32_t victim = UINT32_MAX;
	u64 oldest = UINT64_MAX, age;

	for (i = 0; i < gx->pageCount; i++) {
		if (!gx->pages[i].loaded || !gx->pages[i].texels || gx->pages[i].pinned)
			continue;
		if (gx->pages[i].lastUsed == gx->frameCounter)
			continue;
		age = gx->pages[i].lastUsed + (gx_wished(i) ? GX_WISH_AGE : 0);
		if (age < oldest) {
			oldest = age;
			victim = i;
		}
	}
	return victim;
}

static bool gx_evict_page(GxRenderer *gx)
{
	uint32_t victim = gx_evict_victim(gx);

	if (victim == UINT32_MAX)
		return false;
	WiiMem_texFree(gx->pages[victim].texels);
	gx->pages[victim].texels = NULL;
	gx->pages[victim].loaded = false;
	gx->pagesResident--;
	logInfo("GX: evict TXTR %u (resident %u)\n", victim, gx->pagesResident);
	return true;
}

static bool gx_reclaim_surface(GxRenderer *gx);

/* Orphaned surfaces are pure garbage: drop them all at once, so their holes merge before a page decode needs one.
 * Cached pages go last. */
static void gx_make_room(GxRenderer *gx, u32 want)
{
	/* Surfaces live wherever textures do, so that is the pool whose pressure says "collect now". */
	u32 room = WiiMem_texturesInHeap() ? WiiMem_room() : WiiMem_texFreeBytes();

	if (room < (want > SURF_RECLAIM_BELOW ? want : SURF_RECLAIM_BELOW))
		while (gx_reclaim_surface(gx))
			;
	while (WiiMem_texturesInHeap() && WiiMem_room() < want && gx_evict_page(gx))
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
	u16 *row;

	if (y >= pr->h)
		return;
	/* A row crosses each 4x4 tile as four neighboring texels. */
	row = pr->dst + gx_texel_off(pr->tw, 0, y);
	for (x = 0; x + 4 <= n; x += 4, rgba += 16) {
		u16 *d = row + x * 4;

		d[0] = pack_rgb5a3(rgba[0], rgba[1], rgba[2], rgba[3]);
		d[1] = pack_rgb5a3(rgba[4], rgba[5], rgba[6], rgba[7]);
		d[2] = pack_rgb5a3(rgba[8], rgba[9], rgba[10], rgba[11]);
		d[3] = pack_rgb5a3(rgba[12], rgba[13], rgba[14], rgba[15]);
	}
	for (; x < n; x++, rgba += 4)
		row[(x >> 2) * 16 + (x & 3)] = pack_rgb5a3(rgba[0], rgba[1], rgba[2], rgba[3]);
}

static char gx_fatal[320]; /* a data.win the GPU cannot draw; main shows it once the loop has ended */

const char *GxRenderer_fatal(void)
{
	return gx_fatal[0] ? gx_fatal : NULL;
}

static void gx_page_ready(GxRenderer *gx, uint32_t pageId, u16 *texels, int w, int h, int tw, int th, const char *how,
			  u64 t0)
{
	GxPage *page = &gx->pages[pageId];

	DCFlushRange(texels, tex_bytes(tw, th));
	GX_InitTexObj(&page->obj, texels, (u16)tw, (u16)th, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
	GX_InitTexObjFilterMode(&page->obj, GX_NEAR, GX_NEAR);
	page->texels = texels;
	page->texW = tw;
	page->texH = th;
	page->pngW = w;
	page->pngH = h;
	page->loaded = true;
	gx->pagesResident++;
	GX_InvalidateTexAll();
	logInfo("GX: TXTR %u %dx%d (padded %dx%d, resident %u) %s in %.1f ms\n", pageId, w, h, tw, th,
		gx->pagesResident, how, ticks_to_microsecs(gettime() - t0) / 1000.0);
}

static bool gx_prefetch_collect(GxRenderer *gx, bool wait);

static bool gx_load_page(GxRenderer *gx, uint32_t pageId)
{
	DataWin *dw = gx->base.dataWin;
	GxPage *page;
	Texture *txtr;
	u8 *pixels = NULL;
	u16 *texels = NULL;
	int w, h, tw, th;
	bool streamed;
	u64 t0 = gettime();

	if (pageId >= gx->pageCount)
		return false;
	page = &gx->pages[pageId];
	if (page->loading)
		gx_prefetch_collect(gx, true);
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
	if (w > GX_MAX_TEX_DIM || h > GX_MAX_TEX_DIM) {
		if (!gx_fatal[0]) {
			snprintf(gx_fatal, sizeof gx_fatal, "This data.win was not prepared for the Wii (texture page %u is %dx%d). "
			         "Delete %s, put AM2R_11.zip into %s/ and start again: the first boot builds a Wii-ready 1.5.5.",
			         pageId, w, h, WII_DATA_WIN, WII_APP_DIR);
			if (gx->base.runner)
				gx->base.runner->shouldExit = true;
		}
		goto fail;
	}
	tw = pad4(w);
	th = pad4(h);
	/* One eviction is not always enough: 1.5.5's title frames are half the size of a full page. */
	while (!(texels = WiiMem_texAlloc(tex_bytes(tw, th))) && gx_evict_page(gx))
		;
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
	gx_page_ready(gx, pageId, texels, w, h, tw, th, streamed ? "rows" : "rgba", t0);
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

#define PREFETCH_IDLE_FRAMES 300 /* a prefetch only evicts pages nobody drew for five seconds */

#define STREAM_PAGES 8 /* an animation on this many pages is a stream: in 1.5.5 only the title (30) */

/* Per TPAG item, the next page its animation reaches (-1: none), to load while this one shows; a stream (the title)
 * queues one more, other sprites only one so the room's pages stay. */
static int16_t *gx_ahead, *gx_ahead2;
static uint32_t gx_aheadCount;
static struct {
	int32_t page; /* out on the loader, -1 = none */
	u16 *texels;
	u8 *blob;
	int tw, th;
	u64 t0;
	PageRows rows;
} gx_pf = { -1, NULL, NULL, 0, 0, 0, { NULL, 0, 0, 0 } };

static void gx_build_ahead(DataWin *dw)
{
	uint32_t s, i, j, most = 1;
	int16_t *pages;

	gx_aheadCount = dw->tpag.count;
	gx_ahead = (int16_t *)safeMalloc(gx_aheadCount * sizeof(int16_t));
	gx_ahead2 = (int16_t *)safeMalloc(gx_aheadCount * sizeof(int16_t));
	memset(gx_ahead, 0xff, gx_aheadCount * sizeof(int16_t));
	memset(gx_ahead2, 0xff, gx_aheadCount * sizeof(int16_t));
	for (s = 0; s < dw->sprt.count; s++)
		if (dw->sprt.sprites[s].textureCount > most)
			most = dw->sprt.sprites[s].textureCount;
	pages = (int16_t *)safeMalloc(most * sizeof(int16_t));
	for (s = 0; s < dw->sprt.count; s++) {
		const Sprite *sp = &dw->sprt.sprites[s];
		uint32_t n = sp->textureCount;
		uint32_t distinct = 0;
		bool streams;

		if (n < 2 || !sp->tpagIndices)
			continue;
		for (i = 0; i < n; i++) {
			int32_t t = sp->tpagIndices[i];

			pages[i] = t >= 0 && (uint32_t)t < gx_aheadCount ? dw->tpag.items[t].texturePageId : -1;
			for (j = 0; j < i && pages[j] != pages[i]; j++)
				;
			if (j == i)
				distinct++;
		}
		if (distinct < 2)
			continue;
		streams = distinct >= STREAM_PAGES;
		for (i = 0; i < n; i++) {
			int16_t *next = &gx_ahead[sp->tpagIndices[i]];

			if (pages[i] < 0)
				continue;
			if (streams && pages[i] < PRELOAD_MAX_PAGES)
				gx_streamBits[pages[i] >> 3] |= (u8)(1 << (pages[i] & 7));
			for (j = 1; j < n; j++) {
				int16_t q = pages[(i + j) % n];

				if (q < 0 || q == pages[i] || q == *next)
					continue;
				if (*next < 0) {
					*next = q;
					if (!streams)
						break;
				} else {
					gx_ahead2[sp->tpagIndices[i]] = q;
					break;
				}
			}
		}
	}
	free(pages);
}

static bool gx_prefetch_collect(GxRenderer *gx, bool wait)
{
	GxPage *page;
	char how[40] = "ahead";
	bool ok;

	if (gx_pf.page < 0)
		return false;
	if (wait) {
		u64 t = gettime();

		ok = PageLoader_wait();
		snprintf(how, sizeof how, "ahead, waited %.1f ms", ticks_to_microsecs(gettime() - t) / 1000.0);
	} else if (!PageLoader_poll(&ok)) {
		return false;
	}
	page = &gx->pages[gx_pf.page];
	page->loading = false;
	free(gx_pf.blob);
	if (ok) {
		gx_page_ready(gx, (uint32_t)gx_pf.page, gx_pf.texels, page->pngW, page->pngH, gx_pf.tw, gx_pf.th, how,
			      gx_pf.t0);
		page->lastUsed = gx->frameCounter;
	} else {
		WiiMem_texFree(gx_pf.texels);
		page->pngW = -1; /* the main thread's own load says what is wrong with it */
	}
	gx_pf.page = -1;
	return ok;
}

static bool gx_png_size_at(FILE *f, const Texture *txtr, int *w, int *h)
{
	u8 hdr[33];

	return fseek(f, (long)txtr->blobOffset, SEEK_SET) == 0 && fread(hdr, 1, sizeof hdr, f) == sizeof hdr &&
	       WiiPng_size(hdr, sizeof hdr, w, h) && *w <= GX_MAX_TEX_DIM && *h <= GX_MAX_TEX_DIM;
}

/* The texels are allocated before the loader starts, so the size comes from the PNG header first. */
static bool gx_png_header(DataWin *dw, const Texture *txtr, GxPage *page)
{
	long back = ftell(dw->lazyLoadFile);
	int w, h;
	bool ok = gx_png_size_at(dw->lazyLoadFile, txtr, &w, &h);

	fseek(dw->lazyLoadFile, back, SEEK_SET);
	if (!ok) {
		page->pngW = -1;
		return false;
	}
	page->pngW = w;
	page->pngH = h;
	return true;
}

/* Every page's size before the first frame: a header read mid-game went through data.win's 128 KB-buffered
 * FILE, refilling the whole buffer on the main thread. */
static void gx_read_page_sizes(GxRenderer *gx)
{
	DataWin *dw = gx->base.dataWin;
	FILE *f = fopen(WII_DATA_WIN, "rb");
	u64 t0 = gettime();
	uint32_t i, known = 0;
	int w, h;

	if (!f)
		return;
	setvbuf(f, NULL, _IONBF, 0);
	for (i = 0; i < gx->pageCount; i++) {
		const Texture *txtr = &dw->txtr.textures[i];

		if (txtr->blobOffset && txtr->blobSize && !txtr->blobData && gx_png_size_at(f, txtr, &w, &h)) {
			gx->pages[i].pngW = w;
			gx->pages[i].pngH = h;
			known++;
		}
	}
	fclose(f);
	logInfo("GX: %u of %u page sizes read in %.1f ms\n", known, gx->pageCount,
		ticks_to_microsecs(gettime() - t0) / 1000.0);
}

/* Hands pageId to the loader thread when it is idle and the memory comes without evicting anything drawn lately. */
static void gx_prefetch(GxRenderer *gx, int32_t pageId)
{
	DataWin *dw = gx->base.dataWin;
	GxPage *page;
	const Texture *txtr;
	PageJob job;
	u32 bytes;
	uint32_t v;

	if (pageId < 0 || (uint32_t)pageId >= gx->pageCount || gx_pf.page >= 0 || PageLoader_busy() ||
	    WiiMem_texturesInHeap())
		return;
	page = &gx->pages[pageId];
	if (page->loaded || page->loading || page->failed || page->pinned || page->pngW < 0)
		return;
	txtr = &dw->txtr.textures[pageId];
	if (!txtr->blobOffset || !txtr->blobSize || txtr->blobData || !dw->lazyLoadFile)
		return;
	if (!page->pngW && !gx_png_header(dw, txtr, page))
		return;
	if (WiiMem_room() < txtr->blobSize + PAGE_LOAD_NEED)
		return;
	gx_pf.tw = pad4(page->pngW);
	gx_pf.th = pad4(page->pngH);
	bytes = tex_bytes(gx_pf.tw, gx_pf.th);
	while (!(gx_pf.texels = WiiMem_texAlloc(bytes))) {
		v = gx_evict_victim(gx);
		if (v == UINT32_MAX || gx->pages[v].lastUsed + PREFETCH_IDLE_FRAMES > gx->frameCounter ||
		    (gx_wished(v) && gx_wished((uint32_t)pageId)) || !gx_evict_page(gx))
			return;
	}
	gx_pf.blob = (u8 *)malloc(txtr->blobSize);
	gx_pf.rows = (PageRows){ gx_pf.texels, page->pngW, page->pngH, gx_pf.tw };
	job.offset = txtr->blobOffset;
	job.size = txtr->blobSize;
	job.blob = gx_pf.blob;
	job.clear = gx_pf.texels;
	job.clearBytes = bytes;
	job.emit = gx_page_row;
	job.user = &gx_pf.rows;
	if (!gx_pf.blob || !PageLoader_submit(&job)) {
		free(gx_pf.blob);
		WiiMem_texFree(gx_pf.texels);
		return;
	}
	gx_pf.page = pageId;
	gx_pf.t0 = gettime();
	page->loading = true;
}

static void gx_prefetch_wish(GxRenderer *gx)
{
	uint32_t i;
	GxPage *page;

	for (i = 0; i < gx_wishCount; i++) {
		if ((uint32_t)gx_wish[i] >= gx->pageCount)
			continue;
		page = &gx->pages[gx_wish[i]];
		if (!page->loaded && !page->loading && !page->failed) {
			gx_prefetch(gx, gx_wish[i]);
			return;
		}
	}
}

void GxRenderer_wish(DataWin *dw, const int32_t *tpags, uint32_t count)
{
	uint32_t i;
	int16_t page;

	gx_wishCount = 0;
	for (i = 0; i < count && gx_wishCount < GX_WISH_MAX; i++) {
		page = tpags[i] >= 0 && (uint32_t)tpags[i] < dw->tpag.count ?
		       dw->tpag.items[tpags[i]].texturePageId : -1;
		if (page >= 0 && !gx_wished((uint32_t)page))
			gx_wish[gx_wishCount++] = page;
	}
	if (gx_wishCount) {
		char line[160];
		int n = 0;

		for (i = 0; i < gx_wishCount && n < (int)sizeof line - 6; i++)
			n += snprintf(line + n, sizeof line - n, " %d", gx_wish[i]);
		logInfo("GX: wish %u pages:%s\n", gx_wishCount, line);
	}
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

/* Every texel of a tiled surface one color: each 4x4 block is 16 alpha/red pairs, then 16 green/blue pairs. */
static void gx_tiles_fill(GxSurf *s, u8 r, u8 g, u8 b, u8 a)
{
	u32 ar = ((u32)a << 8 | r) * 0x10001u, gb = ((u32)g << 8 | b) * 0x10001u;
	int i, k;

	for (i = 0; i < s->tilesX * s->tilesY; i++) {
		GxTile *t = &s->tiles[i];
		u32 *p, *end;

		if (!t->texels)
			continue;
		end = (u32 *)t->texels + surf_bytes(t->texW, t->texH) / 4;
		for (p = (u32 *)t->texels; p < end; p += 16)
			for (k = 0; k < 8; k++) {
				p[k] = ar;
				p[8 + k] = gb;
			}
		t->objReady = false;
	}
}

static void gx_prep_tile_obj(GxTile *t)
{
	if (t->objReady || !t->texels)
		return;
	GX_InitTexObj(&t->obj, t->texels, (u16)t->texW, (u16)t->texH, SURF_TEX_FMT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
	GX_InitTexObjFilterMode(&t->obj, GX_NEAR, GX_NEAR);
	t->objReady = true;
}

/* Before a surface's texels are written or freed it takes a buffer of its own and the other side keeps the shared one;
 * bytes move only when the write keeps part of the old picture. */
static void gx_unshare(GxRenderer *gx, GxSurf *s, bool keep)
{
	u16 *shared = s->texels;
	GxSurf *o;

	if (!s->sharer)
		return;
	o = &gx->surfs[s->sharer];
	if (s->spare) {
		s->texels = s->spare;
		s->spare = NULL;
	} else {
		s->texels = o->spare;
		o->spare = NULL;
	}
	s->sharer = o->sharer = 0;
	s->objReady = o->objReady = false;
	if (keep) {
		/* The CPU can read a buffer whose CopyTex is still in flight. */
		GX_DrawDone();
		DCInvalidateRange(shared, surf_bytes(s->texW, s->texH));
		memcpy(s->texels, shared, surf_bytes(s->texW, s->texH));
		DCFlushRange(s->texels, surf_bytes(s->texW, s->texH));
	}
}

/* A whole-surface surface_copy lends the source's texels until either side is written: the water copies the screen
 * every step, and 410 KB of memcpy costs a Wii 4-7 ms. */
static void gx_share(GxRenderer *gx, int32_t srcId, int32_t destId)
{
	GxSurf *src = &gx->surfs[srcId], *dest = &gx->surfs[destId];
	u32 nbytes = surf_bytes(src->texW, src->texH);

	if (srcId == destId)
		return;
	if (src->sharer == destId)
		return;
	gx_unshare(gx, dest, false);
	if (src->sharer) {
		GX_DrawDone();
		DCInvalidateRange(src->texels, nbytes);
		memcpy(dest->texels, src->texels, nbytes);
		DCFlushRange(dest->texels, nbytes);
		dest->objReady = false;
		GX_InvalidateTexAll();
		return;
	}
	dest->spare = dest->texels;
	dest->texels = src->texels;
	dest->sharer = srcId;
	src->sharer = destId;
	dest->objReady = false;
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
		GX_InvalidateTexAll();
		gx->targetDirty = false;
		return;
	} else {
		gx_unshare(gx, s, (s->w | s->h) & 1);
		if (!s->texels)
			return;
		texels = s->texels;
		copyW = s->w;
		copyH = s->h;
		texW = s->texW;
		texH = s->texH;
		s->objReady = false;
	}
	if (copyW > WII_FB_W)
		copyW = WII_FB_W;
	if (copyH > WII_FB_H)
		copyH = WII_FB_H;
	copyW &= ~1;
	copyH &= ~1;
	if (copyW < 2 || copyH < 2 || !texels)
		return;
	u64 tc = gettime();

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
	gx_stat.copies++;
	gx_stat.copyUs += ticks_to_microsecs(gettime() - tc);
	gx_draw_state(gx);
}

static void gx_prep_surf_obj(GxSurf *s)
{
	if (s->objReady || !s->texels)
		return;
	GX_InitTexObj(&s->obj, s->texels, (u16)s->texW, (u16)s->texH, SURF_TEX_FMT, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
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

static bool gx_bind_surface(GxRenderer *gx, int32_t id, bool restore);

/* The composed frame stands in for the host framebuffer until present. */
static void gx_bind_compose(GxRenderer *gx, bool restore)
{
	GxSurf *s = &gx->surfs[gx->composeId];

	if (gx->currentTarget != gx->composeId) {
		gx_bind_surface(gx, gx->composeId, restore);
		return;
	}
	/* Already live: a full bind would stamp its 2×2 PE kick into the finished picture. */
	gx_draw_state(gx);
	gx->base.CPortX = 0;
	gx->base.CPortY = 0;
	gx->base.CPortW = s->w;
	gx->base.CPortH = s->h;
	gx_viewport(0, 0, s->w, s->h);
	gx_ortho(0, (float)s->h, 0, (float)s->w);
}

static void gx_bind_host(GxRenderer *gx)
{
	/* Physical output is always the full framebuffer, whatever window size the game set. */
	int w = (int)WiiVideo_fbWidth();
	int h = (int)WiiVideo_efbHeight();

	if (gx->compose) {
		gx_bind_compose(gx, true);
		return;
	}
	/* Don't recopy the app surface from an EFB that set_target/clear already stomped. */
	if (gx->currentTarget != gx->appSurfId)
		gx_flush_target(gx);
	gx->currentTarget = HOST_TARGET;
	gx->targetBound = true;
	gx->targetDirty = false;
	gx_cur = gx;
	/* Overscan: squeeze everything into the safe area; the border stays black
	 * (EFB clears full each present, and the scissor clips to the viewport). */
	int oh = Overscan_h();
	/* overscan.txt counts 480-line units, so a setting covers the same part of the screen in 240p */
	int ov = Overscan_v() * h / WII_FB_H, oy = Overscan_y() * h / WII_FB_H;
	if (gx_pillarbox && WiiVideo_isWide()) {
		int pw = (w * 3) / 4;

		gx_viewport((w - pw) / 2 + oh + Overscan_x(), ov + oy, pw - 2 * oh, h - 2 * ov);
	} else {
		gx_viewport(oh + Overscan_x(), ov + oy, w - 2 * oh, h - 2 * ov);
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
	bool live;

	if (id < 0 || (uint32_t)id >= gx->surfCount)
		return false;
	s = &gx->surfs[id];
	if (!s->exists)
		return false;
	s->lastUsed = gx->frameCounter;
	/* Setting the target already in the EFB keeps drawing into it; a restore would repaint its last copy
	 * over the frame. */
	live = gx->currentTarget == id && gx->targetBound;
	if (!live)
		gx_flush_target(gx);
	gx->currentTarget = id;
	gx->targetBound = true;
	if (!live)
		gx->targetDirty = false;
	gx_cur = gx;
	if (!live)
		gx_pe_kick();
	gx_draw_state(gx);
	gx->base.CPortX = 0;
	gx->base.CPortY = 0;
	gx->base.CPortW = s->w;
	gx->base.CPortH = s->h;
	vw = s->w;
	vh = s->h;
	if (vw > WII_FB_W)
		vw = WII_FB_W;
	if (vh > WII_FB_H)
		vh = WII_FB_H;
	gx_viewport(0, 0, vw, vh);
	gx_ortho(0, (float)s->h, 0, (float)s->w);
	/* A tiled surface is rasterized on the CPU (emit_quad) and needs no restore; the projection still
	 * has to cover the whole surface, because the culls and the tiled-sprite spans read it. */
	if (restore && !s->tiles && !live)
		gx_restore_surface(gx, id);
	return true;
}


static void gx_surf_release(GxRenderer *gx, GxSurf *s)
{
	int i, n;

	gx_unshare(gx, s, false);
	if (s->tiles) {
		n = s->tilesX * s->tilesY;
		for (i = 0; i < n; i++)
			WiiMem_texFree(s->tiles[i].texels);
		free(s->tiles);
	}
	WiiMem_texFree(s->texels);
	s->tiles = NULL;
	s->texels = NULL;
	s->tilesX = 0;
	s->tilesY = 0;
	s->objReady = false;
}

static bool gx_reclaim_surface(GxRenderer *gx)
{
	int32_t appId = gx->base.runner ? gx->base.runner->applicationSurfaceId : -1;
	u64 oldest = UINT64_MAX;
	uint32_t i, victim = 0;

	for (i = 1; i < gx->surfCount; i++) {
		GxSurf *s = &gx->surfs[i];

		if (!s->exists || (int32_t)i == appId || (int32_t)i == gx->currentTarget)
			continue;
		if (gx->frameCounter - s->lastUsed < SURF_IDLE_FRAMES || s->lastUsed >= oldest)
			continue;
		oldest = s->lastUsed;
		victim = i;
	}
	if (!victim)
		return false;
	logInfo("GX: reclaim idle surface %u (%dx%d, idle %u frames)\n", (unsigned)victim,
	        (int)gx->surfs[victim].w, (int)gx->surfs[victim].h,
	        (unsigned)(gx->frameCounter - gx->surfs[victim].lastUsed));
	gx_surf_release(gx, &gx->surfs[victim]);
	memset(&gx->surfs[victim], 0, sizeof(GxSurf));
	gx->surfs[victim].retired = true;
	return true;
}

/* Surfaces share the texture arena with cached pages, and the pages are the ones that can go. */
static void *gx_surf_memalign(GxRenderer *gx, u32 bytes)
{
	void *p;

	while (!(p = WiiMem_texAlloc(bytes)) && (gx_reclaim_surface(gx) || gx_evict_page(gx)))
		;
	return p;
}

static bool gx_alloc_tiles(GxRenderer *gx, GxSurf *s, int32_t width, int32_t height)
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
			t->texW = pad4(pw);
			t->texH = pad4(ph);
			t->texels = gx_surf_memalign(gx, surf_bytes(t->texW, t->texH));
			if (!t->texels) {
				n = tilesX * tilesY;
				for (i = 0; i < n; i++)
					WiiMem_texFree(tiles[i].texels);
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
	tiled = (width > WII_FB_W || height > WII_FB_H);
	/* Slot 0 is never a real surface — GML inits ids to 0 meaning "none". */
	for (i = 1; i < gx->surfCount; i++) {
		if (!gx->surfs[i].exists && !gx->surfs[i].retired) {
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
	}
	gx_surf_release(gx, &gx->surfs[id]);
	gx->surfs[id].exists = true;
	gx->surfs[id].lastUsed = gx->frameCounter;
	gx->surfs[id].w = width;
	gx->surfs[id].h = height;
	gx->surfs[id].objReady = false;
	if (tiled) {
		if (!gx_alloc_tiles(gx, &gx->surfs[id], width, height)) {
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
	texels = gx_surf_memalign(gx, surf_bytes(tw, th));
	if (!texels) {
		gx->surfs[id].exists = false;
		return -1;
	}
	memset(texels, 0, surf_bytes(tw, th));
	DCFlushRange(texels, surf_bytes(tw, th));
	gx->surfs[id].texW = tw;
	gx->surfs[id].texH = th;
	gx->surfs[id].texels = texels;
	return id;
}

int32_t GxRenderer_addImage(const uint8_t *rgba, int w, int h)
{
	GxRenderer *gx = gx_cur;
	GxPage *page;
	int tw, th;
	u16 *texels;

	if (!gx || w < 1 || h < 1 || w > GX_MAX_TEX_DIM || h > GX_MAX_TEX_DIM ||
	    gx->pageCount >= INT16_MAX) /* TexturePageItem.texturePageId */
		return -1;
	tw = pad4(w);
	th = pad4(h);
	while (!(texels = WiiMem_texAlloc(tex_bytes(tw, th))) && gx_evict_page(gx))
		;
	if (!texels)
		return -1;
	swizzle_rgb5a3(texels, rgba, w, h, tw);
	DCFlushRange(texels, tex_bytes(tw, th));
	gx->pages = (GxPage *)safeRealloc(gx->pages, (gx->pageCount + 1) * sizeof(GxPage));
	gx->boundPage = NULL; /* pointed into the old array */
	page = &gx->pages[gx->pageCount];
	memset(page, 0, sizeof *page);
	GX_InitTexObj(&page->obj, texels, (u16)tw, (u16)th, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
	GX_InitTexObjFilterMode(&page->obj, GX_NEAR, GX_NEAR);
	page->texels = texels;
	page->texW = tw;
	page->texH = th;
	page->loaded = true;
	page->pinned = true;
	page->lastUsed = gx->frameCounter;
	gx->pagesResident++;
	GX_InvalidateTexAll();
	return (int32_t)gx->pageCount++;
}

static u8 *gx_usage; /* one bit per TPAG item drawn; NULL until a sweep asks for it */
static uint32_t gx_usageCount; /* items the bitmap covers; sprite_add can append more later */


static void gx_swap_free(GxSwap *sw)
{
	gx_swapBytes -= tex_bytes(sw->texW, sw->texH);
	WiiMem_texFree(sw->texels);
	memset(sw, 0, sizeof *sw);
}

static bool gx_swap_evict(void)
{
	int i, victim = -1;
	u64 oldest = UINT64_MAX;

	for (i = 0; i < gx_swapCount; i++) {
		if (gx_swaps[i].texels && gx_swaps[i].lastUsed < oldest) {
			oldest = gx_swaps[i].lastUsed;
			victim = i;
		}
	}
	if (victim < 0)
		return false;
	gx_swap_free(&gx_swaps[victim]);
	return true;
}

static GxSwap *gx_swap_build(GxRenderer *gx, int32_t tpagIndex, GxSwap *sw)
{
	DataWin *dw = gx->base.dataWin;
	TexturePageItem *item = &dw->tpag.items[tpagIndex];
	TexturePageItem *pal = &dw->tpag.items[gx->palTpag];
	GxPage *ipage = gx_page(gx, item->texturePageId);
	GxPage *ppage = gx_page(gx, pal->texturePageId);
	u16 from[256], to[256];
	int n = pal->sourceHeight, x, y, i, tw, th;

	if (!ipage || !ppage || gx->palIndex < 1 || gx->palIndex >= pal->sourceWidth)
		return NULL;
	if (n > 256)
		n = 256;
	for (i = 0; i < n; i++) {
		from[i] = ppage->texels[gx_texel_off(ppage->texW, pal->sourceX, pal->sourceY + i)];
		to[i] = ppage->texels[gx_texel_off(ppage->texW, pal->sourceX + gx->palIndex, pal->sourceY + i)];
	}
	tw = pad4(item->sourceWidth);
	th = pad4(item->sourceHeight);
	while (gx_swapBytes + tex_bytes(tw, th) > SWAP_BYTES_MAX && gx_swap_evict())
		;
	while (!(sw->texels = WiiMem_texAlloc(tex_bytes(tw, th))) && (gx_swap_evict() || gx_evict_page(gx)))
		;
	if (!sw->texels)
		return NULL;
	memset(sw->texels, 0, tex_bytes(tw, th));
	for (y = 0; y < item->sourceHeight; y++) {
		for (x = 0; x < item->sourceWidth; x++) {
			u16 c = ipage->texels[gx_texel_off(ipage->texW, item->sourceX + x, item->sourceY + y)];

			for (i = 0; i < n; i++) {
				if (from[i] == c) {
					c = to[i];
					break;
				}
			}
			sw->texels[gx_texel_off(tw, x, y)] = c;
		}
	}
	DCFlushRange(sw->texels, tex_bytes(tw, th));
	sw->tpag = tpagIndex;
	sw->palTpag = gx->palTpag;
	sw->palIndex = gx->palIndex;
	sw->texW = tw;
	sw->texH = th;
	GX_InitTexObj(&sw->obj, sw->texels, (u16)tw, (u16)th, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
	GX_InitTexObjFilterMode(&sw->obj, GX_NEAR, GX_NEAR);
	gx_swapBytes += tex_bytes(tw, th);
	if (gx_swapCount <= 1)
		logInfo("GX: palette swap live: palette %d[%d], %d colors\n", (int)gx->palTpag, gx->palIndex, n);
	return sw;
}

static GxSwap *gx_swap_get(GxRenderer *gx, int32_t tpagIndex)
{
	int i, slot = -1;

	for (i = 0; i < gx_swapCount; i++) {
		GxSwap *sw = &gx_swaps[i];

		if (sw->texels && sw->tpag == tpagIndex && sw->palTpag == gx->palTpag && sw->palIndex == gx->palIndex) {
			sw->lastUsed = gx->frameCounter;
			return sw;
		}
		if (!sw->texels && slot < 0)
			slot = i;
	}
	if (slot < 0) {
		if (gx_swapCount < SWAP_MAX)
			slot = gx_swapCount++;
		else if (gx_swap_evict())
			return gx_swap_get(gx, tpagIndex);
		else
			return NULL;
	}
	gx_swaps[slot].lastUsed = gx->frameCounter;
	return gx_swap_build(gx, tpagIndex, &gx_swaps[slot]);
}

/* The projection's top and bottom for a quad emit_quad is about to lift into the safe area. */
static void gx_clip_rows(float *top, float *bottom)
{
	float lift = gx_cur ? WiiSafe_lift(gx_cur->base.runner) : 0.0f;

	*top = gx_clipT - lift;
	*bottom = gx_clipB - lift;
}

static void gx_draw_tpag(GxRenderer *gx, int32_t tpagIndex, float x0, float y0, float x1, float y1,
			 float x2, float y2, float x3, float y3, float srcX, float srcY, float srcW, float srcH,
			 u8 r0, u8 g0, u8 b0, u8 a0, u8 r1, u8 g1, u8 b1, u8 a1,
			 u8 r2, u8 g2, u8 b2, u8 a2, u8 r3, u8 g3, u8 b3, u8 a3)
{
	DataWin *dw = gx->base.dataWin;
	TexturePageItem *tpag;
	GxPage *page;
	float u0, v0, u1, v1, clipT, clipB;

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	if (gx->skipView && gx->currentTarget == gx->appSurfId) {
		gx_stat.skipped++;
		return;
	}
	/* Fully outside the projection: the scissor would drop it, but only after the bind and the emit. */
	gx_clip_rows(&clipT, &clipB);
	if (fmaxf(fmaxf(x0, x1), fmaxf(x2, x3)) <= gx_clipL || fminf(fminf(x0, x1), fminf(x2, x3)) >= gx_clipR ||
	    fmaxf(fmaxf(y0, y1), fmaxf(y2, y3)) <= clipT || fminf(fminf(y0, y1), fminf(y2, y3)) >= clipB) {
		gx_stat.culled++;
		return;
	}
	if (gx_fill_probe)
		gx_fill_probe(dw, tpagIndex, gx_visible_area(x0, y0, x1, y1, x2, y2, x3, y3));
	if (gx_usage && (uint32_t)tpagIndex < gx_usageCount)
		gx_usage[tpagIndex >> 3] |= (u8)(1 << (tpagIndex & 7));
	tpag = &dw->tpag.items[tpagIndex];
	if (gx->base.currentShader >= 0 && gx->base.currentShader == gx->shPal && gx->palTpag >= 0 &&
	    (uint32_t)gx->palTpag < dw->tpag.count) {
		GxSwap *sw = gx_swap_get(gx, tpagIndex);

		if (sw) {
			u0 = srcX / (float)sw->texW;
			v0 = srcY / (float)sw->texH;
			u1 = (srcX + srcW) / (float)sw->texW;
			v1 = (srcY + srcH) / (float)sw->texH;
			gx_bind_tex(gx, &sw->obj);
			emit_quad(x0, y0, x1, y1, x2, y2, x3, y3,
				  u0, v0, u1, v0, u1, v1, u0, v1,
				  r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3);
			return;
		}
	}
	page = gx_page(gx, tpag->texturePageId);
	if (!page)
		return;
	if ((uint32_t)tpagIndex < gx_aheadCount && gx_ahead[tpagIndex] >= 0) {
		gx_prefetch(gx, gx_ahead[tpagIndex]);
		if (gx->pages[gx_ahead[tpagIndex]].loaded)
			gx_prefetch(gx, gx_ahead2[tpagIndex]);
	}
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
	gx_cur = gx; /* shaders_are_supported() is asked at Game Start, before the first frame */
	Matrix4f_identity(&world);
	renderer->gmlMatrices[MATRIX_WORLD] = world;
	WiiMem_reserveTextures(HEAP_RESERVE);
	gx->pageCount = dataWin->txtr.count;
	gx->pages = (GxPage *)safeCalloc(gx->pageCount, sizeof(GxPage));
	gx_build_ahead(dataWin);
	gx_read_page_sizes(gx);
	if (!PageLoader_start(WII_DATA_WIN))
		logWarn("GX: no loader thread, every page loads on the main thread\n");
	gx->currentTarget = HOST_TARGET;
	gx->appSurfId = -1;
	gx->composeId = -1;
	gx->guiSurf = -1;
	gx->shPal = gx->shGB = -1;
	gx->palTpag = -1;
	gx->palIndex = 1;
	{
		uint32_t i;

		for (i = 0; i < dataWin->shdr.count; i++) {
			const char *n = dataWin->shdr.shaders[i].name;

			if (n && strcmp(n, "shPaletteSwap") == 0)
				gx->shPal = (int32_t)i;
			else if (n && strcmp(n, "shGameBoy") == 0)
				gx->shGB = (int32_t)i;
		}
	}
	logInfo("GX: shaders shPaletteSwap=%d shGameBoy=%d\n", (int)gx->shPal, (int)gx->shGB);
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

	gx_prefetch_collect(gx, true);
	PageLoader_stop();
	free(gx_ahead);
	free(gx_ahead2);
	gx_ahead = gx_ahead2 = NULL;
	gx_aheadCount = 0;
	for (i = 0; i < gx->pageCount; i++)
		WiiMem_texFree(gx->pages[i].texels);
	for (i = 0; i < gx->surfCount; i++)
		gx_surf_release(gx, &gx->surfs[i]);
	free(gx->pages);
	free(gx->surfs);
	free(gx->primVerts);
	free(gx);
}

/* Not a renderer field: the game's first display_reset comes before the renderer's first frame. */
static int gx_lose_in;

/* On the real runner display_reset is a device reset: every surface dies, a few frames later.
 * 1.5.5 depends on it - its wide surface is born 320 wide during oControl's 2-frame init, and is
 * only ever recreated at 426 because that reset wipes it. */
static void gx_lose_surfaces(GxRenderer *gx)
{
	int32_t appId = gx->base.runner ? gx->base.runner->applicationSurfaceId : -1;
	uint32_t i;

	for (i = 1; i < gx->surfCount; i++) {
		if (!gx->surfs[i].exists || (int32_t)i == appId)
			continue;
		gx_surf_release(gx, &gx->surfs[i]);
		memset(&gx->surfs[i], 0, sizeof(GxSurf));
		gx->surfs[i].retired = true;
	}
}

void GxRenderer_loseSurfaces(void)
{
	gx_lose_in = LOSE_SURFACES_FRAMES;
}

/* Compose when a game pixel would not be a whole number of screen pixels wide (the 426-wide view
 * on a 640 framebuffer; 4:3 is an exact 2× and stays on the direct path). Sizes come from the
 * window in game pixels, so the game's own scale setting doesn't matter. */
static void gx_setup_compose(GxRenderer *gx, int32_t windowW, int32_t windowH)
{
	int fbw = (int)WiiVideo_fbWidth();
	int fbh = (int)WiiVideo_efbHeight();
	int cw, ch;

	static bool said;

	gx->compose = false;
	if (!gx_wide_filter || gx->gameH < 1 || windowH < 1)
		return;
	cw = windowW * gx->gameH / windowH;
	ch = (fbh / gx->gameH) * gx->gameH;
	if (cw < 2 || cw > WII_FB_W || ch < 1 || fbw % cw == 0 || cw * 3 > fbw * 2)
		return;
	if (gx->composeId < 0 || (uint32_t)gx->composeId >= gx->surfCount || !gx->surfs[gx->composeId].exists) {
		gx->composeId = gx_alloc_surface(gx, cw, ch);
		if (gx->composeId < 0) {
			if (!said) {
				said = true;
				logWarn("GX: wide compose %dx%d: no surface memory (window %dx%d)\n", cw, ch,
					windowW, windowH);
			}
			return;
		}
		logInfo("GX: wide compose %dx%d -> %d:%d filtered present\n", cw, ch, fbw, cw);
	} else if (gx->surfs[gx->composeId].w != cw || gx->surfs[gx->composeId].h != ch) {
		gxSurfaceResize(&gx->base, gx->composeId, cw, ch);
		if (!gx->surfs[gx->composeId].exists)
			return;
	}
	gx->compose = true;
}

static void gx_want_tpag(DataWin *dw, u8 *want, int32_t tpagIndex)
{
	int16_t page;

	if (tpagIndex < 0 || (uint32_t)tpagIndex >= dw->tpag.count)
		return;
	page = dw->tpag.items[tpagIndex].texturePageId;
	if (page >= 0 && page < PRELOAD_MAX_PAGES)
		want[page >> 3] |= (u8)(1 << (page & 7));
}

static void gx_want_sprite(DataWin *dw, u8 *want, int32_t spriteIndex)
{
	Sprite *sprite;
	uint32_t f;

	if (spriteIndex < 0 || (uint32_t)spriteIndex >= dw->sprt.count)
		return;
	sprite = &dw->sprt.sprites[spriteIndex];
	for (f = 0; sprite->tpagIndices && f < sprite->textureCount; f++)
		gx_want_tpag(dw, want, sprite->tpagIndices[f]);
}

static void gx_want_bgnd(DataWin *dw, u8 *want, int32_t bgIndex)
{
	if (bgIndex >= 0 && (uint32_t)bgIndex < dw->bgnd.count)
		gx_want_tpag(dw, want, dw->bgnd.backgrounds[bgIndex].tpagIndex);
}

/* Every page the room's backgrounds, tiles and placed objects sit on, loaded before its first frame:
 * a page pulled mid-play is a quarter-second hitch. Objects spawned later still load on demand. */
static void gx_preload_room(GxRenderer *gx)
{
	static int32_t lastRoom = -1;
	Runner *runner = gx->base.runner;
	DataWin *dw = gx->base.dataWin;
	Room *room;
	u8 want[PRELOAD_MAX_PAGES / 8] = { 0 };
	uint32_t i, n = 0;
	u64 t0 = gettime();

	if (!runner || runner->currentRoomIndex == lastRoom)
		return;
	lastRoom = runner->currentRoomIndex;
	for (i = 0; i < gx->pageCount && i < PRELOAD_MAX_PAGES; i++)
		if (gx_streamBits[i >> 3] & (1 << (i & 7)))
			gx->pages[i].lastUsed = 0;
	if (lastRoom < 0 || (uint32_t)lastRoom >= dw->room.count || !dw->room.rooms[lastRoom].payloadLoaded)
		return;
	room = &dw->room.rooms[lastRoom];
	for (i = 0; i < 8; i++)
		if (runner->backgrounds[i].visible)
			gx_want_bgnd(dw, want, runner->backgrounds[i].backgroundIndex);
	for (i = 0; i < room->tileCount; i++) {
		if (room->tiles[i].useSpriteDefinition)
			gx_want_sprite(dw, want, room->tiles[i].backgroundDefinition);
		else
			gx_want_bgnd(dw, want, room->tiles[i].backgroundDefinition);
	}
	for (i = 0; i < room->gameObjectCount; i++) {
		int32_t obj = room->gameObjects[i].objectDefinition;

		/* An invisible object is never drawn: oSolid, in nearly every room, would hold a page of Area 5 art. */
		if (obj >= 0 && (uint32_t)obj < dw->objt.count && dw->objt.objects[obj].visible)
			gx_want_sprite(dw, want, dw->objt.objects[obj].spriteId);
	}
	for (i = 0; i < gx->pageCount && i < PRELOAD_MAX_PAGES; i++) {
		if (!(want[i >> 3] & (1 << (i & 7))) || gx->pages[i].loaded)
			continue;
		/* Nothing left to evict but this frame's own pages: the rest load on demand. */
		if ((WiiMem_texturesInHeap() ? WiiMem_room() : WiiMem_texFreeBytes()) < PAGE_LOAD_NEED &&
		    gx_evict_victim(gx) == UINT32_MAX)
			break;
		if (gx_load_page(gx, i))
			gx->pages[i].lastUsed = gx->frameCounter;
		n++;
	}
	if (n)
		logInfo("GX: preload room %d: %u pages in %.1f ms\n", lastRoom, n,
			ticks_to_microsecs(gettime() - t0) / 1000.0);
	/* A room change is already a pause, and a freeze log only survives a power-off once it is on the card. */
	WiiLog_sync();
}

/* The effect sprites (explosions, smoke, splashes, sparks): drawn in every area but rarely, so a room's first
 * explosion pulled their page mid-play. tools/effects_group.py packs them onto one page; keep that resident. */
static const char *const GX_EFFECT_WORDS[] = { "expl", "smoke", "splash", "spark", "debris", "impact", "blast" };
#define GX_EFFECT_PAGES_MAX 2 /* an older pack scatters them over ~20 pages: pinning those would starve the arena */

static bool gx_contains_nocase(const char *s, const char *word)
{
	size_t n = strlen(word);

	for (; *s; s++)
		if (strncasecmp(s, word, n) == 0)
			return true;
	return false;
}

static bool gx_is_effect(const char *name)
{
	unsigned k;

	if (!name || name[0] != 's' || gx_contains_nocase(name, "door")) /* area 4's blast door is scenery */
		return false;
	for (k = 0; k < sizeof GX_EFFECT_WORDS / sizeof GX_EFFECT_WORDS[0]; k++)
		if (gx_contains_nocase(name, GX_EFFECT_WORDS[k]))
			return true;
	return false;
}

static void gx_pin_effects(GxRenderer *gx)
{
	static bool done;
	DataWin *dw = gx->base.dataWin;
	u8 want[PRELOAD_MAX_PAGES / 8] = { 0 };
	uint32_t s, i, n = 0;

	if (done || !dw)
		return;
	done = true;
	for (s = 0; s < dw->sprt.count; s++)
		if (gx_is_effect(dw->sprt.sprites[s].name))
			gx_want_sprite(dw, want, (int32_t)s);
	for (i = 0; i < gx->pageCount && i < PRELOAD_MAX_PAGES; i++)
		n += (want[i >> 3] >> (i & 7)) & 1;
	if (n > GX_EFFECT_PAGES_MAX) {
		logInfo("GX: effects on %u pages, not pinned (an older texture pack)\n", n);
		return;
	}
	for (i = 0; i < gx->pageCount && i < PRELOAD_MAX_PAGES; i++) {
		if (!(want[i >> 3] & (1 << (i & 7))))
			continue;
		gx->pages[i].pinned = gx->pages[i].loaded || gx_load_page(gx, i);
		logInfo("GX: effects page %u %s\n", i, gx->pages[i].pinned ? "resident" : "could not load");
	}
}

static void gxBeginFrame(Renderer *renderer, int32_t gameW, int32_t gameH, int32_t windowW, int32_t windowH)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	int32_t appId;

	gx_stat.frameStart = gettime();
	if (gx_stat.lastEnd)
		gx_stat.otherUs += ticks_to_microsecs(gx_stat.frameStart - gx_stat.lastEnd);
	gx->gameW = gameW;
	gx->gameH = gameH;
	gx->frameCounter++;
	gx_prefetch_collect(gx, false);
	gx->viewActive = false;
	gx->viewSnapped = false;
	gx_quads = 0;
	gx_cur = gx;
	gx_vtx_fmt();
	gx_apply_blend(gx);
	gx_apply_alpha_test(gx);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	if (gx_lose_in > 0 && --gx_lose_in == 0)
		gx_lose_surfaces(gx);
	if ((gx->frameCounter % 60) == 0)
		gx_make_room(gx, MEM_RESERVE);
	gx_pin_effects(gx);
	gx_preload_room(gx);
	gx_setup_compose(gx, windowW, windowH);
	Wii_hooksFrame(renderer->runner);
	Wii_forceGamepadProfile(renderer->runner);
	Wii_setViewClears(renderer->runner);
	Wii_wishPages(renderer->runner);
	WiiSafe_frame(renderer->runner);
	WiiControls_frame(renderer->runner);
	gx->guiSurf = WiiSafe_guiSurface(renderer->runner);
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
	gx->viewSurf = -1;
	gx->viewX = viewX;
	gx->viewY = viewY;
	gx->viewW = viewW;
	gx->viewH = viewH;
	gx->viewPortX = portX;
	gx->viewPortY = portY;
	gx->viewPortW = portW;
	gx->viewPortH = portH;
	gx_ortho((float)viewY, (float)(viewY + viewH), (float)viewX, (float)(viewX + viewW));
	if (renderer->runner->viewsEnabled)
		viewCurrent = renderer->runner->viewCurrent;
	view = &renderer->runner->views[viewCurrent];
	/* 1.5.5 wide mode draws the room twice; view 0's copy goes to the hidden application surface. */
	gx->skipView = viewCurrent == 0 && renderer->runner->viewsEnabled && renderer->runner->usingAppSurface &&
		       !renderer->runner->appSurfaceAutoDraw && renderer->runner->views[1].enabled &&
		       renderer->runner->views[1].surfaceId >= 0;
	gx->base.cameraCurrent = view->cameraId;
	camera = Runner_getCameraById(renderer->runner, gx->base.cameraCurrent);
	if (camera) {
		renderer->gmlMatrices[MATRIX_VIEW] = camera->viewMatrix;
		renderer->gmlMatrices[MATRIX_PROJECTION] = camera->projectionMatrix;
	}
}

static void gxEndView(Renderer *renderer)
{
	((GxRenderer *)renderer)->skipView = false;
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

static void gx_view_ortho(GxRenderer *gx)
{
	gx_ortho((float)gx->viewY, (float)(gx->viewY + gx->viewH), (float)gx->viewX, (float)(gx->viewX + gx->viewW));
}

/* A view with its own surface (1.5.5's 426-wide view) never goes through beginView: the engine binds
 * the surface and hands us the camera. */
static void gxApplyViewProjection(Renderer *renderer, const Matrix4f *viewMatrix, const Matrix4f *projectionMatrix)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GMLCamera *camera = Runner_getCameraById(renderer->runner, renderer->cameraCurrent);

	gx->skipView = false;
	gxApplyProjection(renderer, viewMatrix, projectionMatrix);
	if (!camera || gx->currentTarget < 0)
		return;
	gx->viewActive = true;
	gx->viewSurf = gx->currentTarget;
	gx->viewX = (int32_t)camera->viewX;
	gx->viewY = (int32_t)camera->viewY;
	gx->viewW = camera->viewWidth;
	gx->viewH = camera->viewHeight;
	gx_view_ortho(gx);
}

static void gxBeginGUI(Renderer *renderer, int32_t guiW, int32_t guiH, int32_t portX, int32_t portY,
		       int32_t portW, int32_t portH, int32_t targetSurfaceId)
{
	((GxRenderer *)renderer)->skipView = false;
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
		if (gx->compose) {
			gx_bind_compose(gx, false);
			gxClearScreen(renderer, 0x000000, 1.0f);
			return;
		}
		/* Clear the full EFB, not the overscan-inset viewport: the app surface renders into the
		 * top-left corner and would peek out of the black border once overscan shrinks the picture. */
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

static void gx_draw_overscan_overlay(GxRenderer *gx);

static void gxEndFrameEnd(Renderer *renderer)
{
	((GxRenderer *)renderer)->skipView = false;
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
	/* GameMaker forgets the shader at frame end; a palette swap the subscreen leaves set would recolor the next
	 * room. */
	if (renderer->currentShader >= 0) {
		static int said;

		if (said < 5) {
			said++;
			logInfo("GX: shader %d still set at frame end (palIndex %d palTpag %d) - reset\n",
				renderer->currentShader, gx->palIndex, gx->palTpag);
		}
		renderer->currentShader = -1;
	}
	gx_prefetch_wish(gx);
	gx_stat.lastEnd = gettime();
	gx_stat.drawUs += ticks_to_microsecs(gx_stat.lastEnd - gx_stat.frameStart);
	if (++gx_stat.n == 200) {
		logInfo("GXSTAT draw %.2f ms/frame: emit %.2f ms in %u quads (%u tiles, %u sprites, %u skipped, %u culled), "
			"%u copies %.2f ms, %u binds, %u targets; between draws %.2f ms; fill %.2f Mpx\n",
			gx_stat.drawUs / 200000.0, gx_stat.emitUs / 200000.0, gx_stat.quads / 200, gx_stat.tiles / 200,
			gx_stat.sprites / 200, gx_stat.skipped / 200, gx_stat.culled / 200,
			gx_stat.copies / 200, gx_stat.copyUs / 200000.0, gx_stat.binds / 200, gx_stat.targets / 200,
			gx_stat.otherUs / 200000.0, gx_stat.area / 200e6);
		gx_stat.area = 0;
		if (gx_fill_probe)
			gx_fill_probe(renderer->dataWin, -1, 0.0);
		gx_stat.quads = gx_stat.tiles = gx_stat.sprites = gx_stat.copies = gx_stat.binds = gx_stat.targets = 0;
		gx_stat.skipped = gx_stat.culled = 0;
		gx_stat.emitUs = gx_stat.copyUs = gx_stat.drawUs = gx_stat.otherUs = 0;
		gx_stat.n = 0;
	}
}

/* Every 2 composed columns become 3 screen columns: pure, 50/50, pure. Pass 1 is the plain
 * nearest 3:2 stretch; pass 2 redraws each middle column as the TEV average of two exact texel
 * fetches, so no bilinear sampling (and no sub-pixel bias) is involved. Rows map 1:1. */
static void gx_present_mix_column(float x, float h, float ua, float ub, float v1)
{
	GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
	GX_Position2f32(x, 0);
	GX_Color4u8(255, 255, 255, 255);
	GX_TexCoord2f32(ua, 0);
	GX_TexCoord2f32(ub, 0);
	GX_Position2f32(x + 1, 0);
	GX_Color4u8(255, 255, 255, 255);
	GX_TexCoord2f32(ua, 0);
	GX_TexCoord2f32(ub, 0);
	GX_Position2f32(x + 1, h);
	GX_Color4u8(255, 255, 255, 255);
	GX_TexCoord2f32(ua, v1);
	GX_TexCoord2f32(ub, v1);
	GX_Position2f32(x, h);
	GX_Color4u8(255, 255, 255, 255);
	GX_TexCoord2f32(ua, v1);
	GX_TexCoord2f32(ub, v1);
	GX_End();
}

void GxRenderer_present(void)
{
	GxRenderer *gx = gx_cur;
	GxSurf *s;
	int fbw, n, ox, k;
	float v1, tw, h, u1;

	if (!gx || !gx->compose)
		return;
	s = &gx->surfs[gx->composeId];
	gx->compose = false;
	gx_bind_host(gx); /* flushes the composed frame to its texture on the way */
	gx_viewport(0, 0, (int)WiiVideo_fbWidth(), (int)WiiVideo_efbHeight());
	gxClearScreen(&gx->base, 0x000000, 1.0f);
	gx_bind_host(gx);
	if (!s->exists || !s->texels)
		return;
	gx_prep_surf_obj(s);
	gx_bind_tex(gx, &s->obj);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	fbw = (int)WiiVideo_fbWidth();
	n = s->w / 2;
	ox = (fbw - n * 3) / 2;
	tw = (float)s->texW;
	h = (float)s->h;
	v1 = h / (float)s->texH;
	u1 = (float)(2 * n) / tw;
	emit_quad((float)ox, 0, (float)(ox + 3 * n), 0, (float)(ox + 3 * n), h, (float)ox, h,
		  0, 0, u1, 0, u1, v1, 0, v1,
		  255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);

	GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
	GX_SetNumTexGens(2);
	GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
	GX_SetNumTevStages(2);
	GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_1_2);
	GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_CPREV, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	for (k = 0; k < n; k++)
		gx_present_mix_column((float)(ox + 3 * k + 1), h, (2.0f * (float)k + 0.5f) / tw,
				      (2.0f * (float)k + 1.5f) / tw, v1);
	GX_SetNumTevStages(1);
	GX_SetNumTexGens(1);
	gx_vtx_fmt();
	gx_unbind_tex(gx);
	gx_apply_blend(gx);
}

static void gxDrawSprite(Renderer *renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
			 float xscale, float yscale, float angleDeg, uint32_t color, float alpha)
{
	gx_stat.sprites++;
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
			    float pivotX, float pivotY, float *x0, float *y0, float *x1, float *y1,
			    float *x2, float *y2, float *x3, float *y3)
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
	gx_stat.sprites++;
	GxRenderer *gx = (GxRenderer *)renderer;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	u8 r = (u8)BGR_R(color), g = (u8)BGR_G(color), b = (u8)BGR_B(color), a = unorm(alpha);

	gx_part_corners(x, y, srcW, srcH, xscale, yscale, angleDeg, pivotX, pivotY,
			&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3);
	gx_draw_tpag(gx, tpagIndex, x0, y0, x1, y1, x2, y2, x3, y3,
		     (float)srcOffX, (float)srcOffY, (float)srcW, (float)srcH,
		     r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a);
}

/* A GameMaker 1 room keeps every tile in the draw list (rm_a5h04: 2,256, about 60 on screen); this drops the ones
 * outside the projection before the engine's generic path resolves and clips each, then steps aside for the rest. */
static void gxDrawTile(Renderer *renderer, RoomTile *tile, float offsetX, float offsetY)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float x0 = (float)tile->x + offsetX, y0 = (float)tile->y + offsetY;
	float x1 = x0 + (float)tile->width * tile->scaleX, y1 = y0 + (float)tile->height * tile->scaleY;
	float clipT, clipB;

	if (gx->skipView && gx->currentTarget == gx->appSurfId) {
		gx_stat.sprites++;
		gx_stat.skipped++;
		return;
	}
	/* Most room tiles are offscreen; reject their columns before resolving the HUD lift. */
	if ((x0 <= gx_clipL && x1 <= gx_clipL) || (x0 >= gx_clipR && x1 >= gx_clipR)) {
		gx_stat.sprites++;
		gx_stat.culled++;
		return;
	}
	gx_clip_rows(&clipT, &clipB);
	if ((y0 <= clipT && y1 <= clipT) || (y0 >= clipB && y1 >= clipB)) {
		gx_stat.sprites++;
		gx_stat.culled++;
		return;
	}
	renderer->vtable->drawTile = NULL;
	Renderer_drawTile(renderer, tile, offsetX, offsetY);
	renderer->vtable->drawTile = gxDrawTile;
}

static void gxDrawSpritePartColor(Renderer *renderer, int32_t tpagIndex, int32_t srcOffX, int32_t srcOffY, int32_t srcW,
				  int32_t srcH, float x, float y, float xscale, float yscale, float angleDeg,
				  float pivotX, float pivotY, uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4,
				  float alpha)
{
	gx_stat.sprites++;
	GxRenderer *gx = (GxRenderer *)renderer;
	float x0, y0, x1, y1, x2, y2, x3, y3;
	u8 a = unorm(alpha);

	gx_part_corners(x, y, srcW, srcH, xscale, yscale, angleDeg, pivotX, pivotY,
			&x0, &y0, &x1, &y1, &x2, &y2, &x3, &y3);
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
	gx_stat.sprites++;
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

/* A glyph laid out in the font's units, drawable at any offset: AM2R's outlined text is one string drawn nine times. */
typedef struct {
	float lx0, ly0, lx1, ly1, u0, v0, u1, v1;
	int32_t tc1, tc2, tc3, tc4;
	uint32_t pageId;
} GxGlyph;

/* One drawing of a laid-out string: its offset and alpha, in its own corner colors or one color for all. */
typedef struct {
	double dx, dy;
	bool uniform;
	int32_t color;
	float alpha;
} GxTextPass;

#define GX_TEXT_GLYPHS 1024
static GxGlyph gx_glyphs[GX_TEXT_GLYPHS];

static void gx_text_emit(GxRenderer *gx, uint32_t count, double x, double y, float sx, float sy, float angleRad,
			 bool snap, const GxTextPass *pass)
{
	Matrix4f transform;
	GxPage *page = NULL;
	uint32_t i, pageId = UINT32_MAX;
	u8 a = unorm(pass->alpha);

	Matrix4f_setTransform2D(&transform, (float)(x + pass->dx), (float)(y + pass->dy), sx, sy, angleRad);
	for (i = 0; i < count; i++) {
		const GxGlyph *g = &gx_glyphs[i];
		int32_t tc1 = pass->uniform ? pass->color : g->tc1, tc2 = pass->uniform ? pass->color : g->tc2;
		int32_t tc3 = pass->uniform ? pass->color : g->tc3, tc4 = pass->uniform ? pass->color : g->tc4;
		float px0, py0, px1, py1, px2, py2, px3, py3;

		if (g->pageId != pageId) {
			pageId = g->pageId;
			page = gx_page(gx, pageId);
		}
		if (!page)
			continue;
		gx_bind_tex(gx, &page->obj);
		Matrix4f_transformPoint(&transform, g->lx0, g->ly0, &px0, &py0);
		Matrix4f_transformPoint(&transform, g->lx1, g->ly0, &px1, &py1);
		Matrix4f_transformPoint(&transform, g->lx1, g->ly1, &px2, &py2);
		Matrix4f_transformPoint(&transform, g->lx0, g->ly1, &px3, &py3);
		if (snap) {
			px0 = px3 = floorf(px0 + 0.5f);
			px1 = px2 = floorf(px1 + 0.5f);
			py0 = py1 = floorf(py0 + 0.5f);
			py2 = py3 = floorf(py2 + 0.5f);
		}
		emit_quad(px0, py0, px1, py1, px2, py2, px3, py3,
			  g->u0, g->v0, g->u1, g->v0, g->u1, g->v1, g->u0, g->v1,
			  (u8)BGR_R(tc1), (u8)BGR_G(tc1), (u8)BGR_B(tc1), a,
			  (u8)BGR_R(tc2), (u8)BGR_G(tc2), (u8)BGR_B(tc2), a,
			  (u8)BGR_R(tc3), (u8)BGR_G(tc3), (u8)BGR_B(tc3), a,
			  (u8)BGR_R(tc4), (u8)BGR_G(tc4), (u8)BGR_B(tc4), a);
	}
}

/* Lays the text out once and draws it once per pass, every glyph of a pass before the next pass, as that many
 * separate draws would. A text too long for one layout is drawn pass by pass. */
static void gx_draw_text(GxRenderer *gx, const char *text, double x, double y, float xscale, float yscale,
			 float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float lineSeparation,
			 const GxTextPass *passes, int passCount)
{
	Renderer *renderer = &gx->base;
	DataWin *dw = renderer->dataWin;
	Font *font;
	Sprite *spriteFont;
	TexturePageItem *tpag;
	GxPage *page;
	int32_t textLen, lineCount, lineStart, lineIdx, p;
	float lineStride, totalHeight, valignOffset, angleRad, band, clipT, clipB, sx, sy;
	Matrix4f transform;
	bool snap, overflow = false;
	float cursorY;
	uint32_t count = 0;

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
	sx = xscale * font->scaleX;
	sy = yscale * font->scaleY;
	Matrix4f_setTransform2D(&transform, (float)x, (float)y, sx, sy, angleRad);
	/* Centered odd-width lines start on a half pixel, where GX_NEAR samples sit exactly on texel
	 * seams and real hardware garbles thin glyphs. Keep unrotated 1:1 text on the pixel grid. */
	snap = angleDeg == 0.0f && sx == 1.0f && sy == 1.0f;
	/* How far a line's glyphs can reach from its cursor, for skipping lines off the projection whole:
	 * AM2R's credits draw all 200 rows nine times a frame with 20 on screen. */
	band = fmaxf(lineStride, (float)font->maxGlyphHeight) + fabsf((float)font->spriteOriginYAdjust);
	gx_clip_rows(&clipT, &clipB);
	cursorY = valignOffset - (float)font->ascenderOffset;
	lineStart = 0;
	for (lineIdx = 0; lineIdx < lineCount && !overflow; lineIdx++) {
		int32_t lineEnd = lineStart;
		int32_t lineLen, pos;
		float lineWidth, halignOffset, cursorX;
		uint16_t ch = 0;
		bool hasCh;

		while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd]))
			lineEnd++;
		if (angleDeg == 0.0f) {
			float top, bottom, unused;

			Matrix4f_transformPoint(&transform, 0, cursorY - band, &unused, &top);
			Matrix4f_transformPoint(&transform, 0, cursorY + 2.0f * band, &unused, &bottom);
			if (fmaxf(top, bottom) <= clipT || fminf(top, bottom) >= clipB) {
				cursorY += lineStride;
				lineStart = TextUtils_skipNewline(text, lineEnd, textLen);
				continue;
			}
		}
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
				GxGlyph *g = &gx_glyphs[count];
				TexturePageItem *gtpag = NULL;
				GxPage *gpage = NULL;
				float leftFrac, rightFrac;

				if (spriteFont) {
					int32_t gi = (int32_t)(glyph - font->glyphs);
					int32_t tpagIdx;

					if (gi >= 0 && gi < (int32_t)spriteFont->textureCount) {
						tpagIdx = spriteFont->tpagIndices[gi];
						if (tpagIdx >= 0 && (uint32_t)tpagIdx < dw->tpag.count) {
							gtpag = &dw->tpag.items[tpagIdx];
							gpage = gx_page(gx, gtpag->texturePageId);
						}
					}
				} else {
					gtpag = tpag;
					gpage = page;
				}
				if (gpage && count == GX_TEXT_GLYPHS) {
					if (passCount > 1) {
						overflow = true;
						break;
					}
					gx_text_emit(gx, count, x, y, sx, sy, angleRad, snap, passes);
					count = 0;
					g = &gx_glyphs[0];
				}
				if (gpage && spriteFont) {
					g->u0 = (float)gtpag->sourceX / (float)gpage->texW;
					g->v0 = (float)gtpag->sourceY / (float)gpage->texH;
					g->u1 = (float)(gtpag->sourceX + gtpag->sourceWidth) / (float)gpage->texW;
					g->v1 = (float)(gtpag->sourceY + gtpag->sourceHeight) / (float)gpage->texH;
					g->lx0 = cursorX + (float)glyph->offset;
					g->ly0 = cursorY + (float)(int32_t)gtpag->targetY -
						 (float)font->spriteOriginYAdjust;
					g->lx1 = g->lx0 + (float)gtpag->sourceWidth;
					g->ly1 = g->ly0 + (float)gtpag->sourceHeight;
				} else if (gpage) {
					g->u0 = (float)(gtpag->sourceX + glyph->sourceX) / (float)gpage->texW;
					g->v0 = (float)(gtpag->sourceY + glyph->sourceY) / (float)gpage->texH;
					g->u1 = (float)(gtpag->sourceX + glyph->sourceX + glyph->sourceWidth) /
						(float)gpage->texW;
					g->v1 = (float)(gtpag->sourceY + glyph->sourceY + glyph->sourceHeight) /
						(float)gpage->texH;
					g->lx0 = cursorX + (float)glyph->offset;
					g->ly0 = cursorY;
					g->lx1 = g->lx0 + (float)glyph->sourceWidth;
					g->ly1 = g->ly0 + (float)glyph->sourceHeight;
				}
				if (gpage) {
					leftFrac = (lineWidth > 0.0f) ? (cursorX / lineWidth) : 0.0f;
					rightFrac = (lineWidth > 0.0f) ? ((cursorX + (float)glyph->shift) / lineWidth) :
						    1.0f;
					g->tc1 = Color_lerp(c1, c2, leftFrac);
					g->tc2 = Color_lerp(c1, c2, rightFrac);
					g->tc3 = Color_lerp(c4, c3, rightFrac);
					g->tc4 = Color_lerp(c4, c3, leftFrac);
					g->pageId = gtpag->texturePageId;
					count++;
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
	if (overflow) {
		for (p = 0; p < passCount; p++)
			gx_draw_text(gx, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, lineSeparation,
				     &passes[p], 1);
		return;
	}
	for (p = 0; p < passCount; p++)
		gx_text_emit(gx, count, x, y, sx, sy, angleRad, snap, &passes[p]);
}

static void gxDrawTextColor(Renderer *renderer, const char *text, float x, float y, float xscale, float yscale,
			    float angleDeg, int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha,
			    float lineSeparation)
{
	GxTextPass pass = { 0, 0, false, 0, alpha };

	gx_draw_text((GxRenderer *)renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, lineSeparation,
		     &pass, 1);
}

void GxRenderer_drawOutlinedText(Renderer *renderer, const char *text, double x, double y, int32_t outline,
				 int32_t top, int32_t bottom, float alpha)
{
	static const int8_t offsets[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
					      { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	GxTextPass passes[9];
	int i;

	for (i = 0; i < 8; i++)
		passes[i] = (GxTextPass){ offsets[i][0], offsets[i][1], true, outline, alpha };
	passes[8] = (GxTextPass){ 0, 0, false, 0, alpha };
	gx_draw_text((GxRenderer *)renderer, text, x, y, 1.0f, 1.0f, 0.0f, top, top, bottom, bottom, -1.0f, passes, 9);
}

static void gxDrawText(Renderer *renderer, const char *text, float x, float y, float xscale, float yscale,
		       float angleDeg, float lineSeparation)
{
	int32_t c = (int32_t)renderer->drawColor;

	gxDrawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c, c, c, c, renderer->drawAlpha,
			lineSeparation);
}

static void gxFlush(MAYBE_UNUSED Renderer *renderer) {}

static void gxClearScreen(Renderer *renderer, uint32_t color, float alpha)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	float w = (float)gx->base.CPortW;
	float h = (float)gx->base.CPortH;
	u8 a = unorm(alpha);
	u8 r = (u8)BGR_R(color), g = (u8)BGR_G(color), b = (u8)BGR_B(color);

	/* Out of play AM2R still wipes the HUD layer every step, though it only shows it in play: keeping the last
	 * picture lets the pause screen's adjust pages show the real HUD. */
	if (gx->guiSurf >= 0 && gx->currentTarget == gx->guiSurf && !WiiSafe_ingame(renderer->runner))
		return;

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
	/* A tiled surface is drawn on the CPU, where a clear quad would blend its two million pixels one by one. */
	if (gx_tiled_target(gx)) {
		gx_tiles_fill(gx_tiled_target(gx), r, g, b, a);
		return;
	}
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

/* Tiles past the projection are scissored anyway; skipping them turns a room-wide background into a
 * screen's worth of quads. A mirrored scale draws each tile on the other side of its lattice point. */
static void gx_tile_span(float *start, float *end, float tile, float lo, float hi, bool mirrored)
{
	float first = mirrored ? lo : lo - tile, last = mirrored ? hi + tile : hi;
	float skip = floorf((first - *start) / tile) + 1;

	if (skip > 0)
		*start += skip * tile;
	if (*end > last)
		*end = last;
}

#define PATTERN_MAX 64

/* An unscaled, opaque background tiled over a tiled surface comes out the same in every cell: one cell is converted and
 * its 4x4 blocks copied across (the pause map's 2048x1024 grid). False where that does not hold. */
static bool gx_tile_pattern(GxRenderer *gx, int32_t tpagIndex, float xscale, float yscale, uint32_t color, float alpha,
			    float startX, float endX, float startY, float endY)
{
	static u8 cell[PATTERN_MAX * PATTERN_MAX * 4];
	TexturePageItem *tpag = &gx->base.dataWin->tpag.items[tpagIndex];
	GxSurf *s = gx_tiled_target(gx);
	GxPage *page;
	int w = tpag->boundingWidth, h = tpag->boundingHeight, x0 = (int)startX, y0 = (int)startY;
	int nx = 0, ny = 0, xc0, xc1, yc0, yc1, i, j, tr, c, bx, by;
	float f;

	if (!s || xscale != 1.0f || yscale != 1.0f || (color & 0xFFFFFF) != 0xFFFFFF || alpha != 1.0f ||
	    gx->base.currentShader >= 0 || WiiSafe_lift(gx->base.runner) != 0.0f || tpag->sourceWidth != w ||
	    tpag->sourceHeight != h || tpag->targetX || tpag->targetY || w % 4 || h % 4 || w > PATTERN_MAX ||
	    h > PATTERN_MAX || (float)x0 != startX || (float)y0 != startY || x0 % 4 || y0 % 4 || s->w % 4 || s->h % 4 ||
	    gx_clipL != 0.0f || gx_clipT != 0.0f || gx_clipR != (float)s->w || gx_clipB != (float)s->h)
		return false;
	page = gx_page(gx, tpag->texturePageId);
	if (!page || !page->texels)
		return false;
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int tx = tpag->sourceX + i, ty = tpag->sourceY + j;
			u8 r, g, b, a;

			unpack_rgb5a3(page->texels[((ty >> 2) * (page->texW >> 2) + (tx >> 2)) * 16 +
						   (ty & 3) * 4 + (tx & 3)],
				      &r, &g, &b, &a);
			if (a != 255)
				return false;
			gx_rgba8_pset(cell, w, i, j, r, g, b, a);
		}
	for (f = startX; f < endX; f += (float)w)
		nx++;
	for (f = startY; f < endY; f += (float)h)
		ny++;
	xc0 = x0 < 0 ? 0 : x0;
	yc0 = y0 < 0 ? 0 : y0;
	xc1 = x0 + nx * w < s->w ? x0 + nx * w : s->w;
	yc1 = y0 + ny * h < s->h ? y0 + ny * h : s->h;
	if (gx_usage && (uint32_t)tpagIndex < gx_usageCount)
		gx_usage[tpagIndex >> 3] |= (u8)(1 << (tpagIndex & 7));
	for (tr = 0; tr < s->tilesY; tr++)
		for (c = 0; c < s->tilesX; c++) {
			GxTile *t = gx_tile_at(s, c, tr);
			int ox = c * SURF_TILE_W, oy = tr * SURF_TILE_H;

			if (!t->texels)
				continue;
			for (by = 0; by < t->texH / 4; by++) {
				int y = oy + by * 4;

				if (y < yc0 || y >= yc1)
					continue;
				for (bx = 0; bx < t->texW / 4; bx++) {
					int x = ox + bx * 4;

					if (x < xc0 || x >= xc1)
						continue;
					memcpy((u8 *)t->texels + (by * (t->texW / 4) + bx) * 64,
					       cell + (((y - y0) / 4 % (h / 4)) * (w / 4) +
						       (x - x0) / 4 % (w / 4)) * 64, 64);
				}
			}
			t->objReady = false;
		}
	return true;
}

static void gxDrawSpriteTiled(Renderer *renderer, int32_t tpagIndex, float originX, float originY, float x, float y,
			      float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH,
			      uint32_t color, float alpha)
{
	DataWin *dw = renderer->dataWin;
	TexturePageItem *tpag;
	float ax = fabsf(xscale), ay = fabsf(yscale);
	float tileW, tileH, startX, endX, startY, endY, cx, cy, clipT, clipB;

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
	if (tileX)
		gx_tile_span(&startX, &endX, tileW, gx_clipL, gx_clipR, xscale < 0);
	gx_clip_rows(&clipT, &clipB);
	if (tileY)
		gx_tile_span(&startY, &endY, tileH, clipT, clipB, yscale < 0);
	if (tileX && tileY && gx_tile_pattern((GxRenderer *)renderer, tpagIndex, xscale, yscale, color, alpha,
					      startX, endX, startY, endY))
		return;
	for (cy = startY; cy < endY; cy += tileH) {
		for (cx = startX; cx < endX; cx += tileW)
			gxDrawSprite(renderer, tpagIndex, cx + originX * xscale, cy + originY * yscale,
				     originX, originY, xscale, yscale, 0, color, alpha);
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

static int surface_depth(const Runner *runner)
{
	int i, n = 0;

	for (i = 0; i < MAX_SURFACES; i++)
		if (runner->surfaceStack[i] != -1)
			n++;
	return n;
}

static bool gxSetRenderTarget(Renderer *renderer, int32_t surfaceID, bool implicitApplicationSurface)
{
	gx_stat.targets++;
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
	}
	/* Only the view's own stack entry draws through the camera; set_target of it again draws in surface space. */
	if (gx->viewActive && surfaceID == gx->viewSurf && surface_depth(renderer->runner) == 1)
		gx_view_ortho(gx);
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

/* shGameBoy as a TEV program. Compares add (d + (a > b ? c : 0)), so: stages 0-4 build
 * m = max(R,G,B) in reg0.a, stages 5-7 turn m > 42 / 126 / 210 into 0/255 masks, and each mask
 * lerps the color to the next ramp entry (vertex color, 4×4 texture, reg1, reg2). One pass. */
static bool gx_gb_begin(GxRenderer *gx)
{
	GXColor zero = { 0, 0, 0, 0 }, thr = { 42, 126, 210, 0 };
	GXColor col3 = { gx->gbColor[2][0], gx->gbColor[2][1], gx->gbColor[2][2], 0 };
	GXColor col4 = { gx->gbColor[3][0], gx->gbColor[3][1], gx->gbColor[3][2], 0 };
	static GXTexObj obj;
	int i;

	if (!gx->gbTexels) {
		gx->gbTexels = memalign(32, 32);
		if (!gx->gbTexels)
			return false;
	}
	for (i = 0; i < 16; i++)
		gx->gbTexels[i] = pack_rgb5a3(gx->gbColor[1][0], gx->gbColor[1][1], gx->gbColor[1][2], 255);
	DCFlushRange(gx->gbTexels, 32);
	GX_InitTexObj(&obj, gx->gbTexels, 4, 4, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
	if (gx_cur)
		gx_cur->texBound = false; /* the object may be the bound one */
	GX_InitTexObjFilterMode(&obj, GX_NEAR, GX_NEAR);
	GX_InvalidateTexAll();
	GX_LoadTexObj(&obj, GX_TEXMAP1);

	GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_RED);
	GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_GREEN);
	GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_BLUE);
	GX_SetTevColor(GX_TEVREG0, zero);
	GX_SetTevColor(GX_TEVREG1, col3);
	GX_SetTevColor(GX_TEVREG2, col4);
	GX_SetTevKColor(GX_KCOLOR0, thr);
	GX_SetNumTevStages(9);
	for (i = 0; i < 9; i++) {
		GX_SetTevOrder(i, GX_TEXCOORD0, i == 6 ? GX_TEXMAP1 : GX_TEXMAP0, GX_COLOR0A0);
		GX_SetTevSwapMode(i, GX_TEV_SWAP0,
				  i == 0 ? GX_TEV_SWAP1 : i == 1 ? GX_TEV_SWAP2 : i == 3 ? GX_TEV_SWAP3 : GX_TEV_SWAP0);
		GX_SetTevColorIn(i, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO);
		GX_SetTevColorOp(i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	}
	/* m: reg0.a = R; reg1.a = max(G - R, 0); reg0.a += reg1.a; reg1.a = max(B - reg0.a, 0); reg0.a += reg1.a */
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
	GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_A1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
	GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_A0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
	GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG1);
	GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_A1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
	GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVREG0);
	/* masks: prev.a = (m > threshold) ? vertex alpha (255) : 0; the next stage's color lerps by it */
	GX_SetTevKAlphaSel(GX_TEVSTAGE5, GX_TEV_KASEL_K0_R);
	GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_A0, GX_CA_KONST, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevColorIn(GX_TEVSTAGE6, GX_CC_RASC, GX_CC_TEXC, GX_CC_APREV, GX_CC_ZERO);
	GX_SetTevKAlphaSel(GX_TEVSTAGE6, GX_TEV_KASEL_K0_G);
	GX_SetTevAlphaIn(GX_TEVSTAGE6, GX_CA_A0, GX_CA_KONST, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE6, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevColorIn(GX_TEVSTAGE7, GX_CC_CPREV, GX_CC_C1, GX_CC_APREV, GX_CC_ZERO);
	GX_SetTevKAlphaSel(GX_TEVSTAGE7, GX_TEV_KASEL_K0_B);
	GX_SetTevAlphaIn(GX_TEVSTAGE7, GX_CA_A0, GX_CA_KONST, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE7, GX_TEV_COMP_A8_GT, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetTevColorIn(GX_TEVSTAGE8, GX_CC_CPREV, GX_CC_C2, GX_CC_APREV, GX_CC_ZERO);
	GX_SetTevAlphaIn(GX_TEVSTAGE8, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
	GX_SetTevAlphaOp(GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	return true;
}

static void gx_gb_end(GxRenderer *gx)
{
	GX_SetNumTevStages(1);
	GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
	gx_vtx_fmt();
	gx_unbind_tex(gx); /* GX_SetTevOp puts stage 0's color and alpha inputs back */
}

static bool gx_hud_half;

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
	bool gb = renderer->currentShader >= 0 && renderer->currentShader == gx->shGB;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return;
	s = &gx->surfs[surfaceID];
	if (!s->exists)
		return;
	s->lastUsed = gx->frameCounter;
	if (gx->currentTarget == surfaceID)
		gx_flush_target(gx);
	if (srcWidth <= 0)
		srcWidth = s->w;
	if (srcHeight <= 0)
		srcHeight = s->h;
	/* The HUD's two corner groups slide in from the sides as two halves: its middle is empty. */
	if (surfaceID == gx->guiSurf && !gx_hud_half && Overscan_marginH() > 0 && angleDeg == 0.0f) {
		int32_t half = srcWidth / 2, mh = Overscan_marginH();

		gx_hud_half = true;
		gxDrawSurfaceColor(renderer, surfaceID, srcLeft, srcTop, half, srcHeight, x + (float)mh * xscale, y,
				   xscale, yscale, 0.0f, c1, c2, c3, c4, alpha);
		gxDrawSurfaceColor(renderer, surfaceID, srcLeft + half, srcTop, srcWidth - half, srcHeight,
				   x + (float)(half - mh) * xscale, y, xscale, yscale, 0.0f, c1, c2, c3, c4, alpha);
		gx_hud_half = false;
		return;
	}
	if (surfaceID == gx->guiSurf)
		y += (float)WiiSafe_rows() * yscale;
	angleRad = -angleDeg * ((float)M_PI / 180.0f);
	Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);
	if (s->tiles) {
		int col, row;

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
	if (gb && gx_gb_begin(gx)) {
		const u8 *k = gx->gbColor[0];

		emit_quad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v0, u1, v1, u0, v1,
			  k[0], k[1], k[2], 255, k[0], k[1], k[2], 255, k[0], k[1], k[2], 255, k[0], k[1], k[2], 255);
		gx_gb_end(gx);
		return;
	}
	emit_quad(x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v0, u1, v1, u0, v1,
		  (u8)BGR_R(c1), (u8)BGR_G(c1), (u8)BGR_B(c1), a,
		  (u8)BGR_R(c2), (u8)BGR_G(c2), (u8)BGR_B(c2), a,
		  (u8)BGR_R(c3), (u8)BGR_G(c3), (u8)BGR_B(c3), a,
		  (u8)BGR_R(c4), (u8)BGR_G(c4), (u8)BGR_B(c4), a);
}

/* A line of the adjust pages' own text in the menus' font (AM2R keeps its fonts' indices in globals), centered on x
 * and outlined to read over any picture. */
static void gx_ov_text(GxRenderer *gx, const char *text, float x, float y, float scale)
{
	static const int8_t ring[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
					   { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
	static int32_t fontVar = -2;
	Renderer *r = &gx->base;
	int32_t oldFont = r->drawFont, oldHalign = r->drawHalign, oldValign = r->drawValign, font;
	GxTextPass passes[9];
	int i;

	if (!r->runner || !r->runner->vmContext)
		return;
	if (fontVar == -2)
		fontVar = Wii_varId(r->runner, "fontGUI2", INSTANCE_GLOBAL);
	if (fontVar < 0)
		return;
	font = RValue_toInt32(Instance_getSelfVar(r->runner->vmContext->globalScopeInstance, fontVar));
	for (i = 0; i < 8; i++)
		passes[i] = (GxTextPass){ ring[i][0], ring[i][1], true, 0x000000, 1.0f };
	passes[8] = (GxTextPass){ 0, 0, false, 0, 1.0f };
	r->drawFont = font;
	r->drawHalign = 1;
	r->drawValign = 0;
	gx_draw_text(gx, text, x, y, scale, scale, 0.0f, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, -1.0f, passes, 9);
	r->drawFont = oldFont;
	r->drawHalign = oldHalign;
	r->drawValign = oldValign;
}

/* Under the adjust pages in the pause screen: the game the player left (AM2R froze it without the HUD) and the HUD
 * drawn where the margins put it. Anywhere else the frozen picture is stale or missing. */
static bool gx_draw_preview(GxRenderer *gx, float LW, float LH, float sx, float sy, float viewW)
{
	Runner *runner = gx->base.runner;
	int32_t frozen = WiiSafe_screenSurface(runner);

	if (!WiiSafe_paused(runner) || frozen < 0 || (uint32_t)frozen >= gx->surfCount || !gx->surfs[frozen].exists)
		return false;
	gx_ov_rect(0, 0, LW, LH, 0, 0, 0, 255);
	gxDrawSurfaceColor(&gx->base, frozen, 0, 0, (int32_t)viewW, 240, 0.0f, 0.0f, sx, sy, 0.0f,
			   0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 1.0f);
	if (gx->guiSurf >= 0 && (uint32_t)gx->guiSurf < gx->surfCount && gx->surfs[gx->guiSurf].exists)
		gxDrawSurfaceColor(&gx->base, gx->guiSurf, 0, 0, 0, 0, 0.0f, 0.0f, sx, sy, 0.0f,
				   0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 1.0f);
	gx_unbind_tex(gx);
	return true;
}

/* The game's width in game pixels (320, or 426 in widescreen): AM2R sizes the HUD layer to it, while the pause
 * screen's own camera can be narrower. */
static float gx_view_width(GxRenderer *gx)
{
	Runner *runner = gx->base.runner;
	GMLCamera *cam = runner && runner->viewsEnabled ? Runner_getCameraById(runner, runner->views[0].cameraId) : NULL;

	if (gx->guiSurf >= 0 && (uint32_t)gx->guiSurf < gx->surfCount && gx->surfs[gx->guiSurf].exists)
		return (float)gx->surfs[gx->guiSurf].w;
	return cam && cam->viewWidth > 0 ? cam->viewWidth : 320.0f;
}

/* Resize/Move Screen: a grid and a bright border to line up against the TV's edge, plus the width and height
 * sliders. Drawn in the inset viewport. */
static void gx_draw_screen_page(GxRenderer *gx, float LW, float LH, float ys)
{
	float cx = LW * 0.5f, cy = LH * 0.5f;
	float tlen = 220.0f, tthk = 14.0f;
	float hf = 1.0f - (float)Overscan_h() / (float)OVERSCAN_MAX_H;
	float vf = 1.0f - (float)Overscan_v() / (float)OVERSCAN_MAX_V;
	float cl = 44.0f, ct = 7.0f;
	int i;

	gx_draw_preview(gx, LW, LH, LW / gx_view_width(gx), 2 * ys, gx_view_width(gx));
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

	/* Center lines: A with the d-pad moves the whole picture; the lines show where its middle sits. */
	gx_ov_rect(0, cy + (float)(Overscan_y() * (int)LH / WII_FB_H) - 1, LW, 2, 255, 255, 255, 200);
	gx_ov_rect(cx + (float)Overscan_x() - 1, 0, 2, LH, 255, 255, 255, 200);
	gx_ov_rect(cx - tlen * 0.5f, cy - 40 * ys, tlen, tthk * ys, 30, 30, 40, 230);
	gx_ov_rect(cx - tlen * 0.5f, cy - 40 * ys, tlen * hf, tthk * ys, 90, 200, 255, 255);
	gx_ov_rect(cx - tthk * 0.5f, cy + 12 * ys, tthk, tlen * 0.55f * ys, 30, 30, 40, 230);
	gx_ov_rect(cx - tthk * 0.5f, cy + 12 * ys, tthk, tlen * 0.55f * vf * ys, 255, 170, 90, 255);

	gx_ov_rect(cx - 200 * ys, cy - 120 * ys, 400 * ys, 68 * ys, 0, 0, 0, 190);
	gx_ov_text(gx, "Hold A to move screen", cx, cy - 120 * ys, 2 * ys);
	gx_ov_text(gx, WiiInput_pad() == WII_PAD_NUNCHUK ? "Press C to recenter" : "Press X to recenter", cx,
		   cy - 100 * ys, 2 * ys);
	gx_ov_text(gx, "Press B to exit", cx, cy - 80 * ys, 2 * ys);
}

/* HUD Margins: green lines where the HUD and prompts stop, over the paused game and its HUD when there is one. */
static void gx_draw_hud_page(GxRenderer *gx, float LW, float LH, float ys)
{
	float viewW = gx_view_width(gx), sx = LW / viewW, sy = 2 * ys;
	float mx = (float)Overscan_marginH() * sx, my = (float)Overscan_marginV() * sy;
	float cx = LW * 0.5f, cy = LH * 0.5f;

	if (!gx_draw_preview(gx, LW, LH, sx, sy, viewW))
		gx_ov_rect(0, 0, LW, LH, 0, 0, 0, 110);
	gx_ov_rect(mx, 0, 2, LH, 70, 255, 130, 255);
	gx_ov_rect(LW - mx - 2, 0, 2, LH, 70, 255, 130, 255);
	gx_ov_rect(0, my, LW, 2, 70, 255, 130, 255);
	gx_ov_rect(0, LH - my - 2, LW, 2, 70, 255, 130, 255);

	gx_ov_rect(cx - 260 * ys, cy - 20 * ys, 520 * ys, 48 * ys, 0, 0, 0, 190);
	gx_ov_text(gx, "Use the d-pad to move the HUD", cx, cy - 20 * ys, 2 * ys);
	gx_ov_text(gx, "Press B to exit", cx, cy, 2 * ys);
}

static void gx_draw_overscan_overlay(GxRenderer *gx)
{
	const float LW = (float)WiiVideo_fbWidth();
	const float LH = (float)WiiVideo_efbHeight();
	float ys = LH / (float)WII_FB_H; /* the overlay keeps its on-screen size in 240p */

	gx_ov_drawing = true;
	gx_bind_host(gx);
	gx_ortho(0, LH, 0, LW);
	gx_unbind_tex(gx);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);

	if (Overscan_page() == OVERSCAN_HUD)
		gx_draw_hud_page(gx, LW, LH, ys);
	else
		gx_draw_screen_page(gx, LW, LH, ys);

	gx_unbind_tex(gx);
	gx_apply_blend(gx);
	GX_SetColorUpdate(gx->colorWriteR || gx->colorWriteG || gx->colorWriteB);
	GX_SetAlphaUpdate(gx->colorWriteA);
	gx_ov_drawing = false;
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
	gx_surf_release(gx, s);
	s->w = width;
	s->h = height;
	s->objReady = false;
	tiled = (width > WII_FB_W || height > WII_FB_H);
	if (tiled) {
		if (!gx_alloc_tiles(gx, s, width, height))
			s->exists = false;
		return;
	}
	{
		int tw = pad4(width);
		int th = pad4(height);
		u16 *texels = gx_surf_memalign(gx, surf_bytes(tw, th));

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

/* Sweep-only. The first call starts the recording, so nothing is tracked on a real card. */
uint8_t *GxRenderer_tpagUsage(uint32_t *bytes)
{
	*bytes = gx_cur ? (gx_cur->base.dataWin->tpag.count + 7) / 8 : 0;
	if (!gx_usage && *bytes) {
		gx_usage = calloc(*bytes, 1);
		gx_usageCount = gx_cur->base.dataWin->tpag.count;
	}
	*bytes = (gx_usageCount + 7) / 8;
	return gx_usage;
}

void GxRenderer_memStats(uint32_t *surfCount, uint32_t *surfBytes, uint32_t *pageBytes)
{
	GxRenderer *gx = gx_cur;
	uint32_t i;
	int32_t t;

	*surfCount = *surfBytes = *pageBytes = 0;
	if (!gx)
		return;
	for (i = 0; i < gx->surfCount; i++) {
		GxSurf *sf = &gx->surfs[i];

		if (!sf->exists)
			continue;
		(*surfCount)++;
		if (sf->texels)
			*surfBytes += surf_bytes(sf->texW, sf->texH);
		for (t = 0; sf->tiles && t < sf->tilesX * sf->tilesY; t++)
			*surfBytes += surf_bytes(sf->tiles[t].texW, sf->tiles[t].texH);
	}
	for (i = 0; i < gx->pageCount; i++)
		if (gx->pages[i].texels)
			*pageBytes += tex_bytes(gx->pages[i].texW, gx->pages[i].texH);
}

static void gxSurfaceFree(Renderer *renderer, int32_t surfaceID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return;
	if (surfaceID == renderer->runner->applicationSurfaceId)
		return;
	gx_surf_release(gx, &gx->surfs[surfaceID]);
	memset(&gx->surfs[surfaceID], 0, sizeof(GxSurf));
}

static void gxSurfaceCopy(Renderer *renderer, int32_t destSurfaceID, int32_t destX, int32_t destY, int32_t srcSurfaceID,
			  int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, bool part)
{
	GxRenderer *gx = (GxRenderer *)renderer;
	GxSurf *src, *dest;
	int32_t saved = gx->currentTarget;

	if (srcSurfaceID < 0 || destSurfaceID < 0)
		return;
	if ((uint32_t)srcSurfaceID >= gx->surfCount || (uint32_t)destSurfaceID >= gx->surfCount)
		return;
	src = &gx->surfs[srcSurfaceID];
	dest = &gx->surfs[destSurfaceID];
	if (!src->exists || !dest->exists)
		return;
	src->lastUsed = dest->lastUsed = gx->frameCounter;
	if ((!src->texels && !src->tiles) || (!dest->texels && !dest->tiles))
		return;

	/* Step-time copy (water) runs after beginFrame rebound the app surface without
	 * restore. Flushing then would stamp the host 2× blit over last view's texels. */
	if (gx->currentTarget == srcSurfaceID && gx->targetDirty)
		gx_flush_target(gx);

	if (!part && destX == 0 && destY == 0 && src->w == dest->w && src->h == dest->h &&
	    src->texels && dest->texels && src->texW == dest->texW && src->texH == dest->texH) {
		gx_share(gx, srcSurfaceID, destSurfaceID);
		return;
	}

	if (!gx_bind_surface(gx, destSurfaceID, destX != 0 || destY != 0 || part))
		return;
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	gxDrawSurface(renderer, srcSurfaceID, part ? srcX : 0, part ? srcY : 0, part ? srcW : src->w,
		      part ? srcH : src->h, (float)destX, (float)destY, 1.0f, 1.0f, 0, 0xFFFFFF, 1.0f);
	gx_apply_blend(gx);
	gx_flush_target(gx);
	if (saved == HOST_TARGET)
		gx_bind_host(gx);
	else if (saved >= 0)
		gx_bind_surface(gx, saved, true);
}

static bool gxSurfaceGetPixels(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t surfaceID,
			       MAYBE_UNUSED uint8_t *outRGBA)
{
	return false;
}

/* surface_getpixel_ext: one texel of a surface. 1.5.5 does this nine times at boot on a 4×4
 * surface to learn its morph-trail colors; a live target is copied out first. */
bool GxRenderer_surfacePixel(int32_t surfaceID, int x, int y, uint8_t rgba[4])
{
	GxRenderer *gx = gx_cur;
	GxSurf *s;

	if (!gx || surfaceID < 0 || (uint32_t)surfaceID >= gx->surfCount)
		return false;
	s = &gx->surfs[surfaceID];
	if (!s->exists || (!s->tiles && !s->texels) || x < 0 || y < 0 || x >= s->w || y >= s->h)
		return false;
	if (s->tiles) {
		/* Drawn on the CPU, so its texels are already current. */
		GxTile *t = gx_tile_at(s, x / SURF_TILE_W, y / SURF_TILE_H);

		if (!t->texels)
			return false;
		gx_rgba8_pget((const u8 *)t->texels, t->texW, x % SURF_TILE_W, y % SURF_TILE_H,
			      &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
		return true;
	}
	if (gx->currentTarget == surfaceID) {
		/* Still the live target: peek the EFB itself (a 4×4 copy came back empty in Dolphin). */
		GXColor c;

		GX_DrawDone();
		GX_PeekARGB((u16)x, (u16)y, &c);
		rgba[0] = c.r;
		rgba[1] = c.g;
		rgba[2] = c.b;
		rgba[3] = c.a;
		return true;
	}
	GX_DrawDone(); /* the CPU reads the copy made at the last target switch, so wait for it */
	DCInvalidateRange(s->texels, surf_bytes(s->texW, s->texH));
	gx_rgba8_pget((const u8 *)s->texels, s->texW, x, y, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
	return true;
}

static void gxDrawTiledPart(Renderer *renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW,
			    int32_t srcH, float dstX, float dstY, float dstW, float dstH, uint32_t color, float alpha)
{
	gx_stat.tiles++;
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
				gxDrawSpritePart(renderer, tpagIndex, srcX, srcY, w, h, x, y, 1, 1, 0, x, y,
						 color, alpha);
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

/* Handles are just names we recognize: 1 = palette_index; 11..14 = the GameBoy ramp colors. */
enum { UNI_PAL_INDEX = 1, UNI_GB_COLOR1 = 11 };

static int32_t gxShaderGetUniform(Renderer *renderer, int32_t shaderIndex, char *uniform)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (!uniform)
		return -1;
	if (shaderIndex == gx->shPal && strcmp(uniform, "palette_index") == 0)
		return UNI_PAL_INDEX;
	if (shaderIndex == gx->shGB && strncmp(uniform, "u_color", 7) == 0 && uniform[7] >= '1' && uniform[7] <= '4')
		return UNI_GB_COLOR1 + (uniform[7] - '1');
	return 0;
}

static int32_t gxShaderGetSamplerIndex(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t shaderIndex,
				       MAYBE_UNUSED char *uniform)
{
	return 0;
}

static void gxShaderSetUniformF(Renderer *renderer, int32_t handle, MAYBE_UNUSED int32_t count,
				float v1, MAYBE_UNUSED float v2, MAYBE_UNUSED float v3, MAYBE_UNUSED float v4)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	if (handle == UNI_PAL_INDEX)
		gx->palIndex = (int)v1;
	else if (handle >= UNI_GB_COLOR1 && handle < UNI_GB_COLOR1 + 4) {
		u8 *c = gx->gbColor[handle - UNI_GB_COLOR1];

		c[0] = (u8)v1;
		c[1] = (u8)v2;
		c[2] = (u8)v3;
	}
}

static void gxShaderSetUniformFArray(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle,
				     MAYBE_UNUSED float *values, MAYBE_UNUSED uint32_t count)
{
}

static void gxShaderSetUniformI(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED int32_t handle,
				MAYBE_UNUSED int32_t count, MAYBE_UNUSED int32_t v1, MAYBE_UNUSED int32_t v2,
				MAYBE_UNUSED int32_t v3, MAYBE_UNUSED int32_t v4)
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

/* Only sprite textures (ids from gxSpriteGetTexture) can be a palette; a surface id is ignored. */
static void gxTextureSetStage(Renderer *renderer, MAYBE_UNUSED int32_t slot, uint32_t texID)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	gx->palTpag = (texID >= 1 && texID < 0x10000) ? (int32_t)texID - 1 : -1;
}

static bool gxShaderIsCompiled(Renderer *renderer, int32_t shader)
{
	GxRenderer *gx = (GxRenderer *)renderer;

	return shader >= 0 && (shader == gx->shPal || shader == gx->shGB);
}

static bool gx_shaders_off;

void GxRenderer_setShadersOff(bool off)
{
	gx_shaders_off = off;
}

/* The game wants both of its shaders "compiled" before it uses either. */
static bool gxShadersSupported(void)
{
	return !gx_shaders_off && gx_cur && gx_cur->shPal >= 0 && gx_cur->shGB >= 0;
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
	gx->targetDirty = true;
	gx->primCount = 0;
}

static void gxDrawVertexBuffer(MAYBE_UNUSED Renderer *renderer, MAYBE_UNUSED VertexBuffer *buffer,
			       MAYBE_UNUSED int32_t primitive, MAYBE_UNUSED int32_t texture,
			       MAYBE_UNUSED int32_t offset, MAYBE_UNUSED int32_t count)
{
}

Renderer *GxRenderer_create(void)
{
	GxRenderer *gx = (GxRenderer *)safeCalloc(1, sizeof(GxRenderer));

	gx->base.vtable = &gxVtable;
	/* GameMaker's draw state starts opaque white; 1.5.5 draws before its first draw_set_alpha. */
	gx->base.drawColor = 0xFFFFFF;
	gx->base.drawAlpha = 1.0f;
	gx->base.drawFont = -1;
	gx->base.circlePrecision = 24;
	gxVtable.init = gxInit;
	gxVtable.destroy = gxDestroy;
	gxVtable.beginFrame = gxBeginFrame;
	gxVtable.endFrameInit = gxEndFrameInit;
	gxVtable.endFrameEnd = gxEndFrameEnd;
	gxVtable.beginView = gxBeginView;
	gxVtable.endView = gxEndView;
	gxVtable.applyProjection = gxApplyViewProjection;
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
	gxVtable.drawTile = gxDrawTile;
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
