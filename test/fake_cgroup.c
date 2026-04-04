#define _GNU_SOURCE
#include "fake_cgroup.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_file(const char *dir, const char *name, const char *content)
{
	char path[512];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	fp = fopen(path, "w");
	if (!fp)
		return -1;
	fputs(content, fp);
	fclose(fp);
	return 0;
}

int fake_cg_init(void)
{
	if (mkdir(FAKE_CG_BASE, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

char *fake_cg_create(const char *name, unsigned long initial_usage_bytes,
		     unsigned long initial_refault, char *out_path, int pathsz)
{
	char stat_buf[512];

	snprintf(out_path, pathsz, "%s/%s", FAKE_CG_BASE, name);

	if (mkdir(out_path, 0755) < 0 && errno != EEXIST)
		return NULL;

	/* memory.current — cg_read_uint64 reads this */
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%lu\n", initial_usage_bytes);
		if (write_file(out_path, "memory.current", buf) < 0)
			return NULL;
	}

	/* memory.high — initially "max" */
	if (write_file(out_path, "memory.high", "max\n") < 0)
		return NULL;

	/* memory.stat — cg_read_refault parses this */
	snprintf(stat_buf, sizeof(stat_buf),
		 "anon 0\n"
		 "file 0\n"
		 "kernel 0\n"
		 "shmem 0\n"
		 "workingset_refault_anon %lu\n"
		 "workingset_refault_file 0\n"
		 "workingset_activate_anon 0\n"
		 "workingset_activate_file 0\n",
		 initial_refault);
	if (write_file(out_path, "memory.stat", stat_buf) < 0)
		return NULL;

	return out_path;
}

int fake_cg_set_usage(const char *path, unsigned long bytes)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%lu\n", bytes);
	return write_file(path, "memory.current", buf);
}

int fake_cg_set_refault(const char *path, unsigned long anon, unsigned long file)
{
	char stat_buf[512];

	snprintf(stat_buf, sizeof(stat_buf),
		 "anon 0\n"
		 "file 0\n"
		 "kernel 0\n"
		 "shmem 0\n"
		 "workingset_refault_anon %lu\n"
		 "workingset_refault_file %lu\n"
		 "workingset_activate_anon 0\n"
		 "workingset_activate_file 0\n",
		 anon, file);
	return write_file(path, "memory.stat", stat_buf);
}

static int rm_cb(const char *fpath, const struct stat *sb,
		 int typeflag, struct FTW *ftwbuf)
{
	(void)sb; (void)ftwbuf;

	if (typeflag == FTW_F || typeflag == FTW_SL)
		unlink(fpath);
	else if (typeflag == FTW_DP)
		rmdir(fpath);
	return 0;
}

void fake_cg_destroy(const char *path)
{
	nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

void fake_cg_cleanup(void)
{
	nftw(FAKE_CG_BASE, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}
