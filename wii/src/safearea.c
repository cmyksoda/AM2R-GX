#include "safearea.h"

#include "am2r.h"
#include "assets.h"
#include "controller_art.h"
#include "overscan.h"
#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_image.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

#include <string.h>

/* AM2R's prompt rows sit at view_yview + 212 and + 225; the pause map's status bar ends above 205. */
#define ROW_UPPER 205
#define ROW_LOWER 220
#define NOTIFY_HIDDEN 30 /* oNotification's offset when it is fully slid out */
#define TITLE_MID 120 /* the title's version line is above, its links below */
#define SCORE_BOTTOM 200 /* the score screen's closing lines sit on row 220 */
#define SAFE_BOTTOM (240 - WiiSafe_rows())
#define PROMPT_EDGE 8 /* prompts this close to a side are corner prompts (AM2R puts them at 2 and 318) */
#define PORT_LINK "github.com/cmyksoda/AM2R-GX"
#define LINK_TOP 3 /* the menu font's letters start 3 rows lower than the blog picture's */

enum { SCR_1BUTTON, SCR_2BUTTONS, SCR_COUNT };
static const char *const scriptNames[SCR_COUNT] = { "draw_txt_1button", "draw_txt_2buttons" };
static int32_t scriptCode[SCR_COUNT] = { -1, -1 };

static Instance *first_of(Runner *runner, int32_t obj)
{
	Instance **list;

	if (obj < 0 || !runner->instancesByExactObject || !(list = runner->instancesByExactObject[obj]) ||
	    !arrlen(list))
		return NULL;
	return list[0];
}

/* Game rows the HUD and prompts keep clear of the top and bottom edges: the player's HUD margin. */
int WiiSafe_rows(void)
{
	return Overscan_marginV();
}

static int32_t control_surface(Runner *runner, const char *name, int32_t *var)
{
	static int32_t obj = -2;
	Instance *control;
	RValue v;

	if (!runner)
		return -1;
	if (obj == -2)
		obj = Wii_objectIndex(runner, "oControl");
	if (*var == -2)
		*var = Wii_varId(runner, name, INSTANCE_SELF);
	if (*var < 0 || !(control = first_of(runner, obj)))
		return -1;
	v = Instance_getSelfVar(control, *var);
	return v.type == RVALUE_UNDEFINED ? -1 : RValue_toInt32(v);
}

/* The surface oControl paints the HUD into every step (1.5.5), or -1. */
int32_t WiiSafe_guiSurface(Runner *runner)
{
	static int32_t var = -2;

	return control_surface(runner, "gui_surface", &var);
}

/* The game view oControl froze when the player paused, without the HUD, or -1. */
int32_t WiiSafe_screenSurface(Runner *runner)
{
	static int32_t var = -2;

	return control_surface(runner, "screen_surface", &var);
}

/* A game is being played: AM2R paints the HUD layer only then. */
bool WiiSafe_ingame(Runner *runner)
{
	static int32_t var = -2;

	if (!runner || !runner->vmContext)
		return false;
	if (var == -2)
		var = Wii_varId(runner, "ingame", INSTANCE_GLOBAL);
	return var >= 0 && RValue_toReal(Instance_getSelfVar(runner->vmContext->globalScopeInstance, var)) == 1;
}

/* The pause screen, where screen_surface holds the game the player left. */
bool WiiSafe_paused(Runner *runner)
{
	static int32_t subscreen = -2;

	if (!runner)
		return false;
	if (subscreen == -2)
		subscreen = Wii_roomIndex(runner, "rm_subscreen");
	return subscreen >= 0 && runner->currentRoomIndex == subscreen;
}

static bool global_on(Runner *runner, const char *name, int32_t *var)
{
	if (*var == -2)
		*var = Wii_varId(runner, name, INSTANCE_GLOBAL);
	return *var >= 0 && RValue_toReal(Instance_getSelfVar(runner->vmContext->globalScopeInstance, *var)) != 0;
}

/* Where the HUD layer's right group (the map and the Metroid count) starts, or the layer's width when it shows neither:
 * draw_gui puts it at 250 + widescreen_space, past where its left group can reach. */
int32_t WiiSafe_hudSplit(Runner *runner, int32_t width)
{
	static int32_t map = -2, count = -2, space = -2;
	int32_t x;

	if (!runner || !runner->vmContext)
		return width;
	if (!global_on(runner, "ophudshowmap", &map) && !global_on(runner, "ophudshowmetrcount", &count))
		return width;
	x = control_surface(runner, "widescreen_space", &space);
	x = 250 + (x > 0 ? x : 0);
	return x < width ? x : width;
}

