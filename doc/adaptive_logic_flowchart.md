# cgreclaim — Adaptive memory.high 조정 로직 순서도

```
┌─────────────────────────────────────────────────────────────────┐
│                      cgr_start()                                │
│                  monitor_thread 시작                            │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│            scan_root 초기 스캔 (cgr_scan_cgroups)               │
│            memory.current 있는 디렉토리 = cgroup으로 등록       │
│            inotify watch 설정 (신규/삭제 감지)                  │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│            SETTLE 단계  (10초 대기)                             │
│            memory.high = memory.current  ← 초기 baseline 설정  │
│            [최소값 보장: min_limit (기본 16MB)]                 │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
╔═════════════════════════════════════════════════════════════════╗
║                   메인 폴링 루프                                ║
║                                                                 ║
║  poll()  ──────── 기본 1000ms (poll_interval_ms)               ║
║     │                                                           ║
║     ├─[inotify 이벤트?]──→ cgroup 추가/삭제 처리               ║
║     │                                                           ║
║     │   read_usage()  : memory.current 읽기 (매 poll)          ║
║     │                                                           ║
║     │   poll_count++                                            ║
║     │                                                           ║
║     └─[poll_count >= 5?]─── NO ──→ 다음 poll                  ║
║              │  (REFAULT_SAMPLE_INTERVAL = 5)                   ║
║             YES                                                 ║
║              │  poll_count = 0                                  ║
║              │                                                  ║
║              ▼                                                  ║
║       sample_refault()   ← memory.stat에서 refault 카운터 읽기 ║
║              │                                                  ║
║              ▼                                                  ║
║       ┌──────────────────────────────────────────────┐         ║
║       │     각 cgroup별 refault_urgency 판정          │         ║
║       │                                              │         ║
║       │  slope = refault - prev_refault              │         ║
║       │                                              │         ║
║       │  slope == 0    → IDLE                        │         ║
║       │  slope >= 100  → URGENT  (refault_slope_urgent)        ║
║       │  slope >= 10   → MODERATE(refault_slope_moderate)      ║
║       └──────────────┬───────────────────────────────┘         ║
║                      │                                          ║
║          ┌───────────┼───────────┐                             ║
║          │           │           │                             ║
║          ▼           ▼           ▼                             ║
║        IDLE       MODERATE    URGENT                           ║
║          │           │           │                             ║
║      × 0.95      × 1.10      × 1.20                           ║
║    (-5% 축소)  (+10% 확장)  (+20% 확장)                       ║
║          │           │           │                             ║
║          │    [grow 상한 체크]    │                            ║
║          │  limit >= usage × ratio?                            ║
║          │    YES → grow 중단 (limit_usage_ratio, 기본 2)      ║
║          │    NO  → grow 허용                                  ║
║          └───────────┴───────────┘                             ║
║                      │                                          ║
║                      ▼                                          ║
║         new_limit < min_limit ? → min_limit으로 보정           ║
║         (min_limit_mb 설정, 기본 16MB)                         ║
║                      │                                          ║
║                      ▼                                          ║
║         memory.high = new_limit  (write to cgroup fs)          ║
║                                                                 ║
║  ──────────────────────────────────────────────────────────    ║
║  config_reload_count >= 30 → config 파일 재로드                ║
║  (CONFIG_RELOAD_INTERVAL = 30 polls ≈ 30초)                    ║
╚═════════════════════════════════════════════════════════════════╝
```

---

## 핵심 기준값 요약

| 항목 | 기본값 | 설정 키 | 설명 |
|---|---|---|---|
| `poll_interval_ms` | 1000ms | `poll_interval_ms` | 기본 폴링 주기 |
| `refault_interval_ms` | 1000ms | `refault_interval_ms` | refault 샘플링 주기 |
| `REFAULT_SAMPLE_INTERVAL` | 5 polls | — | refault 샘플링 실제 간격 (≈5초) |
| `CONFIG_RELOAD_INTERVAL` | 30 polls | — | config 파일 재로드 주기 (≈30초) |
| `SETTLE_WAIT_SEC` | 10초 | — | 초기 안정화 대기 |
| `refault_slope_urgent` | **100** | `refault_slope_urgent` | URGENT 판정 기준 |
| `refault_slope_moderate` | **10** | `refault_slope_moderate` | MODERATE 판정 기준 |
| `GROW_FACTOR_URGENT` | **×1.20** | — | URGENT 시 memory.high +20% |
| `GROW_FACTOR_MODERATE` | **×1.10** | — | MODERATE 시 memory.high +10% |
| `SHRINK_FACTOR` | **×0.95** | — | IDLE 시 memory.high −5% |
| `min_limit` | **16MB** | `min_limit_mb` | memory.high 하한선 |
| `limit_usage_ratio` | **2** | `limit_usage_ratio` | grow 상한 배수 (0=비활성) |

> **핵심 철학**: refault가 없으면 꾸준히 5%씩 내려 reclaim 압력을 주고, thrashing이 감지되면 즉시 10~20% 올려 working set을 보호 → 비대칭 속도(올리는 건 빠르게, 내리는 건 천천히)로 oscillation 방지.
> grow 상한(`limit_usage_ratio`)으로 usage 대비 지나치게 큰 limit이 유지되는 것을 막는다.
