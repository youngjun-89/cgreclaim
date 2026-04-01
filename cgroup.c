#define _GNU_SOURCE
#include "cgroup.h"

#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int build_path(char *buf, size_t bufsz,
		      const char *cg_path, const char *key)
{
	int ret;

	ret = snprintf(buf, bufsz, "%s/%s", cg_path, key);
	if (ret < 0 || (size_t)ret >= bufsz) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

int cg_read_uint64(const char *cg_path, const char *key, uint64_t *val)
{
	char path[PATH_MAX];
	char buf[64];
	int fd;
	ssize_t n;

	if (build_path(path, sizeof(path), cg_path, key) < 0)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);

	if (n <= 0) {
		errno = EIO;
		return -1;
	}

	buf[n] = '\0';

	/* "max" means unlimited */
	if (strncmp(buf, "max", 3) == 0) {
		*val = UINT64_MAX;
		return 0;
	}

	errno = 0;
	*val = strtoull(buf, NULL, 10);
	if (errno)
		return -1;

	return 0;
}

int cg_write_uint64(const char *cg_path, const char *key, uint64_t val)
{
	char path[PATH_MAX];
	char buf[64];
	int fd, len;
	ssize_t n;

	if (build_path(path, sizeof(path), cg_path, key) < 0)
		return -1;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;

	if (val == UINT64_MAX)
		len = snprintf(buf, sizeof(buf), "max");
	else
		len = snprintf(buf, sizeof(buf), "%" PRIu64, val);

	n = write(fd, buf, len);
	close(fd);

	if (n != len) {
		if (errno == 0)
			errno = EIO;
		return -1;
	}

	return 0;
}

int cg_write_reclaim(const char *cg_path, uint64_t bytes)
{
	return cg_write_uint64(cg_path, "memory.reclaim", bytes);
}

int cg_file_exists(const char *cg_path, const char *key)
{
	char path[PATH_MAX];
	struct stat st;

	if (build_path(path, sizeof(path), cg_path, key) < 0)
		return 0;

	return stat(path, &st) == 0;
}
