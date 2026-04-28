# Pintos API 레퍼런스 — Project 1에서 쓰는 / 알아야 할 함수 모음

작업하면서 만난 함수들을 한자리에 모아둔 문서. 각 함수마다 시그니처 →
설명 → 주의 → 예제 순서로 정리한다. 예제는 가능하면 이번 프로젝트
(alarm clock, priority scheduling)에서 실제로 쓴 코드를 사용한다.

---

## 0. 자주 쓰는 매크로

### `list_entry(LIST_ELEM, STRUCT, MEMBER)`
- 어떤 구조체의 `list_elem` 멤버 포인터로부터 그 **외곽 구조체 포인터**를 복원.
- 컴파일 타임 매크로 (`offsetof`로 주소 보정).

```c
struct list_elem *e = list_begin(&ready_list);
struct thread *t = list_entry(e, struct thread, elem);
```

### `ASSERT(cond)`
- false면 즉시 커널 panic. 로직상 절대 깨지면 안 되는 invariant 체크용.
- production 빌드에서도 켜져 있어 "성능 문제"로 빼면 안 된다.

```c
ASSERT (intr_get_level() == INTR_OFF);  /* 인터럽트가 꺼진 상태에서만 진행 */
```

### `UNUSED`
- 함수 매개변수 중 일부를 안 쓸 때 컴파일러 경고를 누르는 어트리뷰트.

```c
static bool
cmp_priority(const struct list_elem *a, const struct list_elem *b,
             void *aux UNUSED) { ... }
```

---

## 1. List API — `lib/kernel/list.h`

Pintos의 list는 **양쪽 끝에 sentinel(head/tail)을 둔 이중 연결 리스트**.
개별 노드는 `struct list_elem`으로 들어가고, 우리가 다루는 데이터는
`list_entry`로 외곽 구조체를 복원해 사용한다.

| 함수 | 한줄 설명 |
|------|----------|
| `list_init(list)` | 리스트 초기화. **사용 전 반드시 호출** |
| `list_begin(list)` | 첫 elem (data 노드) |
| `list_end(list)` | sentinel tail. 순회 종료 검사용 |
| `list_next(elem)` | 다음 elem |
| `list_front(list)` | 첫 데이터 elem (= `list_begin`이 가리키는 것) |
| `list_back(list)` | 마지막 데이터 elem |
| `list_push_back(list, elem)` | 뒤에 추가 (FIFO) |
| `list_push_front(list, elem)` | 앞에 추가 |
| `list_pop_front(list)` | 앞 elem 꺼내고 제거 |
| `list_pop_back(list)` | 뒤 elem 꺼내고 제거 |
| `list_remove(elem)` | 임의 위치 elem 제거. 다음 elem 반환 |
| `list_insert(before, elem)` | `before` 앞에 삽입 |
| `list_insert_ordered(list, elem, less, aux)` | 정렬된 위치에 삽입 |
| `list_sort(list, less, aux)` | 전체 정렬 |
| `list_min(list, less, aux)` | less 기준 **가장 작은** elem |
| `list_max(list, less, aux)` | less 기준 **가장 큰** elem |
| `list_empty(list)` | 비었는지 검사 |
| `list_size(list)` | 원소 개수 (순회하므로 O(n)) |

### `list_init`
```c
void list_init (struct list *);
```
사용 전 반드시 호출. 안 하면 sentinel이 자기 자신을 가리키는 상태가 아님 → 즉시 깨짐.

```c
struct list sleep_list;
void thread_init (void) {
    list_init(&sleep_list);
}
```

### `list_push_back` / `list_pop_front` (FIFO 패턴)
```c
void list_push_back (struct list *, struct list_elem *);
struct list_elem *list_pop_front (struct list *);
```
가장 단순한 큐. waiter들을 단순 보관할 때 자주 사용.

