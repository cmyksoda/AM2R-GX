#include "input.h"

#include "input_recording.h"
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

	if (!js || js->mag < 0.2f) {
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
	if (v > -0.2f && v < 0.2f)
		return 0.0f;
	return v;
}

static void apply_move(GamepadSlot *slot, float lx, float ly, bool up, bool down, bool left, bool right)
{
	slot->axisValue[0] = lx;
	slot->axisValue[1] = ly;
	set_btn(slot, 12, up || ly < -0.4f);
	set_btn(slot, 13, down || ly > 0.4f);
	set_btn(slot, 14, left || lx < -0.4f);
	set_btn(slot, 15, right || lx > 0.4f);
	if (up || ly < -0.4f)
		want(VK_UP);
	if (down || ly > 0.4f)
		want(VK_DOWN);
	if (left || lx < -0.4f)
		want(VK_LEFT);
	if (right || lx > 0.4f)
		want(VK_RIGHT);
}

static void poll_classic(GamepadSlot *slot, u32 held, const expansion_t *exp)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_CLASSIC)
		joy_xy(&exp->classic.ljs, &lx, &ly);
	apply_move(slot, lx, ly, held & WPAD_CLASSIC_BUTTON_UP, held & WPAD_CLASSIC_BUTTON_DOWN,
		   held & WPAD_CLASSIC_BUTTON_LEFT, held & WPAD_CLASSIC_BUTTON_RIGHT);
	if (held & WPAD_CLASSIC_BUTTON_B)
		want(90);
	if (held & WPAD_CLASSIC_BUTTON_Y)
		want(88);
	if (held & WPAD_CLASSIC_BUTTON_A)
		want(VK_CONTROL);
	if (held & WPAD_CLASSIC_BUTTON_X)
		want(83);
	if (held & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR))
		want(VK_SHIFT);
	if (held & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL))
		want(65);
	if (held & WPAD_CLASSIC_BUTTON_PLUS)
		want(VK_ENTER);
	if (held & WPAD_CLASSIC_BUTTON_MINUS)
		want(VK_ESCAPE);
	set_btn(slot, 0, held & WPAD_CLASSIC_BUTTON_B);
	set_btn(slot, 1, held & WPAD_CLASSIC_BUTTON_A);
	set_btn(slot, 2, held & WPAD_CLASSIC_BUTTON_Y);
	set_btn(slot, 3, held & WPAD_CLASSIC_BUTTON_X);
	set_btn(slot, 4, held & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL));
	set_btn(slot, 5, held & (WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR));
	set_btn(slot, 8, held & WPAD_CLASSIC_BUTTON_MINUS);
	set_btn(slot, 9, held & WPAD_CLASSIC_BUTTON_PLUS);
	set_btn(slot, 16, held & WPAD_CLASSIC_BUTTON_HOME);
}

static void poll_nunchuk(GamepadSlot *slot, u32 held, const expansion_t *exp)
{
	float lx = 0.0f, ly = 0.0f;

	if (exp->type == WPAD_EXP_NUNCHUK)
		joy_xy(&exp->nunchuk.js, &lx, &ly);
	apply_move(slot, lx, ly, held & WPAD_BUTTON_UP, held & WPAD_BUTTON_DOWN,
		   held & WPAD_BUTTON_LEFT, held & WPAD_BUTTON_RIGHT);
	if (held & WPAD_BUTTON_A)
		want(90);
	if (held & WPAD_BUTTON_B)
		want(88);
	if (held & WPAD_NUNCHUK_BUTTON_C)
		want(VK_CONTROL);
	if (held & WPAD_BUTTON_1)
		want(VK_SHIFT);
	if (held & WPAD_NUNCHUK_BUTTON_Z)
		want(65);
	if (held & WPAD_BUTTON_2)
		want(83);
	if (held & WPAD_BUTTON_PLUS)
		want(VK_ENTER);
	if (held & WPAD_BUTTON_MINUS)
		want(VK_ESCAPE);
	set_btn(slot, 0, held & WPAD_BUTTON_A);
	set_btn(slot, 1, held & WPAD_BUTTON_B);
	set_btn(slot, 9, held & WPAD_BUTTON_PLUS);
	set_btn(slot, 16, held & WPAD_BUTTON_HOME);
}

static void poll_gcpad(GamepadSlot *slot, u16 held)
{
	float lx = gc_axis(PAD_StickX(0));
	float ly = -gc_axis(PAD_StickY(0));

	apply_move(slot, lx, ly, held & PAD_BUTTON_UP, held & PAD_BUTTON_DOWN,
		   held & PAD_BUTTON_LEFT, held & PAD_BUTTON_RIGHT);
	if (held & PAD_BUTTON_A)
		want(90);
	if (held & PAD_BUTTON_B)
		want(88);
	if (held & PAD_BUTTON_Y)
		want(VK_CONTROL);
	if (held & PAD_TRIGGER_R)
		want(VK_SHIFT);
	if (held & PAD_TRIGGER_L)
		want(65);
	if (held & PAD_BUTTON_X)
		want(83);
	if (held & PAD_BUTTON_START)
		want(VK_ENTER);
	if (held & PAD_TRIGGER_Z)
		want(VK_ESCAPE);
	set_btn(slot, 0, held & PAD_BUTTON_A);
	set_btn(slot, 1, held & PAD_BUTTON_B);
	set_btn(slot, 2, held & PAD_BUTTON_X);
	set_btn(slot, 3, held & PAD_BUTTON_Y);
	set_btn(slot, 4, held & PAD_TRIGGER_L);
	set_btn(slot, 5, held & PAD_TRIGGER_R);
	set_btn(slot, 8, held & PAD_TRIGGER_Z);
	set_btn(slot, 9, held & PAD_BUTTON_START);
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

	if (exp.type == WPAD_EXP_CLASSIC)
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
		if (i != 6 && i != 7 && slot->buttonDown[i])
			slot->buttonValue[i] = 1.0f;
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
