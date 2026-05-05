# Pintos Project 1 — 총 정리
## Alarm Clock / Priority Scheduler / Priority Donation

---

## 슬라이드 1 — Alarm Clock: Busy-waiting → Block 방식

### 문제
기존 `timer_sleep()`은 CPU를 계속 점유하며 시간을 확인하는 busy-waiting 방식이었습니다.

```c
/* 기존 코드 — busy-waiting */
while (timer_elapsed(start) < ticks)
    thread_yield();
```

### 해결
thread를 BLOCKED 상태로 만들어 sleep_list에 보관하고, 타이머 인터럽트가 wakeup_tick에 도달한 thread만 깨웁니다.

### 흐름
```
timer_sleep() 호출
→ wakeup_tick 저장 (현재 tick + sleep할 tick)
→ sleep_list에 삽입 (wakeup_tick 오름차순 정렬)
→ thread_block() — BLOCKED 상태

매 tick마다 timer_interrupt() 실행
→ sleep_list 순회
→ wakeup_tick 도달한 thread → thread_unblock()
→ ready_list에 삽입
```

### 핵심 개념
| 용어 | 설명 |
|------|------|
| `wakeup_tick` | 깨어나야 할 절대적인 tick 값 |
| `sleep_list` | BLOCKED thread 보관 리스트 |
| `thread_block()` | RUNNING → BLOCKED |
| `thread_unblock()` | BLOCKED → READY |
| `intr_disable()` | 리스트 삽입 구간 보호 |

### 한 줄 요약
> busy-waiting 제거 → CPU 낭비 없는 block 방식으로 교체

---

## 슬라이드 2 — Priority Scheduler: ready_list 우선순위 정렬

### 문제
기존 round-robin 방식은 높은 우선순위 thread도 줄을 서야 했습니다.

### 해결
모든 ready_list 삽입을 우선순위 내림차순 정렬로 교체했습니다.

### 수정한 함수

| 위치 | 수정 내용 |
|------|----------|
| `thread_unblock()` | `list_push_back` → `list_insert_ordered` |
| `thread_yield()` | `list_push_back` → `list_insert_ordered` |
| `thread_create()` | 생성 후 우선순위 비교, 즉시 yield |
| `thread_set_priority()` | 낮아지면 즉시 yield |
| `sema_up()` | `list_pop_front` → `list_min + cmp_priority` |
| `cond_signal()` | `list_pop_front` → `list_min + cmp_sema_priority` |

### 핵심 개념
- `cmp_priority`: 우선순위 내림차순 비교 함수
- `list_insert_ordered()`: 정렬된 위치에 삽입
- `list_min() + cmp_priority`: waiters에서 최고 우선순위 선택
- `cmp_sema_priority`: semaphore_elem 안의 thread 우선순위 비교

### 한 줄 요약
> 어느 상황에서든 가장 높은 우선순위 thread가 먼저 실행된다

---

## 슬라이드 3 — Priority Donation: Priority Inversion 해결

### 문제: Priority Inversion
```
L(10)  → lock 획득 후 실행 중
H(50)  → lock 요청 → BLOCKED
M(30)  → ready_list 등장 → L 선점

결과:
H → 영원히 실행 불가 (L이 lock을 못 놓음)
L → CPU를 못 받음 (M이 선점)
M → 무한 실행 가능
```

### 해결: Priority Donation
H가 L에게 우선순위를 임시로 기부합니다.
```
H(50) → L에게 donation
→ L의 effective priority = 50
→ L(50) > M(30) → L이 CPU를 받음
→ L이 lock 반환 → H 실행
→ L의 priority → original_priority(10) 복원
```

### struct thread 추가 필드
```c
int original_priority;       /* donation 이전 원래 우선순위 */
struct lock *wait_on_lock;   /* 현재 기다리는 lock (nested donation용) */
struct list donations;       /* 나에게 donation한 thread 목록 */
struct list_elem donation_elem; /* donations 리스트 연결용 */
```

### 구현한 함수 4개

**`donate_priority()`**
- 최대 8단계 체인 탐색 (nested donation)
- holder 우선순위가 나보다 낮으면 올려줌
- `wait_on_lock`으로 체인을 따라 이동

**`lock_acquire()` 수정**
- holder가 있으면 `wait_on_lock` 설정
- `donations`에 현재 thread 추가
- `donate_priority()` 호출

**`remove_with_lock()`**
- 이 lock 때문에 donation한 thread들 제거
- 다른 lock donation은 유지

**`refresh_priority()`**
- `original_priority`로 초기화
- `donations` 비어있으면 복원 완료
- `donations` 남아있으면 최고값 적용

### Effective Priority 개념
```
original_priority  → 변하지 않는 원래 우선순위
effective priority → donation으로 올라갈 수 있는 실제 우선순위
                     (priority 필드가 이 역할)
```

### 처리한 케이스
| 케이스 | 설명 |
|--------|------|
| 기본 donation | H → L에게 우선순위 기부 |
| multiple donation | 여러 thread가 한 thread에게 기부 |
| nested donation | H→M→L 체인, 최대 8단계 |
| donation 중 우선순위 변경 | `original_priority`만 변경, `refresh_priority()`로 재계산 |

### 테스트 결과
```
priority-donate-one       ✅
priority-donate-multiple  ✅
priority-donate-multiple2 ✅
priority-donate-nest      ✅
priority-donate-lower     ✅
priority-donate-sema      ✅
priority-donate-chain     ✅
```

### 한 줄 요약
> donation은 lock을 빨리 놓게 하기 위해 holder에게 우선순위를 임시로 빌려주는 것

---

## 전체 핵심 연결 고리

```
Alarm Clock:
busy-waiting 제거 → thread_block() → sleep_list → timer_interrupt() → thread_unblock()

Priority Scheduler:
thread_unblock() → ready_list 우선순위 정렬 → 최고 우선순위 thread 실행

Priority Donation:
lock_acquire() → donate_priority() → holder 우선순위 상승
lock_release() → remove_with_lock() → refresh_priority() → 원래 우선순위 복원

모든 것의 중심:
어느 상황에서든 가장 높은 우선순위 thread가 CPU를 받는다
```

---

## 예상 질문 대비

**Q. donate_priority()에서 8단계 제한을 두는 이유는?**
> 무한 루프 방지 + 인터럽트 비활성화 시간 최소화. 8단계 이상의 nested lock은 설계가 잘못된 코드입니다.

**Q. original_priority를 따로 저장하는 이유는?**
> donation 취소 시 복원 기준이 필요하기 때문입니다. donation 중에 priority 필드를 직접 바꾸면 donation 효과가 사라집니다.

**Q. priority donation이 lock에만 필요한 이유는?**
> semaphore는 holder 개념이 없어서 donation 대상을 특정할 수 없습니다. lock만 holder를 추적하기 때문입니다.

**Q. refresh_priority()가 필요한 이유는?**
> multiple donation 상황에서 하나의 donation이 취소됐을 때 남은 donation 중 가장 높은 값으로 우선순위를 재계산해야 하기 때문입니다.
