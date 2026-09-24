#include "sweep.h"

#include "instance.h"
#include "log.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"
#include "warp.h"
#include "wii_log.h"
#include "wii_paths.h"

#include <stdio.h>
#include <string.h>

/* MENU marker (Dolphin only): walk the menus by pressing keys when their objects exist, and shoot.
 * Frame-scripted tapes missed six times: the title's timing from a cold boot moves by hundreds
 * of frames between runs. The marker's first word picks the script: "extras" (default) or "pausetips".
 * Runs from platformHandleEvents: keys pressed any later in the frame reach the game as held, never pressed. */
#define MENU_GIVE_UP_FRAMES 3000 /* per step: a title that will not take a press, a screen that never comes */
#define KEY_HOLD 2
#define KEY_GAP 30 /* menus lock input while the cursor slides */

typedef struct {
	const char *object;  /* wait until an instance of this exists... */
	bool landing;        /* ...in the landing room */
	bool retry;          /* screens that ignore early presses: press again until the next step's screen is up */
	int settle;          /* frames to let the screen finish sliding in */
	const char *keys;    /* 'D' Down, 'U' Up, 'L' Left, 'R' Right, 'Z' Z, 'S' Enter+Z, 'E' Esc, '.' shoot, '_' nothing;
	                        one per KEY_HOLD + KEY_GAP frames. Held directions: lowercase d u l r hold one, a digit
	                        holds a set (1 Down, 2 Up, 4 Left, 8 Right, added up); the set stays held across taps
	                        and changes only at the next hold, '_' or '.'. */
	const char *skipIf;  /* a screen the game may show instead (modifiers.ini skips the game-type box) */
} MenuStep;

/* Enter+Z at the title (Z alone can take a thousand frames of retries), Z everywhere else. */
static char fileSelectKeys[16] = "DDDDZ"; /* "extras N" in the marker = N Downs before Z */
static const MenuStep extras[] = {
	{ "oPressStart", false, true, 30, "S", NULL },
	{ "oGameSelMenu", false, false, 300, fileSelectKeys, NULL },
	{ "oOptionsMain", false, false, 60, "DDDZ", NULL },
	{ "oOptionsExtras", false, false, 90, ".", NULL },
	{ NULL, false, false, 0, "", NULL },
};
/* New game to the ship (skipping the intro), Esc, then each of the four pause-menu rows: the
 * tips under them are the centred odd-width lines that garble on half pixels (session 5). */
static const MenuStep pausetips[] = {
	{ "oPressStart", false, true, 30, "S", NULL },
	{ "oGameSelMenu", false, false, 300, "Z", NULL },
	{ "oSlotMenu_normal_only", false, false, 30, "Z", "oSlotMenu2" },
	{ "oSlotMenu2", false, false, 30, "Z", NULL },
	{ "oIntroCutscene", false, true, 60, "S", NULL }, /* Start skips the intro at once (Jaxi, session 9) */
	{ "oCharacter", true, true, 300, "E", NULL },
	{ "oPauseOption", false, false, 60, ".D.D.D.UUUZ", NULL }, /* back up to Continue, take it */
	{ "oCharacter", true, false, 120, "..", NULL }, /* back in the room: the ground after a pause (vWii: blue) */
	{ NULL, false, false, 0, "", NULL },
};
/* Into the game and nothing else: new game (intro skipped) or a continued save, for warp runs. */
static const MenuStep landing[] = {
	{ "oPressStart", false, true, 30, "S", NULL },
	{ "oGameSelMenu", false, false, 300, "Z", NULL },
	{ "oSlotMenu1", false, false, 30, "Z", "oSlotMenu_normal_only" }, /* a used slot asks "continue" first */
	{ "oSlotMenu_normal_only", false, false, 30, "Z", "oCharacter" },
	{ "oSlotMenu2", false, false, 30, "Z", "oCharacter" },
	{ "oIntroCutscene", false, true, 60, "S", "oCharacter" }, /* Start skips the intro at once (Jaxi, session 9) */
	{ "oCharacter", false, false, 30, "", NULL },
	{ NULL, false, false, 0, "", NULL },
};
/* Jaxi's repro (session 10): walk right over the first little hill by the ship, morph, roll left and keep
 * rolling. "morph R L [how]" in the marker = R slots of Right, the morph, then L slots of Left, 32 frames
 * each. how: "still" Down Down standing; "thumb" tap Down, hold Down, Down+Left, Left (a D-pad thumb rolling
 * from Down to Left); "jump J" J jumps with Right held, then a standing morph (over a taller hill); "air J"
 * jumps and morphs in the air; default Down Down with Left already held. A fifth number G replaces KEY_GAP
 * for taps and idles only (G=6: Down Down Left 8 frames apart, a human's speed; holds keep their 32).
 * "morph keys <slots> [G]" spells the slots out by hand instead; a trailing "pause" on either form pauses first. */
