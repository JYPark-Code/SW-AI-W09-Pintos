# Pintos Project 1 — Priority Donation 학습 정리

## 1. 왜 필요한가 — Priority Inversion

priority scheduling만 적용하면 다음 시나리오에서 문제가 생긴다.

```
H (high)  ──┐
M (medium)──┼──> CPU 경쟁
L (low)   ──┘
            └─> L이 lock A 보유 중

진행:
  1. L 실행 → lock A acquire → critical section 진입
  2. H 도착 → lock A acquire 시도 → block (L이 들고 있음)
  3. M 도착 → ready_list 진입
  4. CPU는 ready_list에서 가장 높은 M을 고름 → L은 영원히 못 돔
  5. L이 못 도니 lock A를 release 못 함 → H도 영원히 못 깨어남
```

H는 M보다 우선순위가 높지만, L에 의해 막혀 있고 그 L은 M에게 밀린다.
결국 **H가 M에 의해 간접적으로 밀리는** 현상 = **priority inversion**.

해결 아이디어: **H가 L을 기다리는 동안 H의 우선순위를 L에게 빌려주자.**
그러면 L은 일시적으로 H 우선순위가 되어 M보다 먼저 돌고, 빠르게
critical section을 끝내고 lock을 풀 수 있다 → H 진입 가능. 이게
**priority donation**이다.

> priority scheduling 단계 정리는
> [priority_scheduling_study.md](priority_scheduling_study.md) 참고.

---

## 2. 만족해야 하는 케이스

Pintos 테스트는 다음 시나리오를 모두 검증한다.

| 케이스 | 설명 |
|--------|------|
| **single donation** | H가 L에게 한 번만 donate |
| **multiple donation** | 한 thread가 여러 lock을 들고 있을 때 lock별 donor 모두 추적 |
| **nested donation** | H → M(이 N의 lock 대기) → N 처럼 체인을 따라 전파 |
| **donation 회수** | lock release 시 그 lock 때문 donate 받은 것만 정확히 빼야 함 |
| **set_priority와 공존** | base가 바뀌어도 더 높은 donation이 살아 있으면 유지해야 함 |

이 5가지 요구가 자료구조와 함수 구조를 결정한다.

---

## 3. 자료구조 설계 — 왜 이 4개 필드인가

### `struct thread` (`include/threads/thread.h`)

```c
int original_priority;          /* donation 받기 전 base */
struct lock *wait_on_lock;      /* 지금 기다리는 lock (없으면 NULL) */
struct list donations;          /* 나에게 donate한 thread들 */
struct list_elem donation_elem; /* 다른 thread의 donations에 들어갈 때 사용 */
```

각 필드가 어떤 요구를 푸는지 1:1 매핑:

| 필드 | 푸는 문제 |
|------|-----------|
| `original_priority` | donation 끝나면 어디로 돌아갈지 — base를 잊으면 안 됨 |
| `wait_on_lock` | nested donation 체인을 따라가는 hop pointer (`cur->wait_on_lock->holder`) |
| `donations` | **multiple donation** 추적 — 한 thread가 여러 donor를 받을 수 있음 |
| `donation_elem` | `elem`은 ready_list/waiters/sleep_list 전용. donations에는 별도 elem 필요 |

> ⚠️ **`elem`과 `donation_elem`을 분리한 이유**:
> donor thread는 동시에 lock의 waiters(=`elem` 사용)에도 들어 있고,
> holder의 donations(=`donation_elem` 사용)에도 들어 있다. 같은 elem을
> 두 list에 넣으면 자료구조가 즉시 깨진다.

```c
/* threads/thread.c — init_thread */
t->original_priority = priority;
t->wait_on_lock = NULL;
list_init(&t->donations);
```

생성 시점에 `priority == original_priority`로 시작. donations는 비어 있음.
이걸 빠뜨리면 첫 donation 시점에 list가 깨지거나 garbage priority로
복원돼서 즉사한다.

---

## 4. 코드 흐름 — 순차적으로 왜 이렇게 짰나

`lock_acquire → lock_release → thread_set_priority` 순서로 따라간다.
각 함수에서 어떤 invariant를 지켜야 하는지 보면, 헬퍼 3종이 왜 필요한지
자연스럽게 나온다.

### 4.1 `lock_acquire` — donation의 등록 시점

```c
void
lock_acquire (struct lock *lock) {
    /* ... ASSERT 생략 ... */
    struct thread *cur = thread_current();

    if (lock->holder != NULL) {
        cur->wait_on_lock = lock;
        list_insert_ordered(&lock->holder->donations,
                            &cur->donation_elem,
                            cmp_donation_priority, NULL);
        donate_priority();
    }

    sema_down(&lock->semaphore);   /* ← block */
    cur->wait_on_lock = NULL;      /* 깨어났으니 더 이상 대기 X */
    lock->holder = cur;
}
```

