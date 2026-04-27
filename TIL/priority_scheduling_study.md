# Pintos Project 1 — Priority Scheduling 학습 정리

## 1. 왜 수정했는가

Pintos 기본 스케줄러는 `ready_list`에 `list_push_back`으로 thread를 넣고
`list_pop_front`로 꺼내기 때문에, 더 높은 우선순위 thread가 먼저
실행된다는 보장이 없다.

요구사항은 다음과 같다.
- 더 높은 우선순위 thread는 항상 먼저 CPU를 받는다.
- 자신보다 높은 우선순위 thread가 ready 상태가 되면 즉시 양보(preemption)한다.
- semaphore / condition variable의 waiters도 가장 우선순위가 높은 thread부터 깨운다.

> alarm clock 단계의 sleep/wakeup·`sleep_list` 변경은 [alarm_clock_study.md](alarm_clock_study.md) 참고.

---

## 2. 변경 파일과 함수

### `threads/thread.h`
- `cmp_priority()` 외부 선언 추가 → `synch.c`에서도 ready 우선순위 비교를 사용해야 하므로 헤더에 노출
- `extern struct list sleep_list` 선언 (alarm clock 단계에서 도입)

```c
bool cmp_priority(const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux);

extern struct list sleep_list;
```

### `threads/thread.c`
- `cmp_priority`의 `static` 제거 → 외부 모듈에서 호출 가능
- `thread_set_priority()` — 우선순위 변경 후 즉시 preemption 검사

```c
void
thread_set_priority (int new_priority) {
    thread_current()->priority = new_priority;

    /* ready_list에 더 높은 우선순위가 있으면 즉시 양보 */
    if (!list_empty(&ready_list)) {
        struct thread *highest = list_entry(
            list_max(&ready_list, cmp_priority, NULL),
            struct thread, elem);

        if (highest->priority > thread_current()->priority)
            thread_yield();
    }
}
```

> ⚠️ **검토 필요**: `cmp_priority`는 내림차순(`a->priority > b->priority`)이라
> `list_max`와 함께 쓰면 less function 의미가 뒤집혀 **가장 우선순위가 낮은**
> thread가 반환된다. 의도대로라면 `list_min`을 쓰거나, ready_list가 이미
> 우선순위 정렬되어 있으므로 `list_front(&ready_list)`를 쓰는 것이 맞다.
> 자세한 내용은 §4 참고.

### `threads/synch.c`

#### `sema_up()` — 가장 높은 우선순위 waiter를 깨우도록 수정

기존:
```c
if (!list_empty (&sema->waiters))
    thread_unblock (list_entry (list_pop_front (&sema->waiters),
                struct thread, elem));
sema->value++;
```

문제점:
- `sema_down()`은 `list_push_back`으로 waiters에 추가하므로 정렬되어 있지 않다.
- `list_pop_front`로 꺼내면 우선순위 무시.

수정 후:
```c
sema->value++;
if (!list_empty (&sema->waiters)) {
    /* 1. 가장 높은 우선순위 waiter 찾기
          cmp_priority가 내림차순이므로 list_min이 곧 "최고 우선순위" */
    struct list_elem *max_elem = list_min(&sema->waiters, cmp_priority, NULL);

    /* 2. 리스트에서 제거 */
    list_remove(max_elem);

    /* 3. unblock */
    thread_unblock(list_entry(max_elem, struct thread, elem));

    /* 4. 깨운 thread가 현재보다 우선순위가 높으면 즉시 양보 */
    if (!intr_context())
        thread_yield();
}
```

**포인트**
- `sema->value++`를 `if` 위로 옮긴 이유: `thread_yield()` 시점엔 이미 value가
  증가된 상태여야 다른 thread가 acquire 시도 시 통과할 수 있다.
- `intr_context()` 검사: 인터럽트 핸들러 안에서는 `thread_yield()` 호출 금지.
- `list_min` + `cmp_priority(내림차순)` = 우선순위 최댓값 (§4 참고).

#### `cmp_sema_priority()` — 신규 추가

