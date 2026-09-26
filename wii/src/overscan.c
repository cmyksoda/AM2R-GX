#include "overscan.h"

#include "wii_paths.h"

#include "log.h"

#include <ogc/es.h>
#include <stdio.h>

static int g_h;
static int g_v;
static int g_y; /* vertical shift of the picture, + = down */
static int g_x; /* horizontal shift, + = right */
static int g_vi; /* VI active width, 0 = the mode's own */
static bool g_active;
static char g_path[96]; /* this console's own file; the shared overscan.txt is only a fallback */

/* One card can move between consoles (Jaxi's Wii and Wii U want different insets), so each
 * console keeps its own file, named by its ES device id. */
static const char *own_path(void)
{
	u32 id = 0;

	if (!g_path[0]) {
		if (ES_GetDeviceID(&id) == 0 && id)
			snprintf(g_path, sizeof g_path, "%s/overscan-%08x.txt", WII_APP_DIR, (unsigned)id);
		else
			snprintf(g_path, sizeof g_path, "%s/overscan.txt", WII_APP_DIR);
	}
	return g_path;
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void Overscan_load(void)
{
	FILE *f = fopen(own_path(), "r");
	char shared[96];
	int h = 0, v = 0, vi = 0, y = 0, x = 0, n = 0;

	snprintf(shared, sizeof shared, "%s/overscan.txt", WII_APP_DIR);
	if (!f)
		f = fopen(shared, "r");
	if (f) {
		n = fscanf(f, "%d %d %d %d %d", &h, &v, &vi, &y, &x);
		if (n >= 2) {
			g_h = clampi(h, 0, OVERSCAN_MAX_H);
			g_v = clampi(v, 0, OVERSCAN_MAX_V);
		}
		if (n >= 3 && vi >= 640)
			g_vi = clampi(vi, 640, 720);
		if (n >= 4)
			g_y = clampi(y, -OVERSCAN_MAX_V, OVERSCAN_MAX_V);
		if (n >= 5)
			g_x = clampi(x, -OVERSCAN_MAX_H, OVERSCAN_MAX_H);
		fclose(f);
	}
	logInfo("Overscan: h=%d v=%d vi=%d y=%d x=%d (%s)\n", g_h, g_v, g_vi, g_y, g_x, own_path());
}

static void Overscan_save(void)
{
	FILE *f = fopen(own_path(), "w");

	if (f) {
		if (g_x)
			fprintf(f, "%d %d %d %d %d\n", g_h, g_v, g_vi, g_y, g_x);
		else if (g_y)
			fprintf(f, "%d %d %d %d\n", g_h, g_v, g_vi, g_y);
		else if (g_vi)
			fprintf(f, "%d %d %d\n", g_h, g_v, g_vi);
		else
			fprintf(f, "%d %d\n", g_h, g_v);
		fclose(f);
		logInfo("Overscan saved: h=%d v=%d vi=%d y=%d x=%d\n", g_h, g_v, g_vi, g_y, g_x);
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

int Overscan_y(void)
{
	return g_y;
}

void Overscan_adjustY(int delta)
{
	g_y = clampi(g_y + delta, -OVERSCAN_MAX_V, OVERSCAN_MAX_V);
}

int Overscan_x(void)
{
	return g_x;
}

void Overscan_adjustX(int delta)
{
	g_x = clampi(g_x + delta, -OVERSCAN_MAX_H, OVERSCAN_MAX_H);
}

int Overscan_viWidth(void)
{
	return g_vi;
}
