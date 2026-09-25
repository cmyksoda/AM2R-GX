#include "input.h"

#include "input_recording.h"
#include "warp.h"
#include "vm.h"
#include "stb_ds.h"
#include "log.h"
#include "instance.h"
#include "overscan.h"
#include "platformdefs.h"
#include "runner.h"
#include "runner_gamepad.h"
#include "runner_keyboard.h"

#include <math.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <string.h>
#include <wiiuse/wiiuse.h>
#include <wiiuse/wpad.h>

#define STICK_DEADZONE 0.2f
#define STICK_DIGITAL 0.4f /* stick deflection that counts as a d-pad press */

#define HOME_HOLD_FRAMES 60

static int home_held;
static bool want_vk[GML_KEY_COUNT];
static bool prev_vk[GML_KEY_COUNT];

static void want(int32_t vk)
{
	if (vk >= 0 && vk < GML_KEY_COUNT)
		want_vk[vk] = true;
}

static void set_btn(GamepadSlot *slot, int idx, bool down)
{
	if (idx < 0 || idx >= GP_BUTTON_COUNT)
		return;
	slot->buttonDown[idx] = down;
	if (down)
		slot->buttonValue[idx] = 1.0f;
}

static void joy_xy(const joystick_t *js, float *lx, float *ly)
{
	float rad;

	if (!js || js->mag < STICK_DEADZONE) {
		*lx = 0.0f;
		*ly = 0.0f;
		return;
	}
	rad = js->ang * ((float)M_PI / 180.0f);
	*lx = js->mag * sinf(rad);
	*ly = -js->mag * cosf(rad);
}

static float gc_axis(s8 pos)
{
	float v = (float)pos / 128.0f;

	if (v > 1.0f)
		v = 1.0f;
	if (v < -1.0f)
		v = -1.0f;
	if (v > -STICK_DEADZONE && v < STICK_DEADZONE)
		return 0.0f;
	return v;
}

static void apply_move(GamepadSlot *slot, float lx, float ly, bool up, bool down, bool left, bool right)
{
	up = up || ly < -STICK_DIGITAL;
	down = down || ly > STICK_DIGITAL;
	left = left || lx < -STICK_DIGITAL;
	right = right || lx > STICK_DIGITAL;
	slot->axisValue[0] = lx;
	slot->axisValue[1] = ly;
	set_btn(slot, 12, up);
	set_btn(slot, 13, down);
	set_btn(slot, 14, left);
	set_btn(slot, 15, right);
}

/* Every controller reaches the game only as its gamepad, each button where AM2R's own pad layout puts the action:
 * keyboard keys alongside doubled some actions and left Aim Down unreachable (session 12). */
typedef struct {
	u32 mask;
	int slot;
} ButtonMap;

enum { SLOT_FACE1, SLOT_FACE2, SLOT_FACE3, SLOT_FACE4, SLOT_LB, SLOT_RB, SLOT_LT, SLOT_RT, SLOT_SELECT, SLOT_START,
       SLOT_HOME = 16 };