**왜 이 순서인가?**

1. **lock->holder 검사가 먼저** — holder가 없으면 그냥 락을 잡으면 끝.
   donation이 발생할 일이 없다.
2. **`wait_on_lock` 설정을 sema_down 전에** — sema_down에서 block되는
   동안 다른 thread가 nested chain을 따라올 때, 내 `wait_on_lock`이
   이미 세팅돼 있어야 chain이 끊기지 않는다. (체인 추적은 `wait_on_lock`
   포인터로만 이뤄진다.)
3. **`donations`에 등록 → donate_priority** — holder의 donations에 자기를
   먼저 넣고 나서 priority를 끌어올려야, 나중에 lock_release에서
   `remove_with_lock`이 정확히 나를 찾아 뺄 수 있다.
4. **sema_down 이후 `wait_on_lock = NULL`** — 락을 잡는 데 성공했으니
   더 이상 대기 중이 아니다. 안 풀어주면 chain 추적할 때 endless loop
   소지가 있다.

### 4.2 `donate_priority` — nested chain 따라 전파

```c
static void
donate_priority(void) {
    struct thread *cur = thread_current();
    for (int depth = 0; depth < 8; depth++) {
        if (cur->wait_on_lock == NULL) break;
        struct thread *holder = cur->wait_on_lock->holder;
        if (holder == NULL) break;
        if (holder->priority >= cur->priority) break;

        holder->priority = cur->priority;
        cur = holder;                       /* 다음 hop */
    }
}
```

**왜 이 모양인가?**

- **루프인 이유**: nested 케이스 (`H → M → L`) 때문. 한 번만 올리면 M까지만
  올라가고 L은 그대로 → priority inversion 그대로 남음.
- **depth 8 제한**: Pintos spec 명시. 무한 루프 / 사이클 안전장치.
- **`cur->wait_on_lock == NULL` 종료**: chain 끝. 더 따라갈 hop이 없음.
- **`holder->priority >= cur->priority` 종료 (가지치기)**: holder가 이미
  나보다 높거나 같으면 더 올릴 필요 없음. 이걸 빼면 불필요하게 chain을
  끝까지 다 돌아 성능 손해.
- **`holder->priority = cur->priority` (대입, max 아님)**: 가지치기 조건
  덕분에 여기 도달한 시점이면 `holder->priority < cur->priority`가
  보장돼 있음 → max 호출 불필요.

### 4.3 `lock_release` — donation 회수

```c
void
lock_release (struct lock *lock) {
    /* ... ASSERT 생략 ... */
    remove_with_lock(lock);     /* ① 이 lock 때문에 들어온 donor 제거 */
    refresh_priority();         /* ② effective priority 재계산 */
    lock->holder = NULL;        /* ③ holder 풀기 */
    sema_up(&lock->semaphore);  /* ④ 다음 waiter 깨움 */
}
```

**왜 이 순서인가?**

- **① → ②가 핵심**: 이 lock 때문에 들어온 donor를 먼저 빼야,
  refresh가 "남아 있는 donation들 중 max"를 정확히 본다. 순서를 뒤집으면
  방금 푼 lock의 donor를 후보에 넣은 채 max를 계산해서 priority가 안 내려옴.
- **③ holder = NULL을 sema_up 전에**: sema_up 안에서 깨어난 thread가
  곧장 RUNNING이 되어 `lock->holder` 검사를 할 수 있는데, 이때 NULL이
  보여야 다음 acquirer의 자기 자신 holder 등록이 자연스럽게 된다.
- **④ sema_up 마지막**: 깨어난 thread가 더 높으면 즉시 yield가
  발생할 수 있는데(우리 구현 §priority_scheduling), 그 전에 ①~③이 다
  끝나 있어야 invariant 깨지지 않음.

### 4.4 `remove_with_lock` — multiple donation을 정확히 다룸

```c
static void
remove_with_lock(struct lock *lock) {
    struct thread *cur = thread_current();
    struct list_elem *e;

    for (e = list_begin(&cur->donations);
         e != list_end(&cur->donations);
         /* 비움 */) {
        struct thread *t = list_entry(e, struct thread, donation_elem);
        if (t->wait_on_lock == lock)
            e = list_remove(e);          /* 다음 elem 받기 */
        else
            e = list_next(e);
    }
}
```

**왜 그냥 `list_clear`가 아닌가?**

한 thread가 lock A, B를 동시에 들고 있고 각각 donor가 있을 수 있다.
A를 풀 때 B의 donor까지 같이 빼면 안 됨. **donor의 `wait_on_lock`이
지금 푸는 lock과 같은 것만** 골라서 빼야 한다.

**`list_remove`의 반환값을 다시 e로**: 순회 중 제거할 때의 표준 패턴.
반환값은 다음 elem이라 `list_next`를 따로 부를 필요 없다. (이걸 안 하면
삭제된 노드를 다시 next로 따라가다 깨짐.)

