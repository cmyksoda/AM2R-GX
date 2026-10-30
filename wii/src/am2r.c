#include "am2r.h"
#include "gx_renderer.h"
#include "wii_audio_system.h"

#include "gml_array.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "runner.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "text_utils.h"
#include "utils.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OBJECT_NAMES_SEEN 32

int32_t Wii_objectIndex(Runner *runner, const char *name)
{
	/* Per-frame hooks look objects up by name; a hit is confirmed by name, so a reused buffer can't mislead it. */
	static struct {
		const char *name;
		uint32_t index;
	} seen[OBJECT_NAMES_SEEN];
	static uint32_t next;
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	if (!dw)
		return -1;
	for (i = 0; i < OBJECT_NAMES_SEEN; i++)
		if (seen[i].name == name && seen[i].index < dw->objt.count &&
		    strcmp(dw->objt.objects[seen[i].index].name, name) == 0)
			return (int32_t)seen[i].index;
	for (i = 0; i < dw->objt.count; i++)
		if (strcmp(dw->objt.objects[i].name, name) == 0) {
			seen[next % OBJECT_NAMES_SEEN].name = name;
			seen[next % OBJECT_NAMES_SEEN].index = i;
			next++;
			return (int32_t)i;
		}
	return -1;
}

int32_t Wii_spriteIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->sprt.count; i++)
		if (dw->sprt.sprites[i].name && strcmp(dw->sprt.sprites[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_roomIndex(Runner *runner, const char *name)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->room.count; i++)
		if (dw->room.rooms[i].name && strcmp(dw->room.rooms[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

int32_t Wii_varId(Runner *runner, const char *name, int32_t scope)
{
	DataWin *dw = runner ? runner->dataWin : NULL;
	uint32_t i;

	for (i = 0; dw && i < dw->vari.variableCount; i++) {
		Variable *v = &dw->vari.variables[i];

		if (v->instanceType == scope && v->varID >= 0 && v->name && strcmp(v->name, name) == 0)
			return v->varID;
	}
	return -1;
}

/* check_keys resets global.controltype (0 keyboard, 1 numbered pad, 2 named pad) every step; pinned to 2 after it,
 * as the Wii has only controllers. */
void Wii_forceGamepadProfile(Runner *runner)
{
	static int32_t varId = -2;
	VMContext *vm;

	if (!runner || !runner->vmContext)
		return;
	vm = runner->vmContext;
	if (varId == -2) {
		ptrdiff_t slot = shgeti(vm->varNameMap, "controltype");
		varId = slot >= 0 ? vm->varNameMap[slot].value : -1;
	}
	if (varId >= 0 && vm->globalScopeInstance)
		Instance_setSelfVar(vm->globalScopeInstance, varId, RValue_makeInt32(2));
}

void Wii_setViewClears(Runner *runner)
{
	static int32_t itemRoom = -2;

	if (!runner)
		return;
	if (itemRoom == -2)
		itemRoom = Wii_roomIndex(runner, "itemroom");
	runner->clearViewSurfaces = runner->currentRoomIndex != itemRoom;
}

static int32_t sprite_named(Runner *runner, const char *name)
{
	ptrdiff_t slot = shgeti(runner->assetsByName, name);
	int32_t i = slot >= 0 ? runner->assetsByName[slot].value : -1;

	if (i < 0 || (uint32_t)i >= runner->dataWin->sprt.count || !runner->dataWin->sprt.sprites[i].name ||
	    strcmp(runner->dataWin->sprt.sprites[i].name, name) != 0)
		return -1;
	return i;
}

static void add_sprite(int32_t **tpags, DataWin *dw, int32_t sprite, uint32_t frames)
{
	const Sprite *sp;
	uint32_t f;

	if (sprite < 0 || (uint32_t)sprite >= dw->sprt.count)
		return;
	sp = &dw->sprt.sprites[sprite];
	for (f = 0; sp->tpagIndices && f < sp->textureCount && f < frames; f++)
		arrput(*tpags, sp->tpagIndices[f]);
}

static void add_background(int32_t **tpags, DataWin *dw, const char *prefix)
{
	uint32_t i;

	for (i = 0; i < dw->bgnd.count; i++)
		if (dw->bgnd.backgrounds[i].name && strncmp(dw->bgnd.backgrounds[i].name, prefix, strlen(prefix)) == 0)
			arrput(*tpags, dw->bgnd.backgrounds[i].tpagIndex);
}

/* Samus's animations for one suit: the sprites with Varia and Gravity twins (sX, sVX, sGX); Fusion mode uses
 * sX_fusion, then _varia or _gravity after it. */
static void add_suit(int32_t **tpags, Runner *runner, int32_t suit, bool fusion)
{
	DataWin *dw = runner->dataWin;
	uint32_t i;
	char name[96];

	for (i = 0; i < dw->sprt.count; i++) {
		const char *n = dw->sprt.sprites[i].name;
		int32_t base, gravity, pick, f;

		if (!n || strncmp(n, "sV", 2) != 0 || !n[2])
			continue;
		snprintf(name, sizeof name, "s%s", n + 2);
		base = sprite_named(runner, name);
		snprintf(name, sizeof name, "sG%s", n + 2);
		gravity = sprite_named(runner, name);
		if (base < 0 || gravity < 0)
			continue;
		pick = suit == 1 ? (int32_t)i : suit == 2 ? gravity : base;
		if (fusion) {
			snprintf(name, sizeof name, "s%s_fusion%s", n + 2,
				 suit == 1 ? "_varia" : suit == 2 ? "_gravity" : "");
			f = sprite_named(runner, name);
			if (f >= 0)
				pick = f;
		}
		add_sprite(tpags, dw, pick, UINT32_MAX);
	}
}

/* The metroid kinds whose objects (the metroid, its parts, the triggers that wake it) bring sprites on pages of their
 * own: the Alpha's turn alone sits on an Area 2 page that loaded mid-fight. */
static const char *const metroidKinds[] = { "MAlpha", "MGamma", "MZeta", "MOmega", "Queen" };
#define METROID_KINDS 5

/* A bit per kind with an object in the room; the objects are looked up by name once. */
static int32_t metroids_here(Runner *runner)
{
	static int32_t *objs[METROID_KINDS];
	static bool listed;
	DataWin *dw = runner->dataWin;
	int32_t mask = 0, k;
	uint32_t i;
	ptrdiff_t n;

	if (!listed) {
		listed = true;
		for (i = 0; i < dw->objt.count; i++)
			for (k = 0; k < METROID_KINDS; k++)
				if (dw->objt.objects[i].name && dw->objt.objects[i].name[0] == 'o' &&
				    strncmp(dw->objt.objects[i].name + 1, metroidKinds[k],
					    strlen(metroidKinds[k])) == 0)
					arrput(objs[k], (int32_t)i);
	}
	if (!runner->instancesByExactObject)
		return 0;
	for (k = 0; k < METROID_KINDS; k++)
		for (n = 0; n < arrlen(objs[k]) && !(mask & (1 << k)); n++)
			if (arrlen(runner->instancesByExactObject[objs[k][n]]) > 0)
				mask |= 1 << k;
	return mask;
}

/* Sounds too big for the boot preload that a room's objects play in the middle of action: read during the room's
 * load instead of at their first play (the first Robo Mine to die stalled the chain's frame ~30 ms on a Wii; the
 * Omega's first roar, breath and flames 19-27 ms each). */
#define ROOM_SOUNDS_MAX 12
static const struct {
	const char *object;
	const char *sounds[ROOM_SOUNDS_MAX];
} roomSounds[] = {
	{ "oRoboMine", { "sndElectricExpl", "sndElectricOverload" } },
	{ "oMOmega", { "sndMOmegaIntro", "sndMOmegaBreathe", "sndMOmegaAttack", "sndMOmegaFlame", "sndMOmegaDeath",
		       "sndMOmegaHit", "sndMGammaMorph", "sndMGammaEyes", "sndMZetaJump", "sndMZetaLand",
		       "sndMZetaFootstep", "sndKnockdown" } },
	/* the first Omega's entrance: troopers firing, hit and falling */
	{ "oFirstOmegaTrigger", { "sndMZetaHit", "sndGFTrooperPlasmaShot", "sndGFTrooperGunShot", "sndGFTrooperFall",
				  "sndGFTrooperHit", "sndArachnusSwipe" } },
};

static int32_t sound_named(DataWin *dw, const char *name)
{
	uint32_t i;

	for (i = 0; i < dw->sond.count; i++)
		if (dw->sond.sounds[i].name && strcmp(dw->sond.sounds[i].name, name) == 0)
			return (int32_t)i;
	return -1;
}

static void wish_sounds(Runner *runner)
{
	static int32_t lastRoom = -1;
	size_t k, j;
	int32_t obj;

	if (runner->currentRoomIndex == lastRoom || !runner->audioSystem || !runner->instancesByExactObject)
		return;
	lastRoom = runner->currentRoomIndex;
	for (k = 0; k < sizeof roomSounds / sizeof roomSounds[0]; k++) {
		obj = Wii_objectIndex(runner, roomSounds[k].object);
		if (obj < 0 || arrlen(runner->instancesByExactObject[obj]) == 0)
			continue;
		for (j = 0; j < ROOM_SOUNDS_MAX && roomSounds[k].sounds[j]; j++)
			WiiAudio_prefetch(runner->audioSystem, sound_named(runner->dataWin, roomSounds[k].sounds[j]));
	}
}

/* What the next seconds will draw, for the loader to fetch while idle (a page pulled mid-play stalls a Wii ~140 ms):
 * the title's first frame, the file select, the intro's art, the credits' ship and score screen, and in play Samus's
 * suit and muzzle flash, the HUD, the pause screen and the metroid the room holds. */
void Wii_wishPages(Runner *runner)
{
	enum { NONE = -1, TITLE_START = 100, FILE_SELECT = 101, CREDITS = 102, INTRO = 200, METROID = 1000 };
	static int32_t ingame = -2, suitVar, fusionVar,
		       controllerRoom, titleRoom, introRoom, subscreenRoom, creditsRoom;
	static int32_t last = NONE;
	DataWin *dw;
	Instance *g;
	int32_t key, suit, k, *tpags = NULL;
	uint32_t i;

	if (!runner || !runner->vmContext || !(g = runner->vmContext->globalScopeInstance))
		return;
	wish_sounds(runner);
	dw = runner->dataWin;
	if (ingame == -2) {
		ingame = Wii_varId(runner, "ingame", INSTANCE_GLOBAL);
		suitVar = Wii_varId(runner, "currentsuit", INSTANCE_GLOBAL);
		fusionVar = Wii_varId(runner, "mod_fusion", INSTANCE_GLOBAL);
		controllerRoom = Wii_roomIndex(runner, "rm_controller");
		titleRoom = Wii_roomIndex(runner, "titleroom");
		introRoom = Wii_roomIndex(runner, "gameintroroom");
		subscreenRoom = Wii_roomIndex(runner, "rm_subscreen");
		creditsRoom = Wii_roomIndex(runner, "rm_credits");
	}
	if (runner->currentRoomIndex == controllerRoom) {
		key = TITLE_START;
	} else if (runner->currentRoomIndex == titleRoom) {
		key = FILE_SELECT;
	} else if (runner->currentRoomIndex == creditsRoom) {
		key = CREDITS;
	} else if (runner->currentRoomIndex == introRoom ||
		   (ingame >= 0 && RValue_toReal(Instance_getSelfVar(g, ingame)) == 1)) {
		suit = suitVar >= 0 ? (int32_t)RValue_toReal(Instance_getSelfVar(g, suitVar)) : 0;
		key = (suit >= 0 && suit <= 2 ? suit : 0) * 2 +
		      (fusionVar >= 0 && RValue_toReal(Instance_getSelfVar(g, fusionVar)) == 1) +
		      (runner->currentRoomIndex == introRoom ? INTRO : 0) +
		      /* the pause screen's room has none of the room's objects, and play resumes in that room */
		      (runner->currentRoomIndex == subscreenRoom && last >= 0 ? last / METROID :
		       metroids_here(runner)) * METROID;
	} else {
		return;
	}
	if (key == last)
		return;
	last = key;
	if (key == TITLE_START) {
		add_sprite(&tpags, dw, sprite_named(runner, "sTitleAnimated"), 1);
		add_background(&tpags, dw, "bgAM2RTitle");
		add_background(&tpags, dw, "bgURL");
		add_background(&tpags, dw, "bgVersion");
		add_sprite(&tpags, dw, sprite_named(runner, "sVersionFont"), UINT32_MAX);
	} else if (key == FILE_SELECT) {
		add_background(&tpags, dw, "bgSaveSlot");
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sSaveSlot", 9) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_background(&tpags, dw, "bgGUIMetCount");
	} else if (key == CREDITS) {
		/* the ship that crosses the roll, then the score screen's art and lettering */
		add_sprite(&tpags, dw, sprite_named(runner, "sShip"), UINT32_MAX);
		add_background(&tpags, dw, "bgScoreScreen");
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sGUIFont", 8) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
	} else {
		if (key % METROID >= INTRO) {
			add_background(&tpags, dw, "bgIntro");
			add_background(&tpags, dw, "bgLoading");
		}
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && (strncmp(dw->sprt.sprites[i].name, "sMap", 4) == 0 ||
							 strncmp(dw->sprt.sprites[i].name, "sGUIFont", 8) == 0))
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_suit(&tpags, runner, key % METROID % INTRO / 2, key & 1);
		/* the beam's muzzle flash: the first shot after a load pulled its page mid-play */
		add_sprite(&tpags, dw, sprite_named(runner, "sMflash"), UINT32_MAX);
		for (i = 0; i < dw->sprt.count; i++)
			if (dw->sprt.sprites[i].name && strncmp(dw->sprt.sprites[i].name, "sSubScr", 7) == 0)
				add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
		add_background(&tpags, dw, "bg_SubScr");
		add_background(&tpags, dw, "bgMapScreen");
		for (k = 0; k < METROID_KINDS; k++)
			for (i = 0; (key / METROID) & (1 << k) && i < dw->sprt.count; i++)
				if (dw->sprt.sprites[i].name && dw->sprt.sprites[i].name[0] == 's' &&
				    strncmp(dw->sprt.sprites[i].name + 1, metroidKinds[k],
					    strlen(metroidKinds[k])) == 0)
					add_sprite(&tpags, dw, (int32_t)i, UINT32_MAX);
	}
	GxRenderer_wish(dw, tpags, (uint32_t)arrlen(tpags));
	logInfo("wish: %s, metroids %x, %d texture items\n",
		key == TITLE_START ? "title start" : key == FILE_SELECT ? "file select" : key == CREDITS ? "credits" :
		key % METROID >= INTRO ? "intro" : key & 1 ? "play, fusion" : "play",
		(int)(key / METROID), (int)arrlen(tpags));
	arrfree(tpags);
}

static int32_t coolTextCode = -1;

/* draw_cool_text(x, y, text, outline, top, bottom, alpha) draws its text nine times one pixel apart, copying and laying
 * out the string each time; the credits roll does that with all its rows every frame. */
static RValue wii_draw_cool_text(VMContext *ctx, RValue *args, int32_t argCount)
{
	Renderer *r = ctx->runner->renderer;
	PreprocessedText text;
	char *str;

	if (argCount != 7 || !r)
		return VM_callCodeIndex(ctx, coolTextCode, args, argCount);
	r->drawAlpha = (float)RValue_toReal(args[6]);
	r->drawColor = RValue_toColour(args[3]);
	str = RValue_toString(args[2], ctx->runner->dataWin);
	text = TextUtils_preprocessGmlTextIfNeeded(ctx->runner, str);
	GxRenderer_drawOutlinedText(r, text.text, RValue_toReal(args[0]), RValue_toReal(args[1]), (int32_t)r->drawColor,
				    (int32_t)RValue_toColour(args[4]), (int32_t)RValue_toColour(args[5]), r->drawAlpha);
	PreprocessedText_free(text);
	free(str);
	r->drawAlpha = 1.0f;
	return RValue_makeUndefined();
}

static int32_t resetMapCode = -1, mapVar, dmapVar, logOptionVar, iVar, jVar;

/* reset_map builds a "Resetting i,j" line for each of its 6,400 cells, which writelog drops unless AM2R's log option is
 * on: ~120 ms of every game load. Off, the cells are set here as the script would; on, the script itself runs. */
static RValue wii_reset_map(VMContext *ctx, RValue *args, int32_t argCount)
{
	Instance *g = ctx->globalScopeInstance;
	RValue zero = RValue_makeReal(0), cell = RValue_makeString("0");
	int32_t i, j;

	if (logOptionVar < 0 || RValue_toReal(Instance_getSelfVar(g, logOptionVar)) == 1 || !ctx->currentInstance)
		return VM_callCodeIndex(ctx, resetMapCode, args, argCount);
	for (i = 0; i < 80; i++)
		for (j = 0; j < 80; j++) {
			VM_arraySetWithCoW(ctx, IntRValueHashMap_getOrInsertUndefined(&g->selfVars, mapVar),
					   i * GML_LEGACY_ARRAY_STRIDE + j, cell);
			VM_arraySetWithCoW(ctx, IntRValueHashMap_getOrInsertUndefined(&g->selfVars, dmapVar),
					   i * GML_LEGACY_ARRAY_STRIDE + j, zero);
		}
	/* its loop counters are the caller's own variables */
	Instance_setSelfVar(ctx->currentInstance, iVar, RValue_makeReal(80));
	Instance_setSelfVar(ctx->currentInstance, jVar, RValue_makeReal(80));
	return RValue_makeUndefined();
}

static int32_t mapSurfCode = -1, mapBlockCode = -1, mystrVar, partVars[7];
static BuiltinFunc stringFn, charAtFn;

static BuiltinFunc drawSpriteFn;
static bool mapBlockNative;
static RValue wii_draw_mapblock(VMContext *ctx, RValue *args, int32_t argCount);

/* draw_map_surf(i, j) runs for each of the pause map's 1,682 cells and spends ~30 us a cell turning the cell's string
 * into seven of the caller's variables; here they are set the same way, before the same draw_mapblock. */
static RValue wii_draw_map_surf(VMContext *ctx, RValue *args, int32_t argCount)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	RValue cellVal, mystr, parts[7], call[10], r;
	int32_t cell, k;

	if (argCount != 2 || !self || !g)
		return VM_callCodeIndex(ctx, mapSurfCode, args, argCount);
	cell = RValue_toInt32(args[0]) * GML_LEGACY_ARRAY_STRIDE + RValue_toInt32(args[1]);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_X, "x", RValue_makeReal((RValue_toReal(args[0]) - 3) * 8), -1);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_Y, "y", RValue_makeReal((RValue_toReal(args[1]) - 3) * 8), -1);
	cellVal = GMLArray_getOnArrayRef(IntRValueHashMap_getOrInsertUndefined(&g->selfVars, mapVar), cell);
	mystr = stringFn(ctx, &cellVal, 1);
	Instance_setSelfVar(self, mystrVar, mystr);
	if (mystr.type == RVALUE_STRING && mystr.string && strcmp(mystr.string, "0") != 0) {
		for (k = 0; k < 7; k++) {
			RValue a[2] = { mystr, RValue_makeInt32(k + 1) };

			parts[k] = charAtFn(ctx, a, 2);
			Instance_setSelfVar(self, partVars[k], parts[k]);
		}
		call[0] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_X, "x", -1);
		call[1] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_Y, "y", -1);
		for (k = 0; k < 6; k++)
			call[2 + k] = parts[k];
		call[8] = GMLArray_getOnArrayRef(IntRValueHashMap_getOrInsertUndefined(&g->selfVars, dmapVar), cell);
		call[9] = parts[6];
		r = mapBlockNative ? wii_draw_mapblock(ctx, call, 10) : VM_callCodeIndex(ctx, mapBlockCode, call, 10);
		RValue_free(&r);
		RValue_free(&call[0]);
		RValue_free(&call[1]);
		for (k = 0; k < 7; k++)
			RValue_free(&parts[k]);
	}
	RValue_free(&mystr);
	return RValue_makeUndefined();
}

