#ifndef AM2R_WII_OVERSCAN_H
#define AM2R_WII_OVERSCAN_H

#include <stdbool.h>

/* Per-edge inset in physical framebuffer pixels; the game is squeezed into
 * (h, v)..(fbW-h, fbH-v) so CRT overscan doesn't eat the screen edges. */
#define OVERSCAN_MAX_H 160
#define OVERSCAN_MAX_V 120

/* HUD margins in game pixels: what the HUD and prompts keep clear of each edge, the world staying put.
 * The vertical default is the 240p Test Suite's green lines (22 and 457 of 480). */
#define MARGIN_MAX_H 24
#define MARGIN_MAX_V 20
#define MARGIN_DEFAULT_V 11

typedef enum { OVERSCAN_OFF, OVERSCAN_SCREEN, OVERSCAN_HUD } OverscanPage;

void Overscan_load(void);

bool Overscan_active(void);
OverscanPage Overscan_page(void);
void Overscan_open(OverscanPage page);
void Overscan_close(void);

int Overscan_h(void);
int Overscan_v(void);
int Overscan_y(void); /* vertical shift, + = down (overscan.txt's fourth number) */
int Overscan_x(void); /* horizontal shift, + = right (the fifth) */
int Overscan_viWidth(void);
void Overscan_adjustH(int delta);
void Overscan_adjustV(int delta);
void Overscan_adjustY(int delta);
void Overscan_adjustX(int delta);
void Overscan_recenter(void);
int Overscan_marginH(void);
int Overscan_marginV(void);
void Overscan_adjustMarginH(int delta);
void Overscan_adjustMarginV(int delta);

#endif
