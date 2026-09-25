#include "input.h"

#include "input_recording.h"
#include "am2r.h"
#include "vm.h"
#include "stb_ds.h"
#include "log.h"
#include "instance.h"
#include "overscan.h"
#include "hooks.h"
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
#define NUNCHUK_DEADZONE 0.3f

#define HOME_HOLD_FRAMES 60

static int home_held;
static WiiPad pad_in_use = WII_PAD_CLASSIC;

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

/* Every controller reaches the game only as its gamepad (keyboard keys alongside doubled actions, session 12).
 * Face buttons go by position, bottom = face1, so AM2R's own remap screen names them right; the layout itself
 * lives in the game's bindings (first_run_controls): Jump face1, Fire face3, Morph face2, weapon cycle face4,
 * Aim LB, Arm Weapon RB, Aim Lock LT, Aim Down RT (two-button aim only). */
typedef struct {
	u32 mask;
	int slot;
} ButtonMap;

enum { SLOT_FACE1, SLOT_FACE2, SLOT_FACE3, SLOT_FACE4, SLOT_LB, SLOT_RB, SLOT_LT, SLOT_RT, SLOT_SELECT, SLOT_START,
       SLOT_PADU = 12, SLOT_PADD, SLOT_PADL, SLOT_PADR, SLOT_HOME };

static const ButtonMap classicMap[] = {
	{ WPAD_CLASSIC_BUTTON_B, SLOT_FACE1 },      /* jump, menu accept */
	{ WPAD_CLASSIC_BUTTON_A, SLOT_FACE2 },      /* morph */
	{ WPAD_CLASSIC_BUTTON_Y, SLOT_FACE3 },      /* fire, menu back */
	{ WPAD_CLASSIC_BUTTON_X, SLOT_FACE4 },      /* weapon select */
	{ WPAD_CLASSIC_BUTTON_MINUS, SLOT_FACE4 },  /* weapon select, the Select-button habit */
	{ WPAD_CLASSIC_BUTTON_FULL_L, SLOT_LB },    /* aim diagonal */
	{ WPAD_CLASSIC_BUTTON_FULL_R, SLOT_RB },    /* arm weapon */
	{ WPAD_CLASSIC_BUTTON_ZL, SLOT_LT },        /* aim lock */
	{ WPAD_CLASSIC_BUTTON_PLUS, SLOT_START },
	{ WPAD_CLASSIC_BUTTON_HOME, SLOT_HOME },
	{ 0, 0 },
};
static const ButtonMap gcMap[] = {
	{ PAD_BUTTON_A, SLOT_FACE1 },     /* jump, menu accept */
	{ PAD_BUTTON_X, SLOT_FACE2 },     /* morph */
	{ PAD_BUTTON_B, SLOT_FACE3 },     /* fire, menu back */
	{ PAD_BUTTON_Y, SLOT_FACE4 },     /* weapon select */
	{ PAD_TRIGGER_L, SLOT_LB },       /* aim diagonal */
	{ PAD_TRIGGER_R, SLOT_RB },       /* arm weapon */
	{ PAD_TRIGGER_Z, SLOT_LT },       /* aim lock */
	{ PAD_BUTTON_START, SLOT_START },
	{ 0, 0 },
};
/* The stick moves, so the Remote's d-pad holds the aim: up/down aim diagonally, left/right lock the aim. */
static const ButtonMap nunchukMap[] = {
	{ WPAD_BUTTON_A, SLOT_FACE1 },         /* jump, menu accept */
	{ WPAD_NUNCHUK_BUTTON_C, SLOT_FACE2 }, /* morph */
	{ WPAD_BUTTON_B, SLOT_FACE3 },         /* fire, menu back */
	{ WPAD_BUTTON_MINUS, SLOT_FACE4 },     /* weapon select */
	{ WPAD_NUNCHUK_BUTTON_Z, SLOT_RB },    /* arm weapon */
	{ WPAD_BUTTON_LEFT, SLOT_LT },         /* aim lock */
	{ WPAD_BUTTON_RIGHT, SLOT_LT },
	{ WPAD_BUTTON_PLUS, SLOT_START },
	{ WPAD_BUTTON_HOME, SLOT_HOME },
	{ 0, 0 },
};
/* A Wii Remote alone: the d-pad moves, so only the main actions fit. */
static const ButtonMap remoteMap[] = {
	{ WPAD_BUTTON_2, SLOT_FACE1 }, { WPAD_BUTTON_1, SLOT_FACE3 }, { WPAD_BUTTON_A, SLOT_FACE2 },
	{ WPAD_BUTTON_B, SLOT_RB }, { WPAD_BUTTON_MINUS, SLOT_FACE4 }, { WPAD_BUTTON_PLUS, SLOT_START },
	{ WPAD_BUTTON_HOME, SLOT_HOME }, { 0, 0 },
};

static void apply_map(GamepadSlot *slot, const ButtonMap *map, u32 held)
{
	for (; map->mask; map++)
		if (held & map->mask)
			set_btn(slot, map->slot, true);
}