static int32_t controlUser4Code = -1, screenSurfaceVar;
static BuiltinFunc engineSurfaceFree;

/* oControl's user event 4 frees screen_surface every step but keeps the id, so whichever surface reused the slot (the
 * water's, the lights') was freed each frame. Here the id goes with the surface, back to Create's -1. */
static RValue wii_surface_free(VMContext *ctx, RValue *args, int32_t argCount)
{
	RValue r = engineSurfaceFree(ctx, args, argCount);

	if (ctx->currentCodeIndex == controlUser4Code && ctx->currentInstance)
		Instance_setSelfVar(ctx->currentInstance, screenSurfaceVar, RValue_makeReal(-1));
	return r;
}

/* The bytecode's instance for widescreen_space: oControl's object index in 1.5.5. */
#define WATER_CONTROL_OBJECT 427

enum { WV_MYSURF, WV_AMP, WV_THETA, WV_FREQ, WV_YBOTTOM, WV_YTOP, WV_XX, WV_YY, WV_COUNT };
enum { WF_SURFACE_EXISTS, WF_FLOOR, WF_LERP, WF_ROUND, WF_MAX, WF_MIN, WF_SIN, WF_DRAW, WF_COUNT };
static const char *const waterVarNames[WV_COUNT] = { "mysurf", "AMP", "THETA", "FREQ", "y_bottom", "y_top", "xx",
						     "yy" };
static const char *const waterFnNames[WF_COUNT] = { "surface_exists", "floor", "lerp", "round", "max", "min", "sin",
						    "draw_surface_part_ext" };
static int32_t waterDrawCode = -1, waterVars[WV_COUNT], waveHeightVar, widescreenSpaceVar;
static int32_t waterFns[WF_COUNT];

static RValue water_call(VMContext *ctx, int fn, RValue *args, int32_t argCount)
{
	return ((BuiltinFunc)ctx->funcCallCache[waterFns[fn]].builtin)(ctx, args, argCount);
}

/* The build's -ffast-math may regroup or reciprocate float steps; the bytecode rounds each one on its own. */
#define GML_ROUNDING __attribute__((optimize("no-fast-math", "fp-contract=off")))

