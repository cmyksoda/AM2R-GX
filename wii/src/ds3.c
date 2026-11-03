#include "ds3.h"
#include "hooks.h"
#include "log.h"

#include <gccore.h>
#include <ogc/ipc.h>
#include <ogc/usb.h>
#include <string.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#define SONY_VID 0x054c
#define DS3_PID 0x0268
#define DS4_PID 0x05c4
#define DS4V2_PID 0x09cc
#define DS4_IN_ENDPOINT 0x84
#define DS4_OUT_ENDPOINT 0x03
#define DS4_REPORT_BYTES 64
#define DS4_OUT_BYTES 32
#define REPORT_BYTES 49
#define WAKE_BYTES 17
#define LED_BYTES 35
#define SCAN_FRAMES 120
#define STUCK_FRAMES 120
#define MAX_DEVICES 8
/* libogc's heap charges every block a 32-byte page and an 8-byte header on top of its size, and keeps 8 bytes */
#define IPC_BLOCK(n) ((((n) + 31) & ~31u) + 32 + 8)

/* IOS reads and writes these directly: 32-byte aligned, and in MEM2 (a Wii U's IOS refuses MEM1 buffers). */
typedef struct {
	u8 report[64];
	u8 wake[32];
	u8 leds[64];
} Ds3Buffers;

/* DS3 output report 0x01 as libsicksaxis and embedded-game-controller send it over USB on a Wii: rumble in bytes 1-4
 * (right duration, right on/off, left duration, left force), LED 1 lit in byte 9, then the four LED timings. */
static const u8 led_report[LED_BYTES] = {
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
	0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
	0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
	0x00, 0x00, 0x00, 0x00, 0x00,
};

static s32 heap = -1;
static Ds3Buffers *buf;
static s32 fd = -1;
static volatile bool busy;
static volatile s32 result;
static volatile u32 generation; /* a late answer to a request from before close_pad() is dropped */
static int scan_wait;
static int waited;
static bool ready, shown;
static Ds3Pad last;
static bool ds4; /* the pad open is a DualShock 4: it streams reports on its own, no wake-up */
static u8 rumble_want, rumble_sent; /* big motor force; the small one runs at full when it is past half */
static volatile bool sending; /* the transfer in flight is a rumble report, not a read */
static const char *pad_name = "DS3";

static s32 on_report(s32 res, void *data)
{
	if ((u32)data != generation)
		return 0;
	result = res;
	busy = false;
	return 0;
}

static bool buffers(void)
{
	if (buf)
		return true;
	if (heap < 0)
		heap = iosCreateHeap((IPC_BLOCK(sizeof(Ds3Buffers)) + 8 + 31) & ~31u);
	if (heap >= 0)
		buf = iosAlloc(heap, sizeof(Ds3Buffers));
	return buf != NULL;
}

/* Rumble and LEDs share the output report; one transfer at a time goes to the pad, so a change waits for the read. */
static s32 send_rumble(bool async)
{
	u8 level = rumble_want, small = level > 0x80 ? 0xff : 0;

	if (ds4) {
		memset(buf->leds, 0, DS4_OUT_BYTES);
		buf->leds[0] = 0x05;
		buf->leds[1] = 0x03; /* rumble and light bar */
		buf->leds[4] = small;
		buf->leds[5] = level;
		buf->leds[8] = 0x40; /* light bar blue, as for player 1 */
		return async ? USB_WriteIntrMsgAsync(fd, DS4_OUT_ENDPOINT, DS4_OUT_BYTES, buf->leds, on_report,
						     (void *)generation) :
			       USB_WriteIntrMsg(fd, DS4_OUT_ENDPOINT, DS4_OUT_BYTES, buf->leds);
	}
	memcpy(buf->leds, led_report, LED_BYTES);
	buf->leds[1] = 0xff;
	buf->leds[2] = small ? 1 : 0;
	buf->leds[3] = 0xff;
	buf->leds[4] = level;
	return async ? USB_WriteCtrlMsgAsync(fd, USB_REQTYPE_INTERFACE_SET, USB_REQ_SETREPORT,
					     (USB_REPTYPE_OUTPUT << 8) | 0x01, 0, LED_BYTES, buf->leds, on_report,
					     (void *)generation) :
		       USB_WriteCtrlMsg(fd, USB_REQTYPE_INTERFACE_SET, USB_REQ_SETREPORT, (USB_REPTYPE_OUTPUT << 8) | 0x01,
					0, LED_BYTES, buf->leds);
}

