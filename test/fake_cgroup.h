#ifndef FAKE_CGROUP_H
#define FAKE_CGROUP_H

/*
 * Fake cgroup hierarchy for testing.
 *
 * Creates a directory tree under /tmp that mimics cgroupv2 structure
 * with dummy memory.current, memory.high, memory.stat files.
 * No root or real cgroup filesystem required.
 *
 * Layout after fake_cg_setup("app1"):
 *   <base>/app1/
 *   <base>/app1/memory.current   → "104857600\n"  (100MB)
 *   <base>/app1/memory.high      → "max\n"
 *   <base>/app1/memory.stat      → "workingset_refault_anon 0\n..."
 *
 * All files are read/writable so cg_read_uint64/cg_write_uint64 work.
 */

#define FAKE_CG_BASE	"/tmp/cgreclaim_test_cgroups"

/* Create base directory. Call once before tests. */
int fake_cg_init(void);

/*
 * Create a fake cgroup under FAKE_CG_BASE with given name.
 * initial_usage_bytes: value written to memory.current.
 * initial_refault: value for workingset_refault_anon in memory.stat.
 * Returns full path in out_path (must be >= 512 bytes), or NULL on error.
 */
char *fake_cg_create(const char *name, unsigned long initial_usage_bytes,
		     unsigned long initial_refault, char *out_path, int pathsz);

/* Update memory.current value for a fake cgroup */
int fake_cg_set_usage(const char *path, unsigned long bytes);

/* Update refault counters in memory.stat */
int fake_cg_set_refault(const char *path, unsigned long anon, unsigned long file);

/* Remove a single fake cgroup */
void fake_cg_destroy(const char *path);

/* Remove everything under FAKE_CG_BASE */
void fake_cg_cleanup(void);

#endif /* FAKE_CGROUP_H */
