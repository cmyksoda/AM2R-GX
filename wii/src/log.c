#include "wii_log.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE *g_log;

bool WiiLog_open(const char *path)
{
	g_log = fopen(path, "w");
	if (!g_log)
		return false;
	setvbuf(g_log, NULL, _IONBF, 0);
	fprintf(g_log, "=== AM2R GX Stage 5 log ===\n");
	return true;
}

void WiiLog_close(void)
{
	if (!g_log)
		return;
	fclose(g_log);
	g_log = NULL;
}

void platformLog(const logType type, const char *format, va_list va)
{
	const char *prefix = "";
	va_list copy;

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

	va_copy(copy, va);
	fputs(prefix, stdout);
	vfprintf(stdout, format, va);
	fflush(stdout);

	if (g_log) {
		fputs(prefix, g_log);
		vfprintf(g_log, format, copy);
		fflush(g_log);
	}
	va_end(copy);
}
