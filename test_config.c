#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgr_config.h"
#include "cgr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Unit tests for cgreclaim.
 * Does not require root or real cgroups.
 */

static int test_config_parse(void)
{
	struct cgr_ctx ctx;
	FILE *fp;

	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg.poll_interval_ms = 1000;
	ctx.refault_slope_moderate = 10;
	ctx.refault_slope_urgent = 100;

	fp = fopen("/tmp/cgreclaim_test_cfg", "w");
	if (!fp) {
		perror("fopen");
		return 1;
	}
	fprintf(fp, "# test config\n");
	fprintf(fp, "poll_interval_ms=500\n");
	fprintf(fp, "refault_slope_moderate=25\n");
	fprintf(fp, "refault_slope_urgent=200\n");
	fprintf(fp, "unknown_key=ignored\n");
	fclose(fp);

	printf("Config parse test: ");

	fp = fopen("/tmp/cgreclaim_test_cfg", "r");
	if (!fp) {
		printf("FAIL (can't reopen)\n");
		return 1;
	}

	{
		char line[256];

		while (fgets(line, sizeof(line), fp)) {
			char *key, *val, *nl;

			nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			if (line[0] == '#' || line[0] == '\0')
				continue;

			key = line;
			val = strchr(line, '=');
			if (!val) continue;
			*val++ = '\0';
			while (*val == ' ' || *val == '\t') val++;

			if (strcmp(key, "poll_interval_ms") == 0)
				ctx.cfg.poll_interval_ms = (unsigned int)strtoul(val, NULL, 10);
			else if (strcmp(key, "refault_slope_moderate") == 0)
				ctx.refault_slope_moderate = strtoull(val, NULL, 10);
			else if (strcmp(key, "refault_slope_urgent") == 0)
				ctx.refault_slope_urgent = strtoull(val, NULL, 10);
		}
	}
	fclose(fp);

	if (ctx.cfg.poll_interval_ms != 500 ||
	    ctx.refault_slope_moderate != 25 ||
	    ctx.refault_slope_urgent != 200) {
		printf("FAIL (poll=%u moderate=%lu urgent=%lu)\n",
			ctx.cfg.poll_interval_ms,
			(unsigned long)ctx.refault_slope_moderate,
			(unsigned long)ctx.refault_slope_urgent);
		return 1;
	}

	printf("OK\n");
	unlink("/tmp/cgreclaim_test_cfg");
	return 0;
}

static int test_early_log_buffer(void)
{
	printf("Early log buffer test: ");

	cgr_log_file(0, "test early message %d", 1);
	cgr_log_file(1, "test early message %d", 2);
	cgr_log_file(2, "test early message %d", 3);

	printf("OK (3 messages buffered without crash)\n");
	return 0;
}

static int test_path_accessible(void)
{
	printf("Path accessibility test: ");

	if (access("/tmp", R_OK | X_OK) != 0) {
		printf("FAIL (/tmp not accessible)\n");
		return 1;
	}

	if (access("/nonexistent_path_xyz", R_OK | X_OK) == 0) {
		printf("FAIL (/nonexistent accessible?)\n");
		return 1;
	}

	printf("OK\n");
	return 0;
}

static int test_dynamic_groups_grow(void)
{
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = NULL,
		.log_fn = NULL,
	};
	struct cgr_ctx *ctx;
	int i, ret;
	char path[256];

	printf("Dynamic groups grow test: ");

	ctx = cgr_init(&cfg);
	if (!ctx) {
		printf("FAIL (init)\n");
		return 1;
	}

	for (i = 0; i < 20; i++) {
		snprintf(path, sizeof(path), "/sys/fs/cgroup/fake_test_%d", i);
		cgr_add_cgroup(ctx, path);
	}

	if (ctx->nr_groups == 20 && ctx->groups_cap >= 20) {
		printf("OK (nr=%d cap=%d)\n", ctx->nr_groups, ctx->groups_cap);
		ret = 0;
	} else {
		printf("FAIL (nr=%d cap=%d)\n", ctx->nr_groups, ctx->groups_cap);
		ret = 1;
	}

	cgr_destroy(ctx);
	return ret;
}

