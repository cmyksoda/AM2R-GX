#include "wii_log.h"
#include "log.h"

#include <gccore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static FILE *g_log;
static bool g_gecko;

bool WiiLog_open(const char *path)
{
	g_log = fopen(path, "w");
	if (!g_log)
		return false;
	setvbuf(g_log, NULL, _IONBF, 0);
	fprintf(g_log, "=== AM2R GX log ===\n");
	return true;
}

/* Push the log through libfat's cache onto the card, for a run that may end in a power-off. */
void WiiLog_sync(void)
{
	if (g_log)
		fsync(fileno(g_log));
}

void WiiLog_close(void)
{
	if (!g_log)
		return;
	fclose(g_log);
	g_log = NULL;
}

void WiiLog_enableGecko(void)
{
	g_gecko = usb_isgeckoalive(1);
}

void platformLog(const logType type, const char *format, va_list va)
{
	const char *prefix = "";
	va_list gecko;
	static char line[8192];
	int n;

	switch (type) {
	case LOG_TYPE_WARNING:
		prefix = "Warning: ";
		break;
	case LOG_TYPE_ERROR:
		prefix = "Error: ";
		break;
	case LOG_TYPE_DEBUG:
		prefix = "Debug: ";
		break;
	default:
		break;
	}

	va_copy(gecko, va);
	if (g_log) {
		fputs(prefix, g_log);
		vfprintf(g_log, format, va);
		fflush(g_log);
		/* An error can be the last line a run writes (a failed require() aborts): put it on the card now. */
		if (type == LOG_TYPE_ERROR)
			fsync(fileno(g_log));
	}

	if (g_gecko) {
		n = vsnprintf(line, sizeof line, format, gecko);
		/* Keep a truncated message line-terminated so the next one isn't glued onto it. */
		if (n > (int)sizeof line - 1) {
			n = sizeof line - 1;
			line[n - 1] = '\n';
		}
		usb_sendbuffer_safe(1, prefix, strlen(prefix));
		if (n > 0)
			usb_sendbuffer_safe(1, line, n);
	}
	va_end(gecko);
}
