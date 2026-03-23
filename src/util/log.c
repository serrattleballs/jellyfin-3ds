/**
 * log.c — Debug logging to SD card
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>

#include "util/log.h"

#define LOG_PATH "sdmc:/3ds/jellyfin-3ds/debug.log"

static FILE *s_log_file = NULL;
static LightLock s_log_lock;
static u64 s_start_tick = 0;

void log_init(void)
{
    LightLock_Init(&s_log_lock);
    s_start_tick = svcGetSystemTick();

    mkdir("sdmc:/3ds", 0755);
    mkdir("sdmc:/3ds/jellyfin-3ds", 0755);

    /* Truncate on each launch so the log is fresh */
    s_log_file = fopen(LOG_PATH, "w");
    if (s_log_file) {
        fprintf(s_log_file, "=== jellyfin-3ds debug log ===\n");
        fflush(s_log_file);
    }
}

void log_write(const char *fmt, ...)
{
    if (!s_log_file) return;

    LightLock_Lock(&s_log_lock);

    /* Timestamp in seconds since launch */
    u64 elapsed = svcGetSystemTick() - s_start_tick;
    double sec = (double)elapsed / (double)SYSCLOCK_ARM11;

    fprintf(s_log_file, "[%7.2f] ", sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(s_log_file, fmt, args);
    va_end(args);

    fprintf(s_log_file, "\n");
    fflush(s_log_file);

    LightLock_Unlock(&s_log_lock);
}

void log_close(void)
{
    if (s_log_file) {
        log_write("=== log closed ===");
        fclose(s_log_file);
        s_log_file = NULL;
    }
}
