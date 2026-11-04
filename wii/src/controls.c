#include "controls.h"

#include "am2r.h"
#include "assets.h"
#include "gml_array.h"
#include "input.h"
#include "instance.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "rvalue.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"
#include "wii_video.h"

#include <stdio.h>
#include <string.h>

/* AM2R's Control Options rows (oOptionsControl): 6 is Keyboard Settings, 7 and 9 are Android-only
 * and already hidden. */
#define ROW_KEYBOARD 6
#define ROW_EXIT 10
#define XJOY_VIBRATION 0 /* oOptionsXJoypad's rows */
#define XJOY_EXIT 15
#define XJOY_TITLE 50 /* its op[] slot for the title */
#define OP_SLOTS 64 /* the options screens file rows from 0 and titles, headers and Back from 50 */
#define PLAYSTATION_NAMES 1
#define NINTENDO_NAMES 2

enum { SCR_DEFAULTS, SCR_NAME, SCR_SPRITE, SCR_SAVE, SCR_LOAD, SCR_TEXT, SCR_COUNT };
static const char *const scriptNames[SCR_COUNT] = { "set_default_xjoypad", "get_xjoybtnname", "get_xjoybtnsprite",
						    "save_gameoptions", "load_gameoptions", "get_text" };
static int32_t scriptCode[SCR_COUNT] = { -1, -1, -1, -1, -1, -1 };

/* key/key2: [Buttons] strings, so the words stay in the player's language; key2 joins as "key/key2", text before a
 * key as a prefix. */
typedef struct {
	const char *text, *key, *key2;
} Name;

#define T(text) { text, NULL, NULL }
#define W(key, key2) { NULL, key, key2 }
/* By GameMaker constant from gp_face1: face1-4, LB, RB, LT, RT, select, start; see the maps in input.c. */
static const Name gcNames[10] = {
	T("A"), T("X"), T("B"), T("Y"),
	W("LeftShoulder", NULL), W("RightShoulder", NULL), T("Z"), { "C-", "Down", NULL },
	T(""), W("Start", NULL),
};
static const Name nunchukNames[10] = {
	T("B"), T("C"), T("A"), T("-"),
	W("Up", "Down"), T("Z"), W("Left", "Right"), W("Down", NULL),
	T(""), T("+"),
};
#undef T
#undef W

/* Button glyphs built by the rules of AM2R's own (they rebuild its "LB" pixel for pixel): a gray pill with 3x5 dark
 * letters, each pill pixel under a letter pixel a shade darker. One style for every Wii button. */
enum { G_A, G_B, G_X, G_Y, G_C, G_L, G_R, G_ZL, G_ZR, G_Z, G_PLUS, G_MINUS, G_CDOWN, G_COUNT };
static const char *const glyphText[G_COUNT] = { "A", "B", "X", "Y", "C", "L", "R", "ZL", "ZR", "Z", "+", "-", "Cv" };
static const struct {
	char ch;
	const char *rows[5];
} font3x5[] = {
	{ 'A', { ".#.", "#.#", "###", "#.#", "#.#" } }, { 'B', { "##.", "#.#", "##.", "#.#", "###" } },
	{ 'X', { "#.#", "#.#", ".#.", "#.#", "#.#" } }, { 'Y', { "#.#", "#.#", ".#.", ".#.", ".#." } },
	{ 'L', { "#..", "#..", "#..", "#..", "###" } }, { 'R', { "##.", "#.#", "##.", "#.#", "#.#" } },
	{ 'Z', { "###", "..#", ".#.", "#..", "###" } }, { '+', { "...", ".#.", "###", ".#.", "..." } },
	{ '-', { "...", "...", "###", "...", "..." } }, { 'C', { "###", "#..", "#..", "#..", "###" } },
	{ 'v', { "...", "...", "###", ".#.", "..." } },
};
/* The game's own d-pad glyphs, for the Remote's d-pad. */
enum { N_DPAD, N_UP, N_DOWN, N_COUNT };
static const char *const gameGlyphs[N_COUNT] = { "sDPadBtn", "sBtnUp", "sBtnDown" };
#define KEEP 0
#define OURS(g) ((g) + 1)
#define GAME(n) (-1 - (n))
/* Slots as in the name tables; KEEP leaves AM2R's own glyph. */
static const signed char classicGlyphs[10] = {
	OURS(G_B), OURS(G_A), OURS(G_Y), OURS(G_X),
	OURS(G_L), OURS(G_R), OURS(G_ZL), OURS(G_ZR),
	OURS(G_MINUS), OURS(G_PLUS)
};
static const signed char gcGlyphs[10] = {
	OURS(G_A), OURS(G_X), OURS(G_B), OURS(G_Y),
	OURS(G_L), OURS(G_R), OURS(G_Z), OURS(G_CDOWN),
	KEEP, KEEP
};
static const signed char nunchukGlyphs[10] = {
	OURS(G_B), OURS(G_C), OURS(G_A), OURS(G_MINUS),
	GAME(N_DPAD), OURS(G_Z), GAME(N_DPAD), GAME(N_DOWN),
	KEEP, OURS(G_PLUS)
};

