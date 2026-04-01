#define _GNU_SOURCE
#include "cgreclaim.h"

/* Stub — implemented in commit 3 */

int cgr_start(struct cgr_ctx *ctx)
{
	(void)ctx;
	return CGR_ERR_INVAL;
}

int cgr_stop(struct cgr_ctx *ctx)
{
	(void)ctx;
	return CGR_ERR_INVAL;
}

int cgr_set_foreground(struct cgr_ctx *ctx, const char *path)
{
	(void)ctx;
	(void)path;
	return CGR_ERR_INVAL;
}

int cgr_set_limit(struct cgr_ctx *ctx, const char *path, uint64_t new_limit)
{
	(void)ctx;
	(void)path;
	(void)new_limit;
	return CGR_ERR_INVAL;
}

int cgr_get_status(struct cgr_ctx *ctx, const char *path, struct cgr_status *out)
{
	(void)ctx;
	(void)path;
	(void)out;
	return CGR_ERR_INVAL;
}
