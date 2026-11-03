#ifndef DS3_H
#define DS3_H

#include <gctypes.h>
#include <stdbool.h>

/* A DualShock 3 or 4 on a USB cable, read as a Classic Controller. Experimental: not yet run on hardware. */
typedef struct Ds3Pad {
	u32 classic; /* WPAD_CLASSIC_BUTTON_* bits */
	float x, y;  /* left stick, -1..1, y up */
} Ds3Pad;

void Ds3_init(void);
bool Ds3_poll(Ds3Pad *out); /* once per frame; false while none is plugged in */
void Ds3_rumble(float level); /* 0..1; reaches the pad between reads */
void Ds3_shutdown(void);

#endif