```c
list_push_back(&sema->waiters, &thread_current()->elem);
thread_block();
/* ... */
struct list_elem *e = list_pop_front(&sema->waiters);
struct thread *t = list_entry(e, struct thread, elem);
```

### `list_insert_ordered`
```c
void list_insert_ordered (struct list *, struct list_elem *,
                          list_less_func *, void *aux);
```
정렬 상태를 유지하며 삽입. 매 삽입 O(n)이지만 꺼낼 때 O(1)이 된다.
**삽입은 항상 ordered, 꺼낼 때 front만 보면 된다**는 패턴이 핵심.

```c
/* sleep_list를 wakeup_tick 오름차순으로 유지 */
list_insert_ordered(&sleep_list, &t->elem, cmp_wakeup_tick, NULL);
```

### `list_min` / `list_max` ⚠️ 헷갈림 주의

```c
struct list_elem *list_min (struct list *, list_less_func *, void *aux);
struct list_elem *list_max (struct list *, list_less_func *, void *aux);
```

- 둘 다 **less function**을 받는다. `less(a, b) == true`면 "a가 b보다 작다"로 해석.
- `cmp_priority`처럼 `>`(내림차순) less를 넘기면 의미가 뒤집혀 결과도 뒤집힌다.

| 호출 | `cmp_priority`(내림차순)와 함께 쓰면 |
|------|----------------------------------|
| `list_min(list, cmp_priority, NULL)` | **우선순위가 가장 큰** elem |
| `list_max(list, cmp_priority, NULL)` | **우선순위가 가장 작은** elem |

```c
/* sema_up: 가장 우선순위 높은 waiter 깨우기 */
struct list_elem *max_elem = list_min(&sema->waiters, cmp_priority, NULL);
list_remove(max_elem);
thread_unblock(list_entry(max_elem, struct thread, elem));
```

### `list_remove`
```c
struct list_elem *list_remove (struct list_elem *);
```
임의 위치 elem 제거. 반환값은 **다음 elem**이라 순회 중 제거할 때 유용.

```c
/* timer_interrupt에서 깰 시간이 된 thread를 sleep_list에서 빼기 */
struct list_elem *e = list_begin(&sleep_list);
while (e != list_end(&sleep_list)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (timer_ticks() >= t->wakeup_tick) {
        e = list_remove(e);          /* 다음 elem 받기 */
        thread_unblock(t);
    } else {
        break;                       /* 정렬돼 있으니 중단 */
    }
}
```

### 표준 순회 패턴
```c
for (struct list_elem *e = list_begin(&list);
     e != list_end(&list);
     e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, elem);
    /* ... */
}
```

### `list_less_func` 타입
```c
typedef bool list_less_func (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux);
```
`a < b`면 true. `aux`는 비교에 추가 컨텍스트가 필요할 때만 쓰고 보통 NULL.

```c
static bool cmp_wakeup_tick (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED) {
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    return ta->wakeup_tick < tb->wakeup_tick;   /* 오름차순 */
}
```

---

## 2. Thread API — `threads/thread.h`

| 함수 | 한줄 설명 |
|------|----------|
| `thread_create(name, priority, fn, aux)` | 새 thread 생성. 즉시 ready_list에 들어감 |
| `thread_block()` | RUNNING → BLOCKED. **인터럽트 끈 상태**에서 호출 |
| `thread_unblock(t)` | BLOCKED → READY. ready_list에 삽입 |
| `thread_yield()` | RUNNING → READY. 다른 thread에게 CPU 양보 |
| `thread_current()` | 현재 RUNNING thread 포인터 |
| `thread_tid()` | 현재 thread tid |
| `thread_exit()` | 현재 thread 종료. 돌아오지 않음 |
| `thread_set_priority(p)` | 현재 thread 우선순위 변경 |
| `thread_get_priority()` | 현재 thread 우선순위 조회 |

