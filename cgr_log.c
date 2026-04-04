#define _GNU_SOURCE
#include "cgr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static FILE *log_fp;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str[] = { "ERR", "WARN", "INFO", "DEBUG" };

/*
 * Early-log ring buffer — holds messages produced before the log file
 * can be opened (e.g., /home/root not yet mounted).
 * Flushed on first successful open.
 */
#define EARLY_LOG_MAX	64
#define EARLY_LOG_LINE	256

static char early_buf[EARLY_LOG_MAX][EARLY_LOG_LINE];
static int early_count;

static void early_log_store(const char *msg)
{
	if (early_count < EARLY_LOG_MAX)
		snprintf(early_buf[early_count++], EARLY_LOG_LINE, "%s", msg);
}

static void early_log_flush(void)
{
	int i;

	if (!log_fp || early_count == 0)
		return;

	for (i = 0; i < early_count; i++)
		fprintf(log_fp, "%s\n", early_buf[i]);

	fflush(log_fp);
	early_count = 0;
}

static int cgr_log_open_locked(void)
{
	if (log_fp)
		return 0;

	log_fp = fopen(CGR_LOG_PATH, "a");
	if (!log_fp)
		return -1;

	/* Line-buffered so entries appear promptly */
	setvbuf(log_fp, NULL, _IOLBF, 0);

	/* Flush any early-buffered messages */
	early_log_flush();

	return 0;
}

int cgr_log_open(void)
{
	int ret;

	pthread_mutex_lock(&log_mutex);
	ret = cgr_log_open_locked();

	pthread_mutex_unlock(&log_mutex);
	return ret;
}

void cgr_log_close(void)
{
	pthread_mutex_lock(&log_mutex);

	if (log_fp) {
		fclose(log_fp);
		log_fp = NULL;
	}

	pthread_mutex_unlock(&log_mutex);
}

void cgr_log_file(int level, const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	va_list ap;
	char msg[512];
	int hdr_len;

	pthread_mutex_lock(&log_mutex);

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);

	hdr_len = snprintf(msg, sizeof(msg),
		"%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		ts.tv_nsec / 1000000,
		(level >= 0 && level <= CGR_LOG_DEBUG) ? level_str[level] : "???");

	va_start(ap, fmt);
	vsnprintf(msg + hdr_len, sizeof(msg) - hdr_len, fmt, ap);
	va_end(ap);

	if (cgr_log_open_locked() < 0) {
		/* Can't open yet — buffer for later */
		early_log_store(msg);
		pthread_mutex_unlock(&log_mutex);
		return;
	}

	fprintf(log_fp, "%s\n", msg);

	pthread_mutex_unlock(&log_mutex);
}
