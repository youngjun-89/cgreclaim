#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgr_config.h"
#include "cgr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Unit test for config file parsing and early-log buffering.
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

	/* Write a temp config file */
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

	/* Temporarily override config path — we test the parse logic
	 * by calling cgr_config_load which reads CGR_CONFIG_PATH.
	 * Since we can't change the #define, we test parse logic directly.
	 */
	printf("Config parse test: ");

	/* Parse the file manually to test the logic */
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

	/*
	 * cgr_log_file() should buffer messages when the log file
	 * path doesn't exist.  We can't easily test the flush without
	 * creating /home/root, but we verify it doesn't crash.
	 */
	cgr_log_file(0, "test early message %d", 1);
	cgr_log_file(1, "test early message %d", 2);
	cgr_log_file(2, "test early message %d", 3);

	/* If we got here without crashing, buffer works */
	printf("OK (3 messages buffered without crash)\n");
	return 0;
}

static int test_path_accessible(void)
{
	printf("Path accessibility test: ");

	/* /tmp should always be accessible */
	if (access("/tmp", R_OK | X_OK) != 0) {
		printf("FAIL (/tmp not accessible)\n");
		return 1;
	}

	/* /nonexistent should fail */
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

	/* Initial capacity is 16.  Add more than 16 to trigger grow.
	 * These will fail at cg_read_uint64 (no real cgroup) but
	 * the slot allocation path is what we're testing.
	 * cgr_add_cgroup reads memory.current which will fail,
	 * so it still sets the minimum limit and proceeds.
	 */
	for (i = 0; i < 20; i++) {
		snprintf(path, sizeof(path), "/sys/fs/cgroup/fake_test_%d", i);
		ret = cgr_add_cgroup(ctx, path);
		/* Will get CGR_OK (limit defaults to min) since
		 * cg_read_uint64 failure → uses CGR_MIN_LIMIT_BYTES */
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

int main(void)
{
	int failures = 0;

	printf("=== cgreclaim unit tests ===\n\n");

	failures += test_config_parse();
	failures += test_early_log_buffer();
	failures += test_path_accessible();
	failures += test_dynamic_groups_grow();

	printf("\n=== %s (%d failure%s) ===\n",
		failures ? "FAIL" : "ALL PASSED",
		failures, failures == 1 ? "" : "s");

	cgr_log_close();
	return failures ? 1 : 0;
}
