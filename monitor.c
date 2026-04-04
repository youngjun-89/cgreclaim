#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgr_config.h"
#include "cgroup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Directory that must be mounted before config/log are available */
#define MOUNT_WAIT_PATH	"/home/root"

/*
 * Refault sampling interval: check memory.stat every N polls
 * to avoid per-poll overhead of parsing memory.stat.
 */
#define REFAULT_SAMPLE_INTERVAL	5

/*
 * Rescan interval: how often (in polls) to rescan scan_root for
 * newly appeared cgroups.  With default 1s poll this is ~30 seconds.
 */
#define RESCAN_INTERVAL		30

/* ---------- thrashing detection ---------- */

/*
 * Classify refault pressure by computing the slope (delta refaults per
 * sample window) and comparing against runtime-tunable thresholds
 * stored in ctx (loaded from /home/root/cgreclaim config file).
 *
 * IDLE     — no new refaults; working set fits within memory.high
 * MODERATE — slow growth; some pages are being re-faulted
 * URGENT   — rapid growth; working set significantly exceeds limit
 */
static enum cgr_refault_urgency refault_urgency(const struct cgr_ctx *ctx,
						const struct cgr_group *g)
{
	uint64_t slope;

	if (g->refault <= g->prev_refault)
		return CGR_REFAULT_IDLE;

	slope = g->refault - g->prev_refault;

	if (slope >= ctx->refault_slope_urgent)
		return CGR_REFAULT_URGENT;
	if (slope >= ctx->refault_slope_moderate)
		return CGR_REFAULT_MODERATE;

	return CGR_REFAULT_IDLE;
}

/*
 * Sample refault counters from memory.stat for all active cgroups.
 * Called every REFAULT_SAMPLE_INTERVAL polls to limit overhead.
 */
static void sample_refault(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t rf;

		if (!g->active)
			continue;

		g->prev_refault = g->refault;

		if (cg_read_refault(g->path, &rf) == 0)
			g->refault = rf;
	}
}

/* ---------- adaptive limit adjustment ---------- */

/*
 * Per-cgroup growth/shrink factors.
 * Grow faster than shrink to react quickly to pressure while
 * avoiding oscillation on the way down.
 */
#define GROW_FACTOR_URGENT	1.20	/* +20% on severe refault slope */
#define GROW_FACTOR_MODERATE	1.10	/* +10% on mild refault slope */
#define SHRINK_FACTOR		0.95	/* -5%  when idle (no refaults) */

static const char *urgency_str[] = { "IDLE", "MODERATE", "URGENT" };

/*
 * Adjust memory.high for each cgroup independently based on refault.
 * memory.high is always decreased to apply steady reclaim pressure.
 * When thrashing is detected, memory.high is raised instead to relieve
 * the working set.
 *
 * Never shrinks below CGR_MIN_LIMIT_BYTES.
 * memory.high is a soft limit; the kernel reclaims gradually.
 *
 * Called with ctx->lock held for WRITE.
 */
void cgr_adjust_limits(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		enum cgr_refault_urgency urgency;
		uint64_t slope, new_limit;

		if (!g->active)
			continue;

		urgency = refault_urgency(ctx, g);
		slope = (g->refault > g->prev_refault)
			? g->refault - g->prev_refault : 0;

		switch (urgency) {
		case CGR_REFAULT_URGENT:
			new_limit = (uint64_t)(g->limit * GROW_FACTOR_URGENT);
			if (new_limit == g->limit)
				new_limit = g->limit + CGR_MIN_LIMIT_BYTES;
			break;
		case CGR_REFAULT_MODERATE:
			new_limit = (uint64_t)(g->limit * GROW_FACTOR_MODERATE);
			if (new_limit == g->limit)
				new_limit = g->limit + CGR_MIN_LIMIT_BYTES;
			break;
		default: /* CGR_REFAULT_IDLE */
			new_limit = (uint64_t)(g->limit * SHRINK_FACTOR);
			break;
		}

		if (new_limit < CGR_MIN_LIMIT_BYTES)
			new_limit = CGR_MIN_LIMIT_BYTES;

		if (new_limit == g->limit)
			continue;

		cgr_log(ctx, CGR_LOG_INFO,
			"adjust: %s [%s slope=%lu] %luMB -> %luMB (usage=%luMB)",
			g->path,
			urgency_str[urgency],
			(unsigned long)slope,
			(unsigned long)(g->limit >> 20),
			(unsigned long)(new_limit >> 20),
			(unsigned long)(g->usage >> 20));

		g->limit = new_limit;

		cg_write_uint64(g->path, "memory.high", new_limit);
	}
}

/* ---------- monitor thread ---------- */

/*
 * Forward declaration — defined in cgreclaim.c.
 * Rescans scan_root for new cgroups; already-registered ones are skipped.
 */
int cgr_scan_cgroups(struct cgr_ctx *ctx);

