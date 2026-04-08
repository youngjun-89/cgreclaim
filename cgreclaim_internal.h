#ifndef CGRECLAIM_INTERNAL_H
#define CGRECLAIM_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cgreclaim.h"
#include <pthread.h>

/* Refault pressure urgency levels, derived from refault rate (slope). */
enum cgr_refault_urgency {
	CGR_REFAULT_IDLE = 0,	/* no new refaults */
	CGR_REFAULT_MODERATE,	/* slow refault rate — mild pressure */
	CGR_REFAULT_URGENT,	/* fast refault rate — severe pressure */
};

/* Initial capacity for the dynamic groups array; doubles on each grow. */
#define CGR_GROUPS_INIT_CAP	16

/* Internal per-cgroup state */
struct cgr_group {
	char		path[256];
	uint64_t	limit;		/* current memory.high target */
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
	struct cgr_group	*groups;	/* dynamically allocated array */
	int			groups_cap;	/* allocated slots */
	int			nr_groups;	/* active slots */

	pthread_rwlock_t	lock;
	pthread_t		monitor_tid;
	volatile int		running;
	unsigned int		refault_elapsed_ms;	/* ms since last refault sample */
	unsigned int		config_reload_count;

	/* Runtime-tunable thresholds (reloaded from config file) */
	uint64_t		refault_slope_moderate;	/* default 10 */
	uint64_t		refault_slope_urgent;	/* default 100 */
	uint64_t		min_limit;		/* default CGR_MIN_LIMIT_BYTES (16MB) */
	uint32_t		limit_usage_ratio;	/* default 2 — cap grow at limit > usage * ratio */
	uint32_t		grow_pct_urgent;	/* default 20 — memory.high +20% on URGENT */
	uint32_t		grow_pct_moderate;	/* default 10 — memory.high +10% on MODERATE */
	uint32_t		shrink_pct;		/* default 5  — memory.high -5%  on IDLE */

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
void cgr_adjust_limits(struct cgr_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CGRECLAIM_INTERNAL_H */
