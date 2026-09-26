#include "wii_paths.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

char wiiAppDir[64] = "sd:/apps/am2r";
char wiiGameDir[80] = "sd:/apps/am2r/game";
char wiiDataWin[96] = "sd:/apps/am2r/game/data.win";
char wiiDrive[16] = "sd:";

/* The Homebrew Channel passes "sd:/apps/am2r/boot.dol" or "usb:/apps/am2r/boot.dol"; wiiload and Dolphin
 * pass something else or nothing, and those stay on the SD card. */
void WiiPaths_init(int argc, char **argv)
{
	const char *launch = argc > 0 && argv && argv[0] ? argv[0] : "";
	const char *slash = strrchr(launch, '/'), *colon = strstr(launch, ":/");
	char dir[sizeof wiiAppDir];
	struct stat st;

	if (colon && slash && slash > colon && (size_t)(slash - launch) < sizeof dir &&
	    (size_t)(colon - launch) < sizeof wiiDrive - 1) {
		snprintf(dir, sizeof dir, "%.*s", (int)(slash - launch), launch);
		if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(wiiAppDir, sizeof wiiAppDir, "%s", dir);
			snprintf(wiiDrive, sizeof wiiDrive, "%.*s", (int)(colon - launch) + 1, launch);
		}
	}
	snprintf(wiiGameDir, sizeof wiiGameDir, "%s/game", wiiAppDir);
	snprintf(wiiDataWin, sizeof wiiDataWin, "%s/data.win", wiiGameDir);
}

const char *WiiPaths_media(void)
{
	return strcmp(wiiDrive, "usb:") == 0 ? "the USB drive" : "the SD card";
}