static void poll_usage(struct cgr_ctx *ctx)
{
	int i;
	int do_refault_sample;
	int do_rescan;

	pthread_rwlock_wrlock(&ctx->lock);

	ctx->poll_count++;
	do_refault_sample = (ctx->poll_count >= REFAULT_SAMPLE_INTERVAL);

	if (do_refault_sample) {
		ctx->poll_count = 0;
		sample_refault(ctx);
	}

	/* Periodically rescan for newly appeared cgroups */
	ctx->rescan_count++;
	do_rescan = (ctx->rescan_count >= RESCAN_INTERVAL && ctx->scan_root[0]);
	if (do_rescan)
		ctx->rescan_count = 0;

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t current;

		if (!g->active)
			continue;

		if (cg_read_uint64(g->path, "memory.current", &current) == 0)
			g->usage = current;
	}

	/* Adjust limits only on refault sample boundaries */
	if (do_refault_sample) {
		cgr_log(ctx, CGR_LOG_DEBUG, "poll: refault sample #%u, adjusting limits",
			ctx->poll_count);
		cgr_adjust_limits(ctx);
	}

	pthread_rwlock_unlock(&ctx->lock);

	/*
	 * Rescan outside the lock — cgr_scan_cgroups() calls
	 * cgr_add_cgroup() which takes its own wrlock.
	 * Also reload config file on the same interval.
	 */
	if (do_rescan) {
		int found;

		cgr_config_load(ctx);

		found = cgr_scan_cgroups(ctx);
		if (found > 0)
			cgr_log(ctx, CGR_LOG_INFO,
				"rescan: discovered %d new cgroup(s)", found);
	}
}

static int is_mount_ready(void)
{
	struct stat st;

	return stat(MOUNT_WAIT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static void *monitor_thread(void *arg)
{
	struct cgr_ctx *ctx = arg;
	struct timespec ts;
	unsigned int ms;

	/*
	 * Wait for /home/root to become available before starting
	 * the adjustment loop.  Config file and log file live there.
	 * Logs generated before mount are buffered and flushed later.
	 */
	cgr_log(ctx, CGR_LOG_INFO, "monitor: waiting for %s", MOUNT_WAIT_PATH);
	while (ctx->running && !is_mount_ready()) {
		ts.tv_sec = 1;
		ts.tv_nsec = 0;
		nanosleep(&ts, NULL);
	}

	if (ctx->running) {
		cgr_log(ctx, CGR_LOG_INFO, "monitor: %s available, loading config",
			MOUNT_WAIT_PATH);
		cgr_config_load(ctx);

		/* Initial scan after config is loaded */
		if (ctx->scan_root[0])
			cgr_scan_cgroups(ctx);
	}

	while (ctx->running) {
		poll_usage(ctx);

		ms = ctx->cfg.poll_interval_ms;
		ts.tv_sec = ms / 1000;
		ts.tv_nsec = (ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);
	}

	return NULL;
}

/* ---------- public API ---------- */

int cgr_start(struct cgr_ctx *ctx)
{
	int ret;

	if (!ctx)
		return CGR_ERR_INVAL;

	if (ctx->running)
		return CGR_ERR_BUSY;

	ctx->running = 1;

	ret = pthread_create(&ctx->monitor_tid, NULL, monitor_thread, ctx);
	if (ret != 0) {
		ctx->running = 0;
		cgr_log(ctx, CGR_LOG_ERR, "monitor: failed to create thread");
		return CGR_ERR_IO;
	}

	cgr_log(ctx, CGR_LOG_INFO, "monitor: started (poll_interval=%ums)",
		ctx->cfg.poll_interval_ms);

	return CGR_OK;
}

int cgr_stop(struct cgr_ctx *ctx)
{
	if (!ctx)
		return CGR_ERR_INVAL;

	if (!ctx->running)
		return CGR_OK;

	ctx->running = 0;
	pthread_join(ctx->monitor_tid, NULL);

	cgr_log(ctx, CGR_LOG_INFO, "monitor: stopped");

	return CGR_OK;
}

int cgr_set_foreground(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_group *g;
	int i;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	pthread_rwlock_wrlock(&ctx->lock);

	g = cgr_find_group(ctx, path);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOENT;
	}

	/* Clear all foreground flags, set the new one */
	for (i = 0; i < ctx->groups_cap; i++) {
		if (ctx->groups[i].active)
			ctx->groups[i].is_foreground = 0;
	}
	g->is_foreground = 1;

	cgr_log(ctx, CGR_LOG_INFO, "set_foreground: %s", path);

	pthread_rwlock_unlock(&ctx->lock);

	return CGR_OK;
}

int cgr_set_limit(struct cgr_ctx *ctx, const char *path, uint64_t new_limit)
{
	struct cgr_group *g;
	int ret;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	if (new_limit < CGR_MIN_LIMIT_BYTES)
		new_limit = CGR_MIN_LIMIT_BYTES;

	pthread_rwlock_wrlock(&ctx->lock);

	g = cgr_find_group(ctx, path);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOENT;
	}

	cgr_log(ctx, CGR_LOG_INFO, "set_limit: %s %luMB -> %luMB",
		path, (unsigned long)(g->limit >> 20),
		(unsigned long)(new_limit >> 20));

	g->limit = new_limit;

	ret = cg_write_uint64(path, "memory.high", new_limit);

	pthread_rwlock_unlock(&ctx->lock);

	return ret < 0 ? CGR_ERR_IO : CGR_OK;
}

int cgr_get_status(struct cgr_ctx *ctx, const char *path, struct cgr_status *out)
{
	struct cgr_group *g;

	if (!ctx || !path || !out)
		return CGR_ERR_INVAL;

	pthread_rwlock_rdlock(&ctx->lock);

	g = cgr_find_group(ctx, path);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOENT;
	}

	snprintf(out->path, sizeof(out->path), "%s", g->path);
	out->limit = g->limit;
	out->usage = g->usage;
	out->is_foreground = g->is_foreground;
	out->reclaim_count = g->reclaim_count;

	pthread_rwlock_unlock(&ctx->lock);

	return CGR_OK;
}