/* How far up to draw what the current instance draws. The notification box lifts as it slides in, so a box that is
 * slid out stays below the screen. */
float WiiSafe_lift(Runner *runner)
{
	static int32_t notify = -2, popup, scanning, scanMonster, intro, offset;
	Instance *inst = runner && runner->vmContext ? runner->vmContext->currentInstance : NULL;
	float off;

	if (!inst)
		return 0.0f;
	if (notify == -2) {
		notify = Wii_objectIndex(runner, "oNotification");
		popup = Wii_objectIndex(runner, "oPopupText");
		scanning = Wii_objectIndex(runner, "oScanning");
		scanMonster = Wii_objectIndex(runner, "oScanMonster");
		intro = Wii_objectIndex(runner, "oIntroController");
		offset = Wii_varId(runner, "offset", INSTANCE_SELF);
	}
	if (inst->objectIndex == popup || inst->objectIndex == scanning || inst->objectIndex == scanMonster ||
	    inst->objectIndex == intro)
		return -WiiSafe_rows();
	if (inst->objectIndex != notify || offset < 0)
		return 0.0f;
	off = (float)RValue_toReal(Instance_getSelfVar(inst, offset));
	if (off < 0.0f)
		off = 0.0f;
	if (off > NOTIFY_HIDDEN)
		off = NOTIFY_HIDDEN;
	return -WiiSafe_rows() * (NOTIFY_HIDDEN - off) / NOTIFY_HIDDEN;
}

/* How far right to draw what the current instance draws: the notification box sits in the bottom-left corner. */
float WiiSafe_shift(Runner *runner)
{
	static int32_t notify = -2;
	Instance *inst = runner && runner->vmContext ? runner->vmContext->currentInstance : NULL;

	if (!inst)
		return 0.0f;
	if (notify == -2)
		notify = Wii_objectIndex(runner, "oNotification");
	return inst->objectIndex == notify ? (float)Overscan_marginH() : 0.0f;
}

#define CONTROLLER_ART_ROWS 192 /* bgController's legal lines start below this row */

/* Asophia's Classic Controller Pro over black: it hides bgController's Xbox pad and leaves its legal lines showing. */
static int32_t controller_art(DataWin *dw)
{
	int w, h, n, i;
	uint8_t *rgba = stbi_load_from_memory(controller_art_png, (int)controller_art_png_len, &w, &h, &n, 4);
	int32_t bg;

	if (!rgba)
		return -1;
	if (h > CONTROLLER_ART_ROWS)
		h = CONTROLLER_ART_ROWS;
	for (i = 0; i < w * h; i++) {
		uint8_t *p = rgba + (size_t)i * 4;

		p[0] = (uint8_t)(p[0] * p[3] / 255);
		p[1] = (uint8_t)(p[1] * p[3] / 255);
		p[2] = (uint8_t)(p[2] * p[3] / 255);
		p[3] = 255;
	}
	bg = WiiAssets_addBackground(dw, rgba, w, h);
	stbi_image_free(rgba);
	return bg;
}

/* The first screen is one picture whose legal lines reach row 236; it rises with its caption (oIntroController). */
void WiiSafe_frame(Runner *runner)
{
	static int32_t controllerRoom = -2, art = -2;
	RuntimeBackground *pic, *top;

	if (!runner)
		return;
	if (controllerRoom == -2)
		controllerRoom = Wii_roomIndex(runner, "rm_controller");
	if (runner->currentRoomIndex != controllerRoom)
		return;
	if (art == -2)
		art = controller_art(runner->dataWin);
	pic = &runner->backgrounds[0];
	pic->y = (float)-WiiSafe_rows();
	if (art < 0)
		return;
	top = &runner->backgrounds[1];
	*top = *pic;
	top->backgroundIndex = art;
	top->x = 0.0f;
	top->tileX = top->tileY = false;
}

static GMLCamera *view0(Runner *runner)
{
	return runner->viewsEnabled ? Runner_getCameraById(runner, runner->views[0].cameraId) : NULL;
}

static float view_top(Runner *runner)
{
	GMLCamera *cam = view0(runner);

	return cam ? cam->viewY : 0.0f;
}

static float view_mid(Runner *runner)
{
	GMLCamera *cam = view0(runner);

	return cam ? cam->viewX + cam->viewWidth / 2.0f : 160.0f;
}

