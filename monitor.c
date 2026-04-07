#define _GNU_SOURCE
#include "cgreclaim_internal.h"
#include "cgr_config.h"
#include "cgroup.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Directory that must be mounted before config/log are available */
#define MOUNT_WAIT_PATH	"/home/root"

/*
 * Config reload interval: how often (in polls) to re-read the
 * runtime configuration file.  With default 1s poll this is ~30s.
 */
#define CONFIG_RELOAD_INTERVAL	30

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
				new_limit = g->limit + ctx->min_limit;
			break;
		case CGR_REFAULT_MODERATE:
			new_limit = (uint64_t)(g->limit * GROW_FACTOR_MODERATE);
			if (new_limit == g->limit)
				new_limit = g->limit + ctx->min_limit;
			break;
		default: /* CGR_REFAULT_IDLE */
			new_limit = (uint64_t)(g->limit * SHRINK_FACTOR);
			break;
		}

		if (new_limit < ctx->min_limit)
			new_limit = ctx->min_limit;

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

/* Forward declarations — defined in cgreclaim.c. */
int cgr_scan_cgroups(struct cgr_ctx *ctx);
int cgr_remove_cgroup(struct cgr_ctx *ctx, const char *path);

/*
 * Read memory.current for all active cgroups.
 * Called with ctx->lock held for WRITE.
 */
static void read_usage(struct cgr_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t current;

		if (!g->active)
			continue;

		if (cg_read_uint64(g->path, "memory.current", &current) == 0)
			g->usage = current;
	}
}

/*
 * Check if /home/root is actually accessible.
 * The directory itself is not a mount point — when the parent
 * filesystem (/home) is not yet mounted, the entire path is
 * unreachable.  Use access() which fails with ENOENT or EACCES
 * in that case.
 */
static int is_path_accessible(void)
{
	return access(MOUNT_WAIT_PATH, R_OK | X_OK) == 0;
}

/* ---------- inotify watch table ---------- */

/*
 * Each watched directory gets an entry in the watch table so that
 * inotify events can be mapped back to an absolute path.  The table
 * grows dynamically like the groups array.
 */
#define WATCH_CAP_INIT	32

struct watch_entry {
	int  wd;
	char path[256];	/* same bound as cgr_group.path */
};

struct inotify_state {
	int		     ifd;
	struct watch_entry  *entries;
	int		     nr;
	int		     cap;
};

static void inotify_state_init(struct inotify_state *is)
{
	memset(is, 0, sizeof(*is));
	is->ifd = -1;
}

static void inotify_state_free(struct inotify_state *is)
{
	if (is->ifd >= 0)
		close(is->ifd);
	free(is->entries);
	memset(is, 0, sizeof(*is));
	is->ifd = -1;
}

/* Add an inotify watch on path and record the wd→path mapping. */
static int inotify_add_dir(struct cgr_ctx *ctx, struct inotify_state *is,
			   const char *path)
{
	struct watch_entry *e, *new_entries;
	int i, wd, new_cap;

	/* Skip if already watched */
	for (i = 0; i < is->nr; i++) {
		if (strcmp(is->entries[i].path, path) == 0)
			return 0;
	}

	wd = inotify_add_watch(is->ifd, path,
			       IN_CREATE | IN_MOVED_TO |
			       IN_DELETE | IN_MOVED_FROM | IN_ONLYDIR);
	if (wd < 0) {
		cgr_log(ctx, CGR_LOG_ERR, "inotify: watch %s failed: %s",
			path, strerror(errno));
		return -1;
	}

	if (is->nr == is->cap) {
		new_cap = is->cap ? is->cap * 2 : WATCH_CAP_INIT;
		new_entries = realloc(is->entries,
				      new_cap * sizeof(*new_entries));
		if (!new_entries) {
			inotify_rm_watch(is->ifd, wd);
			return -1;
		}
		is->entries = new_entries;
		is->cap     = new_cap;
	}

	e = &is->entries[is->nr++];
	e->wd = wd;
	snprintf(e->path, sizeof(e->path), "%s", path);

	cgr_log(ctx, CGR_LOG_DEBUG, "inotify: watch added %s (wd=%d)", path, wd);
	return 0;
}

/* Return the path associated with wd, or NULL if not found. */
static const char *inotify_path_for_wd(const struct inotify_state *is, int wd)
{
	int i;

	for (i = 0; i < is->nr; i++) {
		if (is->entries[i].wd == wd)
			return is->entries[i].path;
	}
	return NULL;
}

/*
 * Remove the watch table entry for wd.
 * Called when IN_IGNORED arrives (the kernel already dropped the watch).
 */
static void inotify_cleanup_wd(struct inotify_state *is, int wd)
{
	int i;

	for (i = 0; i < is->nr; i++) {
		if (is->entries[i].wd == wd) {
			is->entries[i] = is->entries[is->nr - 1];
			is->nr--;
			return;
		}
	}
}

