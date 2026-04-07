#define _GNU_SOURCE
#include "cgr_config.h"
#include "cgreclaim_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cgr_config_load(struct cgr_ctx *ctx)
{
	FILE *fp;
	char line[256];

	if (!ctx)
		return -1;

	fp = fopen(CGR_CONFIG_PATH, "r");
	if (!fp) {
		/* Missing file is fine — keep defaults */
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	while (fgets(line, sizeof(line), fp)) {
		char *key, *val, *nl;

		/* Strip newline */
		nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';

		/* Skip comments and blank lines */
		if (line[0] == '#' || line[0] == '\0')
			continue;

		key = line;
		val = strchr(line, '=');
		if (!val)
			continue;

		*val++ = '\0';

		/* Trim leading spaces from value */
		while (*val == ' ' || *val == '\t')
			val++;

		if (strcmp(key, "poll_interval_ms") == 0) {
			unsigned int v = (unsigned int)strtoul(val, NULL, 10);

			if (v > 0) {
				ctx->cfg.poll_interval_ms = v;
				cgr_log(ctx, CGR_LOG_INFO,
					"config: poll_interval_ms=%u", v);
			}
		} else if (strcmp(key, "refault_interval_ms") == 0) {
			unsigned int v = (unsigned int)strtoul(val, NULL, 10);

			if (v > 0) {
				ctx->cfg.refault_interval_ms = v;
				cgr_log(ctx, CGR_LOG_INFO,
					"config: refault_interval_ms=%u", v);
			}
		} else if (strcmp(key, "refault_slope_moderate") == 0) {
			uint64_t v = strtoull(val, NULL, 10);

			if (v > 0) {
				ctx->refault_slope_moderate = v;
				cgr_log(ctx, CGR_LOG_INFO,
					"config: refault_slope_moderate=%lu",
					(unsigned long)v);
			}
		} else if (strcmp(key, "refault_slope_urgent") == 0) {
			uint64_t v = strtoull(val, NULL, 10);

			if (v > 0) {
				ctx->refault_slope_urgent = v;
				cgr_log(ctx, CGR_LOG_INFO,
					"config: refault_slope_urgent=%lu",
					(unsigned long)v);
			}
		}
		/* Unknown keys silently ignored */
	}

	fclose(fp);
	return 0;
}