/* A corner prompt's x moved in by the side margin; a centered one stays. */
static float prompt_x(Runner *runner, float x)
{
	GMLCamera *cam = view0(runner);
	float left = cam ? cam->viewX : 0.0f, width = cam ? cam->viewWidth : 320.0f;

	if (x - left < PROMPT_EDGE)
		return x + (float)Overscan_marginH();
	if (left + width - x < PROMPT_EDGE)
		return x - (float)Overscan_marginH();
	return x;
}

/* Where two prompt rows would not both fit: the upper row's item goes to the middle and the bottom row's
 * prompts join it in their own columns. A bottom row on its own just moves up. */
static struct {
	Instance *inst;
	int frame;
	float y;
} row = { NULL, -1, 0.0f };

static void row_seen(VMContext *ctx, float y)
{
	row.inst = ctx->currentInstance;
	row.frame = ctx->runner->frameCount;
	row.y = y;
}

static bool row_open(VMContext *ctx)
{
	return row.inst == ctx->currentInstance && row.frame == ctx->runner->frameCount;
}

/* draw_txt_*(x, y, text, align (0 centers on x), ...) */
static RValue draw_prompt(VMContext *ctx, int which, RValue *args, int32_t argCount)
{
	float y, rel;

	if (scriptCode[which] < 0)
		return RValue_makeUndefined();
	if (argCount > 3) {
		y = (float)RValue_toReal(args[1]);
		rel = y - view_top(ctx->runner);
		if (rel >= ROW_UPPER && rel < ROW_LOWER) {
			args[0] = RValue_makeReal(view_mid(ctx->runner));
			args[3] = RValue_makeReal(0.0);
			row_seen(ctx, y);
		} else if (rel >= ROW_LOWER) {
			args[1] = RValue_makeReal(row_open(ctx) ? row.y : y - WiiSafe_rows());
		}
		args[0] = RValue_makeReal(prompt_x(ctx->runner, (float)RValue_toReal(args[0])));
	}
	return VM_callCodeIndex(ctx, scriptCode[which], args, argCount);
}

static RValue wii_draw_txt_1button(VMContext *ctx, RValue *args, int32_t argCount)
{
	return draw_prompt(ctx, SCR_1BUTTON, args, argCount);
}

static RValue wii_draw_txt_2buttons(VMContext *ctx, RValue *args, int32_t argCount)
{
	return draw_prompt(ctx, SCR_2BUTTONS, args, argCount);
}

/* The inventory's "[arrows] - Select item" row is a background plus a draw_text, not a prompt script: hold the arrows
 * until the text arrives, then center the pair. */
static BuiltinFunc engineDrawBackground, engineDrawText, engineStringWidth, engineStringHeight, engineDrawRectangle;
static BuiltinFunc engineDrawBackgroundExt, engineDrawTextColour, engineDrawRectangleColour;
static int32_t introText = -2, introFull = -1;
static int32_t tipArrow = -2, subscreenFg = -2, optionsFg = -2, scoreScreen = -2, titleBg = -2,
	       bgVersion = -1, bgUrl = -1;
static struct {
	bool held;
	RValue args[3];
} arrow;

static bool in_object(VMContext *ctx, int32_t obj)
{
	return ctx->currentInstance && ctx->currentInstance->objectIndex == obj;
}

static bool in_subscreen_fg(VMContext *ctx)
{
	return in_object(ctx, subscreenFg);
}

/* The options menus' tip bar: its text moves up and the bar grows up to meet it, still covering the list that scrolls
 * beneath it down to the screen's edge. */
static RValue wii_draw_rectangle(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue lifted[5];

	if (argCount != 5 || !in_object(ctx, optionsFg))
		return engineDrawRectangle(ctx, args, argCount);
	memcpy(lifted, args, sizeof lifted);
	lifted[1] = RValue_makeReal(RValue_toReal(args[1]) - WiiSafe_rows());
	return engineDrawRectangle(ctx, lifted, 5);
}

static RValue wii_draw_background(VMContext *ctx, RValue *args, int32_t argCount)
{
	if (argCount == 3 && tipArrow >= 0 && RValue_toInt32(args[0]) == tipArrow && in_subscreen_fg(ctx)) {
		memcpy(arrow.args, args, sizeof arrow.args);
		arrow.held = true;
		return RValue_makeUndefined();
	}
	return engineDrawBackground(ctx, args, argCount);
}

