#ifndef CGRECLAIM_INTERNAL_H
#define CGRECLAIM_INTERNAL_H

#include "cgreclaim.h"
#include <pthread.h>

/* Internal per-cgroup state */
struct cgr_group {
	char		path[256];
	uint64_t	limit;		/* current memory.max target */
	uint64_t	usage;		/* last polled memory.current */
	uint64_t	refault;	/* last sampled refault counter */
	uint64_t	prev_refault;	/* previous sample (for idle detection) */
	int		is_foreground;
	uint64_t	reclaim_count;
	int		active;		/* slot in use */
};

/* Library context */
struct cgr_ctx {
	struct cgr_config	cfg;
	char			scan_root[256]; /* owned copy */
	struct cgr_group	groups[CGR_MAX_GROUPS];
	int			nr_groups;

	pthread_rwlock_t	lock;
	pthread_t		monitor_tid;
	volatile int		running;
	int			reclaim_supported;
	unsigned int		poll_count;	/* polls since last refault sample */

	void (*log_fn)(int level, const char *fmt, ...);
};

/* Internal logging macro — safe to call even when log_fn is NULL */
#define cgr_log(ctx, lvl, fmt, ...) \
	do { \
		if ((ctx)->log_fn) \
			(ctx)->log_fn((lvl), (fmt), ##__VA_ARGS__); \
	} while (0)

/* Internal helpers shared between cgreclaim.c and monitor.c */
struct cgr_group *cgr_find_group(struct cgr_ctx *ctx, const char *path);
int cgr_do_reclaim(struct cgr_ctx *ctx, struct cgr_group *g, uint64_t target);
void cgr_adjust_limits(struct cgr_ctx *ctx);

#endif /* CGRECLAIM_INTERNAL_H */
