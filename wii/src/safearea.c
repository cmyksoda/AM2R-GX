#include "safearea.h"

#include "am2r.h"
#include "instance.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

#include <string.h>

/* AM2R's prompt rows sit at view_yview + 212 and + 225; the pause map's status bar ends above 205. */
#define ROW_UPPER 205
#define ROW_LOWER 220
#define NOTIFY_HIDDEN 30 /* oNotification's offset when it is fully slid out */

enum { SCR_1BUTTON, SCR_2BUTTONS, SCR_COUNT };
static const char *const scriptNames[SCR_COUNT] = { "draw_txt_1button", "draw_txt_2buttons" };
static int32_t scriptCode[SCR_COUNT] = { -1, -1 };

static Instance *first_of(Runner *runner, int32_t obj)
{
	Instance **list;

	if (obj < 0 || !runner->instancesByExactObject || !(list = runner->instancesByExactObject[obj]) || !arrlen(list))
		return NULL;
	return list[0];
}

/* The surface oControl paints the HUD into every step (1.5.5), or -1. */
int32_t WiiSafe_guiSurface(Runner *runner)
{
	static int32_t obj = -2, var = -2;
	Instance *control;
	RValue v;

	if (!runner)
		return -1;
	if (obj == -2) {
		obj = Wii_objectIndex(runner, "oControl");
		var = Wii_varId(runner, "gui_surface", INSTANCE_SELF);
	}
	if (var < 0 || !(control = first_of(runner, obj)))
		return -1;
	v = Instance_getSelfVar(control, var);
	return v.type == RVALUE_UNDEFINED ? -1 : RValue_toInt32(v);
}

/* How far up to draw what the current instance draws. The notification box lifts as it slides in, so a box that is
 * slid out stays below the screen. */
float WiiSafe_lift(Runner *runner)
{
	static int32_t notify = -2, popup, offset;
	Instance *inst = runner && runner->vmContext ? runner->vmContext->currentInstance : NULL;
	float off;

	if (!inst)
		return 0.0f;
	if (notify == -2) {
		notify = Wii_objectIndex(runner, "oNotification");
		popup = Wii_objectIndex(runner, "oPopupText");
		offset = Wii_varId(runner, "offset", INSTANCE_SELF);
	}
	if (inst->objectIndex == popup)
		return -WII_SAFE_ROWS;
	if (inst->objectIndex != notify || offset < 0)
		return 0.0f;
	off = (float)RValue_toReal(Instance_getSelfVar(inst, offset));
	if (off < 0.0f)
		off = 0.0f;
	if (off > NOTIFY_HIDDEN)
		off = NOTIFY_HIDDEN;
	return -WII_SAFE_ROWS * (NOTIFY_HIDDEN - off) / NOTIFY_HIDDEN;
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

/* Where two prompt rows would not both fit (Jaxi): the upper row's item goes to the middle and the bottom row's
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

/* draw_txt_*(x, y, text, align (0 centres on x), ...) */
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
			args[1] = RValue_makeReal(row_open(ctx) ? row.y : y - WII_SAFE_ROWS);
		}
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
 * until the text arrives, then centre the pair. */
static BuiltinFunc engineDrawBackground, engineDrawText, engineStringWidth, engineDrawRectangle;
static int32_t tipArrow = -2, subscreenFg = -2, optionsFg = -2;
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
	lifted[1] = RValue_makeReal(RValue_toReal(args[1]) - WII_SAFE_ROWS);
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

	if (argCount == 3 && in_object(ctx, optionsFg)) {
		memcpy(moved, args, sizeof moved);
		moved[1] = RValue_makeReal(RValue_toReal(args[1]) - WII_SAFE_ROWS);
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
		if ((tipArrow >= 0 || optionsFg >= 0) && strcmp(name, "draw_text") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_text;
		if (optionsFg >= 0 && strcmp(name, "draw_rectangle") == 0)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_rectangle;
	}
}