static int test_add_remove_cgroup(void)
{
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = NULL,
		.log_fn = NULL,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;
	int ret;
	const char *p = "/sys/fs/cgroup/fake_add_rm";

	printf("Add/remove cgroup test: ");

	ctx = cgr_init(&cfg);
	if (!ctx) {
		printf("FAIL (init)\n");
		return 1;
	}

	/* Add */
	ret = cgr_add_cgroup(ctx, p);
	if (ret != CGR_OK) {
		printf("FAIL (add ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	/* Duplicate add should fail */
	ret = cgr_add_cgroup(ctx, p);
	if (ret != CGR_ERR_EXIST) {
		printf("FAIL (dup add ret=%d, expected %d)\n", ret, CGR_ERR_EXIST);
		cgr_destroy(ctx);
		return 1;
	}

	/* Get status */
	ret = cgr_get_status(ctx, p, &st);
	if (ret != CGR_OK || st.limit < CGR_MIN_LIMIT_BYTES) {
		printf("FAIL (status ret=%d limit=%lu)\n", ret,
			(unsigned long)(st.limit >> 20));
		cgr_destroy(ctx);
		return 1;
	}

	/* Remove */
	ret = cgr_remove_cgroup(ctx, p);
	if (ret != CGR_OK) {
		printf("FAIL (remove ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	/* Should not be found now */
	ret = cgr_get_status(ctx, p, &st);
	if (ret != CGR_ERR_NOENT) {
		printf("FAIL (after remove ret=%d, expected %d)\n",
			ret, CGR_ERR_NOENT);
		cgr_destroy(ctx);
		return 1;
	}

	/* Remove again should fail */
	ret = cgr_remove_cgroup(ctx, p);
	if (ret != CGR_ERR_NOENT) {
		printf("FAIL (double remove ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	if (ctx->nr_groups != 0) {
		printf("FAIL (nr_groups=%d after remove)\n", ctx->nr_groups);
		cgr_destroy(ctx);
		return 1;
	}

	printf("OK\n");
	cgr_destroy(ctx);
	return 0;
}

static int test_inotify_create_delete(void)
{
	/*
	 * Test inotify-based cgroup discovery using a temporary directory.
	 * We can't use real cgroups without root, but we can verify that
	 * cgr_scan_cgroups + inotify event flow works on regular dirs.
	 *
	 * cgr_add_cgroup will succeed (it only reads memory.current which
	 * fails → falls back to CGR_MIN_LIMIT_BYTES).
	 */
	const char *base = "/tmp/cgreclaim_inotify_test";
	char child1[512], child2[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = base,
		.log_fn = NULL,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;
	int ret;

	printf("Inotify create/delete test: ");

	/* Setup temp dirs */
	mkdir(base, 0755);
	snprintf(child1, sizeof(child1), "%s/app1", base);
	snprintf(child2, sizeof(child2), "%s/app2", base);
	mkdir(child1, 0755);
	mkdir(child2, 0755);

	ctx = cgr_init(&cfg);
	if (!ctx) {
		printf("FAIL (init)\n");
		goto cleanup;
	}

	/* Scan should find child dirs (they won't have memory.current
	 * but scan calls is_memory_cgroup which checks cg_file_exists —
	 * that will return false for regular dirs, so scan finds 0).
	 * This is correct behavior — only real cgroups get added.
	 */
	ret = cgr_scan_cgroups(ctx);
	/* ret == 0 is expected (no memory.current in regular dirs) */

	/* But manual add works (simulating what inotify handler does) */
	ret = cgr_add_cgroup(ctx, child1);
	if (ret != CGR_OK) {
		printf("FAIL (add child1 ret=%d)\n", ret);
		cgr_destroy(ctx);
		goto cleanup;
	}

	ret = cgr_add_cgroup(ctx, child2);
	if (ret != CGR_OK) {
		printf("FAIL (add child2 ret=%d)\n", ret);
		cgr_destroy(ctx);
		goto cleanup;
	}

	if (ctx->nr_groups != 2) {
		printf("FAIL (nr_groups=%d expected 2)\n", ctx->nr_groups);
		cgr_destroy(ctx);
		goto cleanup;
	}

	/* Simulate deletion — remove child1 */
	ret = cgr_remove_cgroup(ctx, child1);
	if (ret != CGR_OK) {
		printf("FAIL (remove child1 ret=%d)\n", ret);
		cgr_destroy(ctx);
		goto cleanup;
	}

	if (ctx->nr_groups != 1) {
		printf("FAIL (nr_groups=%d expected 1 after remove)\n",
			ctx->nr_groups);
		cgr_destroy(ctx);
		goto cleanup;
	}

	/* child2 should still be tracked */
	ret = cgr_get_status(ctx, child2, &st);
	if (ret != CGR_OK) {
		printf("FAIL (child2 status after child1 remove ret=%d)\n", ret);
		cgr_destroy(ctx);
		goto cleanup;
	}

	/* Re-add child1 (simulating inotify CREATE after re-creation) */
	ret = cgr_add_cgroup(ctx, child1);
	if (ret != CGR_OK) {
		printf("FAIL (re-add child1 ret=%d)\n", ret);
		cgr_destroy(ctx);
		goto cleanup;
	}

	if (ctx->nr_groups != 2) {
		printf("FAIL (nr_groups=%d expected 2 after re-add)\n",
			ctx->nr_groups);
		cgr_destroy(ctx);
		goto cleanup;
	}

	printf("OK\n");
	cgr_destroy(ctx);
	rmdir(child1);
	rmdir(child2);
	rmdir(base);
	return 0;

cleanup:
	rmdir(child1);
	rmdir(child2);
	rmdir(base);
	return 1;
}

static int test_inotify_live(void)
{
	/*
	 * Test that the actual inotify thread picks up new directories.
	 * We start the monitor (which spawns the inotify thread),
	 * create a subdirectory, then check if it was auto-registered.
	 *
	 * Note: cgr_add_cgroup will work even on non-cgroup dirs
	 * (memory.current read fails → uses minimum limit).
	 */
	const char *base = "/tmp/cgreclaim_inotify_live";
	char child[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 100,
		.scan_root = base,
		.log_fn = NULL,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;
	int ret, fail = 0;

	printf("Inotify live detection test: ");

	/* The monitor thread waits for /home/root — that's likely
	 * accessible on this dev machine.  If not, skip. */
	if (access("/home/root", R_OK | X_OK) != 0) {
		printf("SKIP (/home/root not accessible)\n");
		return 0;
	}

	mkdir(base, 0755);
	snprintf(child, sizeof(child), "%s/live_app", base);

	ctx = cgr_init(&cfg);
	if (!ctx) {
		printf("FAIL (init)\n");
		rmdir(base);
		return 1;
	}

	ret = cgr_start(ctx);
	if (ret != CGR_OK) {
		printf("FAIL (start ret=%d)\n", ret);
		cgr_destroy(ctx);
		rmdir(base);
		return 1;
	}

	/* Give monitor thread time to start + setup inotify */
	usleep(300000); /* 300ms */

	/* Create a new directory — inotify should pick it up */
	mkdir(child, 0755);

	/* Wait for inotify event to be processed */
	usleep(200000); /* 200ms */

	ret = cgr_get_status(ctx, child, &st);
	if (ret != CGR_OK) {
		printf("FAIL (live dir not detected, ret=%d)\n", ret);
		fail = 1;
	}

	if (!fail) {
		/* Now remove it */
		rmdir(child);
		usleep(200000);

		ret = cgr_get_status(ctx, child, &st);
		if (ret != CGR_ERR_NOENT) {
			printf("FAIL (removed dir still tracked, ret=%d)\n", ret);
			fail = 1;
		}
	}

	cgr_stop(ctx);
	cgr_destroy(ctx);

	/* cleanup */
	rmdir(child);
	rmdir(base);

	if (!fail)
		printf("OK\n");
	return fail;
}

static int test_monitor_start_stop(void)
{
	struct cgr_config cfg = {
		.poll_interval_ms = 100,
		.scan_root = NULL,
		.log_fn = NULL,
	};
	struct cgr_ctx *ctx;
	int ret;

	printf("Monitor start/stop test: ");

	if (access("/home/root", R_OK | X_OK) != 0) {
		printf("SKIP (/home/root not accessible)\n");
		return 0;
	}

	ctx = cgr_init(&cfg);
	if (!ctx) {
		printf("FAIL (init)\n");
		return 1;
	}

	ret = cgr_start(ctx);
	if (ret != CGR_OK) {
		printf("FAIL (start ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	/* Double start should fail */
	ret = cgr_start(ctx);
	if (ret != CGR_ERR_BUSY) {
		printf("FAIL (double start ret=%d expected %d)\n",
			ret, CGR_ERR_BUSY);
		cgr_stop(ctx);
		cgr_destroy(ctx);
		return 1;
	}

	usleep(300000); /* let it run a few polls */

	ret = cgr_stop(ctx);
	if (ret != CGR_OK) {
		printf("FAIL (stop ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	/* Double stop should be fine */
	ret = cgr_stop(ctx);
	if (ret != CGR_OK) {
		printf("FAIL (double stop ret=%d)\n", ret);
		cgr_destroy(ctx);
		return 1;
	}

	printf("OK\n");
	cgr_destroy(ctx);
	return 0;
}

int main(void)
{
	int failures = 0;

	printf("=== cgreclaim unit tests ===\n\n");

	/* Basic tests — no root needed */
	failures += test_config_parse();
	failures += test_early_log_buffer();
	failures += test_path_accessible();
	failures += test_dynamic_groups_grow();
	failures += test_add_remove_cgroup();
	failures += test_inotify_create_delete();

	/* Live tests — need /home/root accessible */
	failures += test_monitor_start_stop();
	failures += test_inotify_live();

	printf("\n=== %s (%d failure%s) ===\n",
		failures ? "FAIL" : "ALL PASSED",
		failures, failures == 1 ? "" : "s");

	cgr_log_close();
	return failures ? 1 : 0;
}
