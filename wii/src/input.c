#include "input.h"

#include "input_recording.h"
#include "am2r.h"
#include "controls.h"
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
/* The GameCube pad has no HOME: X + B + Start, the reset its own games use. */
#define GC_QUIT (PAD_BUTTON_X | PAD_BUTTON_B | PAD_BUTTON_START)
/* The display screen's chords: nothing a player holds in play (L + Down + Y was "aim down and fire"). */
#define CLASSIC_DISPLAY (WPAD_CLASSIC_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_MINUS)
#define GC_DISPLAY (PAD_BUTTON_START | PAD_TRIGGER_Z)
#define CHORD_FRAMES 6

static int home_held;
static u32 polled_wheld; /* the last poll's reading, test hooks included */
static u16 polled_gcheld;
static WiiPad pad_in_use = WII_PAD_CLASSIC;
static float rumble_level;
static bool rumble_fresh, motor_on;

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
 * lives in the game's bindings (padBindings). */
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
	{ WPAD_CLASSIC_BUTTON_FULL_L, SLOT_LB },    /* aim diagonal */
	{ WPAD_CLASSIC_BUTTON_FULL_R, SLOT_RB },    /* arm weapon */
	{ WPAD_CLASSIC_BUTTON_ZL, SLOT_LT },        /* aim lock */
	{ WPAD_CLASSIC_BUTTON_ZR, SLOT_RT },        /* Aim Down with two-button aim */
	{ WPAD_CLASSIC_BUTTON_MINUS, SLOT_SELECT },
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
/* A fires and B jumps like Prime; menus still take A as OK (see swap_menus). */
static const ButtonMap nunchukMap[] = {
	{ WPAD_BUTTON_B, SLOT_FACE1 },         /* jump */
	{ WPAD_NUNCHUK_BUTTON_C, SLOT_FACE2 }, /* morph */
	{ WPAD_BUTTON_A, SLOT_FACE3 },         /* fire */
	{ WPAD_BUTTON_MINUS, SLOT_FACE4 },     /* weapon select */
	{ WPAD_NUNCHUK_BUTTON_Z, SLOT_RB },    /* arm weapon */
	{ WPAD_BUTTON_LEFT, SLOT_LT },         /* aim lock */
	{ WPAD_BUTTON_RIGHT, SLOT_LT },
	{ WPAD_BUTTON_PLUS, SLOT_START },
	{ WPAD_BUTTON_HOME, SLOT_HOME },
	{ 0, 0 },
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

/* One slot per button: the remap screen takes the first slot it finds held, so a doubled button can't be bound. */
static void poll_classic(GamepadSlot *slot, u32 held, const expansion_t *exp)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_CLASSIC)
		joy_xy(&exp->classic.ljs, &lx, &ly);
	apply_move(slot, lx, ly, held & WPAD_CLASSIC_BUTTON_UP, held & WPAD_CLASSIC_BUTTON_DOWN,
		   held & WPAD_CLASSIC_BUTTON_LEFT, held & WPAD_CLASSIC_BUTTON_RIGHT);
	apply_map(slot, classicMap, held);
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

static void poll_nunchuk(GamepadSlot *slot, u32 held, const expansion_t *exp, bool twoAim, bool pause)
{
	float lx, ly;

	if (exp->type != WPAD_EXP_NUNCHUK)
		return; /* a Remote alone is not a controller for this game (Jaxi) */
	nunchuk_xy(&exp->nunchuk.js, &lx, &ly);
	/* One-button aim: the d-pad holds the aim button and says which diagonal. */
	apply_move(slot, lx, ly, !twoAim && (held & WPAD_BUTTON_UP), !twoAim && (held & WPAD_BUTTON_DOWN), false, false);
	apply_map(slot, nunchukMap, held);
	if (held & WPAD_BUTTON_UP)
		set_btn(slot, SLOT_LB, true);
	if (held & WPAD_BUTTON_DOWN)
		set_btn(slot, twoAim ? SLOT_RT : SLOT_LB, true);
	if (pause)
		set_btn(slot, SLOT_START, true);
}

/* 1 pauses when it is let go, so holding 1 + 2 for the overscan screen doesn't pause the game on the way. */
static bool remote_one_released(u32 wheld, bool overscan)
{
	static bool armed, prev;
	bool down = wheld & WPAD_BUTTON_1, fire = !down && prev && armed;

	if (down && !prev)
		armed = true;
	if (!down || (wheld & WPAD_BUTTON_2) || overscan)
		armed = false;
	prev = down;
	return fire;
}

