#ifndef CGROUP_H
#define CGROUP_H

#ifdef __cplusplus
extern "C" {
#endif

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

/* Check if a cgroup knob file exists */
int cg_file_exists(const char *cg_path, const char *key);

/*
 * Read refault counters from memory.stat.
 * Returns sum of workingset_refault_anon + workingset_refault_file.
 * Returns 0 on success, -1 on error.
 */
int cg_read_refault(const char *cg_path, uint64_t *refault);

#ifdef __cplusplus
}
#endif

#endif /* CGROUP_H */