/* GML's numeric compare: equal within GML_MATH_EPSILON, otherwise by sign. */
static GML_ROUNDING int gml_compare(GMLReal a, GMLReal b)
{
	GMLReal d = a - b;

	return GMLReal_fabs(d) <= GML_MATH_EPSILON ? 0 : d < 0 ? -1 : 1;
}

static bool real_var(Instance *inst, int32_t var, GMLReal *out)
{
	RValue v = Instance_getSelfVar(inst, var);

	*out = v.real;
	return v.type == RVALUE_REAL;
}

/* A number the bytecode's arithmetic treats exactly as this double: any real, or an int small enough that int math
 * can't wrap where it would not (make_liquid stores room code's integer literals as ints). */
static bool num_var(Instance *inst, int32_t var, GMLReal *out)
{
	RValue v = Instance_getSelfVar(inst, var);

	if (v.type == RVALUE_INT32 && v.int32 > -(1 << 24) && v.int32 < (1 << 24)) {
		*out = (GMLReal)v.int32;
		return true;
	}
	*out = v.real;
	return v.type == RVALUE_REAL;
}

static bool flag_var(Instance *inst, int32_t var, bool *out)
{
	RValue v = Instance_getSelfVar(inst, var);

	*out = RValue_toBool(v);
	return v.type == RVALUE_REAL || v.type == RVALUE_INT32 || v.type == RVALUE_BOOL;
}

static bool real_builtin(VMContext *ctx, int16_t id, const char *name, int32_t index, GMLReal *out)
{
	RValue v = VMBuiltins_getVariable(ctx, ctx->currentInstance, id, name, index);
	bool real = v.type == RVALUE_REAL;

	*out = v.real;
	RValue_free(&v);
	return real;
}

static void water_strip(VMContext *ctx, RValue surf, GMLReal top, GMLReal width, GMLReal height, GMLReal x, GMLReal y)
{
	RValue args[11] = { surf, RValue_makeInt32(0), RValue_makeReal(top), RValue_makeReal(width),
			    RValue_makeReal(height), RValue_makeReal(x), RValue_makeReal(y), RValue_makeInt32(1),
			    RValue_makeInt32(1), RValue_makeInt32(-1), RValue_makeReal(0.2) };
	RValue r = water_call(ctx, WF_DRAW, args, 11);

	RValue_free(&r);
}

/* oWaterFXV2's Draw lays the screen copy back in wavy rows: ~31,000 bytecode ops a frame for ~480 draws. Here it runs
 * as the bytecode does, same builtins in the same order and the same double math; a non-real input interprets it. */
static GML_ROUNDING RValue wii_water_draw(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	Instance *control = VM_findInstanceByTarget(ctx, WATER_CONTROL_OBJECT);
	GMLReal v[WV_COUNT], wave, ws, fps, viewX, viewY, viewW, viewH, y, heightTop, xx = 0, yy;
	RValue surf, r, x, args[3];
	bool exists, ok;
	int k;

	ok = self && g && control && real_var(g, waveHeightVar, &wave) && real_var(control, widescreenSpaceVar, &ws);
	for (k = 0; ok && k < WV_XX; k++)
		ok = real_var(self, waterVars[k], &v[k]);
	ok = ok && real_builtin(ctx, BUILTIN_VAR_FPS, "fps", -1, &fps) && real_builtin(ctx, BUILTIN_VAR_Y, "y", -1, &y);
	ok = ok && real_builtin(ctx, BUILTIN_VAR_VIEW_XVIEW, "view_xview", 0, &viewX) &&
	     real_builtin(ctx, BUILTIN_VAR_VIEW_YVIEW, "view_yview", 0, &viewY) &&
	     real_builtin(ctx, BUILTIN_VAR_VIEW_WVIEW, "view_wview", 0, &viewW) &&
	     real_builtin(ctx, BUILTIN_VAR_VIEW_HVIEW, "view_hview", 0, &viewH);
	if (!ok)
		return VM_interpretCode(ctx, waterDrawCode);

	surf = Instance_getSelfVar(self, waterVars[WV_MYSURF]);
	r = water_call(ctx, WF_SURFACE_EXISTS, &surf, 1);
	exists = RValue_toBool(r);
	RValue_free(&r);
	if (!exists)
		return RValue_makeUndefined();

	/* wave_height = max(round(lerp(wave_height, floor((60 - (fps - 1)) ^ 1.5), 0.1)) - 1, 1),
	 * ^ being xor on int64s */
	x = RValue_makeInt32(RValue_toInt32(RValue_makeInt64(RValue_toInt64(RValue_makeReal(60 - (fps - 1))))) ^
			     RValue_toInt32(RValue_makeInt64((int64_t)1.5)));
	args[0] = RValue_makeReal(wave);
	args[1] = water_call(ctx, WF_FLOOR, &x, 1);
	args[2] = RValue_makeReal(0.1);
	r = water_call(ctx, WF_LERP, args, 3);
	args[0] = water_call(ctx, WF_ROUND, &r, 1);
	args[0] = RValue_makeReal(args[0].real - 1);
	args[1] = RValue_makeInt32(1);
	r = water_call(ctx, WF_MAX, args, 2);
	Instance_setSelfVar(g, waveHeightVar, r);
	wave = r.real;

	args[0] = RValue_makeReal(v[WV_YBOTTOM] - v[WV_YTOP]);
	args[1] = r;
	heightTop = water_call(ctx, WF_MIN, args, 2).real;

	if (gml_compare(viewY + viewH, y) < 0)
		return RValue_makeUndefined();
	for (yy = 0; gml_compare(yy, 240) < 0; yy = yy + wave) {
		args[0] = RValue_makeReal(v[WV_THETA] + v[WV_FREQ] * yy / v[WV_YBOTTOM]);
		xx = v[WV_AMP] * water_call(ctx, WF_SIN, args, 1).real;
		if (gml_compare(yy, v[WV_YTOP]) >= 0) {
			water_strip(ctx, surf, yy, viewW + ws, wave, viewX + xx - ws / 2, viewY + yy);
			if (gml_compare(yy - v[WV_YTOP], wave) < 0)
				water_strip(ctx, surf, v[WV_YTOP], viewW + ws, yy - v[WV_YTOP], viewX + xx - ws / 2,
					    viewY + v[WV_YTOP]);
		}
	}
	Instance_setSelfVar(self, waterVars[WV_XX], RValue_makeReal(xx));
	Instance_setSelfVar(self, waterVars[WV_YY], RValue_makeReal(yy));
	if (gml_compare(heightTop, wave) < 0)
		water_strip(ctx, surf, v[WV_YTOP], viewW + ws, heightTop, viewX + xx - ws / 2, y);
	return RValue_makeUndefined();
}

enum { TF_LIST_SIZE, TF_LIST_VALUE, TF_SET_ALPHA, TF_COUNT };
static const char *const tileFnNames[TF_COUNT] = { "ds_list_size", "ds_list_find_value", "tile_set_alpha" };
static int32_t tileAlphaCode = -1, tileListVar, falphaVar, tileFns[TF_COUNT];

static RValue tile_call(VMContext *ctx, int fn, RValue *args, int32_t argCount)
{
	return ((BuiltinFunc)ctx->funcCallCache[tileFns[fn]].builtin)(ctx, args, argCount);
}

/* oTilesRandomAlpha's user event 0 gives every tile in its list the step's alpha: ~20 bytecode ops a tile, over a
 * thousand tiles in some Area 5 rooms. The same three builtins run here in the same order. */
static GML_ROUNDING RValue wii_tiles_alpha(VMContext *ctx)
{
	Instance *self = ctx->currentInstance;
	RValue list, alpha, size, args[2], r;
	GMLReal i;

	list = self ? Instance_getSelfVar(self, tileListVar) : RValue_makeUndefined();
	alpha = self ? Instance_getSelfVar(self, falphaVar) : RValue_makeUndefined();
	if (list.type != RVALUE_REAL || alpha.type != RVALUE_REAL)
		return VM_interpretCode(ctx, tileAlphaCode);
	for (i = 0;; i = i + 1) {
		size = tile_call(ctx, TF_LIST_SIZE, &list, 1);
		if (gml_compare(i, RValue_toReal(size)) >= 0) {
			RValue_free(&size);
			break;
		}
		RValue_free(&size);
		args[0] = list;
		args[1] = RValue_makeReal(i);
		args[0] = tile_call(ctx, TF_LIST_VALUE, args, 2);
		args[1] = alpha;
		r = tile_call(ctx, TF_SET_ALPHA, args, 2);
		RValue_free(&r);
		RValue_free(&args[0]);
	}
	return RValue_makeUndefined();
}

enum { BF_DESTROY, BF_LERP, BF_COUNT };
static const char *const bubbleFnNames[BF_COUNT] = { "instance_destroy", "lerp" };
static int32_t bubbleStepCode = -1, fadeoutVar, erisVar, waterlevelVar, wateroffsetVar, bubbleFns[BF_COUNT];

static RValue bubble_call(VMContext *ctx, int fn, RValue *args, int32_t argCount)
{
	return ((BuiltinFunc)ctx->funcCallCache[bubbleFns[fn]].builtin)(ctx, args, argCount);
}

static GMLReal builtin_real(VMContext *ctx, int16_t id, const char *name, int32_t index)
{
	RValue v = VMBuiltins_getVariable(ctx, ctx->currentInstance, id, name, index);
	GMLReal r = RValue_toReal(v);

	RValue_free(&v);
	return r;
}

static void set_builtin(VMContext *ctx, int16_t id, const char *name, GMLReal value)
{
	VMBuiltins_setVariable(ctx, ctx->currentInstance, id, name, RValue_makeReal(value), -1);
}

static void bubble_destroy(VMContext *ctx)
{
	RValue r = bubble_call(ctx, BF_DESTROY, NULL, 0);

	RValue_free(&r);
}

/* oLBubble's step runs for every bubble in a water room, ~180 at once in rm_a5h04. The same builtin variables are
 * read and written in the same order as the bytecode; a non-real input interprets it. */
