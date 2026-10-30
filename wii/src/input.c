#include "input.h"

#include "am2r.h"
#include "controls.h"
#include "vm.h"
#include "stb_ds.h"
#include "log.h"
#include "instance.h"
#include "overscan.h"
#include "safearea.h"
#include "hooks.h"
#include "runner.h"
#include "runner_gamepad.h"

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

static int home_held;
static u32 polled_wheld; /* the last poll's reading, test hooks included */
static u16 polled_gcheld;
static WiiPad pad_in_use = WII_PAD_CLASSIC;
static bool classic_is_wiiu;
static float rumble_level;
static bool rumble_fresh, gc_motor, wiiu_motor;

enum { SLOT_FACE1, SLOT_FACE2, SLOT_FACE3, SLOT_FACE4, SLOT_LB, SLOT_RB, SLOT_LT, SLOT_RT, SLOT_SELECT, SLOT_START,
       SLOT_PADU = 12, SLOT_PADD, SLOT_PADL, SLOT_PADR, SLOT_HOME };

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

/* A GameCube stick's gate stops it well short of 128 (one measured 88 right, 104 left), so its rim read as
 * 68-81% pushed, under AM2R's Analog Walking run line (~62-66%): Samus walked unless the stick was pressed hard
 * into the rim. */
#define GC_STICK_REACH 90.0f

static float gc_reach(float v)
{
	v *= 128.0f / GC_STICK_REACH;
	return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
}

static void apply_move(GamepadSlot *slot, float lx, float ly, bool up, bool down, bool left, bool right)
{
	up = up || ly < -STICK_DIGITAL;
	down = down || ly > STICK_DIGITAL;
	left = left || lx < -STICK_DIGITAL;
	right = right || lx > STICK_DIGITAL;
	slot->axisValue[0] = lx;
	slot->axisValue[1] = ly;
	set_btn(slot, SLOT_PADU, up);
	set_btn(slot, SLOT_PADD, down);
	set_btn(slot, SLOT_PADL, left);
	set_btn(slot, SLOT_PADR, right);
}

/* Every controller reaches the game only as its gamepad.
 * Face buttons go by position, bottom = face1, so AM2R's own remap screen names them right; the layout itself
 * lives in the game's bindings (padBindings). */
typedef struct {
	u32 mask;
	int slot;
} ButtonMap;

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
		return; /* a Remote alone is not a controller for this game */
	nunchuk_xy(&exp->nunchuk.js, &lx, &ly);
	/* One-button aim: the d-pad holds the aim button and says which diagonal. */
	apply_move(slot, lx, ly, !twoAim && (held & WPAD_BUTTON_UP), !twoAim && (held & WPAD_BUTTON_DOWN),
		   false, false);
	apply_map(slot, nunchukMap, held);
	if (held & WPAD_BUTTON_UP)
		set_btn(slot, SLOT_LB, true);
	if (held & WPAD_BUTTON_DOWN)
		set_btn(slot, twoAim ? SLOT_RT : SLOT_LB, true);
	if (pause)
		set_btn(slot, SLOT_START, true);
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
	apply_move(slot, lx, ly, (held & PAD_BUTTON_UP) || (!twoAim && cup),
		   (held & PAD_BUTTON_DOWN) || (!twoAim && cdown),
		   held & PAD_BUTTON_LEFT, held & PAD_BUTTON_RIGHT);
	slot->axisValue[0] = gc_reach(lx);
	slot->axisValue[1] = gc_reach(ly);
	apply_map(slot, gcMap, held);
	if (cup)
		set_btn(slot, SLOT_LB, true);
	if (cdown)
		set_btn(slot, twoAim ? SLOT_RT : SLOT_LB, true);
}

/* A tap moves one step; a hold repeats after a beat, every few frames. */
static bool repeat_step(bool held, int *frames, int first, int every)
{
	if (!held) {
		*frames = 0;
		return false;
	}
	++*frames;
	return *frames == 1 || (*frames > first && (*frames - first) % every == 0);
}

/* The Display menu's Resize/Move Screen and HUD Margins rows open these pages with the menus' OK (the bottom button:
 * B on a Classic Controller, A elsewhere, where B is Back). The d-pad (or a stick) sizes the picture or sets the
 * margins, A held moves the picture, X (Nunchuk C) recenters it, and B closes. True while the game must see neutral
 * input: the page is up, or the B that closed it is still down and would also leave the menu underneath. */