/* Start reaches the game a few frames late, so its display chord never pauses on the way; a tap pauses on release. */
static bool start_for_game(bool start, bool partner)
{
	static int frames;
	static bool chord;
	bool tap;

	if (start) {
		chord = chord || partner;
		return !chord && ++frames > CHORD_FRAMES;
	}
	tap = !chord && frames > 0 && frames <= CHORD_FRAMES;
	frames = 0;
	chord = false;
	return tap;
}

/* Held partway down: a long hold of a GameCube trigger to its click is tiring. */
#define GC_TRIGGER_PRESS 96

/* The C-stick aims like the Nunchuk's d-pad: up/down with the aim button, or Aim Up / Aim Down with two-button aim. */
static void poll_gcpad(GamepadSlot *slot, u16 held, const s8 *sticks, bool twoAim)
{
	float lx = gc_axis(sticks[0]);
	float ly = -gc_axis(sticks[1]);
	bool cup = gc_axis(sticks[2]) > STICK_DIGITAL, cdown = gc_axis(sticks[2]) < -STICK_DIGITAL;

	if (PAD_TriggerL(0) > GC_TRIGGER_PRESS)
		held |= PAD_TRIGGER_L;
	if (PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		held |= PAD_TRIGGER_R;
	apply_move(slot, lx, ly, (held & PAD_BUTTON_UP) || (!twoAim && cup), (held & PAD_BUTTON_DOWN) || (!twoAim && cdown),
		   held & PAD_BUTTON_LEFT, held & PAD_BUTTON_RIGHT);
	apply_map(slot, gcMap, held);
	if (cup)
		set_btn(slot, SLOT_LB, true);
	if (cdown)
		set_btn(slot, twoAim ? SLOT_RT : SLOT_LB, true);
}

/* Opens with Plus + Minus (Classic), Start + Z (GameCube) or 1 + 2 (Remote + Nunchuk); a face button closes it. While it is up
 * Left/Right and Up/Down set the width and height, and the game gets neutral input. The shifts move the picture
 * instead: L (Nunchuk C) with Left/Right, R (Nunchuk Z) with Up/Down. Sticks count as the d-pad. */
static void overscan_input(u32 wheld, u16 gcheld, const s8 *sticks, const expansion_t *exp)
{
	static bool prev_combo;
	static bool prev_face;
	static bool guard;
	static unsigned rate;
	float sx = gc_axis(sticks[0]), sy = -gc_axis(sticks[1]), wx = 0.0f, wy = 0.0f;
	/* the Nunchuk's Z and C share their bits with the Classic's Up and Left */
	u32 classic = exp->type == WPAD_EXP_CLASSIC ? wheld : 0, nunchuk = exp->type == WPAD_EXP_NUNCHUK ? wheld : 0;
	bool up, down, left, right, lheld, rheld, face, combo;

	if (exp->type == WPAD_EXP_CLASSIC)
		joy_xy(&exp->classic.ljs, &wx, &wy);
	else if (exp->type == WPAD_EXP_NUNCHUK)
		joy_xy(&exp->nunchuk.js, &wx, &wy);
	up = (classic & WPAD_CLASSIC_BUTTON_UP) || (wheld & WPAD_BUTTON_UP) || (gcheld & PAD_BUTTON_UP) ||
	     wy < -STICK_DIGITAL || sy < -STICK_DIGITAL;
	down = (classic & WPAD_CLASSIC_BUTTON_DOWN) || (wheld & WPAD_BUTTON_DOWN) || (gcheld & PAD_BUTTON_DOWN) ||
	       wy > STICK_DIGITAL || sy > STICK_DIGITAL;
	left = (classic & WPAD_CLASSIC_BUTTON_LEFT) || (wheld & WPAD_BUTTON_LEFT) || (gcheld & PAD_BUTTON_LEFT) ||
	       wx < -STICK_DIGITAL || sx < -STICK_DIGITAL;
	right = (classic & WPAD_CLASSIC_BUTTON_RIGHT) || (wheld & WPAD_BUTTON_RIGHT) || (gcheld & PAD_BUTTON_RIGHT) ||
		wx > STICK_DIGITAL || sx > STICK_DIGITAL;
	lheld = (classic & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) || (gcheld & PAD_TRIGGER_L) ||
		PAD_TriggerL(0) > 64 || (nunchuk & WPAD_NUNCHUK_BUTTON_C);
	rheld = (classic & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR)) || (gcheld & PAD_TRIGGER_R) ||
		PAD_TriggerR(0) > 64 || (nunchuk & WPAD_NUNCHUK_BUTTON_Z);
	face = (classic & (WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_B | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y)) ||
	       (wheld & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_BUTTON_2)) ||
	       (gcheld & (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y));
	combo = (classic & CLASSIC_DISPLAY) == CLASSIC_DISPLAY || (gcheld & GC_DISPLAY) == GC_DISPLAY ||
		(wheld & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) == (WPAD_BUTTON_1 | WPAD_BUTTON_2);

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
	if (left)
		(lheld ? Overscan_adjustX : Overscan_adjustH)(-1);
	if (right)
		(lheld ? Overscan_adjustX : Overscan_adjustH)(1);
	if (up)
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