static GML_ROUNDING RValue wii_bubble_step(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	Instance *control = VM_findInstanceByTarget(ctx, WATER_CONTROL_OBJECT);
	GMLReal level, offset, ws, v, x, y, vx, vy;
	RValue args[3], r;
	bool fadeout, eris, outside;

	if (!self || !g || !control || !flag_var(self, fadeoutVar, &fadeout) || !flag_var(self, erisVar, &eris) ||
	    !num_var(g, waterlevelVar, &level) || !num_var(g, wateroffsetVar, &offset) ||
	    !num_var(control, widescreenSpaceVar, &ws))
		return VM_interpretCode(ctx, bubbleStepCode);

	if (fadeout) {
		set_builtin(ctx, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha",
			    builtin_real(ctx, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha", -1) - 0.1);
		if (gml_compare(builtin_real(ctx, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha", -1), 0) <= 0)
			bubble_destroy(ctx);
	} else if (gml_compare(builtin_real(ctx, BUILTIN_VAR_Y, "y", -1), level + offset + 10) < 0) {
		Instance_setSelfVar(self, fadeoutVar, RValue_makeReal(1));
	}

	if (!eris) {
		v = builtin_real(ctx, BUILTIN_VAR_HSPEED, "hspeed", -1);
		if (gml_compare(v, 0) != 0)
			set_builtin(ctx, BUILTIN_VAR_HSPEED, "hspeed", v * 0.9);
		v = builtin_real(ctx, BUILTIN_VAR_VSPEED, "vspeed", -1);
		if (gml_compare(v, -1) > 0)
			set_builtin(ctx, BUILTIN_VAR_VSPEED, "vspeed", v - 0.02);
		if (gml_compare(builtin_real(ctx, BUILTIN_VAR_VSPEED, "vspeed", -1), 2) > 0)
			set_builtin(ctx, BUILTIN_VAR_VSPEED, "vspeed", 2);
	} else {
		args[0] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_SPEED, "speed", -1);
		args[1] = RValue_makeInt32(0);
		args[2] = RValue_makeReal(0.05);
		r = bubble_call(ctx, BF_LERP, args, 3);
		RValue_free(&args[0]);
		VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_SPEED, "speed", r, -1);
		RValue_free(&r);
	}

	x = builtin_real(ctx, BUILTIN_VAR_X, "x", -1);
	vx = builtin_real(ctx, BUILTIN_VAR_VIEW_XVIEW, "view_xview", 0);
	outside = gml_compare(x, vx - ws / 2) < 0 ||
		  gml_compare(x, vx + builtin_real(ctx, BUILTIN_VAR_VIEW_WVIEW, "view_wview", 0) + ws / 2) > 0;
	if (!outside) {
		y = builtin_real(ctx, BUILTIN_VAR_Y, "y", -1);
		vy = builtin_real(ctx, BUILTIN_VAR_VIEW_YVIEW, "view_yview", 0);
		outside = gml_compare(y, vy) < 0 ||
			  gml_compare(y, vy + builtin_real(ctx, BUILTIN_VAR_VIEW_HVIEW, "view_hview", 0)) > 0;
	}
	if (outside) {
		set_builtin(ctx, BUILTIN_VAR_VISIBLE, "visible", 0);
		if (gml_compare(builtin_real(ctx, BUILTIN_VAR_FPS_REAL, "fps_real", -1), 60) < 0)
			bubble_destroy(ctx);
	}
	return RValue_makeUndefined();
}

/* A GML number as the VM's arithmetic carries it: int op int stays an int (wrapping), anything with a real is real. */
typedef struct {
	bool isInt;
	int32_t i;
	GMLReal r;
} GmlNum;

static GmlNum num_int(int32_t v)
{
	return (GmlNum){ true, v, 0 };
}

static GmlNum num_real(GMLReal v)
{
	return (GmlNum){ false, 0, v };
}

static GMLReal num_val(GmlNum n)
{
	return n.isInt ? (GMLReal)n.i : n.r;
}

static GML_ROUNDING GmlNum num_add(GmlNum a, GmlNum b)
{
	return a.isInt && b.isInt ? num_int((int32_t)((uint32_t)a.i + (uint32_t)b.i))
				  : num_real(num_val(a) + num_val(b));
}

static GML_ROUNDING GmlNum num_sub(GmlNum a, GmlNum b)
{
	return a.isInt && b.isInt ? num_int((int32_t)((uint32_t)a.i - (uint32_t)b.i))
				  : num_real(num_val(a) - num_val(b));
}

static GML_ROUNDING GmlNum num_mul(GmlNum a, GmlNum b)
{
	return a.isInt && b.isInt ? num_int((int32_t)((uint32_t)a.i * (uint32_t)b.i))
				  : num_real(num_val(a) * num_val(b));
}

static RValue num_rvalue(GmlNum n)
{
	return n.isInt ? RValue_makeInt32(n.i) : RValue_makeReal(n.r);
}

static bool get_num(RValue v, GmlNum *out)
{
	if (v.type == RVALUE_INT32)
		*out = num_int(v.int32);
	else if (v.type == RVALUE_REAL)
		*out = num_real(v.real);
	else
		return false;
	return true;
}

static const char mapMarkChars[] = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh";
static const struct {
	int8_t arg;
	char c;
	int16_t sprite;
	int8_t dx, dy;
} mapWalls[] = {
	{ 2, '1', 493, 0, 0 }, { 4, '1', 493, 0, 7 }, { 5, '1', 494, 0, 0 }, { 3, '1', 494, 7, 0 },
	{ 2, '2', 495, 0, 0 }, { 4, '2', 495, 0, 7 }, { 5, '2', 496, 0, 0 }, { 3, '2', 496, 7, 0 },
};
static const struct {
	char c;
	int8_t explored; /* 0: any */
	int8_t sub;
} mapItems[] = {
	{ '1', 0, 0 }, { '2', 0, 1 }, { '3', 1, 2 }, { '3', 2, 5 }, { '4', 1, 2 }, { '4', 2, 3 }, { '5', 0, 4 },
	{ 'U', 0, 8 }, { 'D', 0, 9 }, { 'L', 0, 10 }, { 'R', 0, 11 }, { 'H', 0, 12 }, { 'V', 0, 13 }, { 'C', 0, 14 },
};

static bool map_char_is(RValue v, char c)
{
	return v.string && v.string[0] == c && v.string[1] == '\0';
}

/* Which one-character case of chars a string argument matches, or -1. */
static int map_char_case(RValue v, const char *chars)
{
	const char *hit;

	if (!v.string || v.string[0] == '\0' || v.string[1] != '\0')
		return -1;
	hit = strchr(chars, v.string[0]);
	return hit ? (int)(hit - chars) : -1;
}

static void map_sprite(VMContext *ctx, int32_t sprite, int32_t sub, GmlNum x, GmlNum y)
{
	RValue args[4] = { RValue_makeInt32(sprite), RValue_makeInt32(sub), num_rvalue(x), num_rvalue(y) };
	RValue r = drawSpriteFn(ctx, args, 4);

	RValue_free(&r);
}

/* draw_mapblock draws one map cell from its one-character codes with ~70 string compares in bytecode, for each HUD
 * cell every frame and the pause map's 1,682. Same draws in the same order; any other argument type runs the script. */
static GML_ROUNDING RValue wii_draw_mapblock(VMContext *ctx, RValue *args, int32_t argCount)
{
	GmlNum x, y, explored, dx, dy;
	size_t k;
	int c;

	if (argCount != 10 || !get_num(args[0], &x) || !get_num(args[1], &y) || !get_num(args[8], &explored))
		return VM_callCodeIndex(ctx, mapBlockCode, args, argCount);
	for (k = 2; k < 10; k++)
		if (k != 8 && args[k].type != RVALUE_STRING)
			return VM_callCodeIndex(ctx, mapBlockCode, args, argCount);
	if (gml_compare(num_val(explored), 0) <= 0)
		return RValue_makeUndefined();

	c = map_char_case(args[6], "1234");
	if (c >= 0)
		map_sprite(ctx, 492, c, x, y);
	c = map_char_case(args[9], mapMarkChars);
	if (c >= 0)
		map_sprite(ctx, 498, c, x, y);
	for (k = 0; k < sizeof mapWalls / sizeof mapWalls[0]; k++) {
		if (!map_char_is(args[mapWalls[k].arg], mapWalls[k].c))
			continue;
		dx = mapWalls[k].dx ? num_add(x, num_int(mapWalls[k].dx)) : x;
		dy = mapWalls[k].dy ? num_add(y, num_int(mapWalls[k].dy)) : y;
		map_sprite(ctx, mapWalls[k].sprite, 0, dx, dy);
	}
	for (k = 0; k < sizeof mapItems / sizeof mapItems[0]; k++)
		if (map_char_is(args[7], mapItems[k].c) &&
		    (!mapItems[k].explored || gml_compare(num_val(explored), mapItems[k].explored) == 0))
			map_sprite(ctx, 491, mapItems[k].sub, x, y);
	if (gml_compare(num_val(explored), 10) == 0)
		map_sprite(ctx, 491, 6, x, y);
	if (gml_compare(num_val(explored), 11) == 0)
		map_sprite(ctx, 491, 7, x, y);
	return RValue_makeUndefined();
}

enum { OF_COLOUR_RGB, OF_SET_COLOR, OF_SET_BLEND, OF_RECTANGLE, OF_BACKGROUND_EXT, OF_COUNT };
static const char *const owaterFnNames[OF_COUNT] = { "make_colour_rgb", "draw_set_color", "draw_set_blend_mode",
						     "draw_rectangle", "draw_background_ext" };
static int32_t owaterDrawCode = -1, owaterIVar, owaterYoffsetVar, owaterXoffsetVar, owaterBgVar, owaterAlphaVar,
	       owaterWideVar, owaterFns[OF_COUNT];

static void owater_call(VMContext *ctx, int fn, RValue *args, int32_t argCount)
{
	RValue r = ((BuiltinFunc)ctx->funcCallCache[owaterFns[fn]].builtin)(ctx, args, argCount);

	RValue_free(&r);
}

/* oWater's Draw tints everything under the surface and lays the surface strip in 32-pixel columns across the view: a
 * few dozen columns of ~50 bytecode ops each. Same builtins, order, argument types and arithmetic as the bytecode. */
static GML_ROUNDING RValue wii_owater_draw(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	GmlNum level, yoff, xoff, wide, ws, top, viewX, viewY, viewH, roomW, x, i, count;
	RValue args[8], r;
	GMLReal half;
	int32_t n;

	if (!self || !g || !get_num(Instance_getSelfVar(g, waterlevelVar), &level) ||
	    !get_num(Instance_getSelfVar(self, owaterYoffsetVar), &yoff) ||
	    !get_num(Instance_getSelfVar(self, owaterXoffsetVar), &xoff) ||
	    !get_num(Instance_getSelfVar(self, owaterWideVar), &wide) ||
	    !get_num(Instance_getSelfVar(self, widescreenSpaceVar), &ws))
		return VM_interpretCode(ctx, owaterDrawCode);
	r = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_VIEW_XVIEW, "view_xview", 0);
	if (!get_num(r, &viewX))
		return VM_interpretCode(ctx, owaterDrawCode);
	r = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_VIEW_YVIEW, "view_yview", 0);
	if (!get_num(r, &viewY))
		return VM_interpretCode(ctx, owaterDrawCode);
	r = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_VIEW_HVIEW, "view_hview", 0);
	if (!get_num(r, &viewH))
		return VM_interpretCode(ctx, owaterDrawCode);
	r = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_ROOM_WIDTH, "room_width", -1);
	if (!get_num(r, &roomW))
		return VM_interpretCode(ctx, owaterDrawCode);
	r = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_X, "x", -1);
	if (!get_num(r, &x))
		return VM_interpretCode(ctx, owaterDrawCode);

	top = num_add(level, yoff);
	if (gml_compare(num_val(num_add(viewY, viewH)), num_val(top)) < 0)
		return RValue_makeUndefined();
	i = num_real(0);
	Instance_setSelfVar(self, owaterIVar, num_rvalue(i));
	args[0] = RValue_makeInt32(10);
	args[1] = RValue_makeInt32(10);
	args[2] = RValue_makeInt32(80);
	r = ((BuiltinFunc)ctx->funcCallCache[owaterFns[OF_COLOUR_RGB]].builtin)(ctx, args, 3);
	owater_call(ctx, OF_SET_COLOR, &r, 1);
	RValue_free(&r);
	args[0] = RValue_makeInt32(1);
	owater_call(ctx, OF_SET_BLEND, args, 1);
	half = num_val(ws) / 2.0;
	args[0] = RValue_makeReal(num_val(viewX) - half);
	args[1] = num_rvalue(top);
	args[2] = RValue_makeReal(num_val(num_add(viewX, num_int(320))) + half);
	args[3] = num_rvalue(num_add(viewY, num_int(250)));
	args[4] = RValue_makeInt32(0);
	owater_call(ctx, OF_RECTANGLE, args, 5);

	count = num_add(num_add(num_real(num_val(roomW) / 32.0), num_int(1)), num_mul(num_int(4), wide));
	n = RValue_toInt32(num_rvalue(count));
	for (; n > 0; n--) {
		GmlNum col = num_mul(i, num_int(32));

		if (gml_compare(num_val(col), num_val(num_sub(viewX, num_int(33))) - half) >= 0 &&
		    gml_compare(num_val(col), num_val(num_add(num_add(viewX, num_int(360)), ws))) < 0 &&
		    gml_compare(num_val(num_add(top, num_int(224))), num_val(viewY)) > 0) {
			args[0] = Instance_getSelfVar(self, owaterBgVar);
			args[1] = RValue_makeReal(num_val(num_add(num_add(x, col), xoff)) - half);
			args[2] = num_rvalue(top);
			args[3] = RValue_makeInt32(1);
			args[4] = RValue_makeInt32(1);
			args[5] = RValue_makeInt32(0);
			args[6] = RValue_makeInt32(-1);
			args[7] = Instance_getSelfVar(self, owaterAlphaVar);
			owater_call(ctx, OF_BACKGROUND_EXT, args, 8);
		}
		i = num_add(i, num_int(1));
	}
	Instance_setSelfVar(self, owaterIVar, num_rvalue(i));
	args[0] = RValue_makeInt32(0);
	owater_call(ctx, OF_SET_BLEND, args, 1);
	return RValue_makeUndefined();
}