### 4.5 `refresh_priority` — effective priority 재계산

```c
void
refresh_priority(void) {
    struct thread *cur = thread_current();
    cur->priority = cur->original_priority;

    if (!list_empty(&cur->donations)) {
        struct list_elem *e =
            list_min(&cur->donations, cmp_donation_priority, NULL);
        struct thread *top = list_entry(e, struct thread, donation_elem);
        if (top->priority > cur->priority)
            cur->priority = top->priority;
    }
}
```

**왜 두 단계인가?**

- **1단계: `original_priority`로 초기화** — donation이 모두 사라진
  경우의 정답이다. base로 일단 돌려놓고 시작.
- **2단계: 남은 donation 중 max로 갱신** — donations가 비어 있지 않으면
  그중 가장 높은 값과 base를 비교해 큰 쪽을 채택.

**`list_min` + 내림차순 less 트릭**: `cmp_donation_priority`는
`a->priority > b->priority`(내림차순) less function이다. 그래서
`list_min`을 호출하면 의미가 뒤집혀 **max priority elem**이 나온다.
이건 §priority_scheduling에서 한번 데였던 함정인데, 같은 패턴을 의도적으로
재사용했다.

### 4.6 `thread_set_priority` — base 변경 vs donation

```c
void
thread_set_priority (int new_priority) {
    thread_current()->original_priority = new_priority;
    refresh_priority();

    if (!list_empty(&ready_list)) {
        struct thread *highest = list_entry(
            list_max(&ready_list, cmp_priority, NULL),
            struct thread, elem);
        if (highest->priority > thread_current()->priority)
            thread_yield();
    }
}
```

**왜 `priority`가 아니라 `original_priority`에 대입하나?**

`set_priority(new)`의 의미는 "내 base를 new로 바꿔라"이지
"effective priority를 강제로 new로 만들어라"가 아니다. 만약 더 높은
donation을 받고 있는 상태에서 base를 낮추면, donation은 유지하고
**lock release 시점에야** 낮춰진 base로 떨어져야 한다.

이걸 보장하는 게 `original_priority` 갱신 + `refresh_priority` 조합:
- donations 비어 있음 → priority = new_priority (사용자 의도대로)
- donations 있고 max > new → priority = donation max (보호됨)
- donations 있고 max ≤ new → priority = new (의도대로 상승)

세 케이스가 자연스럽게 한 함수로 처리된다.

### 4.7 `cmp_donation_priority` — 별도 비교 함수

```c
static bool
cmp_donation_priority (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED) {
    struct thread *ta = list_entry(a, struct thread, donation_elem);
    struct thread *tb = list_entry(b, struct thread, donation_elem);
    return ta->priority > tb->priority;
}
```

기존 `cmp_priority`는 `struct thread.elem` 기준이라 `list_entry(a, struct thread, elem)`
을 쓴다. donations 리스트는 `donation_elem`을 사용하므로 그대로 쓰면
**전혀 엉뚱한 메모리를 priority로 해석**한다. 비교 대상이 다른 elem이면
비교 함수도 분리해야 한다.

---

## 5. 실수했다가 고친 포인트

| 증상 | 원인 | 수정 |
|------|------|------|
| `priority-donate-multiple` 실패 | `lock_release`에서 `donations` 전체를 비움 | `remove_with_lock`으로 해당 lock 것만 제거 |
| `priority-donate-chain` 실패 | `donate_priority`에서 첫 hop만 올리고 종료 | `cur = holder`로 다음 hop 전진, depth 8 루프 |
| `priority-donate-lower` 실패 | `set_priority`에서 `priority`에 직접 대입 | `original_priority`만 갱신 + `refresh_priority` |
| 스택/리스트 깨짐 | donations에 `elem`을 넣음 | `donation_elem` 분리 사용 |
| chain 따라가다 무한 루프 의심 | depth 제한 없음 | `for (depth < 8)` |

---

## 6. 테스트 결과

```
priority-donate-multiple   PASS
priority-donate-one        PASS
priority-donate-lower      PASS
priority-donate-chain      PASS
priority-donate-nest       PASS
priority-donate-multiple2  PASS
priority-donate-sema       PASS
priority-condvar           PASS
priority-fifo              PASS
priority-sema              PASS
priority-change            PASS
priority-preempt           PASS
alarm-* (전체)             PASS
```

Project 1 thread 영역 18개 테스트 전체 PASS.

---

## 7. 한 줄 요약

> **요구 5가지(single/multiple/nested/회수/set_priority 공존)를 동시에
> 만족하려면, "base 분리 + chain hop 포인터 + donor 리스트 + 별도 elem"
> 4가지가 모두 필요했고, 그래서 헬퍼 3종(`donate_priority`/
> `remove_with_lock`/`refresh_priority`)이 자연스럽게 나뉘었다.**