/* Looked up by scope: canedit, for one, is also a global somewhere in the data, listed first. */
static int32_t var_id(Runner *runner, const char *name, int32_t scope)
{
	static struct {
		const char *name;
		int32_t scope, id;
	} cache[16];
	static unsigned used;
	unsigned k;

	for (k = 0; k < used; k++)
		if (cache[k].name == name && cache[k].scope == scope)
			return cache[k].id;
	if (used == sizeof cache / sizeof cache[0])
		return Wii_varId(runner, name, scope);
	cache[used].name = name;
	cache[used].scope = scope;
	cache[used].id = Wii_varId(runner, name, scope);
	return cache[used++].id;
}

static Instance *first_instance(Runner *runner, const char *object)
{
	int32_t idx = Wii_objectIndex(runner, object);

	if (idx < 0 || !runner->instancesByExactObject || arrlen(runner->instancesByExactObject[idx]) == 0)
		return NULL;
	return runner->instancesByExactObject[idx][0];
}

static RValue *array_slot(Runner *runner, Instance *inst, const char *name, int32_t i)
{
	int32_t id = var_id(runner, name, INSTANCE_SELF);
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_ARRAY ? GMLArray_slot(v.array, i) : NULL;
}

static Instance *op_row(Runner *runner, Instance *menu, int32_t i)
{
	RValue *v = array_slot(runner, menu, "op", i);
	int key;

	if (!v)
		return NULL;
	key = RValue_toInt32(*v);
	return hmget(runner->instancesById, key);
}

static double var_num(Runner *runner, Instance *inst, const char *name, int32_t scope, double fallback)
{
	int32_t id = inst ? var_id(runner, name, scope) : -1;
	RValue v = id >= 0 ? Instance_getSelfVar(inst, id) : (RValue){ .type = RVALUE_UNDEFINED };

	return v.type == RVALUE_UNDEFINED ? fallback : RValue_toReal(v);
}

static RValue call_script(VMContext *ctx, int which, RValue *args, int32_t argCount)
{
	if (scriptCode[which] < 0)
		return RValue_makeUndefined();
	return VM_callCodeIndex(ctx, scriptCode[which], args, argCount);
}

/* get_text(section, key): the arguments sit in the order the game pushes them. */
static char *button_word(VMContext *ctx, const char *key)
{
	RValue args[2] = { RValue_makeString("Buttons"), RValue_makeString(key) };
	RValue r = call_script(ctx, SCR_TEXT, args, 2);
	char *s = safeStrdup(r.type == RVALUE_STRING && r.string ? r.string : key);

	RValue_free(&r);
	return s;
}

static char *name_text(VMContext *ctx, const Name *n)
{
	const char *pre = n->text ? n->text : "";
	char *a, *b, *both;
	size_t len;

	if (!n->key)
		return safeStrdup(pre);
	a = button_word(ctx, n->key);
	b = n->key2 ? button_word(ctx, n->key2) : safeStrdup("");
	len = strlen(pre) + strlen(a) + strlen(b) + 2;
	both = safeMalloc(len);
	snprintf(both, len, "%s%s%s%s", pre, a, *b ? "/" : "", b);
	free(a);
	free(b);
	return both;
}

static bool nintendo_names(Runner *runner)
{
	return var_num(runner, first_instance(runner, "oControl"), "mod_buttonsconfig", INSTANCE_SELF, 0) ==
	       NINTENDO_NAMES;
}

/* The game's PlayStation prompts for a DualShock (its face slots go by position, as ours do), Nintendo Pro else. */
static int pinned_names(void)
{
	return WiiInput_isDs3() ? PLAYSTATION_NAMES : NINTENDO_NAMES;
}

static int last_pin = NINTENDO_NAMES;

/* A fresh options file says Xbox; every other setting draws buttons no pad in use has. */
static void pin_nintendo_names(Runner *runner)
{
	Instance *control = first_instance(runner, "oControl");
	int32_t id = control ? var_id(runner, "mod_buttonsconfig", INSTANCE_SELF) : -1;

	last_pin = pinned_names();
	if (id >= 0 && var_num(runner, control, "mod_buttonsconfig", INSTANCE_SELF, 0) != last_pin)
		Instance_setSelfVar(control, id, RValue_makeInt32(last_pin));
}

static const char *const *letter_rows(char ch)
{
	unsigned k;

	for (k = 0; k < sizeof font3x5 / sizeof font3x5[0]; k++)
		if (font3x5[k].ch == ch)
			return font3x5[k].rows;
	return NULL;
}