/*
 * Explicitly remove the watch for path (used for IN_MOVED_FROM where the
 * directory still exists at the new location so the kernel does not
 * auto-remove the watch).  inotify_rm_watch triggers IN_IGNORED which
 * will call inotify_cleanup_wd.
 */
static void inotify_cleanup_path(struct inotify_state *is, const char *path)
{
	int i;

	for (i = 0; i < is->nr; i++) {
		if (strcmp(is->entries[i].path, path) == 0) {
			inotify_rm_watch(is->ifd, is->entries[i].wd);
			return;
		}
	}
}

/* ---------- inotify helpers ---------- */

/*
 * Set up inotify on scan_root and add watches for every cgroup that was
 * already discovered during the initial scan.  Returns 0 on success.
 */
static int setup_inotify(struct cgr_ctx *ctx, struct inotify_state *is)
{
	int i;

	is->ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (is->ifd < 0) {
		cgr_log(ctx, CGR_LOG_ERR, "inotify: init failed: %s",
			strerror(errno));
		return -1;
	}

	/* Always watch the scan root itself */
	if (inotify_add_dir(ctx, is, ctx->scan_root) < 0) {
		close(is->ifd);
		is->ifd = -1;
		return -1;
	}

	/* Watch every cgroup subtree discovered during the initial scan */
	pthread_rwlock_rdlock(&ctx->lock);
	for (i = 0; i < ctx->groups_cap; i++) {
		if (ctx->groups[i].active)
			inotify_add_dir(ctx, is, ctx->groups[i].path);
	}
	pthread_rwlock_unlock(&ctx->lock);

	cgr_log(ctx, CGR_LOG_INFO,
		"inotify: watching %s (recursive, %d dirs)",
		ctx->scan_root, is->nr);
	return 0;
}

/*
 * Recursively scan the children of dir for memory cgroups that appeared
 * after inotify started watching (e.g. a whole subtree moved in via rename).
 * Adds each found cgroup and adds an inotify watch so future events are
 * caught.  dir itself must already be added by the caller.
 */
static void scan_and_watch_subtree(struct cgr_ctx *ctx,
				   struct inotify_state *is,
				   const char *dir)
{
	DIR *d;
	struct dirent *de;
	char child[512];
	struct stat st;

	d = opendir(dir);
	if (!d)
		return;

	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;

		snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);

		if (stat(child, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;

		if (!cg_file_exists(child, "memory.current"))
			continue;

		if (cgr_add_cgroup(ctx, child) == CGR_OK)
			cgr_log(ctx, CGR_LOG_INFO, "inotify: added %s", child);

		inotify_add_dir(ctx, is, child);
		scan_and_watch_subtree(ctx, is, child);
	}

	closedir(d);
}

/*
 * Drain and process all pending inotify events.
 * Called from the monitor loop when poll() signals readability.
 */
static void handle_inotify_events(struct cgr_ctx *ctx, struct inotify_state *is)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;

	for (;;) {
		const struct inotify_event *ev;
		char *ptr;

		len = read(is->ifd, buf, sizeof(buf));
		if (len <= 0)
			break;

		for (ptr = buf; ptr < buf + len;
		     ptr += sizeof(*ev) + ev->len) {
			char child_path[512];
			const char *parent;

			ev = (const struct inotify_event *)ptr;

			/*
			 * IN_IGNORED fires when a watch is removed — either
			 * because we called inotify_rm_watch() or because the
			 * watched directory was deleted.  Clean up the table.
			 */
			if (ev->mask & IN_IGNORED) {
				inotify_cleanup_wd(is, ev->wd);
				continue;
			}

			if (!(ev->mask & IN_ISDIR))
				continue;
			if (!ev->len || ev->name[0] == '.')
				continue;

			parent = inotify_path_for_wd(is, ev->wd);
			if (!parent)
				continue;

			snprintf(child_path, sizeof(child_path), "%s/%s",
				 parent, ev->name);

			if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
				if (!cg_file_exists(child_path, "memory.current"))
					continue;
				if (cgr_add_cgroup(ctx, child_path) == CGR_OK)
					cgr_log(ctx, CGR_LOG_INFO,
						"inotify: added %s",
						child_path);
				/*
				 * Watch the new directory so its own children
				 * generate events, then scan for any subtree
				 * that was moved in atomically (IN_MOVED_TO).
				 */
				inotify_add_dir(ctx, is, child_path);
				scan_and_watch_subtree(ctx, is, child_path);
			} else if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
				if (cgr_remove_cgroup(ctx, child_path) == CGR_OK)
					cgr_log(ctx, CGR_LOG_INFO,
						"inotify: removed %s",
						child_path);
				/*
				 * For IN_DELETE the kernel already removed the
				 * watch; for IN_MOVED_FROM the dir still exists
				 * elsewhere so we must drop it explicitly.
				 */
				inotify_cleanup_path(is, child_path);
			}
		}
	}
}

/* ---------- settle phase ---------- */

