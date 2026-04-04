# Idea Sketch: RSS 가중치 기반 refault 판단 개선

## 현재 방식

refault slope (delta refaults per sample window) 만으로 thrashing 판단:
- slope >= urgent_threshold → URGENT (+20%)
- slope >= moderate_threshold → MODERATE (+10%)
- else → IDLE (-5%)

## 문제

동일한 refault slope라도 RSS(Resident Set Size)가 큰 cgroup과 작은 cgroup은
의미가 다르다:
- RSS 1GB, refault 50 → 전체 대비 0.005% re-fault → 큰 문제 아님
- RSS 50MB, refault 50 → 전체 대비 ~0.1% re-fault → 상대적으로 심각

## 제안: Normalized Refault Rate

```
refault_rate = slope / (rss_pages)
```

RSS를 page 단위로 나눠서 정규화하면 cgroup 크기에 무관한 비교가 가능.

### 구현 방안

1. `memory.stat`에서 `anon + file` RSS를 함께 읽기 (이미 memory.current로 근사 가능)
2. normalized rate 계산:
   ```c
   uint64_t rss_pages = g->usage / 4096;
   double norm_rate = (double)slope / (rss_pages ? rss_pages : 1);
   ```
3. threshold도 normalized 값으로 변경:
   ```
   refault_norm_moderate=0.001   (0.1% of pages per window)
   refault_norm_urgent=0.01      (1% of pages per window)
   ```

### 고려사항

- memory.current ≈ RSS + page cache → 순수 RSS가 아님
  - `memory.stat`의 `anon` + `file` 합산이 더 정확
  - 하지만 memory.current로 근사해도 충분할 수 있음
- 너무 작은 cgroup (< 32MB)에서 나눗셈 노이즈 → floor 적용 필요
- config 파일에 norm threshold 추가해서 실험 가능하게

### 실험 계획

1. 현재 slope 기반 로그에 `norm_rate` 필드 추가해서 로깅
2. 다양한 워크로드에서 norm_rate 분포 관찰
3. threshold 결정 후 config 파일로 전환