/* '.' clear, 'a' dark, 'b' light: the pill's rim is dark on its sides and along its two bottom rows. */
static char pill_base(int x, int y, int w)
{
	bool edge = x == 0 || x == w - 1, next = x == 1 || x == w - 2;

	switch (y) {
	case 0:
		return edge ? '.' : next ? 'a' : 'b';
	case 1:
	case 4:
		return edge ? 'a' : 'b';
	case 2:
	case 3:
		return 'b';
	case 5:
		return 'a';
	default:
		return edge ? '.' : 'a';
	}
}

static int glyph_width(int g)
{
	return 4 * (int)strlen(glyphText[g]) + 3;
}

/* 'c' is ink. */
static void draw_glyph(uint8_t *rgba, int stride, int x0, int g)
{
	int w = glyph_width(g), x, y, k;
	char cell[7][12];

	for (y = 0; y < 7; y++)
		for (x = 0; x < w; x++)
			cell[y][x] = pill_base(x, y, w);
	for (k = 0; glyphText[g][k]; k++) {
		const char *const *rows = letter_rows(glyphText[g][k]);

		for (y = 0; rows && y < 5; y++)
			for (x = 0; x < 3; x++)
				if (rows[y][x] == '#')
					cell[1 + y][2 + 4 * k + x] = 'c';
	}
	for (y = 2; y < 5; y++)
		for (x = 0; x < w; x++)
			if (cell[y][x] == 'b' && cell[y - 1][x] == 'c')
				cell[y][x] = 'a';
	for (y = 0; y < 7; y++)
		for (x = 0; x < w; x++) {
			uint8_t *px = rgba + ((size_t)y * stride + x0 + x) * 4;
			uint8_t v = cell[y][x] == 'a' ? 110 : cell[y][x] == 'b' ? 145 : cell[y][x] == 'c' ? 25 : 0;

			px[0] = px[1] = px[2] = v;
			px[3] = cell[y][x] == '.' ? 0 : 255;
		}
}

static void build_glyphs(DataWin *dw, int32_t *sprites)
{
	int cellX[G_COUNT], cellW[G_COUNT], w = 0, g;
	uint8_t *rgba;

	for (g = 0; g < G_COUNT; g++) {
		cellX[g] = w;
		cellW[g] = glyph_width(g);
		w += cellW[g] + 1;
	}
	rgba = (uint8_t *)safeCalloc((size_t)w * 7, 4);
	for (g = 0; g < G_COUNT; g++)
		draw_glyph(rgba, w, cellX[g], g);
	WiiAssets_addSprites(dw, rgba, w, 7, cellX, cellW, G_COUNT, sprites);
	free(rgba);
	logInfo("controls: %d Wii button glyphs added\n", G_COUNT);
}

static bool two_button_aim(Runner *runner)
{
	return var_num(runner, runner->vmContext->globalScopeInstance, "opaimstyle", INSTANCE_GLOBAL, 0) == 1;
}

/* The remap screen's Defaults row: AM2R's own defaults first (vibration, deadzone, ...), then our layout on top.
 * Boot also calls this script, before the options are read; there the stock set must stay for wii_pad_defaults. */
static RValue wii_set_default_xjoypad(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue r = call_script(ctx, SCR_DEFAULTS, args, argCount);

	if (first_instance(ctx->runner, "oOptionsXJoypad")) {
		WiiInput_applyLayout(ctx);
		logInfo("controls: Defaults restores the Wii layout\n");
	}
	WiiInput_flipMenus(ctx);
	return r;
}

/* The options file keeps the real menu slots, whichever pad is in use. */
static RValue wii_save_gameoptions(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue r;

	WiiInput_flipMenus(ctx);
	r = call_script(ctx, SCR_SAVE, args, argCount);
	WiiInput_flipMenus(ctx);
	return r;
}

static RValue wii_load_gameoptions(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue r = call_script(ctx, SCR_LOAD, args, argCount);

	WiiInput_flipMenus(ctx);
	return r;
}

static RValue wii_get_xjoybtnname(VMContext *ctx, RValue *args, int32_t argCount)
{
	WiiPad pad = WiiInput_pad();
	const Name *names = pad == WII_PAD_GAMECUBE ? gcNames : pad == WII_PAD_NUNCHUK ? nunchukNames : NULL;
	int32_t i = argCount > 0 ? RValue_toInt32(args[0]) - GP_FACE1 : -1;
	Name n;

	if (i < 0 || i >= 10 || !nintendo_names(ctx->runner))
		return call_script(ctx, SCR_NAME, args, argCount);
	if (!names) /* the Classic Controller: AM2R's Nintendo names, but its Start button is printed "+" */
		return i == GP_START - GP_FACE1 ? RValue_makeOwnedString(safeStrdup("+")) :
		       call_script(ctx, SCR_NAME, args, argCount);
	n = names[i];
	/* two-button aim: the Nunchuk's d-pad Up alone is the aim-up button */
	if (pad == WII_PAD_NUNCHUK && n.key2 && two_button_aim(ctx->runner))
		n.key2 = NULL;
	return RValue_makeOwnedString(name_text(ctx, &n));
}