/* A DS3 stays silent on USB until its 0xF2 feature report is read. */
static bool open_pad(void)
{
	usb_device_entry list[MAX_DEVICES];
	u8 count = 0;
	s32 r;
	int i;

	if (USB_GetDeviceList(list, MAX_DEVICES, USB_CLASS_HID, &count) < 0)
		return false;
	for (i = 0; i < count; i++) {
		if (list[i].vid != SONY_VID ||
		    (list[i].pid != DS3_PID && list[i].pid != DS4_PID && list[i].pid != DS4V2_PID))
			continue;
		if (USB_OpenDevice(list[i].device_id, SONY_VID, list[i].pid, &fd) < 0) {
			fd = -1;
			return false;
		}
		ds4 = list[i].pid != DS3_PID;
		pad_name = ds4 ? "DS4" : "DS3";
		rumble_sent = rumble_want = 0;
		if (ds4) {
			r = send_rumble(false);
			logInfo("DS4: connected (device %08x, %04x, light bar %d)\n", (unsigned)list[i].device_id,
				list[i].pid, (int)r);
			return true;
		}
		r = USB_WriteCtrlMsg(fd, USB_REQTYPE_INTERFACE_GET, USB_REQ_GETREPORT, (USB_REPTYPE_FEATURE << 8) | 0xf2, 0,
				     WAKE_BYTES, buf->wake);
		if (r < 0) {
			logInfo("DS3: wake-up read failed (%d)\n", (int)r);
			USB_CloseDevice(&fd);
			fd = -1;
			return false;
		}
		r = send_rumble(false);
		logInfo("DS3: connected (device %08x, LED report %d)\n", (unsigned)list[i].device_id, (int)r);
		return true;
	}
	return false;
}

static void close_pad(void)
{
	int i;

	for (i = 0; i < 100 && busy; i++)
		usleep(1000);
	generation++;
	busy = false;
	sending = false;
	if (fd >= 0 && rumble_sent) {
		rumble_want = 0;
		send_rumble(false);
	}
	rumble_sent = 0;
	if (fd >= 0)
		USB_CloseDevice(&fd);
	fd = -1;
	ready = false;
	waited = 0;
	memset(&last, 0, sizeof last);
}

static float axis(u8 raw)
{
	float v = ((float)raw - 128.0f) / 127.0f;

	return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
}

/* DS4 report 0x01: bytes 1-2 left stick (y down); byte 5 = d-pad hat (0 up, clockwise, 8 none) under Square, Cross,
 * Circle, Triangle (bits 4-7); byte 6 bits from 0 = L1 R1 L2 R2 Share Options L3 R3; byte 7 bit 0 = PS. */
static void parse_ds4(const u8 *r)
{
	static const u32 hat[8] = {
		WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_BUTTON_RIGHT, WPAD_CLASSIC_BUTTON_RIGHT,
		WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_RIGHT, WPAD_CLASSIC_BUTTON_DOWN,
		WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_LEFT, WPAD_CLASSIC_BUTTON_LEFT,
		WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_BUTTON_LEFT,
	};
	static const u32 faces[4] = {
		WPAD_CLASSIC_BUTTON_Y, WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_X,
	};
	static const u32 b6[8] = {
		WPAD_CLASSIC_BUTTON_FULL_L, WPAD_CLASSIC_BUTTON_FULL_R, WPAD_CLASSIC_BUTTON_ZL, WPAD_CLASSIC_BUTTON_ZR,
		WPAD_CLASSIC_BUTTON_MINUS, WPAD_CLASSIC_BUTTON_PLUS, 0, 0,
	};
	u32 held = (r[5] & 0x0f) < 8 ? hat[r[5] & 0x0f] : 0;
	int i;

	for (i = 0; i < 4; i++)
		if (r[5] & (0x10u << i))
			held |= faces[i];
	for (i = 0; i < 8; i++)
		if (r[6] & (1u << i))
			held |= b6[i];
	if (r[7] & 1)
		held |= WPAD_CLASSIC_BUTTON_HOME;
	last.classic = held;
	last.x = axis(r[1]);
	last.y = -axis(r[2]);
}

