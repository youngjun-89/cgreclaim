#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgroup.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Refault sampling interval: check memory.stat every N polls
 * to avoid per-poll overhead of parsing memory.stat.
 */
#define REFAULT_SAMPLE_INTERVAL	5

/*
 * When using memory.high fallback, wait this long for the kernel
 * to reclaim before restoring memory.high.
 */
#define HIGH_FALLBACK_WAIT_MS	100

/* ---------- reclaim ---------- */

/*
 * Perform proactive reclaim to bring a cgroup's usage down to target.
 * Called with ctx->lock held for WRITE.
 */
int cgr_do_reclaim(struct cgr_ctx *ctx, struct cgr_group *g, uint64_t target)
{
	uint64_t to_reclaim;
	int ret;

	if (g->usage <= target)
		return 0;

	to_reclaim = g->usage - target;

	if (ctx->reclaim_supported) {
		ret = cg_write_reclaim(g->path, to_reclaim);
	} else {
		/*
		 * Fallback: temporarily lower memory.high to target.
		 * The kernel will reclaim pages above memory.high.
		 * Then restore memory.high to max (unlimited).
		 */
		ret = cg_write_uint64(g->path, "memory.high", target);
		if (ret == 0) {
			struct timespec ts = {
				.tv_sec = HIGH_FALLBACK_WAIT_MS / 1000,
				.tv_nsec = (HIGH_FALLBACK_WAIT_MS % 1000) * 1000000L,
			};
			nanosleep(&ts, NULL);
			cg_write_uint64(g->path, "memory.high", UINT64_MAX);
		}
	}

	if (ret == 0)
		g->reclaim_count++;

	return ret;
}

/* ---------- rebalance ---------- */

/*
 * Redistribute memory.max across all managed cgroups.
 * Foreground cgroup(s) get fg_ratio of the pool.
 * Remaining pool is split equally among background cgroups.
 * Each cgroup's limit is clamped to >= CGR_MIN_LIMIT_BYTES.
 *
 * Called with ctx->lock held for WRITE.
 */
void cgr_rebalance(struct cgr_ctx *ctx)
{
	uint64_t pool = ctx->cfg.total_pool;
	uint64_t fg_budget, bg_budget, bg_each;
	int fg_count = 0, bg_count = 0;
	int i;

	/* Count foreground/background groups */
	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		if (!ctx->groups[i].active)
			continue;
		if (ctx->groups[i].is_foreground)
			fg_count++;
		else
			bg_count++;
	}

	if (fg_count == 0 && bg_count == 0)
		return;

	/*
	 * If no foreground is set, distribute equally.
	 * Otherwise, foreground gets fg_ratio, rest split among background.
	 */
	if (fg_count == 0) {
		bg_each = pool / (bg_count > 0 ? bg_count : 1);
		if (bg_each < CGR_MIN_LIMIT_BYTES)
			bg_each = CGR_MIN_LIMIT_BYTES;

		for (i = 0; i < CGR_MAX_GROUPS; i++) {
			if (!ctx->groups[i].active)
				continue;
			ctx->groups[i].limit = bg_each;
		}
	} else {
		fg_budget = (uint64_t)(pool * ctx->cfg.fg_ratio);
		bg_budget = pool - fg_budget;

		/* Split fg_budget among foreground cgroups */
		uint64_t fg_each = fg_budget / fg_count;
		if (fg_each < CGR_MIN_LIMIT_BYTES)
			fg_each = CGR_MIN_LIMIT_BYTES;

		/* Split bg_budget among background cgroups */
		if (bg_count > 0) {
			bg_each = bg_budget / bg_count;
			if (bg_each < CGR_MIN_LIMIT_BYTES)
				bg_each = CGR_MIN_LIMIT_BYTES;
		} else {
			bg_each = 0;
		}

		for (i = 0; i < CGR_MAX_GROUPS; i++) {
			if (!ctx->groups[i].active)
				continue;
			if (ctx->groups[i].is_foreground)
				ctx->groups[i].limit = fg_each;
			else
				ctx->groups[i].limit = bg_each;
		}
	}

	/* Apply limits — reclaim first if usage exceeds new limit */
	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		struct cgr_group *g = &ctx->groups[i];

		if (!g->active)
			continue;

		if (g->usage > g->limit)
			cgr_do_reclaim(ctx, g, g->limit);

		cg_write_uint64(g->path, "memory.max", g->limit);
	}
}

/* ---------- monitor thread ---------- */

/*
 * Idle detection based on workingset refault counters.
 * A cgroup is idle if no refaults occurred between two samples,
 * meaning its working set fits within its allocation and nothing
 * evicted is being re-accessed.
 */
static int is_idle(struct cgr_group *g)
{
	return g->refault == g->prev_refault;
}

/*
 * Sample refault counters from memory.stat for all active cgroups.
 * Called every REFAULT_SAMPLE_INTERVAL polls to limit overhead.
 */
static void sample_refault(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t rf;

		if (!g->active)
			continue;

		g->prev_refault = g->refault;

		if (cg_read_refault(g->path, &rf) == 0)
			g->refault = rf;
	}
}

static void poll_usage(struct cgr_ctx *ctx)
{
	int i;
	int need_rebalance = 0;
	int do_refault_sample;

	pthread_rwlock_wrlock(&ctx->lock);

	ctx->poll_count++;
	do_refault_sample = (ctx->poll_count >= REFAULT_SAMPLE_INTERVAL);

	if (do_refault_sample) {
		ctx->poll_count = 0;
		sample_refault(ctx);
	}

	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t current;

		if (!g->active)
			continue;

		if (cg_read_uint64(g->path, "memory.current", &current) == 0)
			g->usage = current;

		/*
		 * Only evaluate idle/active transitions on refault
		 * sample boundaries, when we have fresh data.
		 */
		if (!do_refault_sample)
			continue;

		if (!g->is_foreground && !is_idle(g))
			need_rebalance = 1;
		else if (g->is_foreground && is_idle(g))
			need_rebalance = 1;
	}

	if (need_rebalance)
		cgr_rebalance(ctx);

	pthread_rwlock_unlock(&ctx->lock);
}

static void *monitor_thread(void *arg)
{
	struct cgr_ctx *ctx = arg;
	struct timespec ts;
	unsigned int ms;

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
		return CGR_ERR_IO;
	}

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
	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		if (ctx->groups[i].active)
			ctx->groups[i].is_foreground = 0;
	}
	g->is_foreground = 1;

	/* Immediately rebalance */
	cgr_rebalance(ctx);

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

	g->limit = new_limit;

	/* Reclaim if current usage exceeds new limit */
	if (g->usage > new_limit)
		cgr_do_reclaim(ctx, g, new_limit);

	ret = cg_write_uint64(path, "memory.max", new_limit);

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