static RValue wii_get_xjoybtnsprite(VMContext *ctx, RValue *args, int32_t argCount)
{
	static int32_t ours[G_COUNT], game[N_COUNT];
	static bool built;
	WiiPad pad = WiiInput_pad();
	const signed char *table = pad == WII_PAD_GAMECUBE ? gcGlyphs :
				   pad == WII_PAD_NUNCHUK ? nunchukGlyphs : classicGlyphs;
	int32_t i = argCount > 0 ? RValue_toInt32(args[0]) - GP_FACE1 : -1, sprite = -1;
	int g, k;

	if (i < 0 || i >= 10 || !nintendo_names(ctx->runner))
		return call_script(ctx, SCR_SPRITE, args, argCount);
	if (!built) {
		built = true;
		build_glyphs(ctx->dataWin, ours);
		for (k = 0; k < N_COUNT; k++)
			game[k] = Wii_spriteIndex(ctx->runner, gameGlyphs[k]);
	}
	g = table[i];
	if (pad == WII_PAD_NUNCHUK && i == GP_SHOULDERL - GP_FACE1 && two_button_aim(ctx->runner))
		g = GAME(N_UP);
	if (g > 0)
		sprite = ours[g - 1];
	else if (g < 0)
		sprite = game[-1 - g];
	return sprite >= 0 ? RValue_makeInt32(sprite) : call_script(ctx, SCR_SPRITE, args, argCount);
}

static BuiltinFunc engine_draw_rectangle;

/* The options rows mark the chosen one with c_gray at alpha 0.4, about 15% gray once blitted: on a CRT that is under
 * 1% of white's light. In 240p (the CRT mode) it is drawn about 25% gray on black; other modes keep the original. */
static RValue wii_draw_rectangle(VMContext *ctx, RValue *args, int32_t argCount)
{
	static const char *const rows[] = { "oOptionLR", "oOptionSlider", "oOptionSeed" };
	Renderer *renderer = ctx->runner->renderer;
	RValue out;
	size_t k;

	if (!renderer || renderer->drawColor != 0x808080 || renderer->drawAlpha != 0.4f ||
	    WiiVideo_efbHeight() >= WII_FB_H || !ctx->currentInstance)
		return engine_draw_rectangle(ctx, args, argCount);
	for (k = 0; k < sizeof rows / sizeof rows[0]; k++)
		if (ctx->currentInstance->objectIndex == Wii_objectIndex(ctx->runner, rows[k]))
			break;
	if (k == sizeof rows / sizeof rows[0])
		return engine_draw_rectangle(ctx, args, argCount);
	renderer->drawColor = 0xd2d2d2;
	out = engine_draw_rectangle(ctx, args, argCount);
	renderer->drawColor = 0x808080;
	return out;
}

void WiiControls_hookScripts(VMContext *ctx)
{
	static const BuiltinFunc standIns[SCR_COUNT] = {
		wii_set_default_xjoypad, wii_get_xjoybtnname, wii_get_xjoybtnsprite,
		wii_save_gameoptions, wii_load_gameoptions, NULL
	};
	int k;

	for (k = 0; k < SCR_COUNT; k++) {
		ptrdiff_t s = shgeti(ctx->codeIndexByName, (char *)scriptNames[k]);

		scriptCode[k] = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	}
	engine_draw_rectangle = VM_findBuiltin(ctx, "draw_rectangle");
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;

		if (engine_draw_rectangle && ctx->funcCallCache[fi].builtin == (void *)engine_draw_rectangle)
			ctx->funcCallCache[fi].builtin = (void *)wii_draw_rectangle;
		for (k = 0; k < SCR_COUNT && name; k++)
			if (standIns[k] && scriptCode[k] >= 0 && strcmp(name, scriptNames[k]) == 0) {
				ctx->funcCallCache[fi].builtin = (void *)standIns[k];
				ctx->funcCallCache[fi].scriptCodeIndex = -1;
			}
	}
}

/* Hidden the way the game hides its Android rows (invisible, never selected); everything below it in op[] (rows,
 * headers, the Back row) moves up a line. */
static bool hide_row(Runner *runner, Instance *menu, const char *editName, const char *sepName, int32_t hidden)
{
	RValue *edit = array_slot(runner, menu, editName, hidden);
	float sep = (float)var_num(runner, menu, sepName, INSTANCE_SELF, 16);
	Instance *gone = op_row(runner, menu, hidden), *row;
	int32_t i;

	if (!edit || !gone)
		return false;
	RValue_free(edit);
	*edit = RValue_makeInt32(0);
	gone->visible = false;
	for (i = 0; i < OP_SLOTS; i++)
		if ((row = op_row(runner, menu, i)) && row != gone && row->y > gone->y) {
			row->y -= sep;
			if (runner->spatialGrid)
				SpatialGrid_markInstanceAsDirty(runner->spatialGrid, row);
		}
	return true;
}