/* AM2R's Aiming Style: 0 one aim button plus Up/Down, 1 separate Aim Up / Aim Down buttons. */
static bool two_button_aim(Runner *runner)
{
	VMContext *vm = runner ? runner->vmContext : NULL;
	ptrdiff_t slot;
	RValue v;

	if (!vm || !vm->globalScopeInstance || (slot = shgeti(vm->varNameMap, "opaimstyle")) < 0)
		return false;
	v = Instance_getSelfVar(vm->globalScopeInstance, vm->varNameMap[slot].value);
	return v.type != RVALUE_UNDEFINED && RValue_toReal(v) == 1;
}

static void poll_classic(GamepadSlot *slot, u32 held, const expansion_t *exp, bool twoAim)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_CLASSIC)
		joy_xy(&exp->classic.ljs, &lx, &ly);
	apply_move(slot, lx, ly, held & WPAD_CLASSIC_BUTTON_UP, held & WPAD_CLASSIC_BUTTON_DOWN,
		   held & WPAD_CLASSIC_BUTTON_LEFT, held & WPAD_CLASSIC_BUTTON_RIGHT);
	apply_map(slot, classicMap, held);
	if (held & WPAD_CLASSIC_BUTTON_ZR) /* arm weapon again (nicer on the Pro), or Aim Down with two-button aim */
		set_btn(slot, twoAim ? SLOT_RT : SLOT_RB, true);
}

/* Eight directions with a generous deadzone: diagonals from a thumb on a round gate come out clean. */
static void nunchuk_xy(const joystick_t *js, float *lx, float *ly)
{
	float rad;
	int dir;

	*lx = *ly = 0.0f;
	if (!js || js->mag < NUNCHUK_DEADZONE)
		return;
	dir = (int)floorf(js->ang / 45.0f + 0.5f) & 7;
	rad = dir * 45.0f * ((float)M_PI / 180.0f);
	*lx = js->mag * sinf(rad);
	*ly = -js->mag * cosf(rad);
}

static void poll_nunchuk(GamepadSlot *slot, u32 held, const expansion_t *exp, bool twoAim)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_NUNCHUK) {
		nunchuk_xy(&exp->nunchuk.js, &lx, &ly);
		/* One-button aim: the d-pad holds the aim button and says which diagonal. */
		apply_move(slot, lx, ly, !twoAim && (held & WPAD_BUTTON_UP), !twoAim && (held & WPAD_BUTTON_DOWN), false, false);
		apply_map(slot, nunchukMap, held);
		if (held & WPAD_BUTTON_UP)
			set_btn(slot, SLOT_LB, true);
		if (held & WPAD_BUTTON_DOWN)
			set_btn(slot, twoAim ? SLOT_RT : SLOT_LB, true);
	} else {
		apply_move(slot, lx, ly, held & WPAD_BUTTON_UP, held & WPAD_BUTTON_DOWN,
			   held & WPAD_BUTTON_LEFT, held & WPAD_BUTTON_RIGHT);
		apply_map(slot, remoteMap, held);
	}
}

/* Held partway down: a long hold of a GameCube trigger to its click is tiring. */
#define GC_TRIGGER_PRESS 96