enum { LF_SET_BLEND, LF_BG_HEIGHT, LF_BACKGROUND_EXT, LF_COUNT };
static const char *const lavaFnNames[LF_COUNT] = { "draw_set_blend_mode", "background_get_height",
						   "draw_background_ext" };
static int32_t lavaDrawCode = -1, lavaYoffsetVar, lavaBgVar, controlWideVar, lavaFns[LF_COUNT];

static RValue lava_call(VMContext *ctx, int fn, RValue *args, int32_t argCount)
{
	return ((BuiltinFunc)ctx->funcCallCache[lavaFns[fn]].builtin)(ctx, args, argCount);
}

static bool builtin_num(VMContext *ctx, int16_t id, const char *name, int32_t index, GmlNum *out)
{
	RValue v = VMBuiltins_getVariable(ctx, ctx->currentInstance, id, name, index);
	bool ok = get_num(v, out);

	RValue_free(&v);
	return ok;
}

/* oLavaSurface's Draw adds the glow over the whole room, then the surface strip in 32-pixel columns: same builtins,
 * order, argument types and arithmetic as the bytecode. */
static GML_ROUNDING RValue wii_lava_draw(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *g = ctx->globalScopeInstance;
	Instance *control = VM_findInstanceByTarget(ctx, WATER_CONTROL_OBJECT);
	GmlNum level, yoff, wide, ws, viewX, viewY, viewH, roomW, roomH, x, y, i, limit;
	RValue args[8], r, height;
	GMLReal half;

	if (!self || !g || !control || !get_num(Instance_getSelfVar(g, waterlevelVar), &level) ||
	    !get_num(Instance_getSelfVar(self, lavaYoffsetVar), &yoff) ||
	    !get_num(Instance_getSelfVar(control, controlWideVar), &wide) ||
	    !get_num(Instance_getSelfVar(control, widescreenSpaceVar), &ws) ||
	    !builtin_num(ctx, BUILTIN_VAR_VIEW_XVIEW, "view_xview", 0, &viewX) ||
	    !builtin_num(ctx, BUILTIN_VAR_VIEW_YVIEW, "view_yview", 0, &viewY) ||
	    !builtin_num(ctx, BUILTIN_VAR_VIEW_HVIEW, "view_hview", 0, &viewH) ||
	    !builtin_num(ctx, BUILTIN_VAR_ROOM_WIDTH, "room_width", -1, &roomW) ||
	    !builtin_num(ctx, BUILTIN_VAR_ROOM_HEIGHT, "room_height", -1, &roomH) ||
	    !builtin_num(ctx, BUILTIN_VAR_X, "x", -1, &x) || !builtin_num(ctx, BUILTIN_VAR_Y, "y", -1, &y))
		return VM_interpretCode(ctx, lavaDrawCode);

	if (gml_compare(num_val(num_add(viewY, viewH)), num_val(num_sub(num_add(level, yoff), num_int(9)))) < 0)
		return RValue_makeUndefined();
	args[0] = RValue_makeInt32(1);
	r = lava_call(ctx, LF_SET_BLEND, args, 1);
	RValue_free(&r);
	half = num_val(ws) / 2.0;
	args[0] = Instance_getSelfVar(self, lavaBgVar);
	height = lava_call(ctx, LF_BG_HEIGHT, args, 1);
	{
		GmlNum h;
		RValue a[8];

		if (!get_num(height, &h))
			h = num_real(RValue_toReal(height));
		a[0] = RValue_makeInt32(133);
		a[1] = RValue_makeReal(num_val(x) - half);
		a[2] = num_rvalue(num_add(num_add(num_sub(y, num_int(9)), h), yoff));
		a[3] = RValue_makeReal(num_val(roomW) / num_val(num_sub(num_int(10), num_mul(num_int(2), wide))));
		a[4] = RValue_makeReal(num_val(roomH) / 10.0);
		a[5] = RValue_makeInt32(0);
		a[6] = RValue_makeInt32(-1);
		a[7] = RValue_makeReal(0.6);
		r = lava_call(ctx, LF_BACKGROUND_EXT, a, 8);
		RValue_free(&r);
	}
	RValue_free(&height);

	limit = num_add(roomW, ws);
	for (i = num_real(0); gml_compare(num_val(i), num_val(limit)) < 0; i = num_add(i, num_int(32))) {
		GmlNum col = num_add(x, i);

		if (gml_compare(num_val(col), num_val(num_sub(viewX, num_int(33)))) >= 0 &&
		    gml_compare(num_val(col), num_val(num_add(num_add(viewX, num_int(360)), ws))) < 0 &&
		    gml_compare(num_val(num_add(num_add(level, yoff), num_int(15))), num_val(viewY)) > 0) {
			args[0] = Instance_getSelfVar(self, lavaBgVar);
			args[1] = RValue_makeReal(num_val(col) - half);
			args[2] = num_rvalue(num_add(num_sub(y, num_int(9)), yoff));
			args[3] = RValue_makeInt32(1);
			args[4] = RValue_makeInt32(1);
			args[5] = RValue_makeInt32(0);
			args[6] = RValue_makeInt32(-1);
			args[7] = RValue_makeReal(0.8);
			r = lava_call(ctx, LF_BACKGROUND_EXT, args, 8);
			RValue_free(&r);
		}
	}
	args[0] = RValue_makeInt32(0);
	r = lava_call(ctx, LF_SET_BLEND, args, 1);
	RValue_free(&r);
	return RValue_makeUndefined();
}

/* Ours, then AM2R's, in its markup (* heading, / centered, a=b paired, ; next row). The roll scrolls at a speed set by
 * its row count to last as long as its music, so these rows make it a little faster, not longer. */
static const char credits[] = "*AM2R GX;;;;;;;;;;;;"
	"*Wii Port;/cmyksoda;;"
	"*Beta Testers;bonkmaykr=Erzaui;NiuuS=Splatterpunk;;"
	"*Classic Controller Pro Art;/Asophia Mills;;"
	"*Made Possible By;/AM2R Community Developers;/Butterscotch;/Milton 'DoctorM64' Guasti;;"
	"*Special Thanks;/My sweet girlfriend,;/for always supporting me <3;;/syl, for being the sweetest syl ever;;;;"
	"*Another ";

/* AM2R's title card (its name, then 11 blank rows) heads its section the way AM2R heads Metroid II's staff. */
#define AM2R_TITLE " 2 Remake;;;;;;;;;;;;"
#define AM2R_HEADER " 2 Remake Staff;;"

void Wii_addCredits(DataWin *dw)
{
	static char *am2r;

	repeat(dw->strg.count, i) {
		const char *s = dw->strg.strings[i];

		if (strcmp(s, "*Another ") == 0) {
			dw->strg.strings[i] = credits;
		} else if (!am2r && strncmp(s, AM2R_TITLE, sizeof AM2R_TITLE - 1) == 0) {
			am2r = safeMalloc(sizeof AM2R_HEADER + strlen(s + sizeof AM2R_TITLE - 1));
			strcpy(am2r, AM2R_HEADER);
			strcat(am2r, s + sizeof AM2R_TITLE - 1);
			dw->strg.strings[i] = am2r;
		}
	}
}

/* oPBombExpl's collision with a solid runs for every solid a power bomb's blast covers, ~100 a frame:
 * with (other.id) { if (sprite_index is a bomb block's) { event_user(1); visible = 1 } }. For a plain solid that does
 * nothing; a bomb block, or anything unusual, runs the bytecode. */
static int32_t pbombSolidCode = -1;

static GML_ROUNDING RValue wii_pbomb_solid(VMContext *ctx)
{
	static const int blockSprites[] = { 118, 115, 116, 113, 114, 117 };
	Instance *other = ctx->otherInstance, *target;
	RValue r;
	int32_t targetId;
	GMLReal sprite;
	uint32_t k;

	if (!ctx->currentInstance || !other)
		return VM_interpretCode(ctx, pbombSolidCode);
	r = VMBuiltins_getVariable(ctx, other, BUILTIN_VAR_ID, "id", -1);
	targetId = RValue_toInt32(r);
	RValue_free(&r);
	target = targetId >= INSTANCE_ID_BASE ? hmget(ctx->runner->instancesById, targetId) : NULL;
	if (!target || !target->active)
		return VM_interpretCode(ctx, pbombSolidCode);

	r = VMBuiltins_getVariable(ctx, target, BUILTIN_VAR_SPRITE_INDEX, "sprite_index", -1);
	if (r.type != RVALUE_REAL && r.type != RVALUE_INT32 && r.type != RVALUE_INT64) {
		RValue_free(&r);
		return VM_interpretCode(ctx, pbombSolidCode);
	}
	sprite = RValue_toReal(r);
	for (k = 0; k < sizeof blockSprites / sizeof blockSprites[0]; k++)
		if (gml_compare(sprite, blockSprites[k]) == 0)
			return VM_interpretCode(ctx, pbombSolidCode);
	return RValue_makeUndefined();
}

/* skorp_get_lure(spr) runs for every Skorp every frame: in a room whose name has "6a" or "6b" at characters 5-6 it
 * returns the sprite named sprite_get_name(spr) + "A6" or "A6b", elsewhere spr itself. The room's answer is kept. */