### `thread_create`
```c
tid_t thread_create (const char *name, int priority,
                     thread_func *function, void *aux);
```
- 새 thread 만들고 함수 진입점 등록 → **자동으로 unblock(ready)** 됨.
- 새 thread가 우선순위가 더 높으면 호출자가 `thread_yield`를 해야 즉시 동작.

```c
tid_t tid = thread_create("worker", PRI_DEFAULT, worker_fn, NULL);
if (priority > thread_current()->priority)
    thread_yield();
```

### `thread_block` ⚠️ 인터럽트 OFF 필수
```c
void thread_block (void);
```
- 호출 시점부터 thread가 멈춤. **호출 전에 어디에 들어 있는지(어떤 list에 등록했는지) 반드시 기록**해두어야 깨울 수 있다.
- `intr_disable()` 후 호출.

```c
enum intr_level old = intr_disable();
list_insert_ordered(&sleep_list, &t->elem, cmp_wakeup_tick, NULL);
thread_block();                /* 여기서 멈춤 */
intr_set_level(old);           /* 깨어난 후 복구 */
```

### `thread_unblock`
```c
void thread_unblock (struct thread *t);
```
- BLOCKED → READY. ready_list에 추가만 하고 **즉시 양보는 안 함**.
- 인터럽트 컨텍스트(`intr_context()`)에서도 호출 가능.

```c
/* timer_interrupt에서 호출 — intr_context이라 yield 못 함 */
thread_unblock(t);
```

### `thread_yield` ⚠️ 인터럽트 컨텍스트 금지
```c
void thread_yield (void);
```
- 자발적 CPU 양보. 자기를 ready_list에 넣고 schedule 호출.
- **인터럽트 핸들러 안에서는 호출 금지**. `intr_yield_on_return()` 사용.

```c
/* sema_up 직후 — 깨운 thread가 더 높은 우선순위면 즉시 양보 */
thread_unblock(t);
if (!intr_context())
    thread_yield();
```

### `thread_current`
```c
struct thread *thread_current (void);
```
- 항상 현재 RUNNING thread를 반환. 내부에서 magic number로 stack overflow 검사.

```c
thread_current()->wakeup_tick = start + ticks;
```

### `thread_set_priority` / `thread_get_priority`
```c
void thread_set_priority (int new_priority);
int  thread_get_priority (void);
```
- 우선순위 범위: `PRI_MIN(0)` ~ `PRI_MAX(63)`, 기본 `PRI_DEFAULT(31)`.
- 우선순위를 낮추면 즉시 ready_list 최고 우선순위와 비교 후 yield 해야 함.

```c
void thread_set_priority (int new_priority) {
    thread_current()->priority = new_priority;
    if (!list_empty(&ready_list)) {
        struct thread *highest = list_entry(
            list_min(&ready_list, cmp_priority, NULL),  /* ← list_min 주의 */
            struct thread, elem);
        if (highest->priority > thread_current()->priority)
            thread_yield();
    }
}
```

---

## 3. Synchronization API — `threads/synch.h`

### Semaphore

```c
void sema_init (struct semaphore *sema, unsigned value);
void sema_down (struct semaphore *sema);   /* P 연산: value-- (없으면 block) */
bool sema_try_down (struct semaphore *sema);
void sema_up   (struct semaphore *sema);   /* V 연산: value++, waiter 깨움 */
```

- 초기값 1로 만들면 mutex 흉내, N으로 만들면 자원 풀.
- `sema_down`은 인터럽트를 끄고 `value==0`이면 자기를 waiters에 push 후 block.
- `sema_up`은 waiter 하나 unblock 후 value 증가.

```c
struct semaphore sema;
sema_init(&sema, 0);

/* 생산자 */
sema_up(&sema);

/* 소비자 */
sema_down(&sema);
```

### Lock

```c
void lock_init (struct lock *lock);
void lock_acquire (struct lock *lock);
bool lock_try_acquire (struct lock *lock);
void lock_release (struct lock *lock);
bool lock_held_by_current_thread (const struct lock *lock);
```

