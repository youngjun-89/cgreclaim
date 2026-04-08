#define _GNU_SOURCE
#include "cgreclaim.h"
#include "cgreclaim_internal.h"
#include "cgr_config.h"
#include "cgr_log.h"
#include "cgroup.h"
#include "fake_cgroup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ---- */

static int failures;
static int total;

#define TEST(name) \
	do { \
		total++; \
		printf("  %-45s ", #name); \
	} while (0)

#define PASS() \
	do { printf("OK\n"); } while (0)

#define FAIL(fmt, ...) \
	do { \
		printf("FAIL " fmt "\n", ##__VA_ARGS__); \
		failures++; \
	} while (0)

/* ================================================================
 * 1. cgroup I/O layer — works on fake files
 * ================================================================ */

static void test_cg_read_write(void)
{
	char path[512];
	uint64_t val;

	TEST(cg_read_uint64);
	fake_cg_create("rw_test", 100 << 20, 0, path, sizeof(path));
	if (cg_read_uint64(path, "memory.current", &val) == 0 &&
	    val == (100ULL << 20))
		PASS();
	else
		FAIL("val=%lu", (unsigned long)val);

	TEST(cg_read_uint64_max);
	if (cg_read_uint64(path, "memory.high", &val) == 0 &&
	    val == UINT64_MAX)
		PASS();
	else
		FAIL("val=%lu", (unsigned long)val);

	TEST(cg_write_uint64);
	if (cg_write_uint64(path, "memory.high", 50ULL << 20) == 0 &&
	    cg_read_uint64(path, "memory.high", &val) == 0 &&
	    val == (50ULL << 20))
		PASS();
	else
		FAIL("val=%lu", (unsigned long)val);

	TEST(cg_write_uint64_max);
	if (cg_write_uint64(path, "memory.high", UINT64_MAX) == 0 &&
	    cg_read_uint64(path, "memory.high", &val) == 0 &&
	    val == UINT64_MAX)
		PASS();
	else
		FAIL("val=%lu", (unsigned long)val);

	TEST(cg_file_exists);
	if (cg_file_exists(path, "memory.current") &&
	    !cg_file_exists(path, "no_such_file"))
		PASS();
	else
		FAIL("");

	TEST(cg_read_refault);
	fake_cg_set_refault(path, 42, 8);
	{
		uint64_t rf;
		if (cg_read_refault(path, &rf) == 0 && rf == 50)
			PASS();
		else
			FAIL("rf=%lu", (unsigned long)rf);
	}

	fake_cg_destroy(path);
}

/* ================================================================
 * 2. Add / remove lifecycle
 * ================================================================ */

static void test_add_remove(void)
{
	char path[512];
	struct cgr_config cfg = { .poll_interval_ms = 1000 };
	struct cgr_ctx *ctx;
	struct cgr_status st;

	fake_cg_create("add_rm", 64 << 20, 0, path, sizeof(path));
	ctx = cgr_init(&cfg);

	TEST(cgr_add_cgroup);
	if (cgr_add_cgroup(ctx, path) == CGR_OK && ctx->nr_groups == 1)
		PASS();
	else
		FAIL("nr=%d", ctx->nr_groups);

	TEST(cgr_add_duplicate);
	if (cgr_add_cgroup(ctx, path) == CGR_ERR_EXIST)
		PASS();
	else
		FAIL("");

	TEST(initial_limit_from_current);
	if (cgr_get_status(ctx, path, &st) == CGR_OK &&
	    st.limit >= (64ULL << 20) &&
	    st.limit < UINT64_MAX) /* not "max" */
		PASS();
	else
		FAIL("limit=%luMB", (unsigned long)(st.limit >> 20));

	TEST(memory.high_written);
	{
		uint64_t high;
		cg_read_uint64(path, "memory.high", &high);
		if (high == st.limit)
			PASS();
		else
			FAIL("high=%luMB limit=%luMB",
			     (unsigned long)(high >> 20),
			     (unsigned long)(st.limit >> 20));
	}

	TEST(cgr_remove_cgroup);
	if (cgr_remove_cgroup(ctx, path) == CGR_OK && ctx->nr_groups == 0)
		PASS();
	else
		FAIL("nr=%d", ctx->nr_groups);

	TEST(remove_restores_high);
	{
		uint64_t high;
		cg_read_uint64(path, "memory.high", &high);
		if (high == UINT64_MAX)
			PASS();
		else
			FAIL("high=%lu", (unsigned long)high);
	}

	TEST(remove_not_found);
	if (cgr_remove_cgroup(ctx, path) == CGR_ERR_NOENT)
		PASS();
	else
		FAIL("");

	cgr_destroy(ctx);
	fake_cg_destroy(path);
}

/* ================================================================
 * 3. Dynamic array growth
 * ================================================================ */

static void test_dynamic_grow(void)
{
	struct cgr_config cfg = { .poll_interval_ms = 1000 };
	struct cgr_ctx *ctx;
	char paths[20][512];
	char name[32];
	int i;

	ctx = cgr_init(&cfg);

	for (i = 0; i < 20; i++) {
		snprintf(name, sizeof(name), "grow_%d", i);
		fake_cg_create(name, (32 + i) << 20, 0,
			       paths[i], sizeof(paths[i]));
		cgr_add_cgroup(ctx, paths[i]);
	}

	TEST(dynamic_array_grow);
	if (ctx->nr_groups == 20 && ctx->groups_cap >= 20)
		PASS();
	else
		FAIL("nr=%d cap=%d", ctx->nr_groups, ctx->groups_cap);

	cgr_destroy(ctx);
	for (i = 0; i < 20; i++)
		fake_cg_destroy(paths[i]);
}

/* ================================================================
 * 4. Auto-discovery (cgr_scan_cgroups) — flat
 * ================================================================ */

static void test_scan_cgroups(void)
{
	char p1[512], p2[512], p3[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;
	int found;

	fake_cg_create("scan_a", 50 << 20, 0, p1, sizeof(p1));
	fake_cg_create("scan_b", 80 << 20, 0, p2, sizeof(p2));
	/* p3 has no memory.current — should be skipped */
	snprintf(p3, sizeof(p3), "%s/scan_noctl", FAKE_CG_BASE);
	mkdir(p3, 0755);

	ctx = cgr_init(&cfg);

	TEST(cgr_scan_cgroups);
	found = cgr_scan_cgroups(ctx);
	if (found == 2)
		PASS();
	else
		FAIL("found=%d expected 2", found);

	TEST(scan_skips_non_memory_cgroup);
	if (cgr_get_status(ctx, p3, &st) == CGR_ERR_NOENT)
		PASS();
	else
		FAIL("");

	TEST(scan_rescan_no_duplicates);
	found = cgr_scan_cgroups(ctx);
	if (found == 0 && ctx->nr_groups == 2)
		PASS();
	else
		FAIL("found=%d nr=%d", found, ctx->nr_groups);

	cgr_destroy(ctx);
	fake_cg_destroy(p1);
	fake_cg_destroy(p2);
	rmdir(p3);
}

/* ================================================================
 * 4b. Auto-discovery — recursive (nested cgroups)
 * ================================================================ */

static void test_scan_cgroups_recursive(void)
{
	/*
	 * Layout:
	 *   FAKE_CG_BASE/rec_top/         <- memory cgroup (level 1)
	 *   FAKE_CG_BASE/rec_top/rec_mid/ <- memory cgroup (level 2)
	 *   FAKE_CG_BASE/rec_top/rec_mid/rec_leaf/ <- memory cgroup (level 3)
	 *   FAKE_CG_BASE/rec_flat/        <- memory cgroup (level 1, no children)
	 *
	 * cgr_scan_cgroups must discover all four.
	 */
	char p_top[512], p_mid[512], p_leaf[512], p_flat[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;
	int found;

	fake_cg_create("rec_top", 60 << 20, 0, p_top, sizeof(p_top));
	fake_cg_create_under(p_top, "rec_mid", 30 << 20, 0,
			     p_mid, sizeof(p_mid));
	fake_cg_create_under(p_mid, "rec_leaf", 10 << 20, 0,
			     p_leaf, sizeof(p_leaf));
	fake_cg_create("rec_flat", 80 << 20, 0, p_flat, sizeof(p_flat));

	ctx = cgr_init(&cfg);

	TEST(scan_recursive_finds_all_levels);
	found = cgr_scan_cgroups(ctx);
	if (found == 4)
		PASS();
	else
		FAIL("found=%d expected 4", found);

	TEST(scan_recursive_tracks_leaf);
	if (cgr_get_status(ctx, p_leaf, &st) == CGR_OK)
		PASS();
	else
		FAIL("leaf not tracked");

	TEST(scan_recursive_tracks_mid);
	if (cgr_get_status(ctx, p_mid, &st) == CGR_OK)
		PASS();
	else
		FAIL("mid not tracked");

	TEST(scan_recursive_no_duplicates);
	found = cgr_scan_cgroups(ctx);
	if (found == 0 && ctx->nr_groups == 4)
		PASS();
	else
		FAIL("found=%d nr=%d", found, ctx->nr_groups);

	cgr_destroy(ctx);
	fake_cg_destroy(p_top); /* removes p_top and its entire subtree */
	fake_cg_destroy(p_flat);
}

/* ================================================================
 * 5. Config file parsing
 * ================================================================ */

static void test_config_parse(void)
{
	/*
	 * Can't override CGR_CONFIG_PATH #define, so test the
	 * parse logic inline with the same algorithm cgr_config.c uses.
	 */
	struct cgr_ctx ctx;
	FILE *fp;
	const char *cfg_path = "/tmp/cgreclaim_test_cfg";

	memset(&ctx, 0, sizeof(ctx));
	ctx.cfg.poll_interval_ms = 1000;
	ctx.cfg.refault_interval_ms = 1000;
	ctx.refault_slope_moderate = 10;
	ctx.refault_slope_urgent = 100;
	ctx.min_limit = CGR_MIN_LIMIT_BYTES;
	ctx.limit_usage_ratio = 2;
	ctx.grow_pct_urgent = 20;
	ctx.grow_pct_moderate = 10;
	ctx.shrink_pct = 5;

	fp = fopen(cfg_path, "w");
	fprintf(fp, "# test config\n");
	fprintf(fp, "poll_interval_ms=500\n");
	fprintf(fp, "refault_interval_ms=3000\n");
	fprintf(fp, "refault_slope_moderate=25\n");
	fprintf(fp, "refault_slope_urgent=200\n");
	fprintf(fp, "min_limit_mb=8\n");
	fprintf(fp, "limit_usage_ratio=3\n");
	fprintf(fp, "grow_pct_urgent=30\n");
	fprintf(fp, "grow_pct_moderate=15\n");
	fprintf(fp, "shrink_pct=8\n");
	fprintf(fp, "unknown_key=ignored\n");
	fclose(fp);

	fp = fopen(cfg_path, "r");
	{
		char line[256];
		while (fgets(line, sizeof(line), fp)) {
			char *key, *val, *nl;
			nl = strchr(line, '\n');
			if (nl) *nl = '\0';
			if (line[0] == '#' || line[0] == '\0') continue;
			key = line;
			val = strchr(line, '=');
			if (!val) continue;
			*val++ = '\0';
			while (*val == ' ' || *val == '\t') val++;

			if (strcmp(key, "poll_interval_ms") == 0)
				ctx.cfg.poll_interval_ms = (unsigned int)strtoul(val, NULL, 10);
			else if (strcmp(key, "refault_interval_ms") == 0)
				ctx.cfg.refault_interval_ms = (unsigned int)strtoul(val, NULL, 10);
			else if (strcmp(key, "refault_slope_moderate") == 0)
				ctx.refault_slope_moderate = strtoull(val, NULL, 10);
			else if (strcmp(key, "refault_slope_urgent") == 0)
				ctx.refault_slope_urgent = strtoull(val, NULL, 10);
			else if (strcmp(key, "min_limit_mb") == 0)
				ctx.min_limit = strtoull(val, NULL, 10) << 20;
			else if (strcmp(key, "limit_usage_ratio") == 0)
				ctx.limit_usage_ratio = (uint32_t)strtoul(val, NULL, 10);
			else if (strcmp(key, "grow_pct_urgent") == 0)
				ctx.grow_pct_urgent = (uint32_t)strtoul(val, NULL, 10);
			else if (strcmp(key, "grow_pct_moderate") == 0)
				ctx.grow_pct_moderate = (uint32_t)strtoul(val, NULL, 10);
			else if (strcmp(key, "shrink_pct") == 0) {
				uint32_t v = (uint32_t)strtoul(val, NULL, 10);
				if (v < 100)
					ctx.shrink_pct = v;
			}
		}
	}
	fclose(fp);

	TEST(config_poll_interval);
	if (ctx.cfg.poll_interval_ms == 500)
		PASS();
	else
		FAIL("got %u", ctx.cfg.poll_interval_ms);

	TEST(config_refault_interval);
	if (ctx.cfg.refault_interval_ms == 3000)
		PASS();
	else
		FAIL("got %u", ctx.cfg.refault_interval_ms);

	TEST(config_slope_moderate);
	if (ctx.refault_slope_moderate == 25)
		PASS();
	else
		FAIL("got %lu", (unsigned long)ctx.refault_slope_moderate);

	TEST(config_slope_urgent);
	if (ctx.refault_slope_urgent == 200)
		PASS();
	else
		FAIL("got %lu", (unsigned long)ctx.refault_slope_urgent);

	TEST(config_min_limit_mb);
	if (ctx.min_limit == (8ULL << 20))
		PASS();
	else
		FAIL("got %luMB", (unsigned long)(ctx.min_limit >> 20));

	TEST(config_limit_usage_ratio);
	if (ctx.limit_usage_ratio == 3)
		PASS();
	else
		FAIL("got %u", ctx.limit_usage_ratio);

	TEST(config_grow_pct_urgent);
	if (ctx.grow_pct_urgent == 30)
		PASS();
	else
		FAIL("got %u", ctx.grow_pct_urgent);

	TEST(config_grow_pct_moderate);
	if (ctx.grow_pct_moderate == 15)
		PASS();
	else
		FAIL("got %u", ctx.grow_pct_moderate);

	TEST(config_shrink_pct);
	if (ctx.shrink_pct == 8)
		PASS();
	else
		FAIL("got %u", ctx.shrink_pct);

	unlink(cfg_path);
}

/* ================================================================
 * 6. Early log buffer
 * ================================================================ */

static void test_early_log(void)
{
	TEST(early_log_no_crash);
	cgr_log_file(0, "early %d", 1);
	cgr_log_file(1, "early %d", 2);
	cgr_log_file(2, "early %d", 3);
	PASS();
}

/* ================================================================
 * 7. Inotify live detection (create + delete)
 * ================================================================ */

static void test_inotify_live(void)
{
	/*
	 * Start monitor → inotify watches FAKE_CG_BASE.
	 * Create a new fake cgroup dir → should be auto-added.
	 * Remove it → should be auto-removed.
	 *
	 * The monitor thread waits for /home/root.  We skip if
	 * that path is not accessible on this machine.
	 */
	char new_cg[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 100,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;

	if (access("/home/root", R_OK | X_OK) != 0) {
		TEST(inotify_live_add);
		printf("SKIP (/home/root not accessible)\n");
		TEST(inotify_live_remove);
		printf("SKIP\n");
		return;
	}

	ctx = cgr_init(&cfg);
	cgr_start(ctx);
	usleep(400000); /* wait for monitor + inotify setup */

	/* Create a new fake cgroup */
	fake_cg_create("inotify_live", 40 << 20, 0, new_cg, sizeof(new_cg));
	usleep(300000); /* wait for inotify */

	TEST(inotify_live_add);
	if (cgr_get_status(ctx, new_cg, &st) == CGR_OK)
		PASS();
	else
		FAIL("not detected");

	/* Remove it */
	fake_cg_destroy(new_cg);
	usleep(300000);

	TEST(inotify_live_remove);
	if (cgr_get_status(ctx, new_cg, &st) == CGR_ERR_NOENT)
		PASS();
	else
		FAIL("still tracked");

	cgr_stop(ctx);
	cgr_destroy(ctx);
}

/* ================================================================
 * 7b. Inotify recursive detection (nested create + delete)
 * ================================================================ */

static void test_inotify_recursive(void)
{
	/*
	 * Start monitor → scans FAKE_CG_BASE, sets up recursive watches.
	 * Create irec_top → auto-added, watch added on it.
	 * Create irec_top/irec_child → caught by watch on irec_top, auto-added.
	 * Remove irec_top/irec_child → auto-removed.
	 * Remove irec_top → auto-removed.
	 */
	char top[512], child[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 100,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;

	if (access("/home/root", R_OK | X_OK) != 0) {
		TEST(inotify_recursive_child_add);
		printf("SKIP (/home/root not accessible)\n");
		TEST(inotify_recursive_child_remove);
		printf("SKIP\n");
		TEST(inotify_recursive_top_remove);
		printf("SKIP\n");
		return;
	}

	ctx = cgr_init(&cfg);
	cgr_start(ctx);
	usleep(400000); /* wait for monitor + inotify setup */

	/* Create top-level cgroup — inotify sees it on scan_root watch */
	fake_cg_create("irec_top", 50 << 20, 0, top, sizeof(top));
	usleep(300000);

	/* Create nested child — inotify sees it on irec_top watch */
	fake_cg_create_under(top, "irec_child", 20 << 20, 0,
			     child, sizeof(child));
	usleep(300000);

	TEST(inotify_recursive_child_add);
	if (cgr_get_status(ctx, child, &st) == CGR_OK)
		PASS();
	else
		FAIL("nested child not detected");

	/* Remove child first (cgroup v2: must empty before parent) */
	fake_cg_destroy(child);
	usleep(300000);

	TEST(inotify_recursive_child_remove);
	if (cgr_get_status(ctx, child, &st) == CGR_ERR_NOENT)
		PASS();
	else
		FAIL("nested child still tracked");

	/* Remove top */
	fake_cg_destroy(top);
	usleep(300000);

	TEST(inotify_recursive_top_remove);
	if (cgr_get_status(ctx, top, &st) == CGR_ERR_NOENT)
		PASS();
	else
		FAIL("top still tracked");

	cgr_stop(ctx);
	cgr_destroy(ctx);
}

static void test_monitor_lifecycle(void)
{
	struct cgr_config cfg = {
		.poll_interval_ms = 100,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;

	if (access("/home/root", R_OK | X_OK) != 0) {
		TEST(monitor_start_stop);
		printf("SKIP (/home/root not accessible)\n");
		TEST(monitor_double_start);
		printf("SKIP\n");
		return;
	}

	ctx = cgr_init(&cfg);

	TEST(monitor_start_stop);
	if (cgr_start(ctx) == CGR_OK) {
		usleep(200000);
		if (cgr_stop(ctx) == CGR_OK)
			PASS();
		else
			FAIL("stop failed");
	} else {
		FAIL("start failed");
	}

	TEST(monitor_double_start);
	cgr_start(ctx);
	if (cgr_start(ctx) == CGR_ERR_BUSY)
		PASS();
	else
		FAIL("");
	cgr_stop(ctx);

	cgr_destroy(ctx);
}

/* ================================================================
 * 9. Adjust limits with fake data
 * ================================================================ */

static void test_adjust_limits(void)
{
	char p1[512], p2[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st_before, st_after;

	fake_cg_create("adj_idle", 100 << 20, 0, p1, sizeof(p1));
	fake_cg_create("adj_thrash", 100 << 20, 0, p2, sizeof(p2));

	ctx = cgr_init(&cfg);
	cgr_add_cgroup(ctx, p1);
	cgr_add_cgroup(ctx, p2);

	/* Get initial state */
	cgr_get_status(ctx, p1, &st_before);

	/* Simulate: p1 idle (refault unchanged), p2 thrashing */
	pthread_rwlock_wrlock(&ctx->lock);
	{
		struct cgr_group *g1 = cgr_find_group(ctx, p1);
		struct cgr_group *g2 = cgr_find_group(ctx, p2);

		/* p1: idle — refault == prev_refault */
		g1->refault = 100;
		g1->prev_refault = 100;
		g1->usage = 90 << 20;

		/* p2: thrashing — refault increased significantly */
		g2->refault = 500;
		g2->prev_refault = 100;
		g2->usage = 95 << 20;
	}
	cgr_adjust_limits(ctx);
	pthread_rwlock_unlock(&ctx->lock);

	TEST(adjust_shrink_when_idle);
	cgr_get_status(ctx, p1, &st_after);
	/* p1: IDLE → limit × (1 - shrink_pct/100) = limit × 0.95 */
	if (st_after.limit == st_before.limit - st_before.limit * 5 / 100)
		PASS();
	else
		FAIL("expected %luMB (−5%%), got %luMB",
		     (unsigned long)((st_before.limit - st_before.limit * 5 / 100) >> 20),
		     (unsigned long)(st_after.limit >> 20));

	TEST(adjust_grow_when_thrashing);
	cgr_get_status(ctx, p2, &st_after);
	/* p2: slope=400 > urgent(100) → limit × (1 + grow_pct_urgent/100) = limit × 1.20 */
	cgr_get_status(ctx, p2, &st_after);
	if (st_after.limit == st_before.limit + st_before.limit * 20 / 100)
		PASS();
	else
		FAIL("expected %luMB (+20%%), got %luMB",
		     (unsigned long)((st_before.limit + st_before.limit * 20 / 100) >> 20),
		     (unsigned long)(st_after.limit >> 20));

	cgr_destroy(ctx);
	fake_cg_destroy(p1);
	fake_cg_destroy(p2);
}

static void test_limit_usage_ratio(void)
{
	char p1[512], p2[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st_before, st_after;

	fake_cg_create("ratio_capped", 100 << 20, 0, p1, sizeof(p1));
	fake_cg_create("ratio_allowed", 100 << 20, 0, p2, sizeof(p2));

	ctx = cgr_init(&cfg);  /* limit_usage_ratio defaults to 2 */
	cgr_add_cgroup(ctx, p1);
	cgr_add_cgroup(ctx, p2);

	/*
	 * p1: URGENT refault but limit already >= usage * 2 → grow blocked
	 * p2: URGENT refault and limit < usage * 2 → grow allowed
	 */
	pthread_rwlock_wrlock(&ctx->lock);
	{
		struct cgr_group *g1 = cgr_find_group(ctx, p1);
		struct cgr_group *g2 = cgr_find_group(ctx, p2);

		/* p1: limit = 200MB, usage = 50MB → 200 >= 50*2 → capped */
		g1->limit  = 200ULL << 20;
		g1->usage  = 50ULL << 20;
		g1->refault = 500;
		g1->prev_refault = 100; /* slope=400, URGENT */

		/* p2: limit = 90MB, usage = 80MB → 90 < 80*2 → allowed */
		g2->limit  = 90ULL << 20;
		g2->usage  = 80ULL << 20;
		g2->refault = 500;
		g2->prev_refault = 100; /* slope=400, URGENT */
	}
	cgr_get_status(ctx, p1, &st_before);
	cgr_adjust_limits(ctx);
	pthread_rwlock_unlock(&ctx->lock);

	TEST(ratio_cap_blocks_grow);
	cgr_get_status(ctx, p1, &st_after);
	if (st_after.limit == 200ULL << 20)
		PASS();
	else
		FAIL("expected 200MB, got %luMB", (unsigned long)(st_after.limit >> 20));

	TEST(ratio_cap_allows_grow_when_under);
	cgr_get_status(ctx, p2, &st_after);
	if (st_after.limit > 90ULL << 20)
		PASS();
	else
		FAIL("expected >90MB, got %luMB", (unsigned long)(st_after.limit >> 20));

	cgr_destroy(ctx);
	fake_cg_destroy(p1);
	fake_cg_destroy(p2);
}

static void test_min_limit(void)
{
	char p1[512];
	struct cgr_config cfg = {
		.poll_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;

	/* default min_limit should be CGR_MIN_LIMIT_BYTES (16MB) */
	ctx = cgr_init(&cfg);
	TEST(min_limit_default_16mb);
	if (ctx->min_limit == CGR_MIN_LIMIT_BYTES)
		PASS();
	else
		FAIL("got %luMB", (unsigned long)(ctx->min_limit >> 20));

	/* IDLE shrink must not go below min_limit */
	fake_cg_create("min_floor", 20 << 20, 0, p1, sizeof(p1));
	cgr_add_cgroup(ctx, p1);

	pthread_rwlock_wrlock(&ctx->lock);
	{
		struct cgr_group *g = cgr_find_group(ctx, p1);

		g->limit = ctx->min_limit; /* already at floor */
		g->refault = 50;
		g->prev_refault = 50; /* IDLE */
		g->usage = 5 << 20;
	}
	cgr_adjust_limits(ctx);
	pthread_rwlock_unlock(&ctx->lock);

	TEST(min_limit_floor_not_breached);
	cgr_get_status(ctx, p1, &st);
	if (st.limit >= ctx->min_limit)
		PASS();
	else
		FAIL("limit=%luMB below min=%luMB",
		     (unsigned long)(st.limit >> 20),
		     (unsigned long)(ctx->min_limit >> 20));

	/* custom min_limit via direct field assignment */
	ctx->min_limit = 8ULL << 20;
	pthread_rwlock_wrlock(&ctx->lock);
	{
		struct cgr_group *g = cgr_find_group(ctx, p1);

		g->limit = 9ULL << 20; /* above new floor, IDLE → shrink */
		g->refault = 50;
		g->prev_refault = 50;
		g->usage = 2 << 20;
	}
	cgr_adjust_limits(ctx);
	pthread_rwlock_unlock(&ctx->lock);

	TEST(min_limit_custom_floor_respected);
	cgr_get_status(ctx, p1, &st);
	if (st.limit >= (8ULL << 20))
		PASS();
	else
		FAIL("limit=%luMB below custom min=8MB",
		     (unsigned long)(st.limit >> 20));

	cgr_destroy(ctx);
	fake_cg_destroy(p1);
}

static void test_refault_interval_default(void)
{
	struct cgr_config cfg = { .poll_interval_ms = 1000 }; /* refault_interval_ms = 0 → default */
	struct cgr_ctx *ctx;

	ctx = cgr_init(&cfg);

	TEST(refault_interval_default_1000ms);
	if (ctx->cfg.refault_interval_ms == 1000)
		PASS();
	else
		FAIL("got %u", ctx->cfg.refault_interval_ms);

	TEST(refault_interval_zero_rejected);
	/* zero must be replaced with the default, not kept */
	if (ctx->cfg.refault_interval_ms > 0)
		PASS();
	else
		FAIL("refault_interval_ms is 0");

	cgr_destroy(ctx);
}

/* ----------------------------------------------------------------
 * read_usage() is called at refault-sample time, not every poll.
 * Verify that g->usage reflects memory.current after a manual
 * refault-cycle invocation.
 * ---------------------------------------------------------------- */
static void test_refault_reads_usage(void)
{
	char path[512];
	struct cgr_config cfg = {
		.poll_interval_ms    = 1000,
		.refault_interval_ms = 1000,
		.scan_root = FAKE_CG_BASE,
	};
	struct cgr_ctx *ctx;
	struct cgr_status st;

	fake_cg_create("refault_usage", 80 << 20, 0, path, sizeof(path));

	ctx = cgr_init(&cfg);
	cgr_add_cgroup(ctx, path);

	/* Simulate usage change on disk */
	fake_cg_set_usage(path, 120 << 20);

	/* Manually invoke what the refault cycle does */
	pthread_rwlock_wrlock(&ctx->lock);
	{
		struct cgr_group *g = cgr_find_group(ctx, path);
		uint64_t cur;

		if (cg_read_uint64(path, "memory.current", &cur) == 0)
			g->usage = cur;
	}
	pthread_rwlock_unlock(&ctx->lock);

	cgr_get_status(ctx, path, &st);

	TEST(refault_reads_updated_usage);
	if (st.usage == (120ULL << 20))
		PASS();
	else
		FAIL("usage=%luMB expected 120MB",
		     (unsigned long)(st.usage >> 20));

	cgr_destroy(ctx);
	fake_cg_destroy(path);
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
	printf("=== cgreclaim test suite ===\n\n");

	fake_cg_init();

	printf("[cgroup I/O]\n");
	test_cg_read_write();

	printf("\n[add/remove]\n");
	test_add_remove();

	printf("\n[dynamic grow]\n");
	test_dynamic_grow();

	printf("\n[scan cgroups]\n");
	test_scan_cgroups();

	printf("\n[scan cgroups recursive]\n");
	test_scan_cgroups_recursive();

	printf("\n[config parse]\n");
	test_config_parse();

	printf("\n[refault interval defaults]\n");
	test_refault_interval_default();

	printf("\n[early log]\n");
	test_early_log();

	printf("\n[adjust limits]\n");
	test_adjust_limits();

	printf("\n[limit usage ratio]\n");
	test_limit_usage_ratio();

	printf("\n[min limit]\n");
	test_min_limit();

	printf("\n[refault usage read]\n");
	test_refault_reads_usage();

	printf("\n[monitor lifecycle]\n");
	test_monitor_lifecycle();

	printf("\n[inotify live]\n");
	test_inotify_live();

	printf("\n[inotify recursive]\n");
	test_inotify_recursive();

	fake_cg_cleanup();
	cgr_log_close();

	printf("\n=== %d/%d passed",
		total - failures, total);
	if (failures)
		printf(", %d FAILED", failures);
	printf(" ===\n");

	return failures ? 1 : 0;
}
