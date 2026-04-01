#ifndef CGROUP_H
#define CGROUP_H

#include <stdint.h>

/*
 * cgroupv2 filesystem I/O layer
 *
 * All functions take a cgroup directory path (e.g., "/sys/fs/cgroup/app1")
 * and a key (e.g., "memory.current").
 *
 * Return 0 on success, -1 on error (errno set).
 */

/* Read a uint64 value from a cgroup knob file */
int cg_read_uint64(const char *cg_path, const char *key, uint64_t *val);

/* Write a uint64 value to a cgroup knob file */
int cg_write_uint64(const char *cg_path, const char *key, uint64_t val);

/*
 * Write to memory.reclaim to trigger proactive reclaim.
 * Returns 0 on success, -1 on error.
 * If memory.reclaim doesn't exist, errno is set to ENOENT.
 */
int cg_write_reclaim(const char *cg_path, uint64_t bytes);

/* Check if a cgroup knob file exists */
int cg_file_exists(const char *cg_path, const char *key);

#endif /* CGROUP_H */
