# Stage 1 process_wait — semaphore vs `for(;;) timer_sleep(100)` 설계 결정

> Userprog Stage 1 의 `process_wait` 정식 구현을 두고 팀 안에서 두 가지 안이 부딪혔다.
> 어떻게 **semaphore 패턴**으로 합의했는가, 그 결정의 컨텍스트를 정리한다.

---

## 1. 상황 — 두 안이 부딪힘

Stage 0 의 임시 스텁 (`sema_down(&stub)` 으로 영구 블록) 을 정식 구현으로 바꿔야 했다.
"자식 종료를 어떻게 기다릴 것인가" 에 대해 두 안이 나왔다.

### 안 A — 폴링 (`for(;;) timer_sleep(100)`)

```c
int
process_wait (tid_t child_tid) {
    struct thread *child = find_child (child_tid);
    if (child == NULL) return -1;

    for (;;) {
        if (child->status == THREAD_DYING)   /* 또는 별도 done 플래그 */
            break;
        timer_sleep (100);                   /* 100 tick 자고 다시 확인 */
    }
    return child->exit_status;
}
```

특징:
- 자식이 죽었는지 주기적으로 깨어나서 확인
- 코드는 짧고 직관적
- "blocking primitive" 를 새로 도입하지 않아도 됨 (이미 있는 `timer_sleep` 만 사용)

### 안 B — semaphore (부모-자식 동기화)

```c
int
process_wait (tid_t child_tid) {
    struct thread *child = find_child (child_tid);
    if (child == NULL) return -1;

    sema_down (&child->wait_sema);           /* 자식이 process_exit에서 up */
    int exit_status = child->exit_status;
    list_remove (&child->child_elem);
    sema_up (&child->exit_sema);             /* 자식의 do_schedule(DYING) 진입 허락 */
    return exit_status;
}
```

특징:
- 자식이 죽는 순간 정확히 한 번 부모를 깨움
- `wait_sema` + `exit_sema` 두 개로 자식 thread 본체의 수명까지 안전하게 관리
- 자료구조(`children` list, 두 sema) 를 thread.h 에 추가해야 함

---

## 2. 두 안의 기술 비교

| 축 | 폴링 (안 A) | semaphore (안 B) |
|---|---|---|
| **응답성** | 평균 50tick, 최악 100tick 지연 | 즉시 (자식 sema_up 즉시 부모 깨움) |
| **CPU 사용** | 자식 종료 전까지 100tick 마다 깨어나 확인 (낭비) | 부모는 BLOCKED — schedule 대상에서 빠짐 |
| **정확성** | `child->status` 만으로는 부족: 자식이 `DYING` 후 `do_schedule` 까지 가기 전 race 가능 | `process_exit` 에서 명시적 sema_up — race-free |
| **자식 thread 본체 수명** | 부모가 `exit_status` 읽기 전에 자식이 free 되면 use-after-free | `exit_sema` 로 부모 회수 끝날 때까지 자식 보존 |
| **코드 명료성** | 4~5 줄, 한 눈에 이해 | 자료구조 + 시퀀스 다이어그램 필요 |
| **확장성** | wait4·waitpid·SIGCHLD 같은 진짜 OS 의미론으로 확장 어려움 | 동일 패턴이 fork/exec/exit 전반으로 자연스럽게 확장 |
| **테스트 통과** | tick 간격 튜닝 필요, 어떤 테스트는 통과 못 할 가능성 | 표준 패턴 — 정식 테스트셋 (`wait-simple`, `wait-twice` 등) 그대로 통과 |

핵심: **폴링은 단순해 보이지만, race-free 하게 만들려면 결국 sema_up 비슷한 신호 메커니즘을 직접 만들어야 한다.** 그리고 자식 thread 의 use-after-free 를 막기 위한 두 번째 동기화 (안 B 의 `exit_sema` 역할) 도 어차피 필요하다 → 결국 폴링은 **불완전한 안 B** 가 된다.

---

## 3. 설득의 컨텍스트 흐름 — 왜 안 B 였나

기술 비교만으론 의외로 결정이 안 났다. "안 A 가 짧고 일단 통과만 하면 된다" 는 의견도 합리적이었기 때문이다. 그래서 **팀이 이미 쌓아온 컨텍스트** 를 근거로 설득했다.

### Step 1 — CSAPP `proxy lab` 이 우리에게 남긴 것