static bool defaults_done, menus_swapped;

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

static int swap_face(int button)
{
	return button == GP_FACE1 ? GP_FACE3 : button == GP_FACE3 ? GP_FACE1 : button;
}

/* The Nunchuk's A and B sit on each other's slots, so while it is in use the menu bindings are held swapped too: menus,
 * their prompts and the remap screen then all see A as OK. The options file only ever gets the real slots. */
static void swap_menus(VMContext *vm)
{
	static const char *const vars[] = { "opxjoybtn_menu1", "opxjoybtn_menu2" };
	unsigned k;

	for (k = 0; k < 2; k++) {
		double b = global_num(vm, vars[k]);

		if (b >= 0)
			set_global_num(vm, vars[k], swap_face((int)b));
	}
}

/* A fresh Menu Back press becomes Menu OK when an options screen has put its cursor on its Back row. */
static void menu_back(Runner *runner, GamepadSlot *slot)
{
	VMContext *vm = runner->vmContext;
	int ok, back;

	if (!defaults_done || !vm || !vm->globalScopeInstance)
		return;
	ok = (int)global_num(vm, "opxjoybtn_menu1") - GP_FACE1;
	back = (int)global_num(vm, "opxjoybtn_menu2") - GP_FACE1;
	if (ok < 0 || back < 0 || ok >= GP_BUTTON_COUNT || back >= GP_BUTTON_COUNT || !slot->buttonPressed[back] ||
	    slot->buttonDown[ok] || !WiiControls_menuBack(runner))
		return;
	set_btn(slot, ok, true);
	slot->buttonPressed[ok] = true;
}

