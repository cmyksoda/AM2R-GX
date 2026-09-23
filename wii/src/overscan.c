#include "overscan.h"

#include "wii_paths.h"

#include "log.h"

#include <stdio.h>

#define OVERSCAN_PATH WII_APP_DIR "/overscan.txt"

static int g_h;
static int g_v;
static int g_vi; /* VI active width, 0 = the mode's own */
static bool g_active;

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void Overscan_load(void)
{
	FILE *f = fopen(OVERSCAN_PATH, "r");
	int h = 0, v = 0, vi = 0, n = 0;

	if (f) {
		n = fscanf(f, "%d %d %d", &h, &v, &vi);
		if (n >= 2) {
			g_h = clampi(h, 0, OVERSCAN_MAX_H);
			g_v = clampi(v, 0, OVERSCAN_MAX_V);
		}
		if (n == 3)
			g_vi = clampi(vi, 640, 720);
		fclose(f);
	}
	logInfo("Overscan: h=%d v=%d vi=%d\n", g_h, g_v, g_vi);
}

static void Overscan_save(void)
{
	FILE *f = fopen(OVERSCAN_PATH, "w");

	if (f) {
		if (g_vi)
			fprintf(f, "%d %d %d\n", g_h, g_v, g_vi);
		else
			fprintf(f, "%d %d\n", g_h, g_v);
		fclose(f);
	}
}

bool Overscan_active(void)
{
	return g_active;
}

void Overscan_toggle(void)
{
	g_active = !g_active;
	if (!g_active)
		Overscan_save();
}

int Overscan_h(void)
{
	return g_h;
}

int Overscan_v(void)
{
	return g_v;
}

void Overscan_adjustH(int delta)
{
	g_h = clampi(g_h + delta, 0, OVERSCAN_MAX_H);
}

void Overscan_adjustV(int delta)
{
	g_v = clampi(g_v + delta, 0, OVERSCAN_MAX_V);
}

int Overscan_viWidth(void)
{
	return g_vi;
}
