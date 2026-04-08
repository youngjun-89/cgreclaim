# libcgreclaim

cgroupv2 기반 adaptive `memory.high` 관리 라이브러리.

여러 cgroup의 `memory.high`를 자동으로 탐색/조정하고, refault slope를 기반으로
IDLE/MODERATE/URGENT 압력 단계에 맞게 soft limit을 adaptive하게 재조정한다.
내부 pthread가 주기적으로 폴링하며, inotify로 cgroup 추가/제거를 감지한다.

## 주요 기능

- **memory.high only** — `memory.max`는 절대 건드리지 않음. 커널이 soft limit 기반으로 점진적 reclaim
- **cgroup tree 자동 탐색** — `cgr_scan_cgroups()`로 하위 cgroup 자동 등록
- **Adaptive 조정** — refault slope (IDLE/MODERATE/URGENT) 기반으로 memory.high 자동 증감
- **초기 limit** — current usage + 10% headroom으로 시작
- **inotify 감지** — scan_root 하위 cgroup 추가/제거를 event-driven으로 처리
- **런타임 설정** — `/home/root/cgreclaim` 파일을 ~30s마다 reload
- **Early log buffer** — `/home/root` 마운트 전 로그 64개 버퍼링 후 마운트 시 flush

## 빌드

### Make
```bash
make            # libcgreclaim.a + test_cgreclaim
make clean
```

### CMake (standalone)
```bash
mkdir build && cd build
cmake ..
make
```

## CMake 서브디렉토리로 사용

부모 프로젝트의 `CMakeLists.txt`에서:

```cmake
# cgreclaim 소스를 서브디렉토리에 배치 (예: third_party/cgreclaim/)
add_subdirectory(third_party/cgreclaim)

# 타겟에 링크
add_library(your_lib SHARED your_source.c)
target_link_libraries(your_lib PRIVATE cgreclaim)
```

`cgreclaim`은 `-fPIC` 정적 라이브러리로 빌드되므로 `.so`에 직접 링크 가능.

헤더는 `#include "cgreclaim.h"` — `target_link_libraries`가
include 경로를 자동 설정한다.

## 런타임 설정 파일

경로: **`/home/root/cgreclaim`** (plain text, `key=value` 형식)

모니터 thread가 약 30초마다 자동 reload한다. 파일이 없어도 기본값으로 동작한다.

```
# /home/root/cgreclaim
poll_interval_ms       = 1000   # inotify poll 주기 (ms)
refault_interval_ms    = 1000   # refault 샘플링 + memory.high 조정 주기 (ms)
refault_slope_moderate = 10     # MODERATE 압력 임계값
refault_slope_urgent   = 100    # URGENT 압력 임계값
min_limit_mb           = 16     # cgroup당 최소 memory.high (MB)
limit_usage_ratio      = 2      # limit >= usage * ratio 이면 grow 생략
```

로그 파일: **`/home/root/cgreclaim.log`**

## 사용 예시

```c
#include "cgreclaim.h"

struct cgr_config cfg = {
    .scan_root = "/sys/fs/cgroup/apps",   /* 탐색 루트 */
};

struct cgr_ctx *ctx = cgr_init(&cfg);

cgr_scan_cgroups(ctx);   /* 하위 cgroup 자동 등록, 초기 limit = usage + 10% */
cgr_start(ctx);          /* 모니터 thread 시작 */

/* ... */

cgr_stop(ctx);
cgr_destroy(ctx);
```

## GitHub에 올리기

```bash
# 1. GitHub에서 새 repo 생성 (예: cgreclaim)
# 2. remote 추가 + push
git remote add origin git@github.com:<username>/cgreclaim.git
git branch -M main
git push -u origin main
```
