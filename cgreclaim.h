#ifndef CGRECLAIM_H
#define CGRECLAIM_H

#include <stdarg.h>
#include <stdint.h>

/* Minimum memory limit for any cgroup (32MB) */
#define CGR_MIN_LIMIT_BYTES	(32ULL << 20)

/* Maximum number of managed cgroups */
#define CGR_MAX_GROUPS		16

/* Log levels */
enum cgr_log_level {
	CGR_LOG_ERR = 0,
	CGR_LOG_WARN,
	CGR_LOG_INFO,
	CGR_LOG_DEBUG,
};

/* Error codes */
enum cgr_err {
	CGR_OK = 0,
	CGR_ERR_INVAL = -1,	/* invalid argument */
	CGR_ERR_NOMEM = -2,	/* out of memory */
	CGR_ERR_FULL  = -3,	/* max cgroups reached */
	CGR_ERR_EXIST = -4,	/* cgroup already registered */
	CGR_ERR_NOENT = -5,	/* cgroup not found */
	CGR_ERR_IO    = -6,	/* cgroup I/O error */
	CGR_ERR_BUSY  = -7,	/* monitor already running */
};

/* Per-cgroup status info */
struct cgr_status {
	char		path[256];
	uint64_t	limit;		/* current memory.max */
	uint64_t	usage;		/* last known memory.current */
	int		is_foreground;
	uint64_t	reclaim_count;	/* number of reclaims performed */
};

/* Library configuration */
struct cgr_config {
	uint64_t	total_pool;		/* total memory pool in bytes */
	unsigned int	poll_interval_ms;	/* monitor polling interval (default: 1000) */
	double		fg_ratio;		/* fraction of pool for foreground (default: 0.6) */

	/* Optional log callback. If NULL, no logging. */
	void (*log_fn)(int level, const char *fmt, ...);
};

/* Opaque library context */
struct cgr_ctx;

/*
 * Lifecycle
 */
struct cgr_ctx *cgr_init(const struct cgr_config *cfg);
void            cgr_destroy(struct cgr_ctx *ctx);

/*
 * Cgroup management
 */
int cgr_add_cgroup(struct cgr_ctx *ctx, const char *path, uint64_t initial_limit);
int cgr_remove_cgroup(struct cgr_ctx *ctx, const char *path);

/*
 * Monitor thread control
 */
int cgr_start(struct cgr_ctx *ctx);
int cgr_stop(struct cgr_ctx *ctx);

/*
 * App switching — explicit foreground designation
 */
int cgr_set_foreground(struct cgr_ctx *ctx, const char *path);

/*
 * Dynamic limit adjustment (thread-safe)
 */
int cgr_set_limit(struct cgr_ctx *ctx, const char *path, uint64_t new_limit);
int cgr_get_status(struct cgr_ctx *ctx, const char *path, struct cgr_status *out);

#endif /* CGRECLAIM_H */