static bool overscan_input(u32 wheld, u16 gcheld, const s8 *sticks, const expansion_t *exp, OverscanPage row)
{
	static bool opening, closing, prev_b, prev_ok;
	static int rep[4];
	float sx = gc_axis(sticks[0]), sy = -gc_axis(sticks[1]), wx = 0.0f, wy = 0.0f;
	/* the Nunchuk's Z and C share their bits with the Classic's Up and Left */
	u32 classic = exp->type == WPAD_EXP_CLASSIC ? wheld : 0, nunchuk = exp->type == WPAD_EXP_NUNCHUK ? wheld : 0;
	bool up, down, left, right, a, b, ok, center, face, screen;
	static bool prev_center;
	int first, every, step;

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
	a = (classic & WPAD_CLASSIC_BUTTON_A) || (wheld & WPAD_BUTTON_A) || (gcheld & PAD_BUTTON_A);
	b = (classic & WPAD_CLASSIC_BUTTON_B) || (wheld & WPAD_BUTTON_B) || (gcheld & PAD_BUTTON_B);
	ok = pad_in_use == WII_PAD_CLASSIC ? b : a;
	center = (classic & WPAD_CLASSIC_BUTTON_X) || (gcheld & PAD_BUTTON_X) || (nunchuk & WPAD_NUNCHUK_BUTTON_C);
	face = a || b || (classic & (WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y)) ||
	       (wheld & (WPAD_BUTTON_1 | WPAD_BUTTON_2)) || (gcheld & (PAD_BUTTON_X | PAD_BUTTON_Y));

	if (!Overscan_active()) {
		closing = closing && b;
		if (!closing && row != OVERSCAN_OFF && ok && !prev_ok) {
			Overscan_open(row);
			opening = true; /* the button that opened the page is still down on its first frames */
		}
		prev_ok = ok;
		prev_b = b;
		prev_center = center;
		return closing || Overscan_active();
	}
	prev_ok = ok;
	if (opening) {
		opening = face;
		prev_b = b;
		return true;
	}
	if (b && !prev_b) {
		Overscan_close();
		closing = true;
		prev_b = b;
		return true;
	}
	prev_b = b;
	screen = Overscan_page() == OVERSCAN_SCREEN;
	if (screen && center && !prev_center)
		Overscan_recenter();
	prev_center = center;
	first = screen ? 10 : 15;
	every = screen ? 2 : 5;
	step = repeat_step(right, &rep[0], first, every) - repeat_step(left, &rep[1], first, every);
	if (step)
		(!screen ? Overscan_adjustMarginH : a ? Overscan_adjustX : Overscan_adjustH)(step);
	step = repeat_step(down, &rep[2], first, every) - repeat_step(up, &rep[3], first, every);
	if (step)
		(!screen ? Overscan_adjustMarginV : a ? Overscan_adjustY : Overscan_adjustV)(step);
	return true;
}

/* AM2R's gamepad bindings as it ships them, then the Wii layout. A config still holding the
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
	logInfo("controller defaults: names %s, bindings %s\n",
		v.type != RVALUE_UNDEFINED && RValue_toReal(v) == 0 ? "-> Nintendo" : "kept",
		stock ? "stock -> Wii layout, one-button aim, Hold missiles" : "remapped, kept");
	if (!stock)
		return;
	WiiInput_applyLayout(vm);
	set_global_num(vm, "opaimstyle", 0);
	set_global_num(vm, "opmslstyle", 0);
}

/* Sticky: an idle frame keeps the last controller, so a GameCube player's names do not flip back when the pad rests.
 * A plugged-in Classic wins; otherwise a stick pushed as far as a d-pad press counts like a button. */
static void note_pad(u32 wheld, u16 gcheld, const s8 *sticks, const expansion_t *exp)
{
	WiiPad was = pad_in_use;

	if (exp->type == WPAD_EXP_CLASSIC) {
		pad_in_use = WII_PAD_CLASSIC;
		classic_is_wiiu = exp->classic.type == CLASSIC_TYPE_WIIU;
	} else if (gcheld || fabsf(gc_axis(sticks[0])) > STICK_DIGITAL || fabsf(gc_axis(sticks[1])) > STICK_DIGITAL ||
		 fabsf(gc_axis(sticks[2])) > STICK_DIGITAL || PAD_TriggerL(0) > GC_TRIGGER_PRESS ||
		 PAD_TriggerR(0) > GC_TRIGGER_PRESS)
		pad_in_use = WII_PAD_GAMECUBE;
	else if (exp->type == WPAD_EXP_NUNCHUK &&
		 ((wheld & ~(u32)(WPAD_BUTTON_HOME)) || exp->nunchuk.js.mag > STICK_DIGITAL))
		pad_in_use = WII_PAD_NUNCHUK;
	if (pad_in_use != was)
		logInfo("pad in use: %s\n", pad_in_use == WII_PAD_CLASSIC ? "Classic" :
			pad_in_use == WII_PAD_GAMECUBE ? "GameCube" : "Nunchuk");
}

/* In play only the controller picked before it counts: a second pad's touch, or a resting GameCube stick's drift,
 * would fight it (Jaxi: switch at the title). The title, its options and the first screen pick up whichever is used. */
