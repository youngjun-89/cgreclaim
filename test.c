#define _GNU_SOURCE
#include "cgreclaim.h"
#include "cgr_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_CG_BASE	"/sys/fs/cgroup/cgreclaim_test"
#define TEST_CG_A	TEST_CG_BASE "/app_a"
#define TEST_CG_B	TEST_CG_BASE "/app_b"
#define TEST_CG_C	TEST_CG_BASE "/app_c"

static const char *level_str[] = { "ERR", "WARN", "INFO", "DEBUG" };

static void test_log(int level, const char *fmt, ...)
{
	va_list ap;

	if (level > CGR_LOG_INFO)
		return;

	fprintf(stderr, "[%s] ", level_str[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static int setup_test_cgroups(void)
{
	const char *paths[] = { TEST_CG_BASE, TEST_CG_A, TEST_CG_B, TEST_CG_C };
	int i;

	for (i = 0; i < 4; i++) {
		if (mkdir(paths[i], 0755) < 0 && errno != EEXIST) {
			perror(paths[i]);
			return -1;
		}
	}

	/* Enable memory controller in subtree */
	FILE *f = fopen(TEST_CG_BASE "/cgroup.subtree_control", "w");
	if (f) {
		fprintf(f, "+memory");
		fclose(f);
	}

	return 0;
}

static void cleanup_test_cgroups(void)
{
	rmdir(TEST_CG_C);
	rmdir(TEST_CG_B);
	rmdir(TEST_CG_A);
	rmdir(TEST_CG_BASE);
}

static void print_status(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_status st;

	if (cgr_get_status(ctx, path, &st) != CGR_OK) {
		fprintf(stderr, "  %s: (not managed)\n", path);
		return;
	}

	fprintf(stderr, "  %s: limit=%" PRIu64 "MB usage=%" PRIu64 "MB fg=%d reclaims=%" PRIu64 "\n",
		st.path,
		st.limit >> 20,
		st.usage >> 20,
		st.is_foreground,
		st.reclaim_count);
}

int main(void)
{
	uint64_t total_ram = cgr_get_total_ram();
	struct cgr_config cfg = {
		.poll_interval_ms = 500,
		.scan_root = TEST_CG_BASE,
		.log_fn = cgr_log_file,
	};
	struct cgr_ctx *ctx;
	int ret, found;

	fprintf(stderr, "=== cgreclaim test ===\n");
	fprintf(stderr, "System RAM: %" PRIu64 " MB\n", total_ram >> 20);

	/* Open file logger — writes to /home/root/cgreclaim.log */
	if (cgr_log_open() < 0) {
		fprintf(stderr, "WARNING: could not open %s, continuing without file log\n",
			CGR_LOG_PATH);
	}

	/* Setup test cgroups (requires root) */
	if (setup_test_cgroups() < 0) {
		fprintf(stderr, "Failed to create test cgroups (need root?)\n");
		return 1;
	}

	/* Init — pool auto-calculated */
	ctx = cgr_init(&cfg);
	if (!ctx) {
		fprintf(stderr, "cgr_init failed\n");
		ret = 1;
		goto out;
	}

	/* Auto-scan cgroups under TEST_CG_BASE */
	found = cgr_scan_cgroups(ctx);
	fprintf(stderr, "\nScan found %d cgroups\n", found);

	fprintf(stderr, "\n--- initial state (existing limits preserved) ---\n");
	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	/* Start monitor */
	ret = cgr_start(ctx);
	fprintf(stderr, "\nmonitor start: %d\n", ret);

	/* Simulate app switch: A becomes foreground */
	fprintf(stderr, "\n--- set A foreground ---\n");
	cgr_set_foreground(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	sleep(2);

	/* Switch to B */
	fprintf(stderr, "\n--- set B foreground ---\n");
	cgr_set_foreground(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	sleep(1);

	/* Stop & cleanup */
	cgr_stop(ctx);
	fprintf(stderr, "\nmonitor stopped\n");

	cgr_remove_cgroup(ctx, TEST_CG_A);
	cgr_remove_cgroup(ctx, TEST_CG_B);
	cgr_remove_cgroup(ctx, TEST_CG_C);
	cgr_destroy(ctx);
	ret = 0;

out:
	cleanup_test_cgroups();
	cgr_log_close();
	fprintf(stderr, "\n=== done (ret=%d) ===\n", ret);
	return ret;
}