static void poll_gcpad(GamepadSlot *slot, u16 held)
{
	float lx = gc_axis(PAD_StickX(0));
	float ly = -gc_axis(PAD_StickY(0));

	if (PAD_TriggerL(0) > GC_TRIGGER_PRESS)
		held |= PAD_TRIGGER_L;
	if (PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		held |= PAD_TRIGGER_R;
	apply_move(slot, lx, ly, held & PAD_BUTTON_UP, held & PAD_BUTTON_DOWN,
		   held & PAD_BUTTON_LEFT, held & PAD_BUTTON_RIGHT);
	apply_map(slot, gcMap, held);
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

/* AM2R's gamepad bindings as it ships them, then the Wii layout (Jaxi's, session 12). A config still holding the
 * stock set has never been remapped, so it takes the Wii set, one-button aim and Hold missiles; a remap is kept. */
static const struct {
	const char *var;
	int stock, wii;
} padBindings[] = {
	{ "opxjoybtn_a", GP_FACE1, GP_FACE1 },           /* jump */
	{ "opxjoybtn_b", GP_FACE3, GP_FACE3 },           /* fire */
	{ "opxjoybtn_x", GP_SHOULDERRB, GP_FACE2 },      /* morph */
	{ "opxjoybtn_l", GP_SHOULDERL, GP_SHOULDERL },   /* aim (up) */
	{ "opxjoybtn_l2", GP_SHOULDERLB, GP_SHOULDERRB }, /* aim down, two-button aim only */
	{ "opxjoybtn_r", GP_FACE2, GP_SHOULDERR },       /* weapon select / arm weapon */
	{ "opxjoybtn_r2", GP_SHOULDERR, GP_SHOULDERLB }, /* aim lock */
	{ "opxjoybtn_sel", GP_FACE4, GP_FACE4 },         /* weapon cancel / cycle */
	{ "opxjoybtn_str", GP_START, GP_START },
	{ "opxjoybtn_menu1", GP_FACE1, GP_FACE1 },
	{ "opxjoybtn_menu2", GP_FACE2, GP_FACE3 },
};

static double global_num(VMContext *vm, const char *name)
{
	ptrdiff_t slot = shgeti(vm->varNameMap, (char *)name);
	RValue v;

	if (slot < 0)
		return -1;
	v = Instance_getSelfVar(vm->globalScopeInstance, vm->varNameMap[slot].value);
	return v.type == RVALUE_UNDEFINED ? -1 : (double)RValue_toReal(v);
}

static void set_global_num(VMContext *vm, const char *name, int value)
{
	ptrdiff_t slot = shgeti(vm->varNameMap, (char *)name);

	if (slot >= 0)
		Instance_setSelfVar(vm->globalScopeInstance, vm->varNameMap[slot].value, RValue_makeInt32(value));
}

void WiiInput_applyLayout(VMContext *vm)
{
	unsigned k;

	for (k = 0; k < sizeof padBindings / sizeof padBindings[0]; k++)
		set_global_num(vm, padBindings[k].var, padBindings[k].wii);
}

/* Once per boot, when oControl has read the options. The button names: 0 (never chosen) means Xbox names;
 * AM2R's Nintendo names follow the Switch layout by position, which is how the buttons are laid out above. */
static void wii_pad_defaults(Runner *runner)
{
	static bool done;
	VMContext *vm = runner->vmContext;
	int32_t idx = vm ? Wii_objectIndex(runner, "oControl") : -1;
	Instance *ctl;
	ptrdiff_t slot;
	unsigned k;
	bool stock = true;
	RValue v;

	if (done || idx < 0 || !vm->globalScopeInstance || !runner->instancesByExactObject ||
	    arrlen(runner->instancesByExactObject[idx]) == 0)
		return;
	ctl = runner->instancesByExactObject[idx][0];
	slot = shgeti(vm->varNameMap, "mod_buttonsconfig");
	v = slot >= 0 ? Instance_getSelfVar(ctl, vm->varNameMap[slot].value) : (RValue){ .type = RVALUE_UNDEFINED };
	if (v.type == RVALUE_UNDEFINED && runner->frameCount < 600)
		return; /* oControl reads the options a few frames in */
	done = true;
	if (v.type != RVALUE_UNDEFINED && RValue_toReal(v) == 0)
		Instance_setSelfVar(ctl, vm->varNameMap[slot].value, RValue_makeInt32(2));
	for (k = 0; k < sizeof padBindings / sizeof padBindings[0]; k++)
		stock = stock && global_num(vm, padBindings[k].var) == padBindings[k].stock;
	logInfo("controller defaults: names %s, bindings %s\n", v.type != RVALUE_UNDEFINED && RValue_toReal(v) == 0 ? "-> Nintendo" : "kept",
	        stock ? "stock -> Wii layout, one-button aim, Hold missiles" : "remapped, kept");
	if (!stock)
		return;
	WiiInput_applyLayout(vm);
	set_global_num(vm, "opaimstyle", 0);
	set_global_num(vm, "opmslstyle", 0);
}

/* Sticky: an idle frame keeps the last controller, so a GameCube player's names do not flip back when the pad rests. */
static void note_pad(u32 wheld, u16 gcheld, const expansion_t *exp)
{
	if (exp->type == WPAD_EXP_CLASSIC)
		pad_in_use = WII_PAD_CLASSIC;
	else if (gcheld || PAD_TriggerL(0) > GC_TRIGGER_PRESS || PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		pad_in_use = WII_PAD_GAMECUBE;
	else if (wheld & ~(u32)(WPAD_BUTTON_HOME))
		pad_in_use = exp->type == WPAD_EXP_NUNCHUK ? WII_PAD_NUNCHUK : WII_PAD_REMOTE;
}

WiiPad WiiInput_pad(void)
{
	return pad_in_use;
}

void WiiInput_init(void)
{
	WPAD_Init();
	PAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC);
	home_held = 0;
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
	Wii_hooksPad(runner, &wheld, &gcheld, &exp.type);

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

	wii_pad_defaults(runner);
	note_pad(wheld, gcheld, &exp);
	overscan_input(wheld, gcheld);
	if (Overscan_active())
		; /* overlay owns input; leave the slot neutral */
	else if (exp.type == WPAD_EXP_CLASSIC)
		poll_classic(slot, wheld, &exp, two_button_aim(runner));
	else if (gcheld || PAD_StickX(0) || PAD_StickY(0) || PAD_TriggerL(0) > GC_TRIGGER_PRESS || PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		poll_gcpad(slot, gcheld);
	else
		poll_nunchuk(slot, wheld, &exp, two_button_aim(runner));

	for (i = 0; i < GP_BUTTON_COUNT; i++) {
		bool was = slot->buttonDownPrev[i];

		if (slot->buttonDown[i] && !was)
			slot->buttonPressed[i] = true;
		if (!slot->buttonDown[i] && was)
			slot->buttonReleased[i] = true;
	}
	runner->gamepads->connectedCount = 1;

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