```c
static bool
cmp_sema_priority(const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED) {
    struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
    struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

    struct thread *ta = list_entry(list_begin(&sa->semaphore.waiters),
                                   struct thread, elem);
    struct thread *tb = list_entry(list_begin(&sb->semaphore.waiters),
                                   struct thread, elem);
    return ta->priority > tb->priority;
}
```

왜 `list_begin`?
- `cond_wait()`에서 각 thread는 자기 전용 `semaphore_elem`을 만들고
  그 안의 semaphore에 단독으로 `sema_down`한다.
- 따라서 `semaphore_elem.semaphore.waiters`에는 thread가 정확히 1개.
- `list_begin`으로 그 thread를 꺼내 우선순위를 비교한다.

#### `cond_signal()` — 가장 높은 우선순위 thread를 깨우도록 수정

기존:
```c
if (!list_empty (&cond->waiters))
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                struct semaphore_elem, elem)->semaphore);
```

수정 후:
```c
if (!list_empty (&cond->waiters)) {
    struct list_elem *max_sema_elem =
        list_min(&cond->waiters, cmp_sema_priority, NULL);
    list_remove(max_sema_elem);
    sema_up(&list_entry(max_sema_elem,
                        struct semaphore_elem, elem)->semaphore);
}
```

---

## 3. 누적된 thread.c / timer.c 변경 한눈에 보기

| 함수 | 변경 내용 | 단계 |
|------|---------|------|
| `thread_init()` | `list_init(&sleep_list)` 추가 | alarm clock |
| `thread_unblock()` | `list_push_back` → `list_insert_ordered(cmp_priority)` | alarm clock |
| `thread_yield()` | `list_push_back` → `list_insert_ordered(cmp_priority)` | alarm clock |
| `thread_create()` | unblock 후 우선순위 비교 → 새 thread가 높으면 yield | alarm clock |
| `thread_set_priority()` | ready_list 최고 우선순위와 비교, 낮아지면 yield | **priority** |
| `cmp_priority()` | `static` 제거 → 외부 노출 | **priority** |
| `cmp_wakeup_tick()` | 신규 (sleep_list 정렬용) | alarm clock |
| `timer_sleep()` | busy-waiting 제거, block 방식으로 교체 | alarm clock |
| `timer_interrupt()` | sleep_list 순회, wakeup_tick 도달 시 unblock | alarm clock |

---

## 4. `list_min` / `list_max`와 `cmp_priority` 관계 (헷갈림 주의)

`list_min(less)`, `list_max(less)`는 둘 다 **"less" 의미의 비교 함수**를 받는다.
즉 `less(a, b) == true`면 "a가 b보다 작다"로 해석한다.

그런데 우리가 쓰는 `cmp_priority`는:
```c
return a->priority > b->priority;   /* 내림차순 */
```

less 의미로 해석하면 "우선순위가 큰 쪽이 작다"가 된다. 결과적으로:

| 호출 | 실제로 반환되는 것 |
|------|---------------------|
| `list_min(list, cmp_priority, NULL)` | **우선순위가 가장 큰** thread |
| `list_max(list, cmp_priority, NULL)` | **우선순위가 가장 작은** thread |

> 따라서 "가장 높은 우선순위"를 찾을 땐 **`list_min`**을 사용해야 한다.
> `thread_set_priority()`의 `list_max` 사용은 검토 필요.
> ready_list는 이미 `cmp_priority` 내림차순으로 정렬되어 있어서
> `list_front(&ready_list)` 한 줄이면 가장 높은 우선순위 thread를 얻을 수 있다.

---

## 5. 핵심 개념 요약

| 용어 | 설명 |
|------|------|
| Preemption | 더 높은 우선순위 thread가 ready 되면 현재 thread가 즉시 CPU 양보 |
| `cmp_priority` | ready 우선순위 내림차순 비교 (a > b) |
| `cmp_sema_priority` | semaphore_elem 안의 단일 waiter thread 우선순위 비교 |
| `list_min` + 내림차순 less | 최댓값(가장 높은 우선순위) 반환 |
| `intr_context()` | 인터럽트 핸들러 안 여부. true면 `thread_yield()` 호출 금지 |

---

## 6. 테스트 결과

```
(추후 업데이트)
```