- 내부 구현은 value=1 semaphore + holder 추적.
- **재귀 acquire 불가** — 같은 thread가 두 번 acquire하면 deadlock.
- `cond_wait`/`cond_signal` 호출자는 lock 보유자여야 한다 → ASSERT로 강제.

```c
struct lock lock;
lock_init(&lock);

lock_acquire(&lock);
/* critical section */
lock_release(&lock);
```

### Condition Variable

```c
void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);
```

- monitor 패턴. **lock을 들고 있는 상태**에서만 호출.
- `cond_wait`: lock 풀고 → block → 깨어나면 lock 다시 acquire (원자적으로 보장).
- 각 waiter는 자기 전용 `semaphore_elem`을 만들고 그 안의 semaphore에 단독 down.

```c
lock_acquire(&lock);
while (!ready_to_proceed)
    cond_wait(&cond, &lock);
/* ... */
lock_release(&lock);

/* 다른 쪽: */
lock_acquire(&lock);
ready_to_proceed = true;
cond_signal(&cond, &lock);
lock_release(&lock);
```

> `while`로 wrapping하는 이유: spurious wakeup 방어 + signal 후 깨어나기 전
> 다른 thread가 상태를 바꿨을 수 있음.

---

## 4. Interrupt API — `threads/interrupt.h`

```c
enum intr_level intr_get_level (void);
enum intr_level intr_set_level (enum intr_level);
enum intr_level intr_enable (void);
enum intr_level intr_disable (void);
bool intr_context (void);
void intr_yield_on_return (void);
```

- `enum intr_level`: `INTR_OFF`, `INTR_ON`.
- `intr_disable/enable`: 이전 level을 반환 → 복구 시 `intr_set_level(old)`.
- `intr_context()`: 인터럽트 핸들러 안에서 true.
- `intr_yield_on_return()`: 인터럽트 핸들러에서 yield하고 싶을 때
  → 핸들러 종료 직후 yield가 일어남.

### 임계영역 보호 패턴
```c
enum intr_level old = intr_disable();
/* list 조작 등 짧은 critical section */
intr_set_level(old);
```

### 인터럽트 컨텍스트 검사
```c
if (!intr_context())
    thread_yield();         /* 핸들러 안에서는 호출 금지 */
```

### timer_interrupt에서 yield하는 법
```c
void timer_interrupt (struct intr_frame *args UNUSED) {
    /* ... */
    if (some_condition)
        intr_yield_on_return();   /* 직접 yield 호출 X */
}
```

---

## 5. Timer API — `devices/timer.h`

```c
int64_t timer_ticks   (void);            /* 부팅 후 누적 tick */
int64_t timer_elapsed (int64_t then);    /* now - then */
void    timer_sleep   (int64_t ticks);   /* 지정한 tick만큼 대기 */
void    timer_msleep  (int64_t ms);
void    timer_usleep  (int64_t us);
void    timer_nsleep  (int64_t ns);
```

- `TIMER_FREQ = 100Hz` → **1초 = 100 tick** (기본 설정).
- `timer_ticks` 호출 시 인터럽트가 잠깐 꺼졌다 켜짐 (race 방지).
- `timer_sleep`은 우리가 alarm clock 단계에서 sleep_list 기반으로 교체.

### 사용 예
```c
int64_t start = timer_ticks();
/* ... */
if (timer_elapsed(start) >= 5)         /* 5 tick 경과 검사 */
    do_something();

timer_sleep(50);                       /* 0.5초 대기 */
```

### 우리가 구현한 timer_sleep
```c
void timer_sleep (int64_t ticks) {
    int64_t start = timer_ticks();
    ASSERT (intr_get_level() == INTR_ON);

    struct thread *t = thread_current();
    t->wakeup_tick = start + ticks;

    enum intr_level old = intr_disable();
    list_insert_ordered(&sleep_list, &t->elem, cmp_wakeup_tick, NULL);
    thread_block();
    intr_set_level(old);
}
```

