# cgreclaim — cgroup v2 adaptive memory.high manager

## Build

```bash
# CMake (recommended)
mkdir -p build && cd build && cmake .. && make

# Quick manual build
gcc -c cgroup.c cgreclaim.c cgr_config.c cgr_log.c monitor.c -I. -Wall -Werror
ar rcs libcgreclaim.a cgroup.o cgreclaim.o cgr_config.o cgr_log.o monitor.o
```

## Test

```bash
# Build and run test suite (no root required)
gcc -o test/test_cgreclaim \
  test/test_main.c test/fake_cgroup.c \
  cgreclaim.c monitor.c cgroup.c cgr_log.c cgr_config.c \
  -I. -Itest -lpthread -Wall -Werror
./test/test_cgreclaim
```

Tests use a fake cgroup hierarchy under `/tmp/cgreclaim_test_cgroups/`
with dummy `memory.current`, `memory.high`, `memory.stat` files.
No root or real cgroup filesystem needed.

Tests marked SKIP require `/home/root` to be accessible (target board).

**Run tests after every code change.**

## Architecture

- **memory.high only** — never write to `memory.max`. All limit control via `memory.high` (soft limit, kernel reclaims gradually).
- Adaptive loop: poll every 1s, sample refaults every 5s, adjust memory.high based on refault slope (IDLE/MODERATE/URGENT).
- Initial memory.high = current usage + 10% headroom.
- inotify watches `scan_root` for new/removed cgroups — event-driven, not polling.
- `/home/root` mount wait: monitor thread blocks until path is accessible. Early logs buffered (64 entries) and flushed on mount.
- Runtime config: `/home/root/cgreclaim` (key=value) reloaded every ~30s.

## File layout

```
cgreclaim.h          Public API
cgreclaim_internal.h Internal structures, cgr_ctx, cgr_group
cgreclaim.c          Lifecycle, add/remove/scan cgroups
monitor.c            Monitor thread, poll, adjust, inotify watcher
cgroup.c             cgroupv2 filesystem I/O (read/write knob files)
cgr_config.c/h       Runtime config file parser
cgr_log.c/h          Logger with early-buffer support
test/
  test_main.c        Test suite
  fake_cgroup.h/c    Fake cgroup hierarchy for testing
doc/
  idea_*.md          Design sketches for future features
```

## Conventions

- C11, `-Wall -Werror`
- Thread safety via `pthread_rwlock` on `ctx->lock`
- All cgroup writes go to `memory.high`, never `memory.max`
- Commits: imperative mood, short first line, body explains why
