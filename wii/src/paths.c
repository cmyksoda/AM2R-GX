#include "wii_paths.h"

#include "log.h"

#include <dvm.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 32 pages of 64 sectors = 1 MB of MEM1 per mounted drive: 32 KB reads stay as fast as a bigger cache, and the
 * MEM1 left over holds textures. */
#define CACHE_PAGES 32
#define CACHE_SECTORS 64
#define USB_WAIT_TRIES 20 /* x 250 ms */

char wiiAppDir[64] = "sd:/apps/am2r";
char wiiGameDir[80] = "sd:/apps/am2r/game";
char wiiDataWin[96] = "sd:/apps/am2r/game/data.win";
char wiiDrive[16] = "sd:";

static bool is_dir(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* "sd:/apps/am2r/boot.dol" -> "sd:/apps/am2r", when argv[0] starts with drive (the Homebrew Channel's form). */
static bool launch_dir(const char *launch, const char *drive, char *dir, size_t cap)
{
	const char *slash = strrchr(launch, '/');
	size_t n = strlen(drive);

	if (strncmp(launch, drive, n) != 0 || launch[n] != '/' || !slash || (size_t)(slash - launch) >= cap)
		return false;
	snprintf(dir, cap, "%.*s", (int)(slash - launch), launch);
	return true;
}

static void use_dir(const char *dir, const char *drive)
{
	snprintf(wiiAppDir, sizeof wiiAppDir, "%s", dir);
	snprintf(wiiDrive, sizeof wiiDrive, "%s", drive);
}

/* wiiload and Dolphin pass something else as argv[0], or nothing: those run from the SD card. */
bool WiiPaths_init(int argc, char **argv)
{
	const char *launch = argc > 0 && argv && argv[0] ? argv[0] : "";
	char dir[sizeof wiiAppDir];
	bool ready = false;
	int tries;

	if (launch_dir(launch, "usb:", dir, sizeof dir)) {
		/* A drive can still be waking up after the Homebrew Channel hands over. */
		for (tries = 0; tries < USB_WAIT_TRIES &&
				!dvmProbeMountDiscIface("usb", &__io_usbstorage, CACHE_PAGES, CACHE_SECTORS); tries++)
			usleep(250 * 1000);
		if (tries < USB_WAIT_TRIES && is_dir(dir)) {
			use_dir(dir, "usb:");
			ready = true;
		} else {
			logWarn("launched from %s, which is not there: trying the SD card\n", dir);
		}
	}
	if (!ready) {
		if (!dvmProbeMountDiscIface("sd", &__io_wiisd, CACHE_PAGES, CACHE_SECTORS))
			return false;
		if (launch_dir(launch, "sd:", dir, sizeof dir) && is_dir(dir))
			use_dir(dir, "sd:");
	}
	snprintf(wiiGameDir, sizeof wiiGameDir, "%s/game", wiiAppDir);
	snprintf(wiiDataWin, sizeof wiiDataWin, "%s/data.win", wiiGameDir);
	return true;
}

const char *WiiPaths_media(void)
{
	return strcmp(wiiDrive, "usb:") == 0 ? "the USB drive" : "the SD card";
}