/* The cursor starts on row 0; when that row is hidden, Left/Right would still flip it. */
static void cursor_to(Runner *runner, Instance *menu, int32_t row)
{
	Instance *global = runner->vmContext->globalScopeInstance;
	RValue *tip = array_slot(runner, menu, "tip", row);
	int32_t curropt = var_id(runner, "curropt", INSTANCE_GLOBAL),
		tiptext = var_id(runner, "tiptext", INSTANCE_GLOBAL);

	if (curropt >= 0)
		Instance_setSelfVar(global, curropt, RValue_makeInt32(row));
	if (tip && tiptext >= 0)
		Instance_setSelfVar(global, tiptext, *tip);
}

/* Rumble reaches only a GameCube pad or a Wii U Pro Controller. */
static void hide_vibration_row(Runner *runner, Instance *menu)
{
	if (!hide_row(runner, menu, "canedit", "sep", XJOY_VIBRATION))
		return;
	cursor_to(runner, menu, XJOY_VIBRATION + 1);
	logInfo("controls: Vibration hidden (no pad with a motor in use)\n");
}

static void move_instance(Runner *runner, Instance *inst, float y)
{
	inst->y = y;
	if (runner->spatialGrid)
		SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
}

/* Controller Tips is pinned to Nintendo Pro (the one setting that draws Wii buttons), so its row becomes Scanlines and
 * moves up under Widescreen. The per-row arrays, each row's optionid and the op* indexes all shift together, so the
 * game's own cursor and value code carry on unchanged. */
static void move_scanline_row(Runner *runner, Instance *menu)
{
	static const char *const indexVars[] = { "opFullscreen", "opScale", "opVSync", "opSensitivity",
						 "opWidescreen", "opShowHUD", "opShowMap", "opCounterStyle",
						 "opShowHints", "opShowScans", "opLanguage" };
	static const char *const rowArrays[] = { "op", "canEdit", "tip" };
	int32_t wide = (int32_t)var_num(runner, menu, "opWidescreen", INSTANCE_SELF, -1);
	int32_t tips = (int32_t)var_num(runner, menu, "opButtonType", INSTANCE_SELF, -1);
	int32_t optionid = var_id(runner, "optionid", INSTANCE_SELF), id, v, k;
	float sep = (float)var_num(runner, menu, "SPACER", INSTANCE_SELF, 16);
	Instance *row = op_row(runner, menu, tips), *wideRow = op_row(runner, menu, wide), *other;
	RValue moved;
	unsigned a;

	if (wide < 0 || tips <= wide + 1 || !row || !wideRow || optionid < 0) {
		logWarn("controls: no Controller Tips row to make Scanlines (%d, %d)\n", (int)wide, (int)tips);
		return;
	}
	for (a = 0; a < sizeof rowArrays / sizeof rowArrays[0]; a++)
		for (k = wide + 1; k <= tips; k++)
			if (!array_slot(runner, menu, rowArrays[a], k))
				return;
	for (k = 0; k < OP_SLOTS; k++)
		if ((other = op_row(runner, menu, k)) && other != row && other->y > wideRow->y && other->y < row->y)
			move_instance(runner, other, other->y + sep);
	move_instance(runner, row, wideRow->y + sep);
	for (a = 0; a < sizeof rowArrays / sizeof rowArrays[0]; a++) {
		moved = *array_slot(runner, menu, rowArrays[a], tips);
		for (k = tips; k > wide + 1; k--)
			*array_slot(runner, menu, rowArrays[a], k) = *array_slot(runner, menu, rowArrays[a], k - 1);
		*array_slot(runner, menu, rowArrays[a], wide + 1) = moved;
	}
	for (k = wide + 1; k <= tips; k++)
		if ((other = op_row(runner, menu, k)))
			Instance_setSelfVar(other, optionid, RValue_makeInt32(k));
	for (a = 0; a < sizeof indexVars / sizeof indexVars[0]; a++) {
		v = (int32_t)var_num(runner, menu, indexVars[a], INSTANCE_SELF, -1);
		if (v > wide && v < tips && (id = var_id(runner, indexVars[a], INSTANCE_SELF)) >= 0)
			Instance_setSelfVar(menu, id, RValue_makeInt32(v + 1));
	}
	if ((id = var_id(runner, "opButtonType", INSTANCE_SELF)) >= 0)
		Instance_setSelfVar(menu, id, RValue_makeInt32(wide + 1));
	logInfo("controls: Controller Tips row is Scanlines, moved from row %d to %d\n", (int)tips, (int)wide + 1);
}

/* 704 of the line's 720 pixels is the full 4:3 (or 16:9) picture on a TV, so game pixels come out square, as on PC. */
#define ASPECT_WIDTH 704

static bool set_row_text(Instance *row, int32_t var, const char *text)
{
	RValue shown = Instance_getSelfVar(row, var), value;

	if (shown.type == RVALUE_STRING && shown.string && strcmp(shown.string, text) == 0)
		return false;
	value = RValue_makeOwnedString(safeStrdup(text));
	Instance_setSelfVar(row, var, value);
	RValue_free(&value);
	return true;
}

