#ifndef CGRECLAIM_INTERNAL_H
#define CGRECLAIM_INTERNAL_H

#include "cgreclaim.h"
#include <pthread.h>

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
	int			nr_groups;

	pthread_rwlock_t	lock;
	pthread_t		monitor_tid;
	volatile int		running;
	int			reclaim_supported;

	void (*log_fn)(int level, const char *fmt, ...);
};

/* Internal helpers shared between cgreclaim.c and monitor.c */
struct cgr_group *cgr_find_group(struct cgr_ctx *ctx, const char *path);
int cgr_do_reclaim(struct cgr_ctx *ctx, struct cgr_group *g, uint64_t target);
void cgr_rebalance(struct cgr_ctx *ctx);

#endif /* CGRECLAIM_INTERNAL_H */