static RValue wii_draw_text(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue w, moved[3];
	float left, right, dx;

	if (argCount == 3 && in_object(ctx, introText) && introFull >= 0) {
		RValue full = Instance_getSelfVar(ctx->currentInstance, introFull);
		double over = RValue_toReal(args[1]) + RValue_toReal(engineStringHeight(ctx, &full, 1)) - SAFE_BOTTOM;

		/* A long scene (Italian's six lines) rises as a whole, by the full scene's height,
		 * not the typed part's. */
		if (over > 0) {
			memcpy(moved, args, sizeof moved);
			moved[1] = RValue_makeReal(RValue_toReal(args[1]) - over);
			return engineDrawText(ctx, moved, 3);
		}
	}
	if (argCount == 3 && (in_object(ctx, optionsFg) ||
			      (in_object(ctx, scoreScreen) && RValue_toReal(args[1]) >= SCORE_BOTTOM))) {
		memcpy(moved, args, sizeof moved);
		moved[1] = RValue_makeReal(RValue_toReal(args[1]) - WiiSafe_rows());
		return engineDrawText(ctx, moved, 3);
	}
	if (!arrow.held || argCount != 3 || !in_subscreen_fg(ctx))
		return engineDrawText(ctx, args, argCount);
	arrow.held = false;
	w = engineStringWidth(ctx, &args[2], 1);
	left = (float)RValue_toReal(arrow.args[1]);
	right = (float)RValue_toReal(args[0]) + (float)RValue_toReal(w);
	dx = view_mid(ctx->runner) - (left + right) / 2.0f;
	arrow.args[1] = RValue_makeReal(left + dx);
	engineDrawBackground(ctx, arrow.args, 3);
	memcpy(moved, args, sizeof moved);
	moved[0] = RValue_makeReal(RValue_toReal(args[0]) + dx);
	row_seen(ctx, (float)RValue_toReal(args[1]));
	return engineDrawText(ctx, moved, 3);
}

static const TexturePageItem *bg_item(DataWin *dw, int32_t bg)
{
	int32_t t = bg >= 0 && (uint32_t)bg < dw->bgnd.count ? dw->bgnd.backgrounds[bg].tpagIndex : -1;

	return t >= 0 ? &dw->tpag.items[t] : NULL;
}

/* The title's corner lines: the version moves down and right, the links up and left, and their black boxes grow
 * back out to the screen's corners instead of floating. The blog's line is a picture, so ours replaces it as text in
 * the reddit line's font. */
static RValue wii_draw_background_ext(VMContext *ctx, RValue *args, int32_t argCount)
{
	const TexturePageItem *ver = bg_item(ctx->dataWin, bgVersion), *url = bg_item(ctx->dataWin, bgUrl);
	int32_t bg = argCount == 8 ? RValue_toInt32(args[0]) : -1;
	double x, y, h, right, side = Overscan_marginH();
	RValue fill[8], line[8], link = RValue_makeString(PORT_LINK), width;
	int k;

	if (!in_object(ctx, titleBg) || !ver || !url || (bg != bgVersion && bg != bgUrl))
		return engineDrawBackgroundExt(ctx, args, argCount);
	memcpy(fill, args, sizeof fill);
	if (bg == bgVersion) {
		fill[3] = RValue_makeReal(RValue_toReal(args[3]) + side / ver->boundingWidth);
		fill[4] = RValue_makeReal(RValue_toReal(args[4]) + (double)WiiSafe_rows() / ver->boundingHeight);
		return engineDrawBackgroundExt(ctx, fill, 8);
	}
	right = RValue_toReal(args[1]) + url->boundingWidth * RValue_toReal(args[3]);
	width = engineStringWidth(ctx, &link, 1);
	x = right - side - RValue_toReal(width) - 1;
	RValue_free(&width);
	y = RValue_toReal(args[2]) - WiiSafe_rows();
	h = url->boundingHeight * RValue_toReal(args[4]) + WiiSafe_rows();
	fill[0] = RValue_makeInt32(bgVersion);
	fill[1] = RValue_makeReal(x);
	fill[2] = RValue_makeReal(y);
	fill[3] = RValue_makeReal((right - x) / ver->boundingWidth);
	fill[4] = RValue_makeReal(h / ver->boundingHeight);
	engineDrawBackgroundExt(ctx, fill, 8);
	line[0] = RValue_makeReal(x + 1);
	line[1] = RValue_makeReal(y - LINK_TOP);
	line[2] = link;
	for (k = 3; k < 7; k++)
		line[k] = RValue_makeInt32(0xFFFFFF);
	line[7] = args[7];
	return engineDrawTextColour(ctx, line, 8);
}

