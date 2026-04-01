#define _GNU_SOURCE
#include "cgreclaim.h"

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

#define POOL_SIZE	(1536ULL << 20)  /* 1.5 GB */

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
		fprintf(stderr, "  %s: (error getting status)\n", path);
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
	struct cgr_config cfg = {
		.total_pool = POOL_SIZE,
		.poll_interval_ms = 500,
		.fg_ratio = 0.6,
		.log_fn = test_log,
	};
	struct cgr_ctx *ctx;
	int ret;

	fprintf(stderr, "=== cgreclaim test ===\n");
	fprintf(stderr, "Pool: %" PRIu64 " MB\n", (uint64_t)(POOL_SIZE >> 20));

	/* Setup test cgroups (requires root) */
	if (setup_test_cgroups() < 0) {
		fprintf(stderr, "Failed to create test cgroups (need root?)\n");
		return 1;
	}

	/* Init */
	ctx = cgr_init(&cfg);
	if (!ctx) {
		fprintf(stderr, "cgr_init failed\n");
		ret = 1;
		goto out;
	}

	/* Add cgroups with equal initial limits */
	uint64_t each = POOL_SIZE / 3;

	ret = cgr_add_cgroup(ctx, TEST_CG_A, each);
	fprintf(stderr, "add A: %d\n", ret);

	ret = cgr_add_cgroup(ctx, TEST_CG_B, each);
	fprintf(stderr, "add B: %d\n", ret);

	ret = cgr_add_cgroup(ctx, TEST_CG_C, each);
	fprintf(stderr, "add C: %d\n", ret);

	fprintf(stderr, "\n--- initial state ---\n");
	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	/* Start monitor */
	ret = cgr_start(ctx);
	fprintf(stderr, "\nmonitor start: %d\n", ret);

	/* Simulate app switch: A becomes foreground */
	fprintf(stderr, "\n--- set A foreground ---\n");
	ret = cgr_set_foreground(ctx, TEST_CG_A);
	fprintf(stderr, "set_foreground A: %d\n", ret);

	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	/* Wait a bit, then switch to B */
	sleep(2);

	fprintf(stderr, "\n--- set B foreground ---\n");
	ret = cgr_set_foreground(ctx, TEST_CG_B);
	fprintf(stderr, "set_foreground B: %d\n", ret);

	print_status(ctx, TEST_CG_A);
	print_status(ctx, TEST_CG_B);
	print_status(ctx, TEST_CG_C);

	/* Dynamic limit test */
	fprintf(stderr, "\n--- set_limit C = 128MB ---\n");
	ret = cgr_set_limit(ctx, TEST_CG_C, 128ULL << 20);
	fprintf(stderr, "set_limit C: %d\n", ret);
	print_status(ctx, TEST_CG_C);

	/* Stop */
	sleep(1);
	cgr_stop(ctx);
	fprintf(stderr, "\nmonitor stopped\n");

	/* Remove cgroups */
	cgr_remove_cgroup(ctx, TEST_CG_A);
	cgr_remove_cgroup(ctx, TEST_CG_B);
	cgr_remove_cgroup(ctx, TEST_CG_C);

	cgr_destroy(ctx);
	ret = 0;

out:
	cleanup_test_cgroups();
	fprintf(stderr, "\n=== done (ret=%d) ===\n", ret);
	return ret;
}
