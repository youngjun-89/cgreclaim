#define _GNU_SOURCE
#include "cgr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

static FILE *log_fp;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_str[] = { "ERR", "WARN", "INFO", "DEBUG" };

int cgr_log_open(void)
{
	pthread_mutex_lock(&log_mutex);

	if (log_fp) {
		pthread_mutex_unlock(&log_mutex);
		return 0;
	}

	log_fp = fopen(CGR_LOG_PATH, "a");
	if (!log_fp) {
		pthread_mutex_unlock(&log_mutex);
		return -1;
	}

	/* Line-buffered so entries appear promptly */
	setvbuf(log_fp, NULL, _IOLBF, 0);

	pthread_mutex_unlock(&log_mutex);
	return 0;
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

	pthread_mutex_lock(&log_mutex);

	if (!log_fp) {
		pthread_mutex_unlock(&log_mutex);
		return;
	}

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);

	fprintf(log_fp, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] ",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		ts.tv_nsec / 1000000,
		(level >= 0 && level <= CGR_LOG_DEBUG) ? level_str[level] : "???");

	va_start(ap, fmt);
	vfprintf(log_fp, fmt, ap);
	va_end(ap);

	fprintf(log_fp, "\n");

	pthread_mutex_unlock(&log_mutex);
}
