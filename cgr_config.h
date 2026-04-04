#ifndef CGR_CONFIG_H
#define CGR_CONFIG_H

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
 *   poll_interval_ms       — polling interval in milliseconds (default 1000)
 *   refault_slope_moderate — refault slope for MODERATE urgency (default 10)
 *   refault_slope_urgent   — refault slope for URGENT urgency  (default 100)
 */

#define CGR_CONFIG_PATH	"/home/root/cgreclaim"

/*
 * Load configuration from CGR_CONFIG_PATH into ctx.
 * Missing file is not an error — defaults are kept.
 * Returns 0 on success (or file not found), -1 on parse error.
 */
int cgr_config_load(struct cgr_ctx *ctx);

#endif /* CGR_CONFIG_H */
