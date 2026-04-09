/*
 * cgrd — cgreclaim daemon
 *
 * Runs cgreclaim as a long-lived process.  Scans a cgroup subtree for
 * memory-controller-enabled children, starts the adaptive monitor thread,
 * and waits for SIGTERM/SIGINT to shut down cleanly.
 *
 * Usage:
 *   cgrd [--no-log] [scan_root]
 *
 *   --no-log   — disable all file logging (intended for benchmark runs
 *                where log I/O must not perturb memory measurements)
 *   scan_root  — cgroup v2 directory to watch (default: /sys/fs/cgroup)
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cgreclaim.h"
#include "cgr_log.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/* Convenience: emit via cgr_log_file only when logging is enabled. */
#define cgrd_log(no_log, lvl, fmt, ...) \
	do { if (!(no_log)) cgr_log_file((lvl), (fmt), ##__VA_ARGS__); } while (0)

int main(int argc, char *argv[])
{
	int no_log = 0;
	const char *scan_root = "/sys/fs/cgroup";
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-log") == 0) {
			no_log = 1;
		} else {
			scan_root = argv[i];
		}
	}

	if (!no_log)
		cgr_log_open();

	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root        = scan_root,
		.log_fn           = no_log ? NULL : cgr_log_file,
	};

	cgrd_log(no_log, CGR_LOG_INFO, "cgrd starting (scan_root=%s, RAM=%llu MB)",
		 scan_root,
		 (unsigned long long)(cgr_get_total_ram() >> 20));

	struct cgr_ctx *ctx = cgr_init(&cfg);
	if (!ctx) {
		cgrd_log(no_log, CGR_LOG_ERR, "cgr_init failed");
		if (!no_log)
			cgr_log_close();
		return EXIT_FAILURE;
	}

	int found = cgr_scan_cgroups(ctx);
	if (found < 0) {
		cgrd_log(no_log, CGR_LOG_WARN,
			 "cgr_scan_cgroups error (%d), continuing anyway", found);
	} else {
		cgrd_log(no_log, CGR_LOG_INFO, "found %d cgroup(s)", found);
	}

	int rc = cgr_start(ctx);
	if (rc != CGR_OK) {
		cgrd_log(no_log, CGR_LOG_ERR, "cgr_start failed (%d)", rc);
		cgr_destroy(ctx);
		if (!no_log)
			cgr_log_close();
		return EXIT_FAILURE;
	}

	cgrd_log(no_log, CGR_LOG_INFO, "monitor running — waiting for SIGTERM/SIGINT");

	signal(SIGTERM, on_signal);
	signal(SIGINT,  on_signal);

	while (!g_stop)
		sleep(1);

	cgrd_log(no_log, CGR_LOG_INFO, "cgrd stopping");
	cgr_stop(ctx);
	cgr_destroy(ctx);
	if (!no_log)
		cgr_log_close();

	return EXIT_SUCCESS;
}