/* Report 0x01, bits from 0: byte 2 = Select L3 R3 Start Up Right Down Left, byte 3 = L2 R2 L1 R1 Triangle Circle
 * Cross Square, byte 4 bit 0 = PS, bytes 6-7 = left stick (128 centered, y down). Faces map by position on the pad. */
static void parse(const u8 *r)
{
	static const u32 b2[8] = {
		WPAD_CLASSIC_BUTTON_MINUS, 0, 0, WPAD_CLASSIC_BUTTON_PLUS,
		WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_RIGHT, WPAD_CLASSIC_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_LEFT,
	};
	static const u32 b3[8] = {
		WPAD_CLASSIC_BUTTON_ZL, WPAD_CLASSIC_BUTTON_ZR, WPAD_CLASSIC_BUTTON_FULL_L, WPAD_CLASSIC_BUTTON_FULL_R,
		WPAD_CLASSIC_BUTTON_X, WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_Y,
	};
	u32 held = 0;
	int i;

	/* an all-0xFF DS3 report would read as PS held, and a held PS quits; a DS4's byte 1 is its stick */
	if (r[0] != 0x01 || (!ds4 && r[1] == 0xff))
		return;
	if (!shown) {
		shown = true;
		logInfo("%s: first report %02x %02x %02x %02x %02x %02x %02x %02x\n", pad_name, r[0], r[1], r[2], r[3], r[4],
			r[5], r[6], r[7]);
	}
	if (ds4) {
		parse_ds4(r);
		return;
	}
	for (i = 0; i < 8; i++) {
		if (r[2] & (1u << i))
			held |= b2[i];
		if (r[3] & (1u << i))
			held |= b3[i];
	}
	if (r[4] & 1)
		held |= WPAD_CLASSIC_BUTTON_HOME;
	last.classic = held;
	last.x = axis(r[6]);
	last.y = -axis(r[7]);
}

void Ds3_init(void)
{
	USB_Initialize();
	scan_wait = 0;
}

bool Ds3_poll(Ds3Pad *out)
{
	s32 r;

	if (Wii_hooksDs3(out))
		return true;
	if (fd < 0) {
		if (scan_wait-- > 0)
			return false;
		scan_wait = SCAN_FRAMES;
		if (!buffers() || !open_pad())
			return false;
	}
	if (busy && ++waited > STUCK_FRAMES) {
		logInfo("%s: no answer for %d frames, closed\n", pad_name, STUCK_FRAMES);
		close_pad();
		return false;
	}
	if (!busy) {
		waited = 0;
		if (ready && result < 0) {
			logInfo("%s: unplugged (%d)\n", pad_name, (int)result);
			close_pad();
			return false;
		}
		if (ready && !sending)
			parse(buf->report);
		ready = !sending;
		sending = false;
		if (rumble_want != rumble_sent) {
			rumble_sent = rumble_want;
			sending = busy = true;
			if (send_rumble(true) < 0) {
				sending = busy = false;
				logInfo("%s: rumble report failed\n", pad_name);
			}
			*out = last;
			return true;
		}
		busy = true;
		r = ds4 ? USB_ReadIntrMsgAsync(fd, DS4_IN_ENDPOINT, DS4_REPORT_BYTES, buf->report, on_report,
					       (void *)generation) :
			  USB_WriteCtrlMsgAsync(fd, USB_REQTYPE_INTERFACE_GET, USB_REQ_GETREPORT,
						(USB_REPTYPE_INPUT << 8) | 0x01, 0, REPORT_BYTES, buf->report, on_report,
						(void *)generation);
		if (r < 0) {
			busy = false;
			logInfo("%s: report request failed (%d)\n", pad_name, (int)r);
			close_pad();
			return false;
		}
		ready = true;
	}
	*out = last;
	return true;
}

void Ds3_shutdown(void)
{
	close_pad();
}

void Ds3_rumble(float level)
{
	rumble_want = level <= 0.0f ? 0 : level >= 1.0f ? 0xff : (u8)(level * 255.0f);
}