/* By position: B bottom, A right, Y left, X top; L/R/ZL/ZR are the pad's LB/RB/LT/RT. */
static const ButtonMap classicMap[] = {
	{ WPAD_CLASSIC_BUTTON_B, SLOT_FACE1 },      /* jump, menu accept */
	{ WPAD_CLASSIC_BUTTON_A, SLOT_FACE2 },      /* weapon select, menu back */
	{ WPAD_CLASSIC_BUTTON_Y, SLOT_FACE3 },      /* fire */
	{ WPAD_CLASSIC_BUTTON_X, SLOT_FACE4 },      /* weapon cancel */
	{ WPAD_CLASSIC_BUTTON_FULL_L, SLOT_LB },    /* aim up */
	{ WPAD_CLASSIC_BUTTON_FULL_R, SLOT_RB },    /* aim lock */
	{ WPAD_CLASSIC_BUTTON_ZL, SLOT_LT },        /* aim down */
	{ WPAD_CLASSIC_BUTTON_ZR, SLOT_RT },        /* morph */
	{ WPAD_CLASSIC_BUTTON_MINUS, SLOT_SELECT }, /* and Esc: the options menu */
	{ WPAD_CLASSIC_BUTTON_PLUS, SLOT_START },
	{ WPAD_CLASSIC_BUTTON_HOME, SLOT_HOME },
	{ 0, 0 },
};
static const ButtonMap gcMap[] = {
	{ PAD_BUTTON_A, SLOT_FACE1 },     /* jump, menu accept */
	{ PAD_BUTTON_X, SLOT_FACE2 },     /* weapon select, menu back */
	{ PAD_BUTTON_B, SLOT_FACE3 },     /* fire */
	{ PAD_BUTTON_Y, SLOT_FACE4 },     /* weapon cancel */
	{ PAD_TRIGGER_R, SLOT_LB },       /* aim up */
	{ PAD_TRIGGER_Z, SLOT_RB },       /* aim lock */
	{ PAD_TRIGGER_L, SLOT_LT },       /* aim down */
	{ PAD_BUTTON_START, SLOT_START },
	{ 0, 0 },
};
/* The stick moves, so the Wii Remote's d-pad is free for the aim and weapon buttons. */
static const ButtonMap nunchukMap[] = {
	{ WPAD_BUTTON_A, SLOT_FACE1 },         /* jump, menu accept */
	{ WPAD_BUTTON_LEFT, SLOT_FACE2 },      /* weapon select, menu back */
	{ WPAD_BUTTON_B, SLOT_FACE3 },         /* fire */
	{ WPAD_BUTTON_RIGHT, SLOT_FACE4 },     /* weapon cancel */
	{ WPAD_BUTTON_UP, SLOT_LB },           /* aim up */
	{ WPAD_NUNCHUK_BUTTON_Z, SLOT_RB },    /* aim lock */
	{ WPAD_BUTTON_DOWN, SLOT_LT },         /* aim down */
	{ WPAD_NUNCHUK_BUTTON_C, SLOT_RT },    /* morph */
	{ WPAD_BUTTON_MINUS, SLOT_SELECT },
	{ WPAD_BUTTON_PLUS, SLOT_START },
	{ WPAD_BUTTON_HOME, SLOT_HOME },
	{ 0, 0 },
};
/* A Wii Remote alone: the d-pad moves, so weapon select and the triggers' actions have no button. */
static const ButtonMap remoteMap[] = {
	{ WPAD_BUTTON_A, SLOT_FACE1 }, { WPAD_BUTTON_B, SLOT_FACE3 }, { WPAD_BUTTON_1, SLOT_LB },
	{ WPAD_BUTTON_2, SLOT_FACE4 }, { WPAD_BUTTON_MINUS, SLOT_SELECT }, { WPAD_BUTTON_PLUS, SLOT_START },
	{ WPAD_BUTTON_HOME, SLOT_HOME }, { 0, 0 },
};

static void apply_map(GamepadSlot *slot, const ButtonMap *map, u32 held)
{
	for (; map->mask; map++)
		if (held & map->mask)
			set_btn(slot, map->slot, true);
}

static void poll_classic(GamepadSlot *slot, u32 held, const expansion_t *exp)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_CLASSIC)
		joy_xy(&exp->classic.ljs, &lx, &ly);
	apply_move(slot, lx, ly, held & WPAD_CLASSIC_BUTTON_UP, held & WPAD_CLASSIC_BUTTON_DOWN,
		   held & WPAD_CLASSIC_BUTTON_LEFT, held & WPAD_CLASSIC_BUTTON_RIGHT);
	apply_map(slot, classicMap, held);
	if (held & WPAD_CLASSIC_BUTTON_MINUS)
		want(VK_ESCAPE);
}