static int32_t skorpLureCode = -1;
static BuiltinFunc spriteGetNameFn;

static RValue wii_skorp_get_lure(VMContext *ctx, RValue *args, int32_t argCount)
{
	static int32_t lastRoom = -1;
	static const char *suffix;
	Runner *runner = ctx->runner;
	RValue nameRv, r;
	char *name, *full;
	int32_t value;
	size_t len;

	if (argCount < 1 || !charAtFn || !spriteGetNameFn || runner->currentRoomIndex < 0 ||
	    (uint32_t)runner->currentRoomIndex >= ctx->dataWin->room.count ||
	    (args[0].type != RVALUE_REAL && args[0].type != RVALUE_INT32 && args[0].type != RVALUE_INT64))
		return VM_callCodeIndex(ctx, skorpLureCode, args, argCount);
	if (runner->currentRoomIndex != lastRoom) {
		RValue call[2], a, b;
		char *sa, *sb;

		call[0] = RValue_makeOwnedString(safeStrdup(ctx->dataWin->room.rooms[runner->currentRoomIndex].name));
		call[1] = RValue_makeInt32(5);
		a = charAtFn(ctx, call, 2);
		call[1] = RValue_makeInt32(6);
		b = charAtFn(ctx, call, 2);
		sa = RValue_toString(a, ctx->dataWin);
		sb = RValue_toString(b, ctx->dataWin);
		suffix = !strcmp(sa, "6") && !strcmp(sb, "a") ? "A6" :
			 !strcmp(sa, "6") && !strcmp(sb, "b") ? "A6b" : NULL;
		free(sa);
		free(sb);
		RValue_free(&a);
		RValue_free(&b);
		RValue_free(&call[0]);
		lastRoom = runner->currentRoomIndex;
	}
	if (!suffix)
		return args[0];
	nameRv = spriteGetNameFn(ctx, args, 1);
	name = RValue_toString(nameRv, ctx->dataWin);
	RValue_free(&nameRv);
	len = strlen(name);
	full = (char *)safeMalloc(len + 4);
	memcpy(full, name, len);
	strcpy(full + len, suffix);
	value = shget(runner->assetsByName, full);
	free(full);
	free(name);
	r = RValue_makeReal(value);
	return r;
}

/* A mine's death leaves up to ~1,800 oRoboMineProj drawing at once: draw_set_blend_mode(1); draw_self();
 * draw_set_blend_mode(0), the same three calls without the interpreter. */
static int32_t mineProjDrawCode = -1;
static BuiltinFunc setBlendModeFn, drawSelfFn;

static GML_ROUNDING RValue wii_mine_proj_draw(VMContext *ctx)
{
	RValue arg = RValue_makeInt32(1), r;

	r = setBlendModeFn(ctx, &arg, 1);
	RValue_free(&r);
	r = drawSelfFn(ctx, NULL, 0);
	RValue_free(&r);
	arg = RValue_makeInt32(0);
	r = setBlendModeFn(ctx, &arg, 1);
	RValue_free(&r);
	return RValue_makeUndefined();
}

/* A Robo Mine's death (oRoboMine_Other_11) fires 108 projectiles: 36 directions, each at speed 3, 2 and 1. A full
 * chain left ~1,160 alive at once, 50 ms frames on a Wii. While all 108 fit under the cap the bytecode runs as is;
 * past it, the mine fires only the rings that fit, fastest first, in the bytecode's order (Jaxi's choice, B2). */
#define MINE_PROJ_CAP 350
#define MINE_PROJ_OBJECT 351
static int32_t mineDeathCode = -1, enemyDeath2Code = -1, prjVar;
static BuiltinFunc instanceCreateFn;

static int32_t live_mine_projectiles(Runner *runner)
{
	Instance **list = runner->instancesByObject[MINE_PROJ_OBJECT];
	int32_t i, n = (int32_t)arrlen(list), live = 0;

	for (i = 0; i < n; i++)
		live += list[i]->active && !list[i]->destroyed;
	return live;
}

static GML_ROUNDING RValue wii_mine_death(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *prj;
	int32_t room, rings, i, ring, id;
	RValue args[3], r;

	room = self ? MINE_PROJ_CAP - live_mine_projectiles(ctx->runner) : 0;
	if (!self || room >= 108)
		return VM_interpretCode(ctx, mineDeathCode);
	rings = room / 36;
	r = VM_callCodeIndex(ctx, enemyDeath2Code, NULL, 0);
	RValue_free(&r);
	Instance_setSelfVar(self, iVar, RValue_makeInt32(0));
	for (i = 0; i < 36; i++) {
		for (ring = 0; ring < rings; ring++) {
			args[0] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_X, "x", -1);
			args[1] = VMBuiltins_getVariable(ctx, self, BUILTIN_VAR_Y, "y", -1);
			args[2] = RValue_makeInt32(MINE_PROJ_OBJECT);
			r = instanceCreateFn(ctx, args, 3);
			RValue_free(&args[0]);
			RValue_free(&args[1]);
			Instance_setSelfVar(self, prjVar, r);
			id = RValue_toInt32(r);
			prj = hmget(ctx->runner->instancesById, id);
			RValue_free(&r);
			if (!prj)
				continue;
			VMBuiltins_setVariable(ctx, prj, BUILTIN_VAR_DIRECTION, "direction",
					       RValue_makeInt32(i * 10), -1);
			VMBuiltins_setVariable(ctx, prj, BUILTIN_VAR_SPEED, "speed", RValue_makeInt32(3 - ring), -1);
		}
		Instance_setSelfVar(self, iVar, RValue_makeInt32(i + 1));
	}
	return RValue_makeUndefined();
}

/* Crumbling sand (oBlockSand): a crumbling block wakes its neighbors, which redo their edges and tiles. A power bomb in
 * rm_a3h04 runs these dozens of times a frame; here they make the bytecode's calls in its order with its values, and
 * store literals as reals, as the VM does. */
#define SAND_OBJECT 36
#define SAND_SMOKE_OBJECT 378
#define SAND_CRUMBLE_SOUND 209
enum {
	SV_TILE, SV_TILE_C0, SV_TILE_C1, SV_TILE_C2, SV_TILE_C3,
	SV_U, SV_R, SV_D, SV_L, SV_UR, SV_UL, SV_DR, SV_DL,
	SV_C0, SV_C1, SV_C2, SV_C3, SV_SMOKE, SV_COUNT
};
static const char *const sandVarNames[SV_COUNT] = {
	"tileid", "tilesandcrn0", "tilesandcrn1", "tilesandcrn2", "tilesandcrn3",
	"sandu", "sandr", "sandd", "sandl", "sandur", "sandul", "sanddr", "sanddl",
	"sandcrn0", "sandcrn1", "sandcrn2", "sandcrn3", "smk"
};
static int32_t sandVars[SV_COUNT], smokeAdditiveVar;
static int32_t sandCrumbleCode = -1, sandEdgesCode = -1, sandDestroyCode = -1, sandTilesCode = -1, sfxStopCode = -1,
	       sfxPlayCode = -1;
static BuiltinFunc tileExistsFn, tileDeleteFn, activateRegionFn, instancePositionFn, tileAddFn, randomFn,
	instanceDestroyFn;

static GMLReal sand_builtin_real(VMContext *ctx, Instance *inst, int16_t var, const char *name)
{
	RValue r = VMBuiltins_getVariable(ctx, inst, var, name, -1);
	GMLReal v = RValue_toReal(r);

	RValue_free(&r);
	return v;
}

static void sand_free_call(BuiltinFunc fn, VMContext *ctx, RValue *args, int32_t argc)
{
	RValue r = fn(ctx, args, argc);

	RValue_free(&r);
}

/* if (tile_exists(t)) { tile_delete(t); t = -1; } for the block's tile and its four corner tiles */
static void sand_drop_tiles(VMContext *ctx, Instance *self)
{
	int k;

	for (k = SV_TILE; k <= SV_TILE_C3; k++) {
		RValue id = Instance_getSelfVar(self, sandVars[k]), r = tileExistsFn(ctx, &id, 1);
		bool exists = RValue_toBool(r);

		RValue_free(&r);
		if (!exists)
			continue;
		sand_free_call(tileDeleteFn, ctx, &id, 1);
		Instance_setSelfVar(self, sandVars[k], RValue_makeReal(-1));
	}
}

static GML_ROUNDING void sand_wake_area(VMContext *ctx, GMLReal x, GMLReal y)
{
	RValue args[5] = { RValue_makeReal(x - 32), RValue_makeReal(y - 32), RValue_makeInt32(64), RValue_makeInt32(64),
			   RValue_makeInt32(1) };

	sand_free_call(activateRegionFn, ctx, args, 5);
}

static RValue sand_at(VMContext *ctx, GMLReal x, GMLReal y)
{
	RValue args[3] = { RValue_makeReal(x), RValue_makeReal(y), RValue_makeInt32(SAND_OBJECT) };

	return instancePositionFn(ctx, args, 3);
}

static GMLReal sand_random(VMContext *ctx, RValue limit)
{
	RValue r = randomFn(ctx, &limit, 1);
	GMLReal v = RValue_toReal(r);

	RValue_free(&r);
	return v;
}

static void sand_script(VMContext *ctx, int32_t code, int32_t arg)
{
	RValue a = RValue_makeInt32(arg), r = VM_callCodeIndex(ctx, code, &a, 1);

	RValue_free(&r);
}

/* Other_11: the block's tiles go, the area around it wakes, and its eight neighbors set its edge frame and corners */
static GML_ROUNDING RValue wii_sand_edges(VMContext *ctx)
{
	static const signed char at[8][2] = { { 0, -16 }, { 16, 0 }, { 0, 16 }, { -16, 0 },
					      { 16, -16 }, { -16, -16 }, { 16, 16 }, { -16, 16 } };
	/* image_index by sandu, sandr, sandd, sandl as bits 3 to 0 */
	static const unsigned char frameOf[16] = { 15, 13, 12, 5, 11, 9, 8, 1, 14, 6, 10, 2, 7, 3, 4, 0 };
	Instance *self = ctx->currentInstance;
	bool n[8];
	GMLReal x, y;
	int k;

	if (!self)
		return VM_interpretCode(ctx, sandEdgesCode);
	sand_drop_tiles(ctx, self);
	x = sand_builtin_real(ctx, self, BUILTIN_VAR_X, "x");
	y = sand_builtin_real(ctx, self, BUILTIN_VAR_Y, "y");
	sand_wake_area(ctx, x, y);
	for (k = 0; k < 8; k++) {
		RValue r = sand_at(ctx, x + at[k][0], y + at[k][1]);

		n[k] = RValue_toBool(r);
		RValue_free(&r);
		Instance_setSelfVar(self, sandVars[SV_U + k], RValue_makeReal(n[k]));
	}
	Instance_setSelfVar(self, sandVars[SV_C2], RValue_makeReal(n[0] && n[1] && !n[4]));
	Instance_setSelfVar(self, sandVars[SV_C3], RValue_makeReal(n[0] && n[3] && !n[5]));
	Instance_setSelfVar(self, sandVars[SV_C0], RValue_makeReal(n[2] && n[1] && !n[6]));
	Instance_setSelfVar(self, sandVars[SV_C1], RValue_makeReal(n[2] && n[3] && !n[7]));
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index",
			       RValue_makeReal(frameOf[n[0] << 3 | n[1] << 2 | n[2] << 1 | n[3]]), -1);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_ALARM, "alarm", RValue_makeReal(1), 1);
	return RValue_makeUndefined();
}