/* Every Display row already stands in for something, so Aspect Correction is a new one under Widescreen. As in
 * move_scanline_row, the per-row arrays, optionids and op* indexes below it shift together. */
static int32_t insert_aspect_row(Runner *runner, Instance *menu)
{
	static const char *const indexVars[] = { "opButtonType", "opShowHUD", "opShowMap", "opCounterStyle",
						 "opShowHints", "opShowScans", "opLanguage", "menuSize" };
	static const char *const rowArrays[] = { "op", "canEdit", "tip" };
	int32_t wide = (int32_t)var_num(runner, menu, "opWidescreen", INSTANCE_SELF, -1);
	int32_t size = (int32_t)var_num(runner, menu, "menuSize", INSTANCE_SELF, -1), at = wide + 1;
	int32_t optionid = var_id(runner, "optionid", INSTANCE_SELF), object = Wii_objectIndex(runner, "oOptionLR");
	int32_t label = var_id(runner, "label", INSTANCE_SELF), id, v, k;
	float sep = (float)var_num(runner, menu, "SPACER", INSTANCE_SELF, 16);
	Instance *wideRow = op_row(runner, menu, wide), *row, *other;
	RValue arrays[3], added[3];
	unsigned a;

	if (wide < 0 || size <= at || !wideRow || optionid < 0 || label < 0 || object < 0) {
		logWarn("controls: no room for Aspect Correction (%d, %d)\n", (int)wide, (int)size);
		return -1;
	}
	for (a = 0; a < 3; a++) {
		arrays[a] = (id = var_id(runner, rowArrays[a], INSTANCE_SELF)) >= 0 ? Instance_getSelfVar(menu, id) :
										     RValue_makeUndefined();
		if (arrays[a].type != RVALUE_ARRAY)
			return -1;
	}
	for (k = 0; k < OP_SLOTS; k++)
		if ((other = op_row(runner, menu, k)) && other->y > wideRow->y)
			move_instance(runner, other, other->y + sep);
	if (!(row = Runner_createInstance(runner, wideRow->x, wideRow->y + sep, object)))
		return -1;
	set_row_text(row, label, "Aspect Correction");
	added[0] = RValue_makeInt32((int32_t)row->instanceId);
	added[1] = RValue_makeInt32(1);
	added[2] = RValue_makeOwnedString(safeStrdup("Square pixels, as on PC: fills the full width of the TV"));
	for (a = 0; a < 3; a++) {
		GMLArray_growTo(arrays[a].array, size + 2);
		for (k = size + 1; k > at; k--)
			*GMLArray_slot(arrays[a].array, k) = *GMLArray_slot(arrays[a].array, k - 1);
		*GMLArray_slot(arrays[a].array, at) = added[a];
	}
	for (k = at; k <= size + 1; k++)
		if ((other = op_row(runner, menu, k)))
			Instance_setSelfVar(other, optionid, RValue_makeInt32(k));
	for (a = 0; a < sizeof indexVars / sizeof indexVars[0]; a++) {
		v = (int32_t)var_num(runner, menu, indexVars[a], INSTANCE_SELF, -1);
		if (v > wide && (id = var_id(runner, indexVars[a], INSTANCE_SELF)) >= 0)
			Instance_setSelfVar(menu, id, RValue_makeInt32(v + 1));
	}
	if ((id = var_id(runner, "vTargetY", INSTANCE_SELF)) >= 0)
		Instance_setSelfVar(menu, id, RValue_makeReal(RValue_toReal(Instance_getSelfVar(menu, id)) + sep));
	logInfo("controls: Aspect Correction row added at %d\n", (int)at);
	return at;
}

/* The game has no step for this row: a fresh left or right press toggles it here, with the game's own sound. */
static void aspect_row_frame(Runner *runner, Instance *menu, int32_t at)
{
	Instance *control = first_instance(runner, "oControl"), *row = op_row(runner, menu, at);
	int32_t curropt = var_id(runner, "curropt", INSTANCE_GLOBAL), optext = var_id(runner, "optext", INSTANCE_SELF);
	RValue args[2], text;

	if (!row || !control || curropt < 0 || optext < 0)
		return;
	if (var_num(runner, menu, "active", INSTANCE_SELF, 0) != 0 &&
	    RValue_toInt32(Instance_getSelfVar(runner->vmContext->globalScopeInstance, curropt)) == at &&
	    var_num(runner, control, "kRight", INSTANCE_SELF, 0) != var_num(runner, control, "kLeft", INSTANCE_SELF, 0) &&
	    var_num(runner, control, "kRightPushedSteps", INSTANCE_SELF, 1) ==
		    var_num(runner, control, "kLeftPushedSteps", INSTANCE_SELF, 1) &&
	    var_num(runner, control, "kUp", INSTANCE_SELF, 1) == 0 && var_num(runner, control, "kDown", INSTANCE_SELF, 1) == 0) {
		Overscan_setViWidth(Overscan_viWidth() ? 0 : ASPECT_WIDTH);
		WiiVideo_setActiveWidth(Overscan_viWidth());
	}
	args[0] = RValue_makeString("GlobalOptions");
	args[1] = RValue_makeString(Overscan_viWidth() ? "Enabled" : "Disabled");
	text = call_script(runner->vmContext, SCR_TEXT, args, 2);
	set_row_text(row, optext, text.type == RVALUE_STRING && text.string ? text.string : args[1].string);
	RValue_free(&text);
}

