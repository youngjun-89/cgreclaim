#ifndef CGRECLAIM_H
#define CGRECLAIM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdint.h>

/* Minimum memory limit for any cgroup (32MB) */
#define CGR_MIN_LIMIT_BYTES	(32ULL << 20)

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
	CGR_ERR_FULL  = -3,	/* (unused, kept for ABI compat) */
	CGR_ERR_EXIST = -4,	/* cgroup already registered */
	CGR_ERR_NOENT = -5,	/* cgroup not found */
	CGR_ERR_IO    = -6,	/* cgroup I/O error */
	CGR_ERR_BUSY  = -7,	/* monitor already running */
};

/* Per-cgroup status info */
struct cgr_status {
	char		path[256];
	uint64_t	limit;		/* current memory.high */
	uint64_t	usage;		/* last known memory.current */
	int		is_foreground;
	uint64_t	reclaim_count;	/* number of reclaims performed */
};

/* Library configuration */
struct cgr_config {
	unsigned int	poll_interval_ms;	/* monitor polling interval (default: 1000) */

	/*
	 * Root cgroup path to scan for child cgroups.
	 * If set, cgr_scan_cgroups() discovers children automatically.
	 * e.g., "/sys/fs/cgroup" or "/sys/fs/cgroup/user.slice"
	 */
	const char	*scan_root;

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
 * Cgroup management — manual
 */
int cgr_add_cgroup(struct cgr_ctx *ctx, const char *path);
int cgr_remove_cgroup(struct cgr_ctx *ctx, const char *path);

/*
 * Cgroup auto-discovery — scan cfg->scan_root for child cgroups
 * that have the memory controller enabled and index them.
 * Existing memory.max limits are preserved. Returns number found or < 0.
 */
int cgr_scan_cgroups(struct cgr_ctx *ctx);

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

/*
 * Utility — get system total RAM in bytes
 */
uint64_t cgr_get_total_ram(void);

#ifdef __cplusplus
}
#endif

#endif /* CGRECLAIM_H */
