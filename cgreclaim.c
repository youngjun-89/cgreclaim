#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgroup.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>

/* ---------- internal helpers (shared) ---------- */

struct cgr_group *cgr_find_group(struct cgr_ctx *ctx, const char *path)
{
	int i;

	for (i = 0; i < ctx->groups_cap; i++) {
		if (ctx->groups[i].active &&
		    strcmp(ctx->groups[i].path, path) == 0)
			return &ctx->groups[i];
	}
	return NULL;
}

static struct cgr_group *find_free_slot(struct cgr_ctx *ctx)
{
	struct cgr_group *new_groups;
	int i, new_cap, old_cap;

	for (i = 0; i < ctx->groups_cap; i++) {
		if (!ctx->groups[i].active)
			return &ctx->groups[i];
	}

	/* No free slot — grow the array */
	old_cap = ctx->groups_cap;
	new_cap = old_cap * 2;
	new_groups = realloc(ctx->groups, new_cap * sizeof(*new_groups));
	if (!new_groups)
		return NULL;

	memset(new_groups + old_cap, 0, old_cap * sizeof(*new_groups));
	ctx->groups = new_groups;
	ctx->groups_cap = new_cap;

	return &ctx->groups[old_cap];
}

/* ---------- utility ---------- */

uint64_t cgr_get_total_ram(void)
{
	struct sysinfo si;

	if (sysinfo(&si) < 0)
		return 0;

	return (uint64_t)si.totalram * si.mem_unit;
}

/* ---------- lifecycle ---------- */

struct cgr_ctx *cgr_init(const struct cgr_config *cfg)
{
	struct cgr_ctx *ctx;

	if (!cfg)
		return NULL;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->groups = calloc(CGR_GROUPS_INIT_CAP, sizeof(*ctx->groups));
	if (!ctx->groups) {
		free(ctx);
		return NULL;
	}
	ctx->groups_cap = CGR_GROUPS_INIT_CAP;

	ctx->cfg = *cfg;

	if (ctx->cfg.poll_interval_ms == 0)
		ctx->cfg.poll_interval_ms = 1000;

	if (ctx->cfg.refault_interval_ms == 0)
		ctx->cfg.refault_interval_ms = 1000;
	if (cfg->scan_root)
		snprintf(ctx->scan_root, sizeof(ctx->scan_root),
			 "%s", cfg->scan_root);

	ctx->log_fn = cfg->log_fn;

	ctx->refault_slope_moderate = 10;
	ctx->refault_slope_urgent = 100;
	ctx->min_limit = CGR_MIN_LIMIT_BYTES;
	ctx->limit_usage_ratio = 2;

	pthread_rwlock_init(&ctx->lock, NULL);

	cgr_log(ctx, CGR_LOG_INFO, "cgr_init: poll=%ums scan_root=%s",
		ctx->cfg.poll_interval_ms,
		ctx->scan_root[0] ? ctx->scan_root : "(none)");

	return ctx;
}

void cgr_destroy(struct cgr_ctx *ctx)
{
	if (!ctx)
		return;

	if (ctx->running)
		cgr_stop(ctx);

	pthread_rwlock_destroy(&ctx->lock);
	free(ctx->groups);
	free(ctx);
}

/* ---------- cgroup management ---------- */

int cgr_add_cgroup(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_group *g;
	uint64_t current_usage, initial_high;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	pthread_rwlock_wrlock(&ctx->lock);

	if (cgr_find_group(ctx, path)) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_EXIST;
	}

	g = find_free_slot(ctx);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOMEM;
	}

	/*
	 * Start from current usage + 10% headroom rather than
	 * keeping whatever memory.high was (often "max").
	 * The adaptive loop will grow/shrink from here.
	 */
	if (cg_read_uint64(path, "memory.current", &current_usage) < 0)
		current_usage = ctx->min_limit;

	initial_high = (uint64_t)(current_usage * 1.10);
	if (initial_high < ctx->min_limit)
		initial_high = ctx->min_limit;

	cg_write_uint64(path, "memory.high", initial_high);

	snprintf(g->path, sizeof(g->path), "%s", path);
	g->limit = initial_high;
	g->usage = current_usage;
	g->refault = 0;
	g->prev_refault = 0;
	g->is_foreground = 0;
	g->reclaim_count = 0;
	g->active = 1;
	ctx->nr_groups++;

	cgr_log(ctx, CGR_LOG_INFO, "add_cgroup: %s usage=%luMB high=%luMB nr_groups=%d",
		path, (unsigned long)(current_usage >> 20),
		(unsigned long)(initial_high >> 20), ctx->nr_groups);

	pthread_rwlock_unlock(&ctx->lock);

	return CGR_OK;
}

int cgr_remove_cgroup(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_group *g;

	if (!ctx || !path)
		return CGR_ERR_INVAL;

	pthread_rwlock_wrlock(&ctx->lock);

	g = cgr_find_group(ctx, path);
	if (!g) {
		pthread_rwlock_unlock(&ctx->lock);
		return CGR_ERR_NOENT;
	}

	/* Reset memory.high to unlimited before removing */
	cg_write_uint64(path, "memory.high", UINT64_MAX);

	cgr_log(ctx, CGR_LOG_INFO, "remove_cgroup: %s nr_groups=%d",
		path, ctx->nr_groups - 1);

	memset(g, 0, sizeof(*g));
	ctx->nr_groups--;

	pthread_rwlock_unlock(&ctx->lock);

	return CGR_OK;
}

/* ---------- auto-discovery ---------- */

/*
 * Check if a directory is a cgroup with memory controller enabled
 * by looking for memory.current (present when memory controller is active).
 */
static int is_memory_cgroup(const char *path)
{
	return cg_file_exists(path, "memory.current");
}

/*
 * Recursively scan dir for memory cgroups.  Adds each discovered cgroup
 * and descends into it.  dir itself is NOT added (caller's responsibility).
 *
 * In cgroup v2 the memory controller must be enabled in a parent before any
 * child can use it, so a directory without memory.current will never contain
 * memory-enabled descendants — safe to prune early.
 */
static int scan_dir_recursive(struct cgr_ctx *ctx, const char *dir)
{
	DIR *d;
	struct dirent *de;
	char child[512];
	struct stat st;
	int found = 0;

	d = opendir(dir);
	if (!d)
		return 0;

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);

		if (stat(child, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		if (!is_memory_cgroup(child))
			continue;

		if (cgr_add_cgroup(ctx, child) == CGR_OK)
			found++;

		/* Descend regardless: subtree may have more tracked children */
		found += scan_dir_recursive(ctx, child);
	}

	closedir(d);
	return found;
}

int cgr_scan_cgroups(struct cgr_ctx *ctx)
{
	int found;

	if (!ctx || !ctx->scan_root[0])
		return CGR_ERR_INVAL;

	found = scan_dir_recursive(ctx, ctx->scan_root);

	cgr_log(ctx, CGR_LOG_INFO, "scan_cgroups: found %d under %s",
		found, ctx->scan_root);

	return found;
}
