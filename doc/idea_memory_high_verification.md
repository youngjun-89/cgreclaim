# Idea Sketch: memory.high 도달 여부 확인

## 배경

현재 memory.high를 설정하고 adaptive하게 조절하고 있지만,
실제로 커널이 memory.high 임계값에 도달해서 reclaim을 수행했는지
확인하는 방법이 없다.

memory.high는 soft limit이므로:
- 도달하면 커널이 해당 cgroup에 대해 direct reclaim 수행
- 도달하지 않으면 아무 일도 안 일어남
- 즉, memory.high를 낮춰도 실제 효과가 있는지 모름

## 확인 방법 후보

### 방법 1: memory.events 모니터링

```
# cat /sys/fs/cgroup/<cg>/memory.events
low 0
high 42        ← memory.high 도달 횟수
max 0
oom 0
oom_kill 0
oom_group_kill 0
```

`high` 카운터가 증가하면 memory.high에 실제로 도달한 것.

**구현:**
```c
// memory.events에서 "high <count>" 파싱
int cg_read_high_events(const char *cg_path, uint64_t *count);
```

- 매 sample window마다 high event delta 확인
- delta > 0 → memory.high가 실제로 reclaim을 트리거함
- delta == 0 → memory.high가 너무 높아서 의미 없음 → 더 낮춰도 됨

### 방법 2: memory.current vs memory.high 직접 비교

```c
uint64_t current, high;
cg_read_uint64(path, "memory.current", &current);
cg_read_uint64(path, "memory.high", &high);

double ratio = (double)current / high;
// ratio > 0.95 → memory.high에 거의 도달
// ratio < 0.5  → memory.high가 너무 높음
```

단점: 순간 스냅샷이라 polling 사이 도달했다 내려간 경우를 놓침.

### 방법 3: memory.pressure (PSI)

```
# cat /sys/fs/cgroup/<cg>/memory.pressure
some avg10=0.00 avg60=0.00 avg300=0.00 total=12345
full avg10=0.00 avg60=0.00 avg300=0.00 total=67890
```

`total` 값의 delta를 보면 실제 memory pressure 발생 시간(μs) 확인 가능.

## 추천 방향

**방법 1 (memory.events의 high 카운터)이 가장 정확하고 저비용.**

### 활용 시나리오

1. **Shrink 가속**: high event가 0이면 memory.high가 충분히 높다는 뜻
   → SHRINK_FACTOR를 더 공격적으로 (0.90)
2. **Shrink 억제**: high event가 빈번하면 이미 pressure 상태
   → shrink 중단하거나 grow로 전환
3. **refault과 결합**: refault는 없지만 high event가 있으면
   → 커널이 잘 reclaim하고 있어서 working set이 유지되는 상태
   → 현재 memory.high가 적절한 수준

### 구현 계획

1. `cgroup.c`에 `cg_read_memory_events()` 추가
2. `cgr_group`에 `high_events`, `prev_high_events` 필드 추가
3. `sample_refault()`에서 함께 샘플링
4. `cgr_adjust_limits()`에서 refault urgency와 high events를 결합 판단