static void poll_nunchuk(GamepadSlot *slot, u32 held, const expansion_t *exp)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_NUNCHUK) {
		joy_xy(&exp->nunchuk.js, &lx, &ly);
		apply_move(slot, lx, ly, false, false, false, false);
		apply_map(slot, nunchukMap, held);
	} else {
		apply_move(slot, lx, ly, held & WPAD_BUTTON_UP, held & WPAD_BUTTON_DOWN,
			   held & WPAD_BUTTON_LEFT, held & WPAD_BUTTON_RIGHT);
		apply_map(slot, remoteMap, held);
	}
	if (held & WPAD_BUTTON_MINUS)
		want(VK_ESCAPE);
}

static void poll_gcpad(GamepadSlot *slot, u16 held)
{
	float lx = gc_axis(PAD_StickX(0));
	float ly = -gc_axis(PAD_StickY(0));

	apply_move(slot, lx, ly, held & PAD_BUTTON_UP, held & PAD_BUTTON_DOWN,
		   held & PAD_BUTTON_LEFT, held & PAD_BUTTON_RIGHT);
	apply_map(slot, gcMap, held);
	if (gc_axis(PAD_SubStickY(0)) < -STICK_DIGITAL) /* C-stick down: morph (a double tap of Down also works) */
		set_btn(slot, SLOT_RT, true);
}

/* L + D-pad Down + Y opens the overscan overlay; any face button (A/B/X/Y) closes it.
 * While it's up the d-pad drives the H/V sliders (with R held, Up/Down shift the picture
 * instead) and the game gets neutral input. */
static void overscan_input(u32 wheld, u16 gcheld)
{
	static bool prev_combo;
	static bool prev_face;
	static bool guard;
	static unsigned rate;
	bool lheld = (wheld & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) ||
		     (gcheld & PAD_TRIGGER_L) || PAD_TriggerL(0) > 64;
	bool rheld = (wheld & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) ||
		     (gcheld & PAD_TRIGGER_R) || PAD_TriggerR(0) > 64;
	bool down = (wheld & WPAD_CLASSIC_BUTTON_DOWN) || (gcheld & PAD_BUTTON_DOWN);
	bool combo = lheld && down &&
		     ((wheld & WPAD_CLASSIC_BUTTON_Y) || (gcheld & PAD_BUTTON_Y));
	bool face = (wheld & (WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_B |
			      WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y)) ||
		    (gcheld & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y));

	if (!Overscan_active()) {
		if (combo && !prev_combo) {
			Overscan_toggle();
			guard = true; /* ignore the opening buttons until they're released */
		}
	} else if (guard) {
		if (!face && !down)
			guard = false;
	} else if (face && !prev_face) {
		Overscan_toggle();
	}
	prev_combo = combo;
	prev_face = face;

	if (!Overscan_active() || guard || (++rate & 1))
		return;
	if ((wheld & WPAD_CLASSIC_BUTTON_LEFT) || (gcheld & PAD_BUTTON_LEFT))
		Overscan_adjustH(-1);
	if ((wheld & WPAD_CLASSIC_BUTTON_RIGHT) || (gcheld & PAD_BUTTON_RIGHT))
		Overscan_adjustH(1);
	if ((wheld & WPAD_CLASSIC_BUTTON_UP) || (gcheld & PAD_BUTTON_UP))
		(rheld ? Overscan_adjustY : Overscan_adjustV)(-1);
	if (down)
		(rheld ? Overscan_adjustY : Overscan_adjustV)(1);
}

/* AM2R names pad buttons by its [Extras] Controller setting, where 0 (never chosen) means Xbox names. The layouts
 * above are exactly its Nintendo names (B bottom, A right, Y left, X top, L R ZL ZR), so 0 becomes 2 once per boot. */