CSAPP (Computer Systems: A Programmer's Perspective) 의 마지막 lab, **proxy lab** 에서 우리는 멀티스레드 웹 프록시를 만들면서 **producer-consumer 큐를 semaphore 로 직접 구현**했다.

남은 자산:
- `sem_init`, `sem_wait`, `sem_post` 의 의미를 코드로 직접 만져봄
- "신호로 깨우는 패턴" 의 시각·시퀀스 감각이 머리에 박혀 있음
- 폴링이 왜 안 좋은지 (CPU 낭비, latency 지터) 를 글이 아니라 측정으로 경험

→ "semaphore" 라는 단어가 우리에겐 추상 개념이 아니라 **이미 손에 익은 도구**.

### Step 2 — 지난주 Pintos 1주차 — 우리가 직접 만든 것

지난주 Project 1 (threads) 에서:
- **alarm clock** 을 `timer_sleep` busy-wait 에서 sleep_list + sema_down 패턴으로 갈아엎음 ← 폴링→semaphore 전환을 우리 손으로 직접 해본 경험
- **priority scheduling**, **priority donation** 까지 가면서 sema 내부 (waiters list, donation chain) 를 깊게 들여다봄
- 그 결과를 **팀 발표** 까지 했다

남은 자산:
- `struct semaphore` 가 내부적으로 어떻게 waiters list 를 관리하고 thread 를 BLOCKED 로 두는지 정확히 안다
- `sema_down` 시 schedule 이 어디서 끊기는지 코드 라인 단위로 추적 가능
- 발표 자료가 있어 팀 전원이 같은 멘탈 모델을 공유

→ Stage 1 의 `wait_sema`, `exit_sema` 두 개를 도입하는 데 **새로 학습할 게 사실상 없다**.

### Step 3 — 결정타: "안 쓸 이유가 없지 않나"

이 두 자산을 테이블에 올리고 던진 한 줄:

> **"semaphore 는 우리가 두 번이나 만든 도구다.
> 이미 손에 익은 도구를 두고 폴링을 새로 들이밀 이유가 있나?"**

폴링 안의 가장 큰 메리트로 거론됐던 "단순함" 은:
- 우리에겐 semaphore 도 이미 단순하다 (지난주에 발표한 주제)
- 폴링은 race / use-after-free 까지 고려하면 결국 안 단순해진다 (§2 표)

→ 안 B 로 합의.

---

## 4. 최종 구현 (코드는 `process.c`)

```c
int
process_wait (tid_t child_tid) {
    struct thread *cur = thread_current ();
    struct thread *child = NULL;

    /* 1) children list 에서 child_tid 찾기 (없으면 -1) */
    for (struct list_elem *e = list_begin (&cur->children);
         e != list_end (&cur->children); e = list_next (e)) {
        struct thread *t = list_entry (e, struct thread, child_elem);
        if (t->tid == child_tid) { child = t; break; }
    }
    if (child == NULL) return -1;

    /* 2) 자식이 process_exit 에서 up 할 때까지 블록 */
    sema_down (&child->wait_sema);

    /* 3) 깨어난 시점: 자식은 exit_sema 에서 BLOCKED → thread 본체 안전 */
    int exit_status = child->exit_status;
    list_remove (&child->child_elem);

    /* 4) 자식이 do_schedule(DYING) 으로 진입하도록 신호 */
    sema_up (&child->exit_sema);

    return exit_status;
}
```

대응되는 `process_exit`:
```c
void
process_exit (void) {
    struct thread *curr = thread_current ();
    if (curr->pml4 != NULL)
        printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
    process_cleanup ();
    if (curr->pml4 != NULL) {
        sema_up (&curr->wait_sema);     /* 부모를 깨움 */
        sema_down (&curr->exit_sema);   /* 부모가 회수할 때까지 살아있기 */
    }
}
```

자료구조 추가 (`thread.h`, `#ifdef USERPROG`):
```c
struct thread *parent;
struct list children;
struct list_elem child_elem;
struct semaphore wait_sema;
struct semaphore exit_sema;
```

> 더 자세한 시퀀스 다이어그램과 race 분석은 `argument_passing_and_basic_syscalls.md` §10 참조.

---

## 5. 회고

**무엇이 옳았나**:
- "이미 가진 도구로 푼다" 는 결정 — 새로운 학습 부담이 0 이라 곧바로 구현·디버깅에 들어갈 수 있었다
- 정식 테스트 (`wait-simple`, `wait-twice`, `multi-child-fd` 등) 한 번에 통과
- Stage 2 (file syscalls), Stage 3 (fork) 로 가면서 같은 sema 패턴이 그대로 재활용됨 → 누적된 일관성

**만약 안 A 였다면**:
- 처음 한두 테스트는 통과했을 가능성도 있음 (단순 wait-simple)
- 그러나 race 가 드러나는 순간 (`wait-killed`, `multi-child-fd` 등) 에서 디버깅이 폭발했을 것
- 결국 sema 비슷한 신호 메커니즘을 직접 만들어야 했을 것 — "단순함" 이 사라짐

**일반화된 교훈**:
- 설계 결정에서 기술 표 비교가 동률이면 **팀의 누적 컨텍스트** 가 결정 근거가 된다
- "지난 lab/주차에서 이미 만진 도구" 는 표 위에 안 보이지만 실제론 가장 큰 비용 항목
- "안 쓸 이유가 없다" 는 부정형 논거가 의외로 강력 — 새 도구 도입의 입증 책임을 상대 안에 두기 때문

---

## 6. 참고

- `pintos/userprog/process.c` `process_wait`, `process_exit`
- `pintos/include/threads/thread.h` `wait_sema`, `exit_sema`, `children`, `parent`, `child_elem`
- `TIL/argument_passing_and_basic_syscalls.md` §9 (TIMEOUT 버그) §10 (구현 상세)
- `TIL/userprog_stage0_study.md` (스텁 단계의 임시 sema 사용)
- `TIL/priority_donation_study.md` (지난주 sema 학습 자산)
- CSAPP 12장 (concurrent programming) — proxy lab 의 producer-consumer 패턴