/*
 * Wait a fixed period for processes to reach steady state, then set
 * memory.high = memory.current for all cgroups as the initial baseline.
 * The adaptive loop must not start until this baseline is established.
 */
#define SETTLE_WAIT_SEC	10

static void settle_and_baseline(struct cgr_ctx *ctx)
{
	struct timespec ts;
	int i;

	if (ctx->nr_groups == 0)
		return;

	cgr_log(ctx, CGR_LOG_INFO,
		"settle: waiting %ds for memory usage to stabilize",
		SETTLE_WAIT_SEC);

	ts.tv_sec = SETTLE_WAIT_SEC;
	ts.tv_nsec = 0;
	nanosleep(&ts, NULL);

	if (!ctx->running)
		return;

	/* Set memory.high = memory.current for all groups */
	pthread_rwlock_wrlock(&ctx->lock);
	read_usage(ctx);

	for (i = 0; i < ctx->groups_cap; i++) {
		struct cgr_group *g = &ctx->groups[i];
		uint64_t baseline;

		if (!g->active)
			continue;

		baseline = g->usage;
		if (baseline < ctx->min_limit)
			baseline = ctx->min_limit;

		g->limit = baseline;
		cg_write_uint64(g->path, "memory.high", baseline);

		cgr_log(ctx, CGR_LOG_INFO,
			"settle: %s memory.high=%luMB (current=%luMB)",
			g->path,
			(unsigned long)(baseline >> 20),
			(unsigned long)(g->usage >> 20));
	}

	pthread_rwlock_unlock(&ctx->lock);

	cgr_log(ctx, CGR_LOG_INFO, "settle: baseline set, starting monitor loop");
}

/* ---------- monitor main loop ---------- */

static void *monitor_thread(void *arg)
{
	struct cgr_ctx *ctx = arg;
	struct inotify_state istate;
	struct timespec ts;

	inotify_state_init(&istate);

	/*
	 * Wait for /home/root to become available before starting
	 * the adjustment loop.  Config file and log file live there.
	 * Logs generated before mount are buffered and flushed later.
	 */
	cgr_log(ctx, CGR_LOG_INFO, "monitor: waiting for %s", MOUNT_WAIT_PATH);
	while (ctx->running && !is_path_accessible()) {
		ts.tv_sec = 1;
		ts.tv_nsec = 0;
		nanosleep(&ts, NULL);
	}

	if (!ctx->running)
		return NULL;

	cgr_log(ctx, CGR_LOG_INFO, "monitor: %s available, loading config",
		MOUNT_WAIT_PATH);
	cgr_config_load(ctx);

	/* Initial scan */
	if (ctx->scan_root[0])
		cgr_scan_cgroups(ctx);

	/*
	 * Set up inotify watches on scan_root and every already-discovered
	 * cgroup directory so new subdirectories at any depth are caught.
	 */
	if (ctx->scan_root[0])
		setup_inotify(ctx, &istate);

	/*
	 * Settle: wait for system to boot up, then set
	 * memory.high = memory.current as the initial baseline.
	 */
	settle_and_baseline(ctx);

	/*
	 * Main loop: use poll() to multiplex inotify events with the
	 * periodic poll timer.  This replaces the separate inotify thread.
	 */
	while (ctx->running) {
		struct pollfd pfd;
		int nfds = 0;

		if (istate.ifd >= 0) {
			pfd.fd     = istate.ifd;
			pfd.events = POLLIN;
			nfds       = 1;
		}

		poll(nfds ? &pfd : NULL, nfds,
		     (int)ctx->cfg.poll_interval_ms);

		/* Process inotify events if any */
		if (istate.ifd >= 0 && nfds && (pfd.revents & POLLIN))
			handle_inotify_events(ctx, &istate);

		/* Sample refaults, read usage, and adjust limits periodically */
		ctx->refault_elapsed_ms += ctx->cfg.poll_interval_ms;
		if (ctx->refault_elapsed_ms >= ctx->cfg.refault_interval_ms) {
			ctx->refault_elapsed_ms = 0;

			pthread_rwlock_wrlock(&ctx->lock);
			read_usage(ctx);
			sample_refault(ctx);
			cgr_log(ctx, CGR_LOG_DEBUG,
				"refault: sample, adjusting limits");
			cgr_adjust_limits(ctx);
			pthread_rwlock_unlock(&ctx->lock);
		}

		/* Periodically reload config file (outside lock) */
		ctx->config_reload_count++;
		if (ctx->config_reload_count >= CONFIG_RELOAD_INTERVAL) {
			ctx->config_reload_count = 0;
			cgr_config_load(ctx);
		}
	}

	inotify_state_free(&istate);

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

	cgr_log(ctx, CGR_LOG_INFO,
		"monitor: started (poll_interval=%ums refault_interval=%ums)",
		ctx->cfg.poll_interval_ms, ctx->cfg.refault_interval_ms);

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

	if (new_limit < ctx->min_limit)
		new_limit = ctx->min_limit;

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