static void nintendo_names(Runner *runner)
{
	static bool done;
	VMContext *vm = runner->vmContext;
	int32_t idx = vm ? Wii_objectIndex(runner, "oControl") : -1;
	Instance *ctl;
	ptrdiff_t slot;
	RValue v;

	if (done || idx < 0 || !runner->instancesByExactObject || arrlen(runner->instancesByExactObject[idx]) == 0)
		return;
	ctl = runner->instancesByExactObject[idx][0];
	slot = shgeti(vm->varNameMap, "mod_buttonsconfig");
	if (slot < 0) {
		done = true;
		return;
	}
	v = Instance_getSelfVar(ctl, vm->varNameMap[slot].value);
	if (v.type == RVALUE_UNDEFINED) { /* oControl reads the options a few frames in */
		if (runner->frameCount > 600) {
			logInfo("controller names: oControl.mod_buttonsconfig never set\n");
			done = true;
		}
		return;
	}
	done = true;
	logInfo("controller names: mod_buttonsconfig %g at frame %d%s\n", (double)RValue_toReal(v), runner->frameCount,
	        RValue_toReal(v) == 0 ? " -> 2 (Nintendo)" : "");
	if (RValue_toReal(v) == 0)
		Instance_setSelfVar(ctl, vm->varNameMap[slot].value, RValue_makeInt32(2));
}

void WiiInput_init(void)
{
	WPAD_Init();
	PAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC);
	home_held = 0;
	memset(want_vk, 0, sizeof(want_vk));
	memset(prev_vk, 0, sizeof(prev_vk));
}

void WiiInput_poll(Runner *runner)
{
	GamepadSlot *slot;
	u32 wheld;
	u16 gcheld;
	int i;
	expansion_t exp;

	if (!runner || !runner->gamepads)
		return;

	WPAD_ScanPads();
	PAD_ScanPads();
	wheld = WPAD_ButtonsHeld(0);
	gcheld = PAD_ButtonsHeld(0);
	WPAD_Expansion(0, &exp);

	memset(want_vk, 0, sizeof(want_vk));
	slot = &runner->gamepads->slots[0];
	memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
	memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
	memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
	memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
	memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
	memset(slot->axisValue, 0, sizeof(slot->axisValue));
	slot->connected = true;
	slot->jid = 0;
	strncpy(slot->description, "Nintendo Wii Classic Controller", sizeof(slot->description) - 1);
	slot->description[sizeof(slot->description) - 1] = '\0';
	slot->guid[0] = '\0';

	nintendo_names(runner);
	overscan_input(wheld, gcheld);
	if (Overscan_active())
		; /* overlay owns input; leave the slot neutral */
	else if (exp.type == WPAD_EXP_CLASSIC)
		poll_classic(slot, wheld, &exp);
	else if (gcheld || PAD_StickX(0) || PAD_StickY(0))
		poll_gcpad(slot, gcheld);
	else
		poll_nunchuk(slot, wheld, &exp);

	for (i = 0; i < GP_BUTTON_COUNT; i++) {
		bool was = slot->buttonDownPrev[i];

		if (slot->buttonDown[i] && !was)
			slot->buttonPressed[i] = true;
		if (!slot->buttonDown[i] && was)
			slot->buttonReleased[i] = true;
	}
	runner->gamepads->connectedCount = 1;

	if (runner->keyboard && !InputRecording_isPlaybackActive(globalInputRecording)) {
		for (i = 0; i < GML_KEY_COUNT; i++) {
			if (want_vk[i] && !prev_vk[i])
				RunnerKeyboard_onKeyDown(runner->keyboard, i);
			if (!want_vk[i] && prev_vk[i])
				RunnerKeyboard_onKeyUp(runner->keyboard, i);
		}
		memcpy(prev_vk, want_vk, sizeof(prev_vk));
	}
}

bool WiiInput_shouldQuit(void)
{
	u32 w;

	if (!SYS_MainLoop())
		return true;

	WPAD_ScanPads();
	PAD_ScanPads();
	w = WPAD_ButtonsHeld(0);
	if (w & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
		home_held++;
	else
		home_held = 0;
	return home_held >= HOME_HOLD_FRAMES;
}
