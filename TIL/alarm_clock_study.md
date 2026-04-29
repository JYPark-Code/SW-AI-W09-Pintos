# Pintos Project 1 — Alarm Clock 학습 정리

## 1. 왜 수정했는가

기존 `timer_sleep()`은 **busy-waiting** 방식이었다.
CPU를 계속 점유하면서 루프를 돌며 시간을 확인하고, 아직 깰 시간이 아니면 `thread_yield()`로 양보했다가 다시 돌아오는 것을 반복했다.

이는 CPU를 낭비하고 다른 thread가 CPU를 제대로 받지 못하는 문제를 야기한다.
문서에서도 명시적으로 "No busy waiting"을 요구한다.

---

## 2. 해결 방향 — Sleep/Wakeup 메커니즘

thread를 **BLOCKED 상태**로 만들어 `sleep_list`에 보관하고,
`timer_interrupt()`가 매 tick마다 깨울 시간이 된 thread를 `thread_unblock()`으로 깨운다.

### Thread 상태 3가지

| 상태 | 위치 | 설명 |
|------|------|------|
| `THREAD_READY` | `ready_list` | CPU 받을 준비 완료 |
| `THREAD_RUNNING` | — | 현재 CPU 사용 중 |
| `THREAD_BLOCKED` | `sleep_list` | 깨울 때까지 대기 |

---

## 3. 수정한 것들

### `thread.h` — `struct thread`에 필드 추가

```c
int64_t wakeup_tick;  /* 이 thread가 깨어나야 할 tick 값 */
```

`int64_t`를 쓰는 이유: tick은 부팅 후 계속 올라가는 값이라 `int`(약 21억 한계)로는 오버플로우가 발생할 수 있기 때문.

### `thread.c` — 전역 자료구조 추가

```c
struct list sleep_list;  /* 잠든 thread들을 보관하는 리스트 */
```

`thread_init()`에 `list_init(&sleep_list)` 추가.

### `devices/timer.c` — `timer_sleep()` 교체

```c
void timer_sleep(int64_t ticks) {
    int64_t start = timer_ticks();
    ASSERT(intr_get_level() == INTR_ON);

    struct thread *t = thread_current();
    t->wakeup_tick = start + ticks;       /* 1. 깨어날 tick 저장 */

    enum intr_level old_level = intr_disable();  /* 2. 인터럽트 끄기 */
    list_insert_ordered(&sleep_list, &t->elem,
                        cmp_wakeup_tick, NULL);  /* 3. sleep_list에 정렬 삽입 */
    thread_block();                              /* 4. BLOCKED 상태로 전환 */
    intr_set_level(old_level);                   /* 5. 인터럽트 복구 */
}
```

**순서가 중요한 이유:**
`thread_block()` 이후엔 아무 코드도 실행되지 않는다.
따라서 sleep_list 삽입을 반드시 먼저 해야 한다.
삽입 전에 block되면 이 thread를 영원히 찾을 수 없게 된다.

**인터럽트를 끄는 이유:**
`list_insert_ordered()` 실행 도중 `timer_interrupt()`가 끼어들면
리스트가 반쯤 삽입된 상태에서 순회가 일어나 리스트 구조가 깨진다.

### `devices/timer.c` — `timer_interrupt()`에 wakeup 로직 추가

```c
/* 매 tick마다 sleep_list를 순회하여 깨울 thread를 unblock */
struct list_elem *e = list_begin(&sleep_list);
while (e != list_end(&sleep_list)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (timer_ticks() >= t->wakeup_tick) {
        e = list_remove(e);
        thread_unblock(t);
    } else {
        break;  /* 정렬되어 있으므로 이후는 볼 필요 없음 */
    }
}
```

---

## 4. 비교 함수 2개

### `cmp_priority` — ready_list 우선순위 내림차순 정렬

```c
/* ready_list를 우선순위 기준으로 내림차순 정렬하기 위한 비교 함수.
   우선순위가 높은 thread가 리스트 앞쪽에 오도록 한다. */
static bool
cmp_priority(const struct list_elem *a,
             const struct list_elem *b,
             void *aux)
{
    (void) aux;
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->priority > tb->priority;  /* 내림차순 */
}
```

### `cmp_wakeup_tick` — sleep_list wakeup_tick 오름차순 정렬

```c
/* sleep_list를 wakeup_tick 기준으로 오름차순 정렬하기 위한 비교 함수.
   더 빨리 깨어나야 하는 thread가 리스트 앞쪽에 오도록 한다.
   timer_interrupt()에서 wakeup_tick을 넘은 첫 번째 thread를 만나면
   이후는 검사하지 않고 즉시 중단할 수 있어 효율적이다. */
static bool
cmp_wakeup_tick(const struct list_elem *a,
                const struct list_elem *b,
                void *aux)
{
    (void) aux;
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->wakeup_tick < tb->wakeup_tick;  /* 오름차순 */
}
```

**두 함수의 차이:**
- `cmp_priority` → 내림차순 (높은 게 앞으로) → `>`
- `cmp_wakeup_tick` → 오름차순 (작은 게 앞으로) → `<`

---

## 5. 핵심 개념 요약

| 용어 | 설명 |
|------|------|
| `tick` | 타이머 인터럽트가 발생할 때마다 1씩 올라가는 카운터. 1초 = 100 tick |
| `wakeup_tick` | 현재 tick + sleep할 tick. "몇 번째 tick에 깨어날지" 절대값 |
| `sleep_list` | BLOCKED 상태의 thread를 wakeup_tick 오름차순으로 보관 |
| `thread_block()` | RUNNING → BLOCKED. 인터럽트 꺼진 상태에서 호출해야 함 |
| `thread_unblock()` | BLOCKED → READY. ready_list에 삽입 |
| `timer_interrupt()` | 매 tick마다 하드웨어가 자동 호출. sleep_list 순회하여 깨울 thread unblock |
| `intr_disable()` | 인터럽트 끄기. 리스트 조작 구간 보호 |
| `list_insert_ordered()` | 정렬된 위치에 삽입. push_back 대신 써서 순회 최적화 |

---

## 6. 테스트 결과

```
pass  alarm-single
pass  alarm-multiple
pass  alarm-simultaneous
pass  alarm-zero
pass  alarm-negative
pass  alarm-priority  
```