---

## 6. Priority Donation 관련 자료구조 / 헬퍼

`priority_scheduling`의 다음 단계인 **priority donation**은 lock 보유자가
대기자보다 낮은 우선순위일 때 일시적으로 우선순위를 끌어올려 **priority
inversion**을 막는 기법이다. 여기서 다루는 건 Pintos가 기본 제공하는
API가 아니라 **우리가 직접 만져야 할 자료구조와 작성해야 할 헬퍼**들이다.

### 6.1 `struct thread` 추가 필드 (`include/threads/thread.h`)

```c
int             priority;           /* 현재 우선순위 (= effective priority) */
int             original_priority;  /* donation 받기 전 원래 우선순위 */
struct lock    *wait_on_lock;       /* 지금 기다리고 있는 lock, 없으면 NULL */
struct list     donations;          /* 나에게 donation 한 thread들의 리스트 */
struct list_elem donation_elem;     /* 다른 thread의 donations에 들어갈 때 사용 */
```

| 필드 | 역할 | 갱신 시점 |
|------|------|-----------|
| `priority` | 스케줄러가 보는 값. donation으로 일시 상승 가능 | donation, refresh, set_priority |
| `original_priority` | donation 끝나면 돌아갈 baseline | 생성 시 / `thread_set_priority` |
| `wait_on_lock` | nested donation 체인을 따라가는 핵심 포인터 | `lock_acquire` 시작 / 락 획득 직후 |
| `donations` | 같은 thread가 여러 lock을 들고 있을 때 lock 별로 들어온 donation을 모두 추적 | `lock_acquire` / `lock_release` |
| `donation_elem` | `donations` 리스트 연결용. **`elem`과 분리**되어야 함 (ready_list와 동시에 들어 있을 수 있음) | — |

> ⚠️ `elem`은 ready_list / waiters / sleep_list 용이고
> `donation_elem`은 다른 thread의 `donations` 용. **절대 같이 쓰면 안 됨.**

### 6.2 `struct lock` (`include/threads/synch.h`)

```c
struct lock {
    struct thread *holder;           /* 현재 lock을 보유한 thread (없으면 NULL) */
    struct semaphore semaphore;      /* 내부 binary semaphore */
};
```

donation 체인 추적은 `current->wait_on_lock->holder` → `holder->wait_on_lock->holder`
… 형태로 따라간다. `holder`만 있으면 충분.

### 6.3 작성해야 할 헬퍼 3종

`synch.c` 상단에서 이미 선언만 해둔 함수들. 호출 위치까지 같이 정리.

#### `donate_priority(void)` — nested donation 전파
```c
static void donate_priority (void);
```
- 호출 위치: **`lock_acquire`**에서 `sema_down` 직전 (락 보유자가 있을 때).
- 동작: 현재 thread의 priority를 `wait_on_lock->holder`에게 전달하고,
  그 holder가 또 다른 lock을 기다리고 있으면 그 holder에게도 전달.
- **depth ≤ 8** 제한 (Pintos spec).
- 종료 조건: `wait_on_lock == NULL` || `holder == NULL` ||
  holder의 priority가 이미 충분히 큼.

#### `refresh_priority(void)` — donation 끝난 뒤 effective priority 재계산
```c
static void refresh_priority (void);
```
- 호출 위치:
  1. `lock_release` 직후 (그 lock으로 받은 donation을 정리한 후)
  2. `thread_set_priority` 안 (사용자가 base를 바꿨을 때)
- 동작:
  ```
  priority = original_priority
  if (donations 비어있지 않음)
      priority = max(priority, donations 안의 최대 priority)
  ```

#### `remove_with_lock(struct lock *lock)` — 특정 lock 관련 donation 정리
```c
static void remove_with_lock (struct lock *lock);
```
- 호출 위치: **`lock_release`** 안, `sema_up` 직전.
- 동작: 현재 thread의 `donations`를 순회하며 `wait_on_lock == lock`
  인 entry를 모두 제거.
