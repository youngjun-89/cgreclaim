#ifndef CGR_CONFIG_H
#define CGR_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cgreclaim_internal.h"

/*
 * Runtime configuration file.
 * Parsed on monitor start and periodically re-read so that
 * tuning parameters can be changed without restarting.
 *
 * Format — plain key=value, one per line.  Lines starting with '#'
 * are comments.  Unknown keys are silently ignored.
 *
 * Supported keys:
 *   poll_interval_ms       — inotify event poll timeout in ms (default 1000)
 *   refault_interval_ms    — refault sample + memory.high adjust period in ms (default 1000)
 *   refault_slope_moderate — refault slope for MODERATE urgency (default 10)
 *   refault_slope_urgent   — refault slope for URGENT urgency  (default 100)
 *   min_limit_mb           — minimum memory.high per cgroup in MB (default 16)
 *   limit_usage_ratio      — don't grow memory.high if limit >= usage * ratio (default 2)
 */

#define CGR_CONFIG_PATH	"/home/root/cgreclaim"

/*
 * Load configuration from CGR_CONFIG_PATH into ctx.
 * Missing file is not an error — defaults are kept.
 * Returns 0 on success (or file not found), -1 on parse error.
 */
int cgr_config_load(struct cgr_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CGR_CONFIG_H */