static char morphKeys[80] = "rrrrrrrr_lDDllllllllllll.";
static const MenuStep morph[] = {
	{ "oPressStart", false, true, 30, "S", NULL },
	{ "oGameSelMenu", false, false, 300, "Z", NULL },
	{ "oSlotMenu1", false, false, 30, "Z", "oSlotMenu_normal_only" },
	{ "oSlotMenu_normal_only", false, false, 30, "Z", "oCharacter" },
	{ "oSlotMenu2", false, false, 30, "Z", "oCharacter" },
	{ "oIntroCutscene", false, true, 60, "S", "oCharacter" },
	{ "oCharacter", true, false, 120, morphKeys, NULL },
	{ NULL, false, false, 0, "", NULL },
};
/* The same with a pause menu opened and closed first: Jaxi had just left the pause menu when she fell. */
static const MenuStep morphPause[] = {
	{ "oPressStart", false, true, 30, "S", NULL },
	{ "oGameSelMenu", false, false, 300, "Z", NULL },
	{ "oSlotMenu1", false, false, 30, "Z", "oSlotMenu_normal_only" },
	{ "oSlotMenu_normal_only", false, false, 30, "Z", "oCharacter" },
	{ "oSlotMenu2", false, false, 30, "Z", "oCharacter" },
	{ "oIntroCutscene", false, true, 60, "S", "oCharacter" },
	{ "oCharacter", true, true, 320, "E", NULL },
	{ "oPauseOption", false, false, 60, "Z", NULL }, /* Continue is the row the cursor starts on */
	{ "oCharacter", true, false, 60, morphKeys, NULL },
	{ NULL, false, false, 0, "", NULL },
};
static const MenuStep *steps = extras;
static int keyGap = KEY_GAP;
#define CENSUS_EVERY 120
#define CENSUS_MAX 14

static bool has_instance(Runner *runner, const char *name)
{
	int32_t idx = Wii_objectIndex(runner, name);
	Instance **list;
	int32_t i, n;

	if (idx < 0 || !runner->instancesByExactObject)
		return false;
	list = runner->instancesByExactObject[idx];
	n = (int32_t)arrlen(list);
	for (i = 0; i < n; i++)
		if (list[i] && !list[i]->destroyed)
			return true;
	return false;
}

/* Which objects are alive right now, so a miss says what was actually on screen. */
static void census(Runner *runner)
{
	char line[400];
	int len = 0, shown = 0, seen[CENSUS_MAX];
	int32_t i, n = (int32_t)arrlen(runner->instances), j;

	for (i = 0; i < n && shown < CENSUS_MAX; i++) {
		Instance *inst = runner->instances[i];

		if (!inst || inst->destroyed)
			continue;
		for (j = 0; j < shown && seen[j] != inst->objectIndex; j++)
			;
		if (j < shown || inst->objectIndex < 0 || (uint32_t)inst->objectIndex >= runner->dataWin->objt.count)
			continue;
		seen[shown++] = inst->objectIndex;
		len += snprintf(line + len, sizeof line - len, " %s", runner->dataWin->objt.objects[inst->objectIndex].name);
		if (len >= (int)sizeof line - 40)
			break;
	}
	logInfo("MENU census frame=%d room=%d live=%d:%s\n", runner->frameCount, runner->currentRoomIndex, (int)n, line);
}

/* Every numeric self variable of the watched object: which one is the cursor shows itself. */
static void var_census(Runner *runner, const char *object, const char *when)
{
	VMContext *vm = runner->vmContext;
	int32_t idx = Wii_objectIndex(runner, object);
	Instance **list;
	char line[400] = "";
	int len = 0, shown = 0;
	ptrdiff_t i, n;

	if (!vm || idx < 0 || !runner->instancesByExactObject)
		return;
	list = runner->instancesByExactObject[idx];
	if (arrlen(list) < 1 || !list[0])
		return;
	n = shlen(vm->varNameMap);
	for (i = 0; i < n && shown < 40 && len < (int)sizeof line - 48; i++) {
		RValue v = Instance_getSelfVar(list[0], vm->varNameMap[i].value);

		if (v.type != RVALUE_REAL && v.type != RVALUE_INT32 && v.type != RVALUE_BOOL)
			continue;
		len += snprintf(line + len, sizeof line - len, " %s=%g", vm->varNameMap[i].key, (double)RValue_toReal(v));
		shown++;
	}
	logInfo("MENU %s %s:%s\n", object, when, line);
}