- 이게 필요한 이유: 한 thread가 여러 lock을 들고 있으면 `donations`에는
  서로 다른 lock 때문에 들어온 donor들이 섞여 있음 → 지금 푸는 lock 것만
  골라서 빼야 함.

### 6.4 호출 시점 한눈에 보기 ⭐

```
init_thread:
    priority           = priority_arg
    original_priority  = priority_arg
    wait_on_lock       = NULL
    list_init(&donations)

lock_acquire:
    if (lock->holder != NULL) {
        current->wait_on_lock = lock
        list_insert_ordered(&holder->donations,
                            &current->donation_elem,
                            cmp_donation_priority, NULL)
        donate_priority()
    }
    sema_down(&lock->semaphore)        ← 여기서 block / 깨어남
    current->wait_on_lock = NULL
    lock->holder = current

lock_release:
    remove_with_lock(lock)
    refresh_priority()
    lock->holder = NULL
    sema_up(&lock->semaphore)

thread_set_priority(new):
    original_priority = new
    refresh_priority()
    /* effective priority가 ready_list 최댓값보다 낮아지면 yield */
```

> mlfqs 모드(Project 1 Part 4)에서는 donation을 끄도록 `if (!thread_mlfqs)`
> 분기를 둬야 하지만, 지금 단계에선 무시해도 OK.

### 6.5 `donations` 리스트 비교 함수

기존 `cmp_priority`는 `struct thread.elem`을 가정한다. donations는
`donation_elem`을 사용하므로 별도 비교 함수가 필요.

```c
static bool
cmp_donation_priority (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED) {
    struct thread *ta = list_entry(a, struct thread, donation_elem);
    struct thread *tb = list_entry(b, struct thread, donation_elem);
    return ta->priority > tb->priority;   /* 내림차순 less */
}
```

`list_insert_ordered`로 항상 정렬 유지 → `list_front`만 봐도 max 가능.
또는 `list_min(donations, cmp_donation_priority, NULL)`로 뽑아도 됨
(섹션 1에서 봤던 `list_min` ↔ "내림차순 less" 함정 그대로 적용).

---

## 7. 자주 쓰는 패턴 모음

### 패턴 1 — "인터럽트 끄고 list 조작 후 block"
```c
enum intr_level old = intr_disable();
list_insert_ordered(&some_list, &t->elem, cmp_fn, NULL);
thread_block();
intr_set_level(old);
```
- list 조작 도중 timer_interrupt가 끼어들면 자료구조가 깨질 수 있어 disable 필수.

### 패턴 2 — "정렬된 list에서 조건 만족하는 prefix만 처리"
```c
struct list_elem *e = list_begin(&sleep_list);
while (e != list_end(&sleep_list)) {
    struct thread *t = list_entry(e, struct thread, elem);
    if (CONDITION) {
        e = list_remove(e);
        thread_unblock(t);
    } else {
        break;     /* 정렬돼 있어 뒤는 볼 필요 없음 */
    }
}
```

### 패턴 3 — "가장 높은 우선순위 waiter 깨우기"
```c
struct list_elem *e = list_min(&waiters, cmp_priority, NULL);
list_remove(e);
thread_unblock(list_entry(e, struct thread, elem));
if (!intr_context())
    thread_yield();
```

### 패턴 4 — "condition variable 표준 사용"
```c
lock_acquire(&lock);
while (!CONDITION)
    cond_wait(&cond, &lock);
/* CONDITION 만족, 작업 수행 */
lock_release(&lock);
```

### 패턴 5 — "nested donation 체인 따라 priority 전파"
```c
/* donate_priority 본체 — depth 8 제한 */
struct thread *cur = thread_current();
for (int depth = 0; depth < 8; depth++) {
    if (cur->wait_on_lock == NULL) break;
    struct thread *holder = cur->wait_on_lock->holder;
    if (holder == NULL) break;
    if (holder->priority >= cur->priority) break;  /* 이미 충분 */
    holder->priority = cur->priority;
    cur = holder;                                  /* 다음 hop */
}
```

