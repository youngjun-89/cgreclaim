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

/* ---------- idle detection ---------- */

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
#define GROW_FACTOR	1.10	/* +10% when refault detected */
#define SHRINK_FACTOR	0.98	/* -2% when idle (was 5%, too aggressive) */

/*
 * Headroom above current usage when shrinking.
 * Prevents setting memory.max exactly at usage, which would
 * trigger OOM kills from small transient allocations.
 */
#define SHRINK_HEADROOM	1.10	/* keep 10% headroom above usage */

/*
 * Adjust memory.max for each cgroup independently based on refault.
 *   refault detected → grow limit (working set exceeds allocation)
 *   no refault       → shrink limit (has headroom to give back)
 *
 * Never shrinks below current usage (avoids unnecessary reclaim)
 * or CGR_MIN_LIMIT_BYTES, whichever is larger.
 *
 * Called with ctx->lock held for WRITE.
 */
void cgr_adjust_limits(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t new_limit;
		uint64_t floor;

		if (!g->active)
			continue;

		if (!is_idle(g)) {
			/* Pressure — grow */
			if (g->limit > UINT64_MAX / 2) {
				/* Avoid overflow for very large limits */
				new_limit = UINT64_MAX;
			} else {
				new_limit = (uint64_t)(g->limit * GROW_FACTOR);
				if (new_limit == g->limit)
					new_limit = g->limit + CGR_MIN_LIMIT_BYTES;
			}
		} else {
			/* Idle — shrink */
			new_limit = (uint64_t)(g->limit * SHRINK_FACTOR);

			/*
			 * Never shrink below current usage + headroom.
			 * Using stale usage without headroom triggers OOM
			 * kills from transient allocations between polls.
			 */
			floor = (uint64_t)(g->usage * SHRINK_HEADROOM);
			if (floor < CGR_MIN_LIMIT_BYTES)
				floor = CGR_MIN_LIMIT_BYTES;
			if (new_limit < floor)
				new_limit = floor;
		}

		if (new_limit < CGR_MIN_LIMIT_BYTES)
			new_limit = CGR_MIN_LIMIT_BYTES;

		if (new_limit == g->limit)
			continue;

		cgr_log(ctx, CGR_LOG_INFO,
			"adjust: %s %s %luMB -> %luMB (usage=%luMB refault=%lu prev_refault=%lu)",
			g->path,
			!is_idle(g) ? "GROW" : "SHRINK",
			(unsigned long)(g->limit >> 20),
			(unsigned long)(new_limit >> 20),
			(unsigned long)(g->usage >> 20),
			(unsigned long)g->refault,
			(unsigned long)g->prev_refault);

		g->limit = new_limit;

		cg_write_uint64(g->path, "memory.max", new_limit);
	}
}

/* ---------- monitor thread ---------- */

static void poll_usage(struct cgr_ctx *ctx)
{
	int i;
	int do_refault_sample;

	pthread_rwlock_wrlock(&ctx->lock);

	ctx->poll_count++;
	do_refault_sample = (ctx->poll_count >= REFAULT_SAMPLE_INTERVAL);

	if (do_refault_sample) {
		ctx->poll_count = 0;
		sample_refault(ctx);
	}

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
