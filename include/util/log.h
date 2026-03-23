/**
 * log.h — Debug logging to SD card
 *
 * Writes timestamped log entries to sdmc:/3ds/jellyfin-3ds/debug.log
 * FTP the file off the 3DS after a test run for analysis.
 */

#ifndef JFIN_LOG_H
#define JFIN_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize logging. Opens/creates the log file.
 * Call once at startup.
 */
void log_init(void);

/**
 * Write a log entry. printf-style formatting.
 * Thread-safe (uses a lock).
 */
void log_write(const char *fmt, ...);

/**
 * Flush and close the log file.
 */
void log_close(void);

#ifdef __cplusplus
}
#endif

#endif /* JFIN_LOG_H */