/* Destroy_0: the area wakes, and every neighboring block gets alarm[0] = 1 (with (instance_position(...))) */
static GML_ROUNDING RValue wii_sand_destroy(VMContext *ctx)
{
	static const signed char at[8][2] = { { 0, -16 }, { 16, 0 }, { 0, 16 }, { -16, 0 },
					      { 16, -16 }, { 16, 16 }, { -16, 16 }, { -16, -16 } };
	Instance *self = ctx->currentInstance, *n;
	GMLReal x, y;
	int k;

	if (!self)
		return VM_interpretCode(ctx, sandDestroyCode);
	x = sand_builtin_real(ctx, self, BUILTIN_VAR_X, "x");
	y = sand_builtin_real(ctx, self, BUILTIN_VAR_Y, "y");
	sand_wake_area(ctx, x, y);
	for (k = 0; k < 8; k++) {
		RValue r = sand_at(ctx, x + at[k][0], y + at[k][1]);
		int32_t id = RValue_toInt32(r);

		RValue_free(&r);
		n = id >= INSTANCE_ID_BASE ? hmget(ctx->runner->instancesById, id) : NULL;
		if (n && n->active && !n->destroyed)
			VMBuiltins_setVariable(ctx, n, BUILTIN_VAR_ALARM, "alarm", RValue_makeReal(1), 0);
	}
	return RValue_makeUndefined();
}

/* Other_10: two puffs of smoke, the block's tiles go, then the crumble sound and the block itself */
static GML_ROUNDING RValue wii_sand_crumble(VMContext *ctx)
{
	Instance *self = ctx->currentInstance, *smoke;
	GMLReal x, y, ry, rx, v;
	RValue args[3], r;
	int32_t id;
	int i;

	if (!self)
		return VM_interpretCode(ctx, sandCrumbleCode);
	for (i = 0; i < 2; i++) {
		x = sand_builtin_real(ctx, self, BUILTIN_VAR_X, "x");
		y = sand_builtin_real(ctx, self, BUILTIN_VAR_Y, "y");
		/* the bytecode pushes the last argument first: y's random is drawn before x's */
		ry = sand_random(ctx, RValue_makeInt32(10));
		rx = sand_random(ctx, RValue_makeInt32(10));
		args[0] = RValue_makeReal((x + 2) + rx);
		args[1] = RValue_makeReal((y + 2) + ry);
		args[2] = RValue_makeInt32(SAND_SMOKE_OBJECT);
		r = instanceCreateFn(ctx, args, 3);
		Instance_setSelfVar(self, sandVars[SV_SMOKE], r);
		id = RValue_toInt32(r);
		smoke = hmget(ctx->runner->instancesById, id);
		RValue_free(&r);
		if (!smoke)
			continue;
		v = 0.6 + sand_random(ctx, RValue_makeReal(0.7));
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_IMAGE_SPEED, "image_speed", RValue_makeReal(v), -1);
		Instance_setSelfVar(smoke, smokeAdditiveVar, RValue_makeReal(0));
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_SPRITE_INDEX, "sprite_index", RValue_makeReal(100), -1);
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_IMAGE_ALPHA, "image_alpha", RValue_makeReal(0.4), -1);
		v = 250 + sand_random(ctx, RValue_makeInt32(40));
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_DIRECTION, "direction", RValue_makeReal(v), -1);
		v = sand_random(ctx, RValue_makeInt32(1));
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_SPEED, "speed", RValue_makeReal(v), -1);
		VMBuiltins_setVariable(ctx, smoke, BUILTIN_VAR_DEPTH, "depth", RValue_makeReal(-101), -1);
	}
	sand_drop_tiles(ctx, self);
	VMBuiltins_setVariable(ctx, self, BUILTIN_VAR_ALARM, "alarm", RValue_makeReal(1), 0);
	sand_script(ctx, sfxStopCode, SAND_CRUMBLE_SOUND);
	sand_script(ctx, sfxPlayCode, SAND_CRUMBLE_SOUND);
	sand_free_call(instanceDestroyFn, ctx, NULL, 0);
	return RValue_makeUndefined();
}

/* tile_add(background, left, top, w, h, x, y, depth) */
static RValue sand_tile_add(VMContext *ctx, int32_t bg, RValue left, GMLReal x, GMLReal y, int32_t depth)
{
	RValue args[8] = { RValue_makeInt32(bg), left, RValue_makeInt32(0), RValue_makeInt32(16), RValue_makeInt32(16),
			   RValue_makeReal(x), RValue_makeReal(y), RValue_makeInt32(depth) };

	return tileAddFn(ctx, args, 8);
}

/* Alarm_1: the edge tile for image_index, then each corner tile the block has */
static GML_ROUNDING RValue wii_sand_tiles(VMContext *ctx)
{
	Instance *self = ctx->currentInstance;
	GMLReal x, y, frame;
	RValue r;
	int k;

	if (!self)
		return VM_interpretCode(ctx, sandTilesCode);
	x = sand_builtin_real(ctx, self, BUILTIN_VAR_X, "x");
	y = sand_builtin_real(ctx, self, BUILTIN_VAR_Y, "y");
	frame = sand_builtin_real(ctx, self, BUILTIN_VAR_IMAGE_INDEX, "image_index");
	r = sand_tile_add(ctx, 320, RValue_makeReal(frame * 16), x, y, -100);
	Instance_setSelfVar(self, sandVars[SV_TILE], r);
	RValue_free(&r);
	for (k = 0; k < 4; k++) {
		if (!RValue_toBool(Instance_getSelfVar(self, sandVars[SV_C0 + k])))
			continue;
		r = sand_tile_add(ctx, 321, RValue_makeInt32(k * 16), x, y, -101);
		Instance_setSelfVar(self, sandVars[SV_TILE_C0 + k], r);
		RValue_free(&r);
	}
	return RValue_makeUndefined();
}

static int32_t code_of_length(VMContext *ctx, const char *name, uint32_t length)
{
	ptrdiff_t i = shgeti(ctx->codeIndexByName, name);

	if (i < 0 || ctx->dataWin->code.entries[ctx->codeIndexByName[i].value].length != length)
		return -1;
	return ctx->codeIndexByName[i].value;
}

static void hook_sand(VMContext *ctx)
{
	bool ok;
	int k;

	sandCrumbleCode = code_of_length(ctx, "gml_Object_oBlockSand_Other_10", 744);
	sandEdgesCode = code_of_length(ctx, "gml_Object_oBlockSand_Other_11", 2832);
	sandDestroyCode = code_of_length(ctx, "gml_Object_oBlockSand_Destroy_0", 676);
	sandTilesCode = code_of_length(ctx, "gml_Object_oBlockSand_Alarm_1", 472);
	k = (int)shgeti(ctx->codeIndexByName, "sfx_stop");
	sfxStopCode = k >= 0 ? ctx->codeIndexByName[k].value : -1;
	k = (int)shgeti(ctx->codeIndexByName, "sfx_play");
	sfxPlayCode = k >= 0 ? ctx->codeIndexByName[k].value : -1;
	tileExistsFn = VM_findBuiltin(ctx, "tile_exists");
	tileDeleteFn = VM_findBuiltin(ctx, "tile_delete");
	activateRegionFn = VM_findBuiltin(ctx, "instance_activate_region");
	instancePositionFn = VM_findBuiltin(ctx, "instance_position");
	tileAddFn = VM_findBuiltin(ctx, "tile_add");
	randomFn = VM_findBuiltin(ctx, "random");
	instanceDestroyFn = VM_findBuiltin(ctx, "instance_destroy");
	smokeAdditiveVar = Wii_varId(ctx->runner, "additive", INSTANCE_SELF);
	ok = sandCrumbleCode >= 0 && sandEdgesCode >= 0 && sandDestroyCode >= 0 && sandTilesCode >= 0 &&
	     sfxStopCode >= 0 && sfxPlayCode >= 0 && tileExistsFn && tileDeleteFn && activateRegionFn &&
	     instancePositionFn && tileAddFn && randomFn && instanceDestroyFn && instanceCreateFn &&
	     smokeAdditiveVar >= 0 && Wii_objectIndex(ctx->runner, "oBlockSand") == SAND_OBJECT;
	for (k = 0; k < SV_COUNT; k++) {
		sandVars[k] = Wii_varId(ctx->runner, sandVarNames[k], INSTANCE_SELF);
		ok = ok && sandVars[k] >= 0;
	}
	if (!ok)
		return;
	VM_setNativeCode(ctx, sandCrumbleCode, wii_sand_crumble);
	VM_setNativeCode(ctx, sandEdgesCode, wii_sand_edges);
	VM_setNativeCode(ctx, sandDestroyCode, wii_sand_destroy);
	VM_setNativeCode(ctx, sandTilesCode, wii_sand_tiles);
}

