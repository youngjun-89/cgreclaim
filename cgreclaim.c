#define _GNU_SOURCE
#include "cgreclaim.h"
#include "cgroup.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Internal per-cgroup state */
struct cgr_group {
	char		path[256];
	uint64_t	limit;		/* current memory.max target */
	uint64_t	usage;		/* last polled memory.current */
	uint64_t	prev_usage;	/* previous poll (for idle detection) */
	int		is_foreground;
	uint64_t	reclaim_count;
	int		active;		/* slot in use */
};

/* Library context */
struct cgr_ctx {
	struct cgr_config	cfg;
	struct cgr_group	groups[CGR_MAX_GROUPS];
	int			nr_groups;	/* number of active groups */

	pthread_rwlock_t	lock;
	pthread_t		monitor_tid;
	volatile int		running;	/* monitor thread flag */
	int			reclaim_supported;

	/* log helper */
	void (*log_fn)(int level, const char *fmt, ...);
};

/* ---------- internal helpers ---------- */

static struct cgr_group *find_group(struct cgr_ctx *ctx, const char *path)
{
	int i;

	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		if (ctx->groups[i].active &&
		    strcmp(ctx->groups[i].path, path) == 0)
			return &ctx->groups[i];
	}
	return NULL;
}

static struct cgr_group *find_free_slot(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < CGR_MAX_GROUPS; i++) {
		if (!ctx->groups[i].active)
			return &ctx->groups[i];
	}
	return NULL;
}

__attribute__((unused))
static void log_msg(struct cgr_ctx *ctx, int level, const char *fmt, ...)
{
	va_list ap;

	if (!ctx->log_fn)
		return;

	va_start(ap, fmt);
	/* Forward to callback — caller expects variadic, so we call directly */
	ctx->log_fn(level, fmt, ap);
	va_end(ap);
}

/* ---------- lifecycle ---------- */

struct cgr_ctx *cgr_init(const struct cgr_config *cfg)
{
	struct cgr_ctx *ctx;

	if (!cfg || cfg->total_pool == 0)
		return NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->cfg = *cfg;

	/* defaults */
	if (ctx->cfg.poll_interval_ms == 0)
		ctx->cfg.poll_interval_ms = 1000;
	if (ctx->cfg.fg_ratio <= 0.0 || ctx->cfg.fg_ratio >= 1.0)
		ctx->cfg.fg_ratio = 0.6;

	ctx->log_fn = cfg->log_fn;

	pthread_rwlock_init(&ctx->lock, NULL);

	/*
	 * Detect memory.reclaim support by checking the root cgroup.
	 * Not all kernels have this knob (added in 5.19).
	 */
	ctx->reclaim_supported = cg_file_exists("/sys/fs/cgroup",
						"memory.reclaim");

	return ctx;
}

void cgr_destroy(struct cgr_ctx *ctx)
{
	if (!ctx)
		return;

	if (ctx->running)
		cgr_stop(ctx);

	pthread_rwlock_destroy(&ctx->lock);
	free(ctx);
}

/* ---------- cgroup management ---------- */

int cgr_add_cgroup(struct cgr_ctx *ctx, const char *path, uint64_t initial_limit)
{
	struct cgr_group *g;
	int ret;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	if (initial_limit < CGR_MIN_LIMIT_BYTES)
		initial_limit = CGR_MIN_LIMIT_BYTES;

	pthread_rwlock_wrlock(&ctx->lock);

	if (find_group(ctx, path)) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_EXIST;
	}

	g = find_free_slot(ctx);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_FULL;
	}

	snprintf(g->path, sizeof(g->path), "%s", path);
	g->limit = initial_limit;
	g->usage = 0;
	g->prev_usage = 0;
	g->is_foreground = 0;
	g->reclaim_count = 0;
	g->active = 1;
	ctx->nr_groups++;

	pthread_rwlock_unlock(&ctx->lock);

	/* Apply memory.max immediately */
	ret = cg_write_uint64(path, "memory.max", initial_limit);
	if (ret < 0)
		return CGR_ERR_IO;

	return CGR_OK;
}

int cgr_remove_cgroup(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_group *g;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	pthread_rwlock_wrlock(&ctx->lock);

	g = find_group(ctx, path);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOENT;
	}

	/* Reset memory.max to unlimited before removing */
	cg_write_uint64(path, "memory.max", UINT64_MAX);

	memset(g, 0, sizeof(*g));
	ctx->nr_groups--;

	pthread_rwlock_unlock(&ctx->lock);

	return CGR_OK;
}
