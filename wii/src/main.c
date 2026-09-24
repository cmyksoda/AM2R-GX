#include "gx_renderer.h"
#include "input.h"
#include "log.h"
#include "loop.h"
#include "netlog.h"
#include "overscan.h"
#include "patcher.h"
#include "wii_log.h"
#include "wii_mem.h"
#include "wii_paths.h"
#include "wii_video.h"

#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_DIR WII_APP_DIR "/save"

static void wait_home(const char *why)
{
	logError("%s\nHold HOME to quit.\n", why);
	WiiVideo_panic("startup failed", why);
	while (SYS_MainLoop()) {
		if (WiiInput_shouldQuit())
			break;
		VIDEO_WaitVSync();
	}
}

static void log_mallinfo(const char *when)
{
	struct mallinfo mi = mallinfo();

	logInfo("mallinfo %s: arena=%d ordblks=%d uordblks=%d fordblks=%d keepcost=%d\n",
	        when, mi.arena, mi.ordblks, mi.uordblks, mi.fordblks, mi.keepcost);
	/* Once sbrk crosses into MEM2, mallinfo counts the ~240 MB address hole as in-use.
	 * The arenas are the trustworthy figure. */
	logInfo("arena %s: mem1_free=%u mem2_free=%u\n",
	        when, (unsigned)SYS_GetArena1Size(), (unsigned)SYS_GetArena2Size());
}

/* Dolphin hands out zeroed RAM; a real Wii doesn't. Dirtying the unused arenas makes
 * uninitialised reads misbehave in the emulator the way they would on hardware. */
static void poison_arenas(void)
{
	memset(SYS_GetArena1Lo(), 0xA5, SYS_GetArena1Size());
	memset(SYS_GetArena2Lo(), 0xA5, SYS_GetArena2Size());
	logInfo("POISON sentinel found -- unused arenas filled with 0xA5\n");
}

/* wiiload can hand the DOL "NAME=text" arguments: each becomes game/NAME for this run only (a marker such as
 * MENU=landing or "warp.txt=241 16 1856 1200"), so the host needs no FTP transfer to arm a hands-free run.
 * HBC passes the first argument as argv[0] (no program name in front); an SD launch has the DOL path there. */
static char argMarkers[8][32];
static int argMarkerCount;

static void markers_from_args(int argc, char **argv)
{
	int i;

	for (i = 0; i < argc && argMarkerCount < 8; i++) {
		const char *eq = strchr(argv[i], '=');
		char path[96];
		FILE *f;

		if (!strncmp(argv[i], "LOGTO=", 6)) {
			NetLog_setTarget(argv[i] + 6);
			continue;
		}
		if (!eq || eq == argv[i] || eq - argv[i] >= (int)sizeof argMarkers[0] || strchr(argv[i], '/') || strstr(argv[i], ".."))
			continue;
		memcpy(argMarkers[argMarkerCount], argv[i], eq - argv[i]);
		argMarkers[argMarkerCount][eq - argv[i]] = 0;
		snprintf(path, sizeof path, WII_GAME_DIR "/%s", argMarkers[argMarkerCount]);
		f = fopen(path, "w");
		if (!f)
			continue;
		fprintf(f, "%s\n", eq + 1);
		fclose(f);
		logInfo("marker from args: %s = %s\n", argMarkers[argMarkerCount], eq + 1);
		argMarkerCount++;
	}
}

static void markers_from_args_remove(void)
{
	int i;
	char path[96];

	for (i = 0; i < argMarkerCount; i++) {
		snprintf(path, sizeof path, WII_GAME_DIR "/%s", argMarkers[i]);
		unlink(path);
	}
}

int main(int argc, char **argv)
{
	CommandLineArgs args;
	bool playback;
	int rc;

	WiiMem_init();
	WiiInput_init();
	if (!WiiVideo_init()) {
		wait_home("WiiVideo_init failed");
		return 1;
	}

	/* 256 pages (1 MB) instead of libfat's 16 KB default: the game stats a file every frame, and
	 * with the directory sectors evicted each stat cost 17 ms on a real card (vWii, session 9). */
	if (!fatInit(256, true)) {
		wait_home("fatInitDefault failed (no SD?)");
		return 1;
	}

	mkdir(WII_APP_DIR, 0777);
	mkdir(WII_GAME_DIR, 0777);
	mkdir(SAVE_DIR, 0777);

	if (chdir(WII_APP_DIR) != 0)
		logWarn("chdir(%s) failed — paths stay absolute\n", WII_APP_DIR);

	if (!WiiLog_open(WII_LOG_PATH))
		logWarn("could not open %s — console only\n", WII_LOG_PATH);
	markers_from_args(argc, argv);
	NetLog_init();

	Overscan_load();
	WiiVideo_setActiveWidth(Overscan_viWidth());

	logInfo("AM2R GX\n");
	logInfo("mode %ux%u xfb=%u aspect=%s\n", WiiVideo_fbWidth(), WiiVideo_efbHeight(),
		WiiVideo_rmode() ? WiiVideo_rmode()->xfbHeight : 0,
		WiiVideo_isWide() ? "16:9 stretch" : "4:3 fill");
	log_mallinfo("boot");

	playback = access(WII_GAME_DIR "/PLAYBACK", R_OK) == 0;
	if (playback)
		WiiLog_enableGecko(); /* Dolphin: the patcher's progress is worth watching live too */
	WiiPatcher_run();
	if (access(WII_DATA_WIN, R_OK) != 0) {
		wait_home(WiiPatcher_error() ? WiiPatcher_error()
		                             : "no game data: put your AM2R_11.zip in " WII_APP_DIR "/ (with the patch/ folder) and boot again");
		WiiLog_close();
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.exitAtFrame = -1; // run until the player holds HOME
	args.speedMultiplier = 1.0;
	args.osType = OS_WINDOWS;
	args.windowWidth = WII_FB_W;
	args.windowHeight = WII_FB_H;
	args.profilerFramesBetween = 200; // periodic ms/frame; enough for perf without per-frame SD spam
	args.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
	args.lazyRooms = true;
	args.lazyTextures = true;
	args.lazyAudio = true;
	args.renderer = NOOP;
	args.dataWinPath = WII_DATA_WIN;
	args.saveFolder = SAVE_DIR;

	/* Dolphin-only tape replay: a PLAYBACK sentinel beside the game enables it.
	 * Never present on real SD cards, so hardware keeps live controller input. */
	if (playback) {
		args.playbackInputsPath = WII_GAME_DIR "/am2r_inputs.json";
		/* 1.5.5 calls randomize() at boot; a tape only replays if that stays a no-op. */
		args.hasSeed = true;
		logInfo("PLAYBACK sentinel found -- replaying %s\n", args.playbackInputsPath);
	}
	if (access(WII_GAME_DIR "/POISON", R_OK) == 0)
		poison_arenas();

	rc = loop(args, argv[0] ? argv[0] : "boot.dol");
	if (GxRenderer_fatal())
		wait_home(GxRenderer_fatal());
	log_mallinfo("exit");
	logInfo("loop returned %d\n", rc);
	freeCommandLineArgs(&args);
	markers_from_args_remove();
	WiiLog_close();
	NetLog_send(); /* after the close: the file must be complete */
	return rc;
}