static int key_vk(int key)
{
	switch (key) {
	case 'D': case 'd': return VK_DOWN;
	case 'U': case 'u': return VK_UP;
	case 'L': case 'l': return VK_LEFT;
	case 'R': case 'r': return VK_RIGHT;
	case 'E': return VK_ESCAPE;
	default: return 'Z';
	}
}

static void press(Runner *runner, char key, int *held)
{
	RunnerKeyboard_onKeyDown(runner->keyboard, key_vk(key));
	if (key == 'S')
		RunnerKeyboard_onKeyDown(runner->keyboard, VK_ENTER);
	*held = key;
}

static void release(Runner *runner, int *held)
{
	if (*held < 0)
		return;
	RunnerKeyboard_onKeyUp(runner->keyboard, key_vk(*held));
	if (*held == 'S')
		RunnerKeyboard_onKeyUp(runner->keyboard, VK_ENTER);
	*held = -1;
}

/* The direction set a hold character asks for, or -1 for a tap / idle / shot. */
static int hold_mask(char c)
{
	switch (c) {
	case 'd': return 1;
	case 'u': return 2;
	case 'l': return 4;
	case 'r': return 8;
	default: return (c >= '0' && c <= '9') ? c - '0' : -1;
	}
}

/* Held slots keep the walk's 32 frames; taps, idles and shots take the script's gap. */
static int slot_len(char c)
{
	return hold_mask(c) >= 0 ? KEY_HOLD + KEY_GAP : KEY_HOLD + keyGap;
}

static void set_holds(Runner *runner, int *held, int want)
{
	static const int vk[4] = { VK_DOWN, VK_UP, VK_LEFT, VK_RIGHT };
	int b;

	for (b = 0; b < 4; b++) {
		if ((want & (1 << b)) && !(*held & (1 << b)))
			RunnerKeyboard_onKeyDown(runner->keyboard, vk[b]);
		else if (!(want & (1 << b)) && (*held & (1 << b)))
			RunnerKeyboard_onKeyUp(runner->keyboard, vk[b]);
	}
	*held = want;
}

static bool step_ready(Runner *runner, const MenuStep *s)
{
	return s->object && has_instance(runner, s->object) &&
	       (!s->landing || runner->currentRoomIndex == Wii_am2rIds(runner)->landingRoom);
}

static double named_var(Runner *runner, Instance *inst, const char *name)
{
	VMContext *vm = runner->vmContext;
	ptrdiff_t slot = shgeti(vm->varNameMap, (char *)name);
	RValue v;

	if (slot < 0 || !inst)
		return -1;
	v = Instance_getSelfVar(inst, vm->varNameMap[slot].value);
	return v.type == RVALUE_UNDEFINED ? -1 : (double)RValue_toReal(v);
}

/* What check_keys produced this step, and the file select's cursor: one line per frame while keys
 * are being pressed (and a moment after), so a press that did nothing shows where it was lost. */
static void key_trace(Runner *runner)
{
	VMContext *vm = runner->vmContext;
	int32_t idx = vm ? Wii_objectIndex(runner, "oControl") : -1;
	Instance *ctl = (idx >= 0 && runner->instancesByExactObject && arrlen(runner->instancesByExactObject[idx]) > 0)
	                    ? runner->instancesByExactObject[idx][0] : NULL;
	RunnerKeyboardState *kb = runner->keyboard;

	if (!ctl)
		return;
	logInfo("MENU trace frame=%d down=%d z=%d | kDown=%g steps=%g kMenu1=%g m1steps=%g kA=%g | curropt=%g controltype=%g\n",
	        runner->frameCount, (int)kb->keyDown[VK_DOWN], (int)kb->keyDown['Z'],
	        named_var(runner, ctl, "kDown"), named_var(runner, ctl, "kDownPushedSteps"),
	        named_var(runner, ctl, "kMenu1"), named_var(runner, ctl, "kMenu1PushedSteps"), named_var(runner, ctl, "kA"),
	        named_var(runner, vm->globalScopeInstance, "curropt"), named_var(runner, vm->globalScopeInstance, "controltype"));
}