static bool pad_locked(Runner *runner)
{
	static int32_t title = -2, options, controller;
	static bool locked;
	bool was = locked;

	if (title == -2) {
		title = Wii_roomIndex(runner, "titleroom");
		options = Wii_roomIndex(runner, "rm_options");
		controller = Wii_roomIndex(runner, "rm_controller");
	}
	if (WiiSafe_ingame(runner))
		locked = true;
	else if (runner->currentRoomIndex == title || runner->currentRoomIndex == options ||
		 runner->currentRoomIndex == controller)
		locked = false;
	if (locked != was)
		logInfo("controller %s: %s\n", locked ? "locked for play" : "free again at the title",
			pad_in_use == WII_PAD_CLASSIC ? "Classic" :
			pad_in_use == WII_PAD_GAMECUBE ? "GameCube" : "Nunchuk");
	return locked;
}

WiiPad WiiInput_pad(void)
{
	return pad_in_use;
}

/* A Classic Controller would buzz the Remote it hangs from; a Wii U Pro Controller has its own motor. */
bool WiiInput_hasRumble(void)
{
	return pad_in_use == WII_PAD_GAMECUBE || (pad_in_use == WII_PAD_CLASSIC && classic_is_wiiu);
}

/* AM2R calls gamepad_set_vibration every step while its Vibration option is on, so a step without a call stops. */
void WiiInput_rumble(float left, float right)
{
	float level = left > right ? left : right;

	rumble_level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
	rumble_fresh = true;
}

static void set_motors(bool on)
{
	bool gc = on && pad_in_use == WII_PAD_GAMECUBE, wiiu = on && pad_in_use == WII_PAD_CLASSIC;

	if (gc != gc_motor) {
		PAD_ControlMotor(PAD_CHAN0, gc ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
		gc_motor = gc;
	}
	if (wiiu != wiiu_motor) {
		WPAD_Rumble(WPAD_CHAN_0, wiiu);
		wiiu_motor = wiiu;
	}
}

/* Both motors are on or off, so a level is a share of frames. */
static void drive_rumble(void)
{
	static float acc;
	static bool active;
	static int frames, frames_on;
	float level = rumble_fresh && WiiInput_hasRumble() ? rumble_level : 0.0f;
	bool on;

	rumble_fresh = false;
	if ((level > 0.0f) != active) {
		active = level > 0.0f;
		acc = 1.0f; /* a new rumble starts with a pulse */
		if (active)
			logInfo("rumble on (%.2f, %s)\n", level,
				pad_in_use == WII_PAD_GAMECUBE ? "GameCube" : "Wii U Pro");
		else
			logInfo("rumble off (motor on %d of %d frames)\n", frames_on, frames);
		frames = frames_on = 0;
	} else {
		acc += level;
	}
	on = active && acc >= 1.0f;
	if (on)
		acc -= 1.0f;
	frames += active;
	frames_on += on;
	set_motors(on);
}

void WiiInput_stopRumble(void)
{
	PAD_ControlMotor(PAD_CHAN0, PAD_MOTOR_STOP_HARD);
	WPAD_Rumble(WPAD_CHAN_0, 0);
	gc_motor = wiiu_motor = false;
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
	bool neutral, pause, classic, start;
	OverscanPage row;
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
	if (!pad_locked(runner))
		note_pad(wheld, gcheld, sticks, &exp);
	if (defaults_done && (pad_in_use == WII_PAD_NUNCHUK) != menus_swapped) {
		swap_menus(runner->vmContext);
		menus_swapped = !menus_swapped;
		logInfo("menu buttons: %s\n", menus_swapped ? "Nunchuk (A = OK)" : "real");
	}
	drive_rumble();
	row = Overscan_active() ? OVERSCAN_OFF : WiiControls_displayRow(runner);
	neutral = overscan_input(wheld, gcheld, sticks, &exp, row);
	/* Only the controller in use is read, its own pause button included. */
	classic = pad_in_use == WII_PAD_CLASSIC && exp.type == WPAD_EXP_CLASSIC;
	pause = pad_in_use == WII_PAD_NUNCHUK && (wheld & WPAD_BUTTON_1);
	start = classic ? wheld & WPAD_CLASSIC_BUTTON_PLUS :
			pad_in_use == WII_PAD_GAMECUBE && (gcheld & PAD_BUTTON_START);
	if (neutral)
		;
	else if (classic)
		poll_classic(slot, wheld & ~(u32)WPAD_CLASSIC_BUTTON_PLUS, &exp);
	else if (pad_in_use == WII_PAD_GAMECUBE)
		poll_gcpad(slot, gcheld & ~PAD_BUTTON_START, sticks, two_button_aim(runner));
	else if (pad_in_use == WII_PAD_NUNCHUK)
		poll_nunchuk(slot, wheld, &exp, two_button_aim(runner), pause);
	if (start && !neutral)
		set_btn(slot, SLOT_START, true);
	/* Left/Right on the two adjust rows would still flip the window settings the rows stand in for. */
	if (row != OVERSCAN_OFF) {
		slot->buttonDown[SLOT_PADL] = slot->buttonDown[SLOT_PADR] = false;
		slot->buttonValue[SLOT_PADL] = slot->buttonValue[SLOT_PADR] = 0.0f;
		slot->axisValue[0] = 0.0f;
	}

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