/* Scanlines draw nothing in 240p (a CRT has real ones there), so their row leaves the menu while 240p is on and the
 * rows below close up, the way hide_row does; turning 240p off brings it back with its setting kept. */
static void fit_scanline_row(Runner *runner, Instance *menu, bool *hidden)
{
	int32_t row = (int32_t)var_num(runner, menu, "opButtonType", INSTANCE_SELF, -1);
	int32_t vsync = var_id(runner, "opvsync", INSTANCE_GLOBAL), k;
	float sep = (float)var_num(runner, menu, "SPACER", INSTANCE_SELF, 16);
	Instance *line = op_row(runner, menu, row), *other;
	RValue *edit = array_slot(runner, menu, "canEdit", row);
	bool hide;

	if (!line || !edit || vsync < 0)
		return;
	hide = RValue_toReal(Instance_getSelfVar(runner->vmContext->globalScopeInstance, vsync)) != 0.0;
	if (hide == *hidden)
		return;
	*hidden = hide;
	RValue_free(edit);
	*edit = RValue_makeInt32(hide ? 0 : 1);
	line->visible = !hide;
	for (k = 0; k < OP_SLOTS; k++)
		if ((other = op_row(runner, menu, k)) && other != line &&
		    (hide ? other->y > line->y : other->y >= line->y))
			move_instance(runner, other, other->y + (hide ? -sep : sep));
	logInfo("controls: Scanlines row %s (240p %s)\n", hide ? "hidden" : "shown", hide ? "on" : "off");
}

/* The game steps Controller Tips by one either way; that step moves the scanline level before the pin undoes it. */
static void scanline_value(Runner *runner, Instance *menu)
{
	Instance *control = first_instance(runner, "oControl");
	int32_t row = (int32_t)var_num(runner, menu, "opButtonType", INSTANCE_SELF, -1);
	int32_t curropt = var_id(runner, "curropt", INSTANCE_GLOBAL), optext = var_id(runner, "optext", INSTANCE_SELF);
	double config = var_num(runner, control, "mod_buttonsconfig", INSTANCE_SELF, last_pin);
	Instance *line = op_row(runner, menu, row);
	RValue args[2], text, shown;
	char level[32];

	if (!line || optext < 0)
		return;
	/* against last frame's pin: a pad switch that changes the prompt style is no step */
	if (config != last_pin && curropt >= 0 &&
	    RValue_toInt32(Instance_getSelfVar(runner->vmContext->globalScopeInstance, curropt)) == row)
		Overscan_stepScanlines(config > last_pin ? 1 : -1);
	pin_nintendo_names(runner);
	if (Overscan_scanlines()) {
		snprintf(level, sizeof level, "%d%%", Overscan_scanlines() * 25);
	} else {
		args[0] = RValue_makeString("GlobalOptions");
		args[1] = RValue_makeString("Disabled");
		text = call_script(runner->vmContext, SCR_TEXT, args, 2);
		snprintf(level, sizeof level, "%s", text.type == RVALUE_STRING && text.string ? text.string : "Off");
		RValue_free(&text);
	}
	shown = Instance_getSelfVar(line, optext);
	if (shown.type == RVALUE_STRING && shown.string && strcmp(shown.string, level) == 0)
		return;
	text = RValue_makeOwnedString(safeStrdup(level));
	Instance_setSelfVar(line, optext, text);
	RValue_free(&text);
}

/* A value beside the two adjust rows would only show the window settings they stand in for. */
static void blank_value(Runner *runner, Instance *menu, const char *rowVar)
{
	Instance *row = op_row(runner, menu, (int32_t)var_num(runner, menu, rowVar, INSTANCE_SELF, -1));
	int32_t optext = var_id(runner, "optext", INSTANCE_SELF);
	RValue v, empty;

	if (!row || optext < 0)
		return;
	v = Instance_getSelfVar(row, optext);
	if (v.type == RVALUE_STRING && v.string && !v.string[0])
		return;
	empty = RValue_makeOwnedString(safeStrdup(""));
	Instance_setSelfVar(row, optext, empty);
	RValue_free(&empty);
}