static Instance *samus_instance(Runner *runner)
{
	const Am2rIds *ids = Wii_am2rIds(runner);
	Instance **list;
	int32_t i, n;

	if (ids->character < 0 || !runner->instancesByExactObject)
		return NULL;
	list = runner->instancesByExactObject[ids->character];
	n = (int32_t)arrlen(list);
	for (i = 0; i < n; i++)
		if (list[i] && !list[i]->destroyed)
			return list[i];
	return NULL;
}

/* Samus every frame of the gameplay script: a fall through the floor shows as y running away
 * in the log, no eyes needed. */
static void pos_trace(Runner *runner, char key, char tap)
{
	Instance *samus = samus_instance(runner);

	if (!samus)
		return;
	logInfo("MENU pos frame=%d key=%c%c x=%.2f y=%.2f hs=%.2f vs=%.2f morph=%g state=%g mask=%d\n", runner->frameCount, key, tap,
	        samus->x, samus->y, samus->hspeed, samus->vspeed, named_var(runner, samus, "morphball"),
	        named_var(runner, samus, "statetype"), (int)samus->maskIndex);
}

/* The held state lives under keyboard_set_map's translation of the key. */
static bool key_down(RunnerKeyboardState *kb, int key)
{
	return kb->keyDown[kb->keyMap[key]];
}

/* TRACE marker: for a bug only Jaxi's hands on the Wii trigger. One line whenever Samus, her mask or the
 * pad changes; keys = 1 Down 2 Up 4 Left 8 Right 16 Z 32 X. Synced to the card once a second. */
void Wii_traceTick(Runner *runner)
{
	static int armed, lines, lroom = -1, lmask = -1, lkeys = -1;
	static float lx, ly;
	RunnerKeyboardState *kb;
	Instance *samus;
	FILE *f;
	int keys;

	if (armed < 0 || !runner || !runner->keyboard)
		return;
	if (armed == 0) {
		f = fopen(WII_GAME_DIR "/TRACE", "r");
		armed = f ? 1 : -1;
		if (!f)
			return;
		fclose(f);
		logInfo("TRACE armed\n");
	}
	samus = samus_instance(runner);
	if (!samus)
		return;
	kb = runner->keyboard;
	keys = (key_down(kb, VK_DOWN) ? 1 : 0) | (key_down(kb, VK_UP) ? 2 : 0) | (key_down(kb, VK_LEFT) ? 4 : 0) |
	       (key_down(kb, VK_RIGHT) ? 8 : 0) | (key_down(kb, 'Z') ? 16 : 0) | (key_down(kb, 'X') ? 32 : 0);
	if (runner->currentRoomIndex == lroom && samus->x == lx && samus->y == ly && samus->maskIndex == lmask && keys == lkeys)
		return;
	lroom = runner->currentRoomIndex;
	lx = samus->x;
	ly = samus->y;
	lmask = samus->maskIndex;
	lkeys = keys;
	logInfo("TRACE frame=%d room=%d x=%.2f y=%.2f mask=%d keys=%d\n", runner->frameCount, lroom, lx, ly, lmask, keys);
	if (++lines % 60 == 0)
		WiiLog_sync();
}