void Wii_hookAm2rScripts(VMContext *ctx)
{
	static const char *const partNames[7] = { "wallU", "wallR", "wallD", "wallL", "color", "special", "corner" };
	ptrdiff_t s = shgeti(ctx->codeIndexByName, "reset_map"), t = shgeti(ctx->codeIndexByName, "draw_cool_text");
	ptrdiff_t m = shgeti(ctx->codeIndexByName, "draw_map_surf"), b = shgeti(ctx->codeIndexByName, "draw_mapblock");
	ptrdiff_t u = shgeti(ctx->codeIndexByName, "gml_Object_oControl_Other_14");
	ptrdiff_t w = shgeti(ctx->codeIndexByName, "gml_Object_oWaterFXV2_Draw_0");
	ptrdiff_t a = shgeti(ctx->codeIndexByName, "gml_Object_oTilesRandomAlpha_Other_10");
	ptrdiff_t lb = shgeti(ctx->codeIndexByName, "gml_Object_oLBubble_Step_0");
	ptrdiff_t ow = shgeti(ctx->codeIndexByName, "gml_Object_oWater_Draw_0");
	ptrdiff_t ls = shgeti(ctx->codeIndexByName, "gml_Object_oLavaSurface_Draw_0");
	ptrdiff_t pb = shgeti(ctx->codeIndexByName, "gml_Object_oPBombExpl_Collision_268");
	ptrdiff_t sl = shgeti(ctx->codeIndexByName, "skorp_get_lure");
	ptrdiff_t mp = shgeti(ctx->codeIndexByName, "gml_Object_oRoboMineProj_Draw_0");
	ptrdiff_t md = shgeti(ctx->codeIndexByName, "gml_Object_oRoboMine_Other_11");
	ptrdiff_t ed = shgeti(ctx->codeIndexByName, "gml_Script_enemy_death2");
	bool resetMap, mapSurf, water, tiles, bubbles, owater, lava;
	int k;

	resetMapCode = s >= 0 ? ctx->codeIndexByName[s].value : -1;
	coolTextCode = t >= 0 ? ctx->codeIndexByName[t].value : -1;
	mapVar = Wii_varId(ctx->runner, "map", INSTANCE_GLOBAL);
	dmapVar = Wii_varId(ctx->runner, "dmap", INSTANCE_GLOBAL);
	logOptionVar = Wii_varId(ctx->runner, "opwritelog", INSTANCE_GLOBAL);
	iVar = Wii_varId(ctx->runner, "i", INSTANCE_SELF);
	jVar = Wii_varId(ctx->runner, "j", INSTANCE_SELF);
	resetMap = resetMapCode >= 0 && mapVar >= 0 && dmapVar >= 0 && iVar >= 0 && jVar >= 0;
	mapSurfCode = m >= 0 ? ctx->codeIndexByName[m].value : -1;
	mapBlockCode = b >= 0 ? ctx->codeIndexByName[b].value : -1;
	mystrVar = Wii_varId(ctx->runner, "mystr", INSTANCE_SELF);
	stringFn = VM_findBuiltin(ctx, "string");
	charAtFn = VM_findBuiltin(ctx, "string_char_at");
	drawSpriteFn = VM_findBuiltin(ctx, "draw_sprite");
	spriteGetNameFn = VM_findBuiltin(ctx, "sprite_get_name");
	if (sl >= 0 && ctx->dataWin->code.entries[ctx->codeIndexByName[sl].value].length == 316)
		skorpLureCode = ctx->codeIndexByName[sl].value;
	/* the sprite numbers below are 1.5.5's, baked into this length of bytecode */
	mapBlockNative = mapBlockCode >= 0 && drawSpriteFn && ctx->dataWin->code.entries[mapBlockCode].length == 5016;
	mapSurf = mapSurfCode >= 0 && mapBlockCode >= 0 && mapVar >= 0 && dmapVar >= 0 && mystrVar >= 0 &&
		  stringFn && charAtFn;
	for (k = 0; k < 7; k++) {
		partVars[k] = Wii_varId(ctx->runner, partNames[k], INSTANCE_SELF);
		mapSurf = mapSurf && partVars[k] >= 0;
	}
	screenSurfaceVar = Wii_varId(ctx->runner, "screen_surface", INSTANCE_SELF);
	engineSurfaceFree = VM_findBuiltin(ctx, "surface_free");
	if (u >= 0 && screenSurfaceVar >= 0 && engineSurfaceFree)
		controlUser4Code = ctx->codeIndexByName[u].value;
	waveHeightVar = Wii_varId(ctx->runner, "wave_height", INSTANCE_GLOBAL);
	widescreenSpaceVar = Wii_varId(ctx->runner, "widescreen_space", INSTANCE_SELF);
	water = w >= 0 && waveHeightVar >= 0 && widescreenSpaceVar >= 0 &&
		Wii_objectIndex(ctx->runner, "oControl") == WATER_CONTROL_OBJECT;
	for (k = 0; k < WV_COUNT; k++) {
		waterVars[k] = Wii_varId(ctx->runner, waterVarNames[k], INSTANCE_SELF);
		water = water && waterVars[k] >= 0;
	}
	for (k = 0; k < WF_COUNT; k++)
		waterFns[k] = -1;
	for (k = 0; k < TF_COUNT; k++)
		tileFns[k] = -1;
	for (k = 0; k < BF_COUNT; k++)
		bubbleFns[k] = -1;
	for (k = 0; k < OF_COUNT; k++)
		owaterFns[k] = -1;
	for (k = 0; k < LF_COUNT; k++)
		lavaFns[k] = -1;
	lavaYoffsetVar = Wii_varId(ctx->runner, "yoffset", INSTANCE_SELF);
	lavaBgVar = Wii_varId(ctx->runner, "bg", INSTANCE_SELF);
	controlWideVar = Wii_varId(ctx->runner, "widescreen", INSTANCE_SELF);
	owaterIVar = Wii_varId(ctx->runner, "i", INSTANCE_SELF);
	owaterYoffsetVar = Wii_varId(ctx->runner, "yoffset", INSTANCE_SELF);
	owaterXoffsetVar = Wii_varId(ctx->runner, "xoffset", INSTANCE_SELF);
	owaterBgVar = Wii_varId(ctx->runner, "bg", INSTANCE_SELF);
	owaterAlphaVar = Wii_varId(ctx->runner, "spr_alpha", INSTANCE_SELF);
	owaterWideVar = Wii_varId(ctx->runner, "widescreen", INSTANCE_SELF);
	fadeoutVar = Wii_varId(ctx->runner, "fadeout", INSTANCE_SELF);
	erisVar = Wii_varId(ctx->runner, "eris", INSTANCE_SELF);
	waterlevelVar = Wii_varId(ctx->runner, "waterlevel", INSTANCE_GLOBAL);
	wateroffsetVar = Wii_varId(ctx->runner, "wateroffset", INSTANCE_GLOBAL);
	tileListVar = Wii_varId(ctx->runner, "tile_list", INSTANCE_SELF);
	falphaVar = Wii_varId(ctx->runner, "falpha", INSTANCE_SELF);
	repeat(ctx->funcCallCacheCount, fi) {
		const char *name = ctx->dataWin->func.functions[fi].name;
		void *standIn = NULL;

		for (k = 0; name && k < WF_COUNT; k++)
			if (strcmp(name, waterFnNames[k]) == 0)
				waterFns[k] = (int32_t)fi;
		for (k = 0; name && k < TF_COUNT; k++)
			if (strcmp(name, tileFnNames[k]) == 0)
				tileFns[k] = (int32_t)fi;
		for (k = 0; name && k < BF_COUNT; k++)
			if (strcmp(name, bubbleFnNames[k]) == 0)
				bubbleFns[k] = (int32_t)fi;
		for (k = 0; name && k < OF_COUNT; k++)
			if (strcmp(name, owaterFnNames[k]) == 0)
				owaterFns[k] = (int32_t)fi;
		for (k = 0; name && k < LF_COUNT; k++)
			if (strcmp(name, lavaFnNames[k]) == 0)
				lavaFns[k] = (int32_t)fi;

		if (name && resetMap && strcmp(name, "reset_map") == 0)
			standIn = (void *)wii_reset_map;
		else if (name && coolTextCode >= 0 && strcmp(name, "draw_cool_text") == 0)
			standIn = (void *)wii_draw_cool_text;
		else if (name && mapSurf && strcmp(name, "draw_map_surf") == 0)
			standIn = (void *)wii_draw_map_surf;
		else if (name && mapBlockNative && strcmp(name, "draw_mapblock") == 0)
			standIn = (void *)wii_draw_mapblock;
		else if (name && controlUser4Code >= 0 && strcmp(name, "surface_free") == 0)
			standIn = (void *)wii_surface_free;
		else if (name && skorpLureCode >= 0 && strcmp(name, "skorp_get_lure") == 0)
			standIn = (void *)wii_skorp_get_lure;
		if (standIn) {
			ctx->funcCallCache[fi].builtin = standIn;
			ctx->funcCallCache[fi].scriptCodeIndex = -1;
		}
	}
	for (k = 0; k < WF_COUNT; k++)
		water = water && waterFns[k] >= 0 && ctx->funcCallCache[waterFns[k]].builtin;
	if (water) {
		waterDrawCode = ctx->codeIndexByName[w].value;
		VM_setNativeCode(ctx, waterDrawCode, wii_water_draw);
	}
	tiles = a >= 0 && tileListVar >= 0 && falphaVar >= 0;
	for (k = 0; k < TF_COUNT; k++)
		tiles = tiles && tileFns[k] >= 0 && ctx->funcCallCache[tileFns[k]].builtin;
	if (tiles) {
		tileAlphaCode = ctx->codeIndexByName[a].value;
		VM_setNativeCode(ctx, tileAlphaCode, wii_tiles_alpha);
	}
	bubbles = lb >= 0 && fadeoutVar >= 0 && erisVar >= 0 && waterlevelVar >= 0 && wateroffsetVar >= 0 &&
		  widescreenSpaceVar >= 0 && Wii_objectIndex(ctx->runner, "oControl") == WATER_CONTROL_OBJECT;
	for (k = 0; k < BF_COUNT; k++)
		bubbles = bubbles && bubbleFns[k] >= 0 && ctx->funcCallCache[bubbleFns[k]].builtin;
	if (bubbles) {
		bubbleStepCode = ctx->codeIndexByName[lb].value;
		VM_setNativeCode(ctx, bubbleStepCode, wii_bubble_step);
	}
	owater = ow >= 0 && waterlevelVar >= 0 && widescreenSpaceVar >= 0 && owaterIVar >= 0 && owaterYoffsetVar >= 0 &&
		 owaterXoffsetVar >= 0 && owaterBgVar >= 0 && owaterAlphaVar >= 0 && owaterWideVar >= 0;
	for (k = 0; k < OF_COUNT; k++)
		owater = owater && owaterFns[k] >= 0 && ctx->funcCallCache[owaterFns[k]].builtin;
	if (owater) {
		owaterDrawCode = ctx->codeIndexByName[ow].value;
		VM_setNativeCode(ctx, owaterDrawCode, wii_owater_draw);
	}
	lava = ls >= 0 && waterlevelVar >= 0 && widescreenSpaceVar >= 0 && lavaYoffsetVar >= 0 && lavaBgVar >= 0 &&
	       controlWideVar >= 0 && Wii_objectIndex(ctx->runner, "oControl") == WATER_CONTROL_OBJECT;
	for (k = 0; k < LF_COUNT; k++)
		lava = lava && lavaFns[k] >= 0 && ctx->funcCallCache[lavaFns[k]].builtin;
	if (lava) {
		lavaDrawCode = ctx->codeIndexByName[ls].value;
		VM_setNativeCode(ctx, lavaDrawCode, wii_lava_draw);
	}
	if (pb >= 0 && ctx->dataWin->code.entries[ctx->codeIndexByName[pb].value].length == 180) {
		pbombSolidCode = ctx->codeIndexByName[pb].value;
		VM_setNativeCode(ctx, pbombSolidCode, wii_pbomb_solid);
	}
	setBlendModeFn = VM_findBuiltin(ctx, "draw_set_blend_mode");
	drawSelfFn = VM_findBuiltin(ctx, "draw_self");
	if (mp >= 0 && setBlendModeFn && drawSelfFn &&
	    ctx->dataWin->code.entries[ctx->codeIndexByName[mp].value].length == 52) {
		mineProjDrawCode = ctx->codeIndexByName[mp].value;
		VM_setNativeCode(ctx, mineProjDrawCode, wii_mine_proj_draw);
	}
	instanceCreateFn = VM_findBuiltin(ctx, "instance_create");
	prjVar = Wii_varId(ctx->runner, "prj", INSTANCE_SELF);
	if (md >= 0 && ed >= 0 && instanceCreateFn && iVar >= 0 && prjVar >= 0 &&
	    ctx->dataWin->code.entries[ctx->codeIndexByName[md].value].length == 400 &&
	    Wii_objectIndex(ctx->runner, "oRoboMineProj") == MINE_PROJ_OBJECT) {
		mineDeathCode = ctx->codeIndexByName[md].value;
		enemyDeath2Code = ctx->codeIndexByName[ed].value;
		VM_setNativeCode(ctx, mineDeathCode, wii_mine_death);
	}
	hook_sand(ctx);
}
