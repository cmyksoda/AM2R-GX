#include "sweep.h"

#include "instance.h"
#include "log.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "vm.h"
#include "warp.h"
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
	const char *keys;    /* 'D' Down, 'Z' Z, 'S' Enter+Z, 'E' Esc, '.' shoot; one per KEY_HOLD + KEY_GAP frames */
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
	{ "oIntroCutscene", false, true, 60, "Z", NULL },
	{ "oCharacter", true, true, 300, "E", NULL },
	{ "oPauseOption", false, false, 60, ".D.D.D.", NULL },
	{ NULL, false, false, 0, "", NULL },
};
static const MenuStep *steps = extras;
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
	return key == 'D' ? VK_DOWN : key == 'E' ? VK_ESCAPE : 'Z';
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

void Wii_menuTick(Runner *runner)
{
	static int armed, step, since = -1, heldKey = -1, waited;
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
		else if (fscanf(f, "%d", &t) == 1 && t >= 0 && t < 12) {
			memset(fileSelectKeys, 'D', (size_t)t);
			fileSelectKeys[t] = 'Z';
			fileSelectKeys[t + 1] = '\0';
		}
		fclose(f);
		armed = 1;
		logInfo("MENU armed script=%s\n", steps == pausetips ? "pausetips" : "extras");
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
	k = t / (KEY_HOLD + KEY_GAP);
	if (k < (int)strlen(s->keys)) {
		if (t % (KEY_HOLD + KEY_GAP) >= KEY_HOLD)
			release(runner, &heldKey);
		else if (s->keys[k] == '.' && t % (KEY_HOLD + KEY_GAP) == 0)
			Wii_requestShot(runner->frameCount, "menu", s->keys[k + 1] == '\0' && !steps[step + 1].object);
		else if (s->keys[k] != '.' && heldKey < 0)
			press(runner, s->keys[k], &heldKey);
		return;
	}
	release(runner, &heldKey);
	if (s->retry && !step_ready(runner, &steps[step + 1])) {
		since = s->settle > 60 ? s->settle - 60 : 0; /* not through yet: press again in a moment */
		return;
	}
	var_census(runner, s->object, "after");
	logInfo("MENU step=%d done frame=%d\n", step, runner->frameCount);
	step++;
	since = -1;
	waited = 0;
}
