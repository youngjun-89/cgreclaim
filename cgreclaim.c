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

	ctx->cfg = *cfg;

	if (ctx->cfg.poll_interval_ms == 0)
		ctx->cfg.poll_interval_ms = 1000;

	if (cfg->scan_root)
		snprintf(ctx->scan_root, sizeof(ctx->scan_root),
			 "%s", cfg->scan_root);

	ctx->log_fn = cfg->log_fn;

	pthread_rwlock_init(&ctx->lock, NULL);

	/*
	 * Detect memory.reclaim support by checking the root cgroup.
	 * Not all kernels have this knob (added in 5.19).
	 */
	ctx->reclaim_supported = cg_file_exists("/sys/fs/cgroup",
						"memory.reclaim");

	cgr_log(ctx, CGR_LOG_INFO, "cgr_init: poll=%ums scan_root=%s reclaim_supported=%d",
		ctx->cfg.poll_interval_ms,
		ctx->scan_root[0] ? ctx->scan_root : "(none)",
		ctx->reclaim_supported);

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

int cgr_add_cgroup(struct cgr_ctx *ctx, const char *path)
{
	struct cgr_group *g;
	uint64_t current_max;

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
		return CGR_ERR_FULL;
	}

	/* Read current memory.max — keep it as-is */
	if (cg_read_uint64(path, "memory.max", &current_max) < 0)
		current_max = UINT64_MAX;

	snprintf(g->path, sizeof(g->path), "%s", path);
	g->limit = current_max;
	g->usage = 0;
	g->refault = 0;
	g->prev_refault = 0;
	g->is_foreground = 0;
	g->reclaim_count = 0;
	g->active = 1;
	ctx->nr_groups++;

	cgr_log(ctx, CGR_LOG_INFO, "add_cgroup: %s limit=%lu nr_groups=%d",
		path, (unsigned long)(current_max >> 20), ctx->nr_groups);

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

	/* Reset memory.max to unlimited before removing */
	cg_write_uint64(path, "memory.max", UINT64_MAX);

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

int cgr_scan_cgroups(struct cgr_ctx *ctx)
{
	DIR *dir;
	struct dirent *de;
	char child_path[512];
	struct stat st;
	int found = 0;

	if (!ctx || !ctx->scan_root[0])
		return CGR_ERR_INVAL;

	dir = opendir(ctx->scan_root);
	if (!dir)
		return CGR_ERR_IO;

	/* Discover and index child cgroups, preserving their current limits */
	while ((de = readdir(dir)) != NULL) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(child_path, sizeof(child_path), "%s/%s",
			 ctx->scan_root, de->d_name);

		if (stat(child_path, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		if (!is_memory_cgroup(child_path))
			continue;

		if (cgr_add_cgroup(ctx, child_path) == CGR_OK)
			found++;

		if (ctx->nr_groups >= CGR_MAX_GROUPS)
			break;
	}

	closedir(dir);

	cgr_log(ctx, CGR_LOG_INFO, "scan_cgroups: found %d under %s",
		found, ctx->scan_root);

	return found;
}