void WiiInput_flipMenus(VMContext *vm)
{
	if (menus_swapped)
		swap_menus(vm);
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
	VMContext *vm = runner->vmContext;
	int32_t idx = vm ? Wii_objectIndex(runner, "oControl") : -1;
	Instance *ctl;
	ptrdiff_t slot;
	unsigned k;
	bool stock = true;
	RValue v;

	if (defaults_done || idx < 0 || !vm->globalScopeInstance || !runner->instancesByExactObject ||
	    arrlen(runner->instancesByExactObject[idx]) == 0)
		return;
	ctl = runner->instancesByExactObject[idx][0];
	slot = shgeti(vm->varNameMap, "mod_buttonsconfig");
	v = slot >= 0 ? Instance_getSelfVar(ctl, vm->varNameMap[slot].value) : (RValue){ .type = RVALUE_UNDEFINED };
	if (v.type == RVALUE_UNDEFINED && runner->frameCount < 600)
		return; /* oControl reads the options a few frames in */
	defaults_done = true;
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
static void note_pad(u32 wheld, u16 gcheld, s8 csticky, const expansion_t *exp)
{
	if (exp->type == WPAD_EXP_CLASSIC)
		pad_in_use = WII_PAD_CLASSIC;
	else if (gcheld || fabsf(gc_axis(csticky)) > STICK_DIGITAL || PAD_TriggerL(0) > GC_TRIGGER_PRESS ||
		 PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		pad_in_use = WII_PAD_GAMECUBE;
	else if ((wheld & ~(u32)(WPAD_BUTTON_HOME)) && exp->type == WPAD_EXP_NUNCHUK)
		pad_in_use = WII_PAD_NUNCHUK;
}

WiiPad WiiInput_pad(void)
{
	return pad_in_use;
}

/* AM2R calls gamepad_set_vibration every step while its Vibration option is on, so a step without a call stops. */
void WiiInput_rumble(float left, float right)
{
	float level = left > right ? left : right;

	rumble_level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
	rumble_fresh = true;
}

/* Only the GameCube pad has a motor the game can reach, and it is on or off: a level is a share of frames. */
static void drive_rumble(void)
{
	static float acc;
	static bool active;
	float level = rumble_fresh && pad_in_use == WII_PAD_GAMECUBE ? rumble_level : 0.0f;
	bool on;

	rumble_fresh = false;
	if ((level > 0.0f) != active) {
		active = level > 0.0f;
		acc = 1.0f; /* a new rumble starts with a pulse */
		if (active)
			logInfo("rumble on (%.2f)\n", level);
		else
			logInfo("rumble off\n");
	} else {
		acc += level;
	}
	on = active && acc >= 1.0f;
	if (on)
		acc -= 1.0f;
	if (on != motor_on) {
		PAD_ControlMotor(PAD_CHAN0, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
		motor_on = on;
	}
}

void WiiInput_stopRumble(void)
{
	PAD_ControlMotor(PAD_CHAN0, PAD_MOTOR_STOP_HARD);
	motor_on = false;
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
	s8 sticks[3];
	int i;
	bool overscan, pause, classic, start;
	expansion_t exp;

	if (!runner || !runner->gamepads)
		return;

	WPAD_ScanPads();
	PAD_ScanPads();
	wheld = WPAD_ButtonsHeld(0);
	gcheld = PAD_ButtonsHeld(0);
	sticks[0] = PAD_StickX(0);
	sticks[1] = PAD_StickY(0);
	sticks[2] = PAD_SubStickY(0);
	WPAD_Expansion(0, &exp);
	Wii_hooksPad(runner, &wheld, &gcheld, &exp, sticks);
	polled_wheld = wheld;
	polled_gcheld = gcheld;

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
	note_pad(wheld, gcheld, sticks[2], &exp);
	if (defaults_done && (pad_in_use == WII_PAD_NUNCHUK) != menus_swapped) {
		swap_menus(runner->vmContext);
		menus_swapped = !menus_swapped;
		logInfo("menu buttons: %s\n", menus_swapped ? "Nunchuk (A = OK)" : "real");
	}
	drive_rumble();
	overscan = Overscan_active();
	overscan_input(wheld, gcheld, sticks, &exp);
	pause = remote_one_released(wheld, overscan || Overscan_active());
	classic = exp.type == WPAD_EXP_CLASSIC;
	start = start_for_game(classic ? wheld & WPAD_CLASSIC_BUTTON_PLUS : gcheld & PAD_BUTTON_START,
			       classic ? wheld & WPAD_CLASSIC_BUTTON_MINUS : gcheld & PAD_TRIGGER_Z);
	if (Overscan_active())
		; /* overlay owns input; leave the slot neutral */
	else if (classic)
		poll_classic(slot, wheld & ~(u32)WPAD_CLASSIC_BUTTON_PLUS, &exp);
	else if (gcheld || sticks[0] || sticks[1] || gc_axis(sticks[2]) != 0.0f || PAD_TriggerL(0) > GC_TRIGGER_PRESS ||
		 PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		poll_gcpad(slot, gcheld & ~PAD_BUTTON_START, sticks, two_button_aim(runner));
	else
		poll_nunchuk(slot, wheld, &exp, two_button_aim(runner), pause);
	if (start && !Overscan_active())
		set_btn(slot, SLOT_START, true);

	for (i = 0; i < GP_BUTTON_COUNT; i++) {
		bool was = slot->buttonDownPrev[i];

		if (slot->buttonDown[i] && !was)
			slot->buttonPressed[i] = true;
		if (!slot->buttonDown[i] && was)
			slot->buttonReleased[i] = true;
	}
	menu_back(runner, slot);
	runner->gamepads->connectedCount = 1;

}

bool WiiInput_shouldQuit(void)
{
	u32 w;
	u16 g;

	if (!SYS_MainLoop())
		return true;

	WPAD_ScanPads();
	PAD_ScanPads();
	w = WPAD_ButtonsHeld(0) | polled_wheld;
	g = PAD_ButtonsHeld(0) | polled_gcheld;
	if ((w & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) || (g & GC_QUIT) == GC_QUIT)
		home_held++;
	else
		home_held = 0;
	return home_held >= HOME_HOLD_FRAMES;
}
