#include "controls.h"

#include "am2r.h"
#include "assets.h"
#include "gml_array.h"
#include "input.h"
#include "instance.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

/* AM2R's Control Options rows (oOptionsControl): 6 is Keyboard Settings, 7 and 9 are Android-only and already hidden. */
#define ROW_KEYBOARD 6
#define ROW_EXIT 10
#define XJOY_VIBRATION 0 /* oOptionsXJoypad's rows */
#define XJOY_EXIT 15
#define XJOY_TITLE 50 /* its op[] slot for the title */
#define OP_SLOTS 64 /* the options screens file rows from 0 and titles, headers and Back from 50 */
#define GP_FACE1 32769
#define GP_SHOULDERL 32773
#define GP_START 32778
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
	T("A"), T("X"), T("B"), T("Y"), W("LeftShoulder", NULL), W("RightShoulder", NULL), T("Z"), { "C-", "Down", NULL }, T(""),
	W("Start", NULL),
};
static const Name nunchukNames[10] = {
	T("B"), T("C"), T("A"), T("-"), W("Up", "Down"), T("Z"), W("Left", "Right"), W("Down", NULL), T(""), T("+"),
};
#undef T
#undef W

/* Button glyphs built by the rules of AM2R's own (they rebuild its "LB" pixel for pixel): a grey pill with 3x5 dark
 * letters, each pill pixel under a letter pixel a shade darker. One style for every Wii button (Jaxi). */
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
static const signed char classicGlyphs[10] = { OURS(G_B), OURS(G_A), OURS(G_Y), OURS(G_X), OURS(G_L), OURS(G_R), OURS(G_ZL),
					       OURS(G_ZR), OURS(G_MINUS), OURS(G_PLUS) };
static const signed char gcGlyphs[10] = { OURS(G_A), OURS(G_X), OURS(G_B), OURS(G_Y), OURS(G_L), OURS(G_R), OURS(G_Z),
					  OURS(G_CDOWN), KEEP, KEEP };
static const signed char nunchukGlyphs[10] = { OURS(G_B), OURS(G_C), OURS(G_A), OURS(G_MINUS), GAME(N_DPAD), OURS(G_Z),
					       GAME(N_DPAD), GAME(N_DOWN), KEEP, OURS(G_PLUS) };

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
	return var_num(runner, first_instance(runner, "oControl"), "mod_buttonsconfig", INSTANCE_SELF, 0) == NINTENDO_NAMES;
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
		return i == GP_START - GP_FACE1 ? RValue_makeOwnedString(safeStrdup("+")) : call_script(ctx, SCR_NAME, args, argCount);
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
	const signed char *table = pad == WII_PAD_GAMECUBE ? gcGlyphs : pad == WII_PAD_NUNCHUK ? nunchukGlyphs : classicGlyphs;
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

void WiiControls_hookScripts(VMContext *ctx)
{
	static const BuiltinFunc standIns[SCR_COUNT] = { wii_set_default_xjoypad, wii_get_xjoybtnname, wii_get_xjoybtnsprite,
							 wii_save_gameoptions, wii_load_gameoptions, NULL };
	int k;

	for (k = 0; k < SCR_COUNT; k++) {
		ptrdiff_t s = shgeti(ctx->codeIndexByName, (char *)scriptNames[k]);

		scriptCode[k] = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	}
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;

		for (k = 0; k < SCR_COUNT && name; k++)
			if (standIns[k] && scriptCode[k] >= 0 && strcmp(name, scriptNames[k]) == 0) {
				ctx->funcCallCache[fi].builtin = (void *)standIns[k];
				ctx->funcCallCache[fi].scriptCodeIndex = -1;
			}
	}
}

/* Hidden the way the game hides its Android rows (invisible, never selected); the rows below move up a line. */
/* Everything the menu keeps in op[] below the hidden row moves up a line: the rows, and the section headers and the
 * Back row the game files at other indices. */
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
	int32_t curropt = var_id(runner, "curropt", INSTANCE_GLOBAL), tiptext = var_id(runner, "tiptext", INSTANCE_GLOBAL);

	if (curropt >= 0)
		Instance_setSelfVar(global, curropt, RValue_makeInt32(row));
	if (tip && tiptext >= 0)
		Instance_setSelfVar(global, tiptext, *tip);
}

/* Rumble reaches only a GameCube pad. */
static void hide_vibration_row(Runner *runner, Instance *menu)
{
	if (!hide_row(runner, menu, "canedit", "sep", XJOY_VIBRATION))
		return;
	cursor_to(runner, menu, XJOY_VIBRATION + 1);
	logInfo("controls: Vibration hidden (no GameCube pad in use)\n");
}

/* Fullscreen and Window Size mean nothing on a TV; the game leaves both out on Android the same way. */
static void hide_window_rows(Runner *runner, Instance *menu)
{
	int32_t full = (int32_t)var_num(runner, menu, "opFullscreen", INSTANCE_SELF, -1);
	int32_t scale = (int32_t)var_num(runner, menu, "opScale", INSTANCE_SELF, -1);

	if (full != 0 || scale != 1)
		return;
	if (!hide_row(runner, menu, "canEdit", "SPACER", full) || !hide_row(runner, menu, "canEdit", "SPACER", scale))
		return;
	cursor_to(runner, menu, scale + 1);
	logInfo("controls: Fullscreen and Window Size hidden\n");
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
 * Menu Back out of its options, so only the title side needs this. rowVar: a screen that keeps its Back row in a variable. */
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
		row = backRows[k].rowVar ? (int32_t)var_num(runner, menu, backRows[k].rowVar, INSTANCE_SELF, -1) : backRows[k].row;
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
	Instance *menu;

	if (!runner || !runner->vmContext)
		return;
	if ((menu = first_instance(runner, "oOptionsControl")) && menu->instanceId != controlMenu) {
		controlMenu = menu->instanceId;
		if (hide_row(runner, menu, "canedit", "sep", ROW_KEYBOARD))
			logInfo("controls: Keyboard Settings hidden\n");
	}
	if ((menu = first_instance(runner, "oOptionsDisplay")) && menu->instanceId != displayMenu) {
		displayMenu = menu->instanceId;
		hide_window_rows(runner, menu);
	}
	if ((menu = first_instance(runner, "oOptionsXJoypad")) && menu->instanceId != joypadMenu) {
		joypadMenu = menu->instanceId;
		retitle_joypad(runner, menu);
		if (WiiInput_pad() != WII_PAD_GAMECUBE)
			hide_vibration_row(runner, menu);
	}
}