### 패턴 6 — "donation 끝난 뒤 effective priority 재계산 (refresh_priority)"
```c
struct thread *cur = thread_current();
cur->priority = cur->original_priority;            /* baseline 복원 */

if (!list_empty(&cur->donations)) {
    /* donations가 cmp_donation_priority(내림차순 less)로 정렬되어 있다면
       front가 최대 — list_min과 list_front 어느 쪽이든 OK */
    struct list_elem *e = list_front(&cur->donations);
    struct thread *top = list_entry(e, struct thread, donation_elem);
    if (top->priority > cur->priority)
        cur->priority = top->priority;
}
```

### 패턴 7 — "특정 lock 때문에 들어온 donation만 골라서 제거 (remove_with_lock)"
```c
struct thread *cur = thread_current();
struct list_elem *e = list_begin(&cur->donations);
while (e != list_end(&cur->donations)) {
    struct thread *t = list_entry(e, struct thread, donation_elem);
    if (t->wait_on_lock == lock)
        e = list_remove(e);          /* 다음 elem 받기 */
    else
        e = list_next(e);
}
```

### 패턴 8 — "lock_acquire에서 donation 등록"
```c
struct thread *cur = thread_current();
if (lock->holder != NULL) {
    cur->wait_on_lock = lock;
    list_insert_ordered(&lock->holder->donations,
                        &cur->donation_elem,
                        cmp_donation_priority, NULL);
    donate_priority();
}
sema_down(&lock->semaphore);            /* 여기서 block */
cur->wait_on_lock = NULL;               /* 깨어났으면 더 이상 대기 X */
lock->holder = cur;
```

---

## 8. 흔히 하는 실수 체크리스트

- [ ] `list_init` 안 부르고 `list_push_back` 호출 → 즉시 깨짐
- [ ] `thread_block` 호출 전에 어떤 list에도 자기를 등록 안 함 → 영원히 못 깨움
- [ ] `thread_block` 호출 시 인터럽트 ON 상태 → race condition
- [ ] 인터럽트 핸들러 안에서 `thread_yield` 직접 호출 → panic
- [ ] `cmp_priority`(내림차순)에 `list_max` 사용 → 의도와 반대
- [ ] `cond_wait`을 `if`로 wrapping (정답: `while`)
- [ ] `lock_acquire` 호출자가 이미 lock 보유 → deadlock (재귀 lock 아님)
- [ ] `sema_up` 직후 yield 안 함 → 깨운 thread가 더 높은 우선순위여도 안 돌아감

### Priority donation 전용

- [ ] `init_thread`에서 `list_init(&donations)` 누락 → 첫 donation 시점에 깨짐
- [ ] `init_thread`에서 `original_priority` 초기화 누락 → `refresh_priority`가 garbage 사용
- [ ] `donations`에 `elem`을 넣음 (정답: `donation_elem`) → ready_list / waiters 자료구조 손상
- [ ] `thread_set_priority`에서 `priority`에 직접 대입하고 끝 → 더 높은 donation을 무시하게 됨 (정답: `original_priority` 갱신 + `refresh_priority`)
- [ ] `cmp_donation_priority`가 `>`(내림차순)인데 `list_max` 사용 → 가장 낮은 donation 반환
- [ ] `lock_acquire`에서 `wait_on_lock = NULL`을 `sema_down` **전에** 풀어버림 → nested donation 체인이 끊어져 전파 실패
- [ ] `lock_release`에서 `remove_with_lock` 없이 `refresh_priority`만 호출 → 이미 풀린 lock의 donor가 계속 남아 priority가 안 내려옴
- [ ] donation chain에서 depth 제한 없음 → 사이클이나 깊은 체인에서 무한 루프 가능 (Pintos spec: 최대 8)