void Wii_menuTick(Runner *runner)
{
	static int armed, step, since = -1, heldKey = -1, holdMask, waited;
	const MenuStep *s;
	FILE *f;
	char word[16] = "";
	int t, k;

	if (armed < 0 || !runner || !runner->keyboard)
		return;
	if (armed == 0) {
		f = fopen(WII_GAME_DIR "/MENU", "r");
		if (!f) {
			armed = -1;
			return;
		}
		if (fscanf(f, "%15s", word) == 1 && strcmp(word, "pausetips") == 0)
			steps = pausetips;
		else if (strcmp(word, "landing") == 0)
			steps = landing;
		else if (strcmp(word, "morph") == 0) {
			int r, l, j = 0, g = 0, n;
			char how[16] = "", then[16] = "", rest[160] = "";

			if (!fgets(rest, sizeof rest, f))
				rest[0] = '\0';
			/* The slots spelled out by hand, or built from the counts. */
			if (sscanf(rest, " keys %79s %d %15s", morphKeys, &g, then) < 1 &&
			    sscanf(rest, "%d %d %15s %d %d %15s", &r, &l, how, &j, &g, then) >= 2 && r >= 0 && l >= 0 && j >= 0 && j < 8 &&
			    r + l + 2 * j < (int)sizeof morphKeys - 6) {
				memset(morphKeys, 'r', (size_t)r);
				n = r;
				for (t = 0; t < j && (strcmp(how, "jump") == 0 || strcmp(how, "air") == 0); t++) {
					morphKeys[n++] = 'Z';
					if (strcmp(how, "jump") == 0)
						morphKeys[n++] = 'r'; /* stay on Right through the jump and land walking */
				}
				memcpy(morphKeys + n, strcmp(how, "still") == 0 || strcmp(how, "jump") == 0 ? "_DD_" :
				                      strcmp(how, "air") == 0 ? "DD__" : strcmp(how, "thumb") == 0 ? "_D15" : "_lDD", 4);
				memset(morphKeys + n + 4, 'l', (size_t)l);
				strcpy(morphKeys + n + 4 + l, ".");
			}
			if (g > 0)
				keyGap = g;
			steps = strcmp(then, "pause") == 0 ? morphPause : morph;
		} else if (fscanf(f, "%d", &t) == 1 && t >= 0 && t < 12) {
			memset(fileSelectKeys, 'D', (size_t)t);
			fileSelectKeys[t] = 'Z';
			fileSelectKeys[t + 1] = '\0';
		}
		fclose(f);
		armed = 1;
		logInfo("MENU armed script=%s keys=%s gap=%d\n", steps == pausetips ? "pausetips" : steps == landing ? "landing" :
		        steps == morph ? "morph" : steps == morphPause ? "morph+pause" : "extras", steps == morph || steps == morphPause ? morphKeys : "", keyGap);
	}
	s = &steps[step];
	if (!s->object)
		return;
	if (since < 0) {
		if (s->skipIf && has_instance(runner, s->skipIf)) {
			logInfo("MENU step=%d skipped: %s is up instead of %s\n", step, s->skipIf, s->object);
			step++;
			waited = 0;
			return;
		}
		if (!step_ready(runner, s)) {
			if (++waited % CENSUS_EVERY == 0)
				census(runner);
			if (waited > MENU_GIVE_UP_FRAMES) {
				logInfo("MENU abort step=%d waiting for %s\nTAPE done\n", step, s->object);
				census(runner);
				armed = -1;
			}
			return;
		}
		since = 0;
		logInfo("MENU step=%d %s present frame=%d\n", step, s->object, runner->frameCount);
		census(runner);
		var_census(runner, s->object, "before");
	}
	t = since++ - s->settle;
	if (t < 0)
		return;
	if (step == 1 && t < (int)strlen(s->keys) * (KEY_HOLD + KEY_GAP) + 60)
		key_trace(runner);
	for (k = 0; s->keys[k] && t >= slot_len(s->keys[k]); k++)
		t -= slot_len(s->keys[k]);
	if (s->keys[k]) {
		char c = s->keys[k];
		int phase = t;

		if ((steps == morph || steps == morphPause) && s->landing)
			pos_trace(runner, holdMask ? (char)('0' + holdMask) : '-', heldKey < 0 ? c : (char)heldKey);
		if (hold_mask(c) >= 0)
			set_holds(runner, &holdMask, hold_mask(c));
		else if (c == '_' || c == '.') {
			set_holds(runner, &holdMask, 0);
			release(runner, &heldKey);
			if (c == '.' && phase == 0)
				Wii_requestShot(runner->frameCount, "menu", s->keys[k + 1] == '\0' && !steps[step + 1].object);
		} else if (phase >= KEY_HOLD)
			release(runner, &heldKey);
		else if (heldKey < 0)
			press(runner, c, &heldKey);
		return;
	}
	release(runner, &heldKey);
	set_holds(runner, &holdMask, 0);
	if (s->retry && !step_ready(runner, &steps[step + 1])) {
		since = s->settle > 60 ? s->settle - 60 : 0; /* not through yet: press again in a moment */
		return;
	}
	var_census(runner, s->object, "after");
	logInfo("MENU step=%d done frame=%d\n", step, runner->frameCount);
	step++;
	since = -1;
	waited = 0;
	/* A gameplay script quits at its end so the Wii runner gets its log back. */
	if ((steps == morph || steps == morphPause) && !steps[step].object) {
		logInfo("MENU script done, quitting\n");
		runner->shouldExit = true;
	}
}
