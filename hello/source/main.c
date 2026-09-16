#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>
#include <gccore.h>
#include <ogc/conf.h>
#include <ogc/pad.h>
#include <aesndlib.h>
#include <wiiuse/wpad.h>

#define FIFO_SIZE       (256 * 1024)
#define SURF_W          320
#define SURF_H          240
#define CELL            16
#define BEEP_HZ         440.0f
#define BEEP_RATE       44100
#define BEEP_MS         200
#define HOME_HOLD_FRAMES 60

#define RGB565(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define COL_NAVY   RGB565(24, 40, 72)
#define COL_SAND   RGB565(232, 208, 128)
#define COL_ORANGE RGB565(224, 120, 32)
#define COL_WHITE  RGB565(255, 255, 255)
#define COL_BLACK  RGB565(0, 0, 0)

static void *xfb[2];
static u32 fb;
static GXRModeObj *rmode;
static GXTexObj tex_obj;
static u16 *texels;
static AESNDPB *voice;
static s16 *beep_pcm;
static u32 beep_bytes;
static u32 home_held;

/* 8×8, bit7 = left. Indices: 0=' ', 1-26=A-Z, 27-36=0-9. */
static const u8 font8[][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	{0x3C,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},
	{0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00},
	{0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00},
	{0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00},
	{0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00},
	{0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00},
	{0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00},
	{0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},
	{0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00},
	{0x1E,0x04,0x04,0x04,0x04,0x44,0x38,0x00},
	{0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00},
	{0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00},
	{0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00},
	{0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00},
	{0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
	{0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00},
	{0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00},
	{0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00},
	{0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00},
	{0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
	{0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
	{0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00},
	{0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00},
	{0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00},
	{0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x00},
	{0x7E,0x02,0x04,0x18,0x20,0x40,0x7E,0x00},
	{0x3C,0x42,0x46,0x5A,0x62,0x42,0x3C,0x00},
	{0x08,0x18,0x08,0x08,0x08,0x08,0x3E,0x00},
	{0x3C,0x42,0x02,0x0C,0x30,0x40,0x7E,0x00},
	{0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00},
	{0x04,0x0C,0x14,0x24,0x7E,0x04,0x04,0x00},
	{0x7E,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00},
	{0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00},
	{0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x00},
	{0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00},
	{0x3C,0x42,0x42,0x42,0x3E,0x02,0x3C,0x00},
};

static int glyph_index(char c)
{
	if (c == ' ')
		return 0;
	if (c >= 'A' && c <= 'Z')
		return 1 + (c - 'A');
	if (c >= 'a' && c <= 'z')
		return 1 + (c - 'a');
	if (c >= '0' && c <= '9')
		return 27 + (c - '0');
	return 0;
}

static void tex_put(int x, int y, u16 c)
{
	int tx, ty, off;

	if ((unsigned)x >= SURF_W || (unsigned)y >= SURF_H)
		return;
	tx = x >> 2;
	ty = y >> 2;
	off = (ty * (SURF_W >> 2) + tx) * 16 + (y & 3) * 4 + (x & 3);
	texels[off] = c;
}

static void tex_glyph(int x, int y, char ch, u16 fg, u16 bg)
{
	const u8 *g = font8[glyph_index(ch)];
	int row, col;

	for (row = 0; row < 8; row++) {
		for (col = 0; col < 8; col++) {
			u16 c = (g[row] & (0x80 >> col)) ? fg : bg;
			if (c != bg || bg != 0xFFFF)
				tex_put(x + col, y + row, c);
		}
	}
}

static void tex_text(int x, int y, const char *s, u16 fg)
{
	int i;

	/* 1px black outline so glyphs read on both checker cells. */
	for (i = 0; s[i]; i++) {
		int gx = x + i * 8;
		int row, col;
		const u8 *g = font8[glyph_index(s[i])];

		for (row = 0; row < 8; row++) {
			for (col = 0; col < 8; col++) {
				if (g[row] & (0x80 >> col)) {
					tex_put(gx + col - 1, y + row, COL_BLACK);
					tex_put(gx + col + 1, y + row, COL_BLACK);
					tex_put(gx + col, y + row - 1, COL_BLACK);
					tex_put(gx + col, y + row + 1, COL_BLACK);
				}
			}
		}
		tex_glyph(gx, y, s[i], fg, 0xFFFF);
	}
}

static void fill_surface(void)
{
	int x, y;

	for (y = 0; y < SURF_H; y++) {
		for (x = 0; x < SURF_W; x++) {
			int cell = ((x / CELL) + (y / CELL)) & 1;
			u16 c = cell ? COL_SAND : COL_NAVY;

			if (x == 0 || y == 0 || x == SURF_W - 1 || y == SURF_H - 1)
				c = COL_ORANGE;
			tex_put(x, y, c);
		}
	}
	tex_text((SURF_W - 7 * 8) / 2, 8, "AM2R GX", COL_WHITE);
	tex_text((SURF_W - 18 * 8) / 2, SURF_H - 16, "A BEEP  HOME QUIT", COL_WHITE);
	DCFlushRange(texels, SURF_W * SURF_H * sizeof(u16));
}

static GXRModeObj *pick_mode(void)
{
	GXRModeObj *m = VIDEO_GetPreferredMode(NULL);

	/* Pal576*Scale resamples 480→576 and blurs exact 2× pixel art. */
	if (m->xfbHeight > 480) {
		if (CONF_GetProgressiveScan() && VIDEO_HaveComponentCable())
			m = &TVEurgb60Hz480Prog;
		else
			m = &TVEurgb60Hz480IntDf;
	}
	return m;
}

static void make_beep(void)
{
	int n = (BEEP_RATE * BEEP_MS) / 1000;
	int i;
	u32 bytes;

	n = (n + 15) & ~15;
	bytes = (u32)n * sizeof(s16);
	beep_pcm = memalign(32, bytes);
	if (!beep_pcm)
		return;
	memset(beep_pcm, 0, bytes);
	for (i = 0; i < n; i++) {
		float t = (float)i / (float)BEEP_RATE;
		float env = 1.0f;
		if (i < 400)
			env = (float)i / 400.0f;
		else if (i > n - 800)
			env = (float)(n - i) / 800.0f;
		beep_pcm[i] = (s16)(sinf(2.0f * (float)M_PI * BEEP_HZ * t) * 18000.0f * env);
	}
	DCFlushRange(beep_pcm, bytes);
	beep_bytes = bytes;
}

static void play_beep(void)
{
	if (!voice || !beep_pcm)
		return;
	AESND_PlayVoice(voice, VOICE_MONO16, beep_pcm, beep_bytes, BEEP_RATE, 0, false);
}

static void draw_quad(void)
{
	Mtx mv;
	Mtx44 proj;

	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	GX_InvVtxCache();
	GX_InvalidateTexAll();

	guOrtho(proj, 0, 480, 0, 640, 0, 300);
	GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
	guMtxIdentity(mv);
	guMtxTransApply(mv, mv, 0.0f, 0.0f, -5.0f);
	GX_LoadPosMtxImm(mv, GX_PNMTX0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

	GX_SetNumChans(1);
	GX_SetNumTexGens(1);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

	GX_InitTexObj(&tex_obj, texels, SURF_W, SURF_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjFilterMode(&tex_obj, GX_NEAR, GX_NEAR);
	GX_LoadTexObj(&tex_obj, GX_TEXMAP0);

	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
	GX_Position2f32(0.0f, 0.0f);
	GX_TexCoord2f32(0.0f, 0.0f);
	GX_Position2f32(640.0f, 0.0f);
	GX_TexCoord2f32(1.0f, 0.0f);
	GX_Position2f32(640.0f, 480.0f);
	GX_TexCoord2f32(1.0f, 1.0f);
	GX_Position2f32(0.0f, 480.0f);
	GX_TexCoord2f32(0.0f, 1.0f);
	GX_End();
}

static u32 down_a(void)
{
	u32 w = WPAD_ButtonsDown(0);
	u16 p = PAD_ButtonsDown(0);

	if (w & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A))
		return 1;
	if (p & PAD_BUTTON_A)
		return 1;
	return 0;
}

static u32 held_home(void)
{
	u32 w = WPAD_ButtonsHeld(0);

	if (w & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
		return 1;
	return 0;
}

int main(int argc, char **argv)
{
	void *fifo;
	f32 yscale;
	u32 xfbHeight;
	GXColor clear = {0, 0, 0, 255};
	int first = 1;
	int aspect;

	(void)argc;
	(void)argv;

	VIDEO_Init();
	WPAD_Init();
	PAD_Init();

	rmode = pick_mode();
	aspect = CONF_GetAspectRatio();
	printf("AM2R GX hello  mode %ux%u xfb=%u aspect=%s\n",
	       rmode->fbWidth, rmode->efbHeight, rmode->xfbHeight,
	       aspect == CONF_ASPECT_16_9 ? "16:9 (ignored)" : "4:3");

	fb = 0;
	xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb[fb]);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (rmode->viTVMode & VI_NON_INTERLACE)
		VIDEO_WaitVSync();
	fb ^= 1;

	fifo = memalign(32, FIFO_SIZE);
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_SetCopyClear(clear, 0x00ffffff);
	GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
	yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
	xfbHeight = GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
	GX_SetDispCopyDst(rmode->fbWidth, xfbHeight);
	GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_FALSE, rmode->vfilter);
	GX_SetFieldMode(rmode->field_rendering,
			((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
	if (rmode->aa)
		GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
	else
		GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetCullMode(GX_CULL_NONE);
	GX_CopyDisp(xfb[fb], GX_TRUE);
	GX_SetDispCopyGamma(GX_GM_1_0);

	texels = memalign(32, SURF_W * SURF_H * sizeof(u16));
	if (!texels)
		return 1;
	fill_surface();

	AESND_Init();
	voice = AESND_AllocateVoice(NULL);
	make_beep();
	play_beep();

	while (SYS_MainLoop()) {
		WPAD_ScanPads();
		PAD_ScanPads();

		if (down_a())
			play_beep();

		if (held_home())
			home_held++;
		else
			home_held = 0;
		if (home_held >= HOME_HOLD_FRAMES)
			exit(0);

		draw_quad();
		GX_DrawDone();
		GX_SetColorUpdate(GX_TRUE);
		GX_CopyDisp(xfb[fb], GX_TRUE);
		VIDEO_SetNextFramebuffer(xfb[fb]);
		if (first) {
			VIDEO_SetBlack(false);
			first = 0;
		}
		VIDEO_Flush();
		VIDEO_WaitVSync();
		fb ^= 1;
	}
	return 0;
}
