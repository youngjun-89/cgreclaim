# libcgreclaim

cgroupv2 기반 동적 메모리 한도 관리 라이브러리.

여러 cgroup의 `memory.max`를 자동으로 탐색/분배하고, app switching 시
foreground에 메모리를 집중 배분한다. 내부 pthread가 idle/active를 감지하여
adaptive하게 limit을 재조정한다.

## 주요 기능

- **cgroup tree 자동 탐색** — `cgr_scan_cgroups()`로 하위 cgroup 자동 등록
- **동적 memory.max 재분배** — foreground/background 비율 기반
- **App switching** — API 호출(`cgr_set_foreground`) + 자동 idle 감지
- **Proactive reclaim** — limit 줄일 때 `memory.reclaim` 사용 (미지원 커널은 `memory.high` fallback)
- **Pool 자동 계산** — 시스템 RAM에서 커널 예약분(384MB)을 뺀 값 사용

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

## 사용 예시

```c
#include "cgreclaim.h"

struct cgr_config cfg = {
    .total_pool = 0,                          /* auto-detect */
    .scan_root  = "/sys/fs/cgroup/apps",      /* 탐색 루트 */
    .fg_ratio   = 0.6,
};

struct cgr_ctx *ctx = cgr_init(&cfg);

cgr_scan_cgroups(ctx);    /* 하위 cgroup 자동 등록 + 균등 분배 */
cgr_start(ctx);           /* 모니터 thread 시작 */

/* app switch */
cgr_set_foreground(ctx, "/sys/fs/cgroup/apps/browser");

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
