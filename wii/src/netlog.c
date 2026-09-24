#include "netlog.h"
#include "log.h"
#include "wii_paths.h"

#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <network.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GRABS_DIR WII_APP_DIR "/grabs"
#define CHUNK 2048 /* every net_write is copied into libogc's small net heap first */
#define MAX_GRABS 16

static char g_target[40];

void NetLog_setTarget(const char *ipport)
{
	snprintf(g_target, sizeof g_target, "%s", ipport);
}

static s32 init_done(s32 result, void *usr)
{
	(void)result;
	(void)usr;
	return 0;
}

/* The IOS network module brings the link up on its own clock (DHCP included): give it 20 s. */
static bool wait_net(void)
{
	int i;

	if (net_init_async(init_done, NULL) < 0)
		return false;
	for (i = 0; i < 200; i++) {
		s32 st = net_get_status();

		if (st != -EBUSY)
			return st >= 0;
		usleep(100000);
	}
	return false;
}

static bool send_all(s32 s, const void *data, int n)
{
	const u8 *p = data;

	while (n > 0) {
		s32 w = net_write(s, p, n > CHUNK ? CHUNK : n);

		if (w <= 0)
			return false;
		p += w;
		n -= w;
	}
	return true;
}

static bool send_file(s32 s, const char *path, const char *name, u8 *buf)
{
	FILE *f = fopen(path, "rb");
	char hdr[128];
	long size;
	int n;

	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	n = snprintf(hdr, sizeof hdr, "FILE %s %ld\n", name, size);
	if (!send_all(s, hdr, n)) {
		fclose(f);
		return false;
	}
	while (size > 0) {
		n = fread(buf, 1, CHUNK, f);
		if (n <= 0 || !send_all(s, buf, n))
			break;
		size -= n;
	}
	fclose(f);
	return size == 0;
}

void NetLog_send(void)
{
	static char grabs[MAX_GRABS][32];
	struct sockaddr_in sa;
	struct dirent *e;
	char *colon;
	int i, count = 0;
	s32 s = -1;
	bool ok;
	u8 *buf;
	DIR *d;

	if (!g_target[0]) {
		FILE *f = fopen(WII_GAME_DIR "/LOGTO", "r");

		if (!f)
			return;
		if (fgets(g_target, sizeof g_target, f))
			g_target[strcspn(g_target, "\r\n")] = 0;
		fclose(f);
	}
	colon = strchr(g_target, ':');
	if (!colon)
		return;
	*colon = 0;
	memset(&sa, 0, sizeof sa);
	sa.sin_len = sizeof sa;
	sa.sin_family = AF_INET;
	sa.sin_port = htons(atoi(colon + 1));
	sa.sin_addr.s_addr = inet_addr(g_target);

	buf = memalign(32, CHUNK);
	ok = buf && wait_net();
	if (ok) {
		s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
		ok = s >= 0 && net_connect(s, (struct sockaddr *)&sa, sizeof sa) >= 0;
	}
	ok = ok && send_file(s, WII_LOG_PATH, "profile.txt", buf);

	/* Collect first: unlinking while readdir walks the same directory confuses libfat. */
	d = ok ? opendir(GRABS_DIR) : NULL;
	while (d && count < MAX_GRABS && (e = readdir(d)))
		if (!strncmp(e->d_name, "efb-", 4) && strlen(e->d_name) < sizeof grabs[0])
			strcpy(grabs[count++], e->d_name);
	if (d)
		closedir(d);
	for (i = 0; ok && i < count; i++) {
		char path[96], name[64];

		snprintf(path, sizeof path, GRABS_DIR "/%.31s", grabs[i]);
		snprintf(name, sizeof name, "grabs/%.31s", grabs[i]);
		ok = send_file(s, path, name, buf);
		if (ok)
			unlink(path);
	}
	ok = ok && send_all(s, "END\n", 4);

	if (s >= 0)
		net_close(s);
	net_deinit();
	free(buf);
	logInfo("LOGTO %s:%s: %s (%d grabs)\n", g_target, colon + 1, ok ? "sent" : "failed", count);
}