static RValue wii_draw_text_colour(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue moved[8];
	double x, y;

	if (argCount != 8 || !in_object(ctx, titleBg))
		return engineDrawTextColour(ctx, args, argCount);
	memcpy(moved, args, sizeof moved);
	x = RValue_toReal(args[0]);
	y = RValue_toReal(args[1]);
	moved[0] = RValue_makeReal(y < TITLE_MID ? x + Overscan_marginH() : x - Overscan_marginH());
	moved[1] = RValue_makeReal(y < TITLE_MID ? y + WiiSafe_rows() : y - WiiSafe_rows());
	return engineDrawTextColour(ctx, moved, 8);
}

/* The links' dark band runs to the right edge: lifted with them, and reaching further left as they move in. */
static RValue wii_draw_rectangle_colour(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue lifted[9];

	if (argCount != 9 || !in_object(ctx, titleBg))
		return engineDrawRectangleColour(ctx, args, argCount);
	memcpy(lifted, args, sizeof lifted);
	lifted[0] = RValue_makeReal(RValue_toReal(args[0]) - Overscan_marginH());
	lifted[1] = RValue_makeReal(RValue_toReal(args[1]) - WiiSafe_rows());
	lifted[3] = RValue_makeReal(RValue_toReal(args[3]) - WiiSafe_rows());
	return engineDrawRectangleColour(ctx, lifted, 9);
}

static int32_t background_index(DataWin *dw, const char *name)
{
	uint32_t i;

	for (i = 0; i < dw->bgnd.count; i++)
		if (dw->bgnd.backgrounds[i].name && strcmp(dw->bgnd.backgrounds[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

void WiiSafe_hookScripts(VMContext *ctx)
{
	static const BuiltinFunc standIns[SCR_COUNT] = { wii_draw_txt_1button, wii_draw_txt_2buttons };
	int k;

	for (k = 0; k < SCR_COUNT; k++) {
		ptrdiff_t s = shgeti(ctx->codeIndexByName, (char *)scriptNames[k]);

		scriptCode[k] = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	}
	engineDrawBackground = VM_findBuiltin(ctx, "draw_background");
	engineDrawText = VM_findBuiltin(ctx, "draw_text");
	engineStringWidth = VM_findBuiltin(ctx, "string_width");
	engineDrawRectangle = VM_findBuiltin(ctx, "draw_rectangle");
	if (engineDrawBackground && engineDrawText && engineStringWidth) {
		tipArrow = background_index(ctx->dataWin, "bg_SubScrTipArrow1");
		subscreenFg = Wii_objectIndex(ctx->runner, "oSS_Fg");
	}
	if (engineDrawText && engineDrawRectangle)
		optionsFg = Wii_objectIndex(ctx->runner, "oOptionsFG");
	if (engineDrawText)
		scoreScreen = Wii_objectIndex(ctx->runner, "oScoreScreen");
	engineStringHeight = VM_findBuiltin(ctx, "string_height");
	if (engineDrawText && engineStringHeight) {
		introText = Wii_objectIndex(ctx->runner, "oIntroText1");
		introFull = Wii_varId(ctx->runner, "drawtext", INSTANCE_SELF);
	}
	engineDrawBackgroundExt = VM_findBuiltin(ctx, "draw_background_ext");
	engineDrawTextColour = VM_findBuiltin(ctx, "draw_text_colour");
	engineDrawRectangleColour = VM_findBuiltin(ctx, "draw_rectangle_colour");
	if (engineDrawBackgroundExt && engineDrawTextColour && engineDrawRectangleColour && engineStringWidth) {
		titleBg = Wii_objectIndex(ctx->runner, "oDrawTitleBG");
		bgVersion = background_index(ctx->dataWin, "bgVersion");
		bgUrl = background_index(ctx->dataWin, "bgURL");
	}
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;

		if (!name)
			continue;
		for (k = 0; k < SCR_COUNT; k++)
			if (scriptCode[k] >= 0 && strcmp(name, scriptNames[k]) == 0) {
				ctx->funcCallCache[fi].builtin = (void *)standIns[k];
				ctx->funcCallCache[fi].scriptCodeIndex = -1;
			}
		if (tipArrow >= 0 && strcmp(name, "draw_background") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_background;
		if ((tipArrow >= 0 || optionsFg >= 0 || scoreScreen >= 0 || introText >= 0) &&
		    strcmp(name, "draw_text") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_text;
		if (optionsFg >= 0 && strcmp(name, "draw_rectangle") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_rectangle;
		if (titleBg >= 0 && strcmp(name, "draw_background_ext") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_background_ext;
		if (titleBg >= 0 && strcmp(name, "draw_text_colour") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_text_colour;
		if (titleBg >= 0 && strcmp(name, "draw_rectangle_colour") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_rectangle_colour;
	}
}
