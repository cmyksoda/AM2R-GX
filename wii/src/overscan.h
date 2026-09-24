#ifndef AM2R_WII_OVERSCAN_H
#define AM2R_WII_OVERSCAN_H

#include <stdbool.h>

/* Per-edge inset in physical framebuffer pixels; the game is squeezed into
 * (h, v)..(fbW-h, fbH-v) so CRT overscan doesn't eat the screen edges. */
#define OVERSCAN_MAX_H 160
#define OVERSCAN_MAX_V 120

void Overscan_load(void);

bool Overscan_active(void);
void Overscan_toggle(void);

int Overscan_h(void);
int Overscan_v(void);
int Overscan_y(void); /* vertical shift, + = down (overscan.txt's fourth number) */
int Overscan_viWidth(void);
void Overscan_adjustH(int delta);
void Overscan_adjustV(int delta);
void Overscan_adjustY(int delta);

#endif
