/*
 * cgrd — cgreclaim daemon
 *
 * Runs cgreclaim as a long-lived process.  Scans a cgroup subtree for
 * memory-controller-enabled children, starts the adaptive monitor thread,
 * and waits for SIGTERM/SIGINT to shut down cleanly.
 *
 * Usage:
 *   cgrd [scan_root]
 *
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

int main(int argc, char *argv[])
{
	const char *scan_root = (argc > 1) ? argv[1] : "/sys/fs/cgroup";

	/* Open log file before doing anything else so early errors are captured. */
	cgr_log_open();

	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root        = scan_root,
		.log_fn           = cgr_log_file,
	};

	cgr_log_file(CGR_LOG_INFO, "cgrd starting (scan_root=%s, RAM=%llu MB)",
		     scan_root,
		     (unsigned long long)(cgr_get_total_ram() >> 20));

	struct cgr_ctx *ctx = cgr_init(&cfg);
	if (!ctx) {
		cgr_log_file(CGR_LOG_ERR, "cgr_init failed");
		cgr_log_close();
		return EXIT_FAILURE;
	}

	int found = cgr_scan_cgroups(ctx);
	if (found < 0) {
		cgr_log_file(CGR_LOG_WARN, "cgr_scan_cgroups error (%d), continuing anyway", found);
	} else {
		cgr_log_file(CGR_LOG_INFO, "found %d cgroup(s)", found);
	}

	int rc = cgr_start(ctx);
	if (rc != CGR_OK) {
		cgr_log_file(CGR_LOG_ERR, "cgr_start failed (%d)", rc);
		cgr_destroy(ctx);
		cgr_log_close();
		return EXIT_FAILURE;
	}

	cgr_log_file(CGR_LOG_INFO, "monitor running — waiting for SIGTERM/SIGINT");

	signal(SIGTERM, on_signal);
	signal(SIGINT,  on_signal);

	while (!g_stop)
		sleep(1);

	cgr_log_file(CGR_LOG_INFO, "cgrd stopping");
	cgr_stop(ctx);
	cgr_destroy(ctx);
	cgr_log_close();

	return EXIT_SUCCESS;
}