OverscanPage WiiControls_displayRow(Runner *runner)
{
	int32_t curropt, row;
	Instance *menu;

	if (!runner || !runner->vmContext || (curropt = var_id(runner, "curropt", INSTANCE_GLOBAL)) < 0 ||
	    !(menu = first_instance(runner, "oOptionsDisplay")) ||
	    var_num(runner, menu, "active", INSTANCE_SELF, 0) == 0)
		return OVERSCAN_OFF;
	row = RValue_toInt32(Instance_getSelfVar(runner->vmContext->globalScopeInstance, curropt));
	if (row == (int32_t)var_num(runner, menu, "opFullscreen", INSTANCE_SELF, -1))
		return OVERSCAN_SCREEN;
	if (row == (int32_t)var_num(runner, menu, "opScale", INSTANCE_SELF, -1))
		return OVERSCAN_HUD;
	return OVERSCAN_OFF;
}

/* "Joypad Configuration": the title the game already has, in every language, for its other pad screen. */
static void retitle_joypad(Runner *runner, Instance *menu)
{
	VMContext *vm = runner->vmContext;
	Instance *title = op_row(runner, menu, XJOY_TITLE);
	int32_t text = var_id(runner, "text", INSTANCE_SELF);
	RValue args[2] = { RValue_makeString("OptionsControlJoypad"), RValue_makeString("Title_Joypad") };
	RValue r;

	if (!title || text < 0)
		return;
	r = call_script(vm, SCR_TEXT, args, 2);
	if (r.type == RVALUE_STRING) {
		Instance_setSelfVar(title, text, r);
		logInfo("controls: joypad screen titled \"%s\"\n", r.string);
	}
	RValue_free(&r);
}

/* AM2R's options screens leave only by their Back row, so Menu Back picks it. In play the pause screen already takes
 * Menu Back out of its options, so only the title side needs this.
 * rowVar: a screen that keeps its Back row in a variable. */
static const struct {
	const char *object, *rowVar;
	int32_t row;
} backRows[] = {
	{ "oOptionsMain", NULL, 4 },
	{ "oOptionsDisplay", "menuSize", 0 },
	{ "oOptionsSound", NULL, 3 },
	{ "oOptionsControl", NULL, ROW_EXIT },
	{ "oOptionsXJoypad", NULL, XJOY_EXIT },
	{ "oOptionsExtras", "lastitem", 0 },
	{ "oOptionsGame", "lastitem", 0 },
};

bool WiiControls_menuBack(Runner *runner)
{
	int32_t curropt = var_id(runner, "curropt", INSTANCE_GLOBAL), row;
	Instance *menu;
	unsigned k;

	if (curropt < 0 || first_instance(runner, "oSS_Control"))
		return false;
	for (k = 0; k < sizeof backRows / sizeof backRows[0]; k++) {
		menu = first_instance(runner, backRows[k].object);
		/* editing: a row waiting for a button to bind, which may be this very one */
		if (!menu || var_num(runner, menu, "active", INSTANCE_SELF, 0) == 0 ||
		    var_num(runner, menu, "editing", INSTANCE_SELF, 0) != 0)
			continue;
		row = backRows[k].rowVar ? (int32_t)var_num(runner, menu, backRows[k].rowVar, INSTANCE_SELF, -1) :
		      backRows[k].row;
		if (row < 0)
			return false;
		Instance_setSelfVar(runner->vmContext->globalScopeInstance, curropt, RValue_makeInt32(row));
		logInfo("controls: Menu Back leaves %s by row %d\n", backRows[k].object, (int)row);
		return true;
	}
	return false;
}

void WiiControls_frame(Runner *runner)
{
	static uint32_t controlMenu, joypadMenu, displayMenu;
	static bool scanlineRowHidden;
	static int32_t aspectRow = -1;
	Instance *menu;

	if (!runner || !runner->vmContext)
		return;
	if ((menu = first_instance(runner, "oOptionsDisplay")) && menu->instanceId == displayMenu)
		scanline_value(runner, menu);
	pin_nintendo_names(runner);
	if ((menu = first_instance(runner, "oOptionsControl")) && menu->instanceId != controlMenu) {
		controlMenu = menu->instanceId;
		if (hide_row(runner, menu, "canedit", "sep", ROW_KEYBOARD))
			logInfo("controls: Keyboard Settings hidden\n");
	}
	if ((menu = first_instance(runner, "oOptionsDisplay"))) {
		if (menu->instanceId != displayMenu) {
			displayMenu = menu->instanceId;
			move_scanline_row(runner, menu);
			aspectRow = insert_aspect_row(runner, menu);
			scanlineRowHidden = false;
		}
		if (aspectRow >= 0)
			aspect_row_frame(runner, menu, aspectRow);
		fit_scanline_row(runner, menu, &scanlineRowHidden);
		blank_value(runner, menu, "opFullscreen");
		blank_value(runner, menu, "opScale");
	}
	if ((menu = first_instance(runner, "oOptionsXJoypad")) && menu->instanceId != joypadMenu) {
		joypadMenu = menu->instanceId;
		retitle_joypad(runner, menu);
		if (!WiiInput_hasRumble())
			hide_vibration_row(runner, menu);
	}
}
