# Pintos Project 2 — userprog stage 1 발표 워크스루

argument passing + halt/exit + process_wait/exit 동기화 작업의
**개념 설명 + 코드 투어** 가이드.

발표나 코드리뷰에서 "어디부터 봐야 하나요?" 질문을 받았을 때 이 문서 하나로 따라갈 수 있도록 정리.

---

## 0. 한 줄 요약

> stage 0의 영구 블록 스텁을 정식 wait/exit 동기화로 교체하고,
> argument passing + halt + exit 시스템 콜을 구현해
> args / halt / exit 7개 테스트를 PASS시켰다.

| 테스트 | 결과 |
|---|---|
| args-none / args-single / args-multiple / args-many / args-dbl-space | PASS |
| halt | PASS |
| exit | PASS |

---

## 1. 핵심 개념 4가지

### 1.1 Argument Passing — 유저 스택에 argc/argv 배치

`do_iret`로 유저 모드 진입할 때 스택이 비어 있으면 `main(argc, argv)`의 인자가 쓰레기가 된다.
load() 직후의 빈 유저 스택에 6단계로 argc/argv를 배치해야 한다.

**스택 레이아웃** (높→낮 방향으로 push):

```
높은 주소
┌──────────────────────────┐
│ argv[0] 문자열 \0        │
│ argv[1] 문자열 \0        │
│ ...                      │
├──────────────────────────┤
│ word-align padding       │   ← rsp & ~7
├──────────────────────────┤
│ NULL sentinel (8B)       │   ← argv[argc] = 0
├──────────────────────────┤
│ argv[argc-1] 포인터      │
│ ...                      │
│ argv[0] 포인터 (8B)      │   ← rsi 가 여기를 가리킴
├──────────────────────────┤
│ fake return = 0 (8B)     │   ← rsp 가 여기
└──────────────────────────┘
낮은 주소
```

**x86-64 ABI 핵심**:
- `rdi` = 1번째 인자 → `argc`
- `rsi` = 2번째 인자 → `argv[]` 배열의 시작 주소
- `do_iret`이 인터럽트 프레임의 R.rdi/R.rsi를 그대로 레지스터에 복원하므로,
  `intr_frame`에 직접 박아두면 main의 인자로 전달된다.

**왜 rsi를 명시 저장하는가**: fake return push 이후 `rsp + 8`로 계산해도 되지만,
의도가 안 드러나고 오프셋 실수 위험. push **전에** `_if->R.rsi = (uint64_t) rsp`로 캡처.

### 1.2 SYS_HALT / SYS_EXIT — 종료 입구

| syscall | 동작 |
|---|---|
| `SYS_HALT (0)` | `power_off()` 즉시 호출. QEMU shutdown. |
| `SYS_EXIT (1)` | `rdi` → `thread->exit_status` 저장 → `thread_exit()` |

`thread_exit()`이 내부에서 `process_exit()`을 호출하므로 직접 안 부른다.

### 1.3 process_wait/exit 동기화 — 두 세마포어

부모(`process_wait`)가 자식 종료를 기다렸다가 `exit_status`를 회수해야 한다.
하지만 자식 thread struct가 죽기 전에 부모가 read해야 dangling pointer를 피한다.

**두 세마포어의 역할**:

| 세마포어 | down | up | 목적 |
|---|---|---|---|
| `wait_sema` | 부모 (process_wait) | 자식 (process_exit) | 자식 종료 신호 |
| `exit_sema` | 자식 (process_exit) | 부모 (process_wait) | exit_status 회수 동안 자식 보존 |

**시퀀스 다이어그램**:

```
[부모: process_wait]                [자식: process_exit]
sema_down(child->wait_sema) ⏸    
                                    printf("name: exit(N)")
                                    process_cleanup()
                                    sema_up(self->wait_sema) ──┐
   ⏸ ← ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ──┘
                                    sema_down(self->exit_sema) ⏸
status = child->exit_status        (BLOCKED — struct alive)
list_remove(&child->child_elem)
sema_up(child->exit_sema) ──┐
                              └→ ⏸ 깨어남
return status                     return → thread_exit
                                  → do_schedule(DYING) → struct 해제
```

★ 자식이 `exit_sema`에서 BLOCKED인 구간에서 부모가 안전하게 `exit_status` read.

### 1.4 부모-자식 연결 — `thread_create`의 자동 등록

유저가 `thread_create`로 새 스레드를 만들면 자동으로 호출 스레드의 `children` list에 등록되도록 만들었다. `process_wait`은 이 list를 순회해 자식을 찾는다.

```c
#ifdef USERPROG
    t->parent = thread_current ();
    list_push_back (&thread_current ()->children, &t->child_elem);
#endif
```

---

## 2. 전체 흐름 도식 — `pintos -- run "args-single onearg"` 한 사이클

### 2.1 부팅 + 자식 생성

```
[initial_thread (main)]                          [kernel space]
    │
    ├─ run_actions("args-single onearg")
    │
    ├─ process_create_initd("args-single onearg")
    │     └─ thread_create("args-single onearg", initd, fn_copy)
    │            ├─ palloc_get_page(PAL_ZERO) → t
    │            ├─ init_thread(t, name, pri)
    │            ├─ t->parent = current  ◄── 자동 연결
    │            ├─ list_push_back(&current->children, &t->child_elem)
    │            └─ thread_unblock(t)
    │                  ↓ ready_list로
    │                                          [initd thread t]
    └─ process_wait(initd_tid)              (스케줄 대기)
          ├─ children 검색 → 발견
          └─ sema_down(&initd->wait_sema) ⏸ BLOCKED
```

### 2.2 자식 실행 — process_exec

```
[initd thread]
    │
    └─ process_exec(f_name="args-single onearg")
         ├─ process_cleanup()                 ← 기존 pml4 회수
         │
         ├─ strtok_r 파싱
         │     argv = ["args-single", "onearg"], argc = 2
         │
         ├─ strlcpy(thread->name, argv[0])    ← 16자 잘림 방지
         │
         ├─ load("args-single", &_if)
         │     ├─ pml4_create + process_activate
         │     ├─ ELF 로드
         │     └─ setup_stack: _if.rsp = USER_STACK = 0x47480000
         │
         ├─ argument_stack(argv, argc, &_if)
         │     ── 6단계로 rsp/rdi/rsi 세팅
         │
         ├─ palloc_free_page(file_name)        ← argv 사용 끝난 후
         │
         └─ do_iret(&_if) ──→ [user mode 진입]
```

### 2.3 user mode → exit() → 동기화 → 종료

```
[user: args-single]
    └─ main(2, argv) → ... → exit(0)
         │ syscall (rax=SYS_EXIT, rdi=0)
         ▼
[kernel: syscall_handler]
    └─ thread_current()->exit_status = 0
       thread_exit()
         └─ process_exit()
              ├─ printf("args-single: exit(0)\n")
              ├─ process_cleanup()
              ├─ sema_up(curr->wait_sema) ──→ initial_thread 깨움
              └─ sema_down(curr->exit_sema) ⏸

[initial_thread 깨어남]
    ├─ exit_status = child->exit_status  (=0)
    ├─ list_remove(&child->child_elem)
    ├─ sema_up(&child->exit_sema) ──→ initd 깨움
    └─ return → main() → power_off() ◄── shutdown

                                       [initd 깨어남]
                                          └─ thread_exit
                                                → do_schedule(DYING)
```

### 2.4 시점별 invariant

| 시점 | initial_thread | initd | child struct alive? |
|---|---|---|---|
| `sema_down(wait)` 직전 | RUNNING | READY | YES |
| 자식 `sema_up(wait)` 직후 | READY | RUNNING | YES |
| 자식 `sema_down(exit)` 진입 | RUNNING | BLOCKED | YES (★) |
| 부모 `sema_up(exit)` 직후 | RUNNING | READY | YES |
| 자식 `do_schedule(DYING)` | RUNNING/READY | DYING | NO |

★ 부모가 안전하게 read하는 구간.

---

## 3. 코드 투어 — 어디 어디부터 보면 되는가

발표용 추천 순서. 실행 흐름과 일치하도록 5곳을 짰다.

### ① 자료구조 — `pintos/include/threads/thread.h` 105~116

```c
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint64_t *pml4;
    int exit_status;

    struct thread *parent;
    struct list children;
    struct list_elem child_elem;
    struct semaphore wait_sema;
    struct semaphore exit_sema;
#endif
```

**한 마디**: "wait 동기화에 필요한 5필드. 두 세마포어가 핵심."

### ② 부모-자식 연결 — `pintos/threads/thread.c::thread_create` (≈200행)

```c
init_thread (t, name, priority);
tid = t->tid = allocate_tid ();

#ifdef USERPROG
    t->parent = thread_current ();
    list_push_back (&thread_current ()->children, &t->child_elem);
#endif
```

`init_thread`에는 `list_init(&t->children)` + `sema_init` 두 줄도 추가.

**한 마디**: "thread_create로 만들면 자동 연결. process_wait이 children 순회해서 찾는다."

### ③ argument passing — `pintos/userprog/process.c::process_exec` + `argument_stack`

`process_exec`의 흐름 (한 화면에 다 보일 정도로 짧음):

```c
for (token = strtok_r (file_name, " ", &save_ptr);
     token != NULL && argc < ARGV_MAX;
     token = strtok_r (NULL, " ", &save_ptr))
    argv[argc++] = token;

strlcpy (thread_current ()->name, argv[0],
         sizeof thread_current ()->name);

success = load (argv[0], &_if);
if (!success) {
    palloc_free_page (file_name);
    return -1;
}

argument_stack (argv, argc, &_if);
palloc_free_page (file_name);    /* ← 반드시 argument_stack 후 */

do_iret (&_if);
```

`argument_stack`은 6단계 코드. 1.1의 다이어그램과 함께 보여준다.

**한 마디**:
- `process_exec`: "파싱 → 이름 갱신 → load → 스택 세팅 → 해제 순서. 해제를 먼저 하면 dangling pointer."
- `argument_stack`: "rsi는 fake return push 전에 캡처. push 후 +8 계산하지 않는 이유는 의도 명시."

### ④ syscall 라우팅 — `pintos/userprog/syscall.c::syscall_handler`

```c
case SYS_HALT:
    power_off ();
    NOT_REACHED ();

case SYS_EXIT: {
    int status = (int) f->R.rdi;
    thread_current ()->exit_status = status;
    thread_exit ();
    NOT_REACHED ();
}
```

**한 마디**: "유저 프로그램의 입구. exit는 status를 thread에 박아두고 thread_exit 호출 → 내부에서 process_exit."

### ⑤ 동기화 핵심 — `pintos/userprog/process.c::process_wait` + `process_exit`

**좌우 분할 화면**으로 띄우면 좋음.

`process_wait`:
```c
for (e = list_begin (&cur->children); e != list_end (&cur->children);
     e = list_next (e)) {
    struct thread *t = list_entry (e, struct thread, child_elem);
    if (t->tid == child_tid) { child = t; break; }
}
if (child == NULL) return -1;

sema_down (&child->wait_sema);
exit_status = child->exit_status;
list_remove (&child->child_elem);
sema_up (&child->exit_sema);

return exit_status;
```

`process_exit`:
```c
if (is_user_process)
    printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

process_cleanup ();

if (is_user_process) {
    sema_up (&curr->wait_sema);
    sema_down (&curr->exit_sema);
}
```

**한 마디**: "한 세마포어로는 race. 두 세마포어로 부모가 read 끝낼 때까지 자식 struct를 살려둔다."
→ 2.4 invariant 표를 같이 띄워 ★ 구간을 짚는다.

---

## 4. 발표 화면 전환 순서 (실전)

```
[작업 분해 표 — 14.2 sty]
    ↓
① thread.h (자료구조)
    ↓
② thread.c (부모-자식 연결)
    ↓
2.2 다이어그램 + ③ process_exec + argument_stack
    ↓
④ syscall.c
    ↓
2.3 다이어그램 + ⑤ process_wait/exit (좌우 분할)
    ↓
2.4 invariant 표
```

---

## 5. 예상 질문 대비

| 질문 | 답할 곳 |
|---|---|
| thread name 잘림은 어디서 고쳤나 | `process_exec`의 `strlcpy(thread->name, argv[0], ...)` 한 줄 |
| init_thread는 왜 건드렸나 | `init_thread` 안의 `list_init(&t->children)` + 두 sema_init |
| exit_status 초기값은 | `init_thread`의 `memset(t, 0, ...)`로 0 (Unix 관례) |
| load() 안에서는 안 건드렸나 | `setup_stack`은 그대로, `_if->rsp = USER_STACK`만 확인 |
| 왜 wait 테스트는 못 도나 | `syscall_handler`에 SYS_FORK/SYS_WAIT 라우팅 없음 + `__do_fork` 미구현 |
| timer_sleep 꼼수 안 쓴 이유 | wait 테스트는 어차피 못 풀고, 1초 타이밍 race 위험. 정식 stage 1 sema는 어차피 다음 단계에 재사용. |

---

## 6. 구현 과정에서 마주친 두 버그 (선택 발표 소재)

### Bug 1 — Page Fault `0x4747effe`

증상: args/halt/exit 7개가 동일 주소에서 커널 패닉.

추적:
- `0x4747effe < USER_STACK - PGSIZE (0x4747F000)` → 매핑 외 영역
- 모든 테스트가 같은 주소 → 인자 길이 무관
- `rdx = 0xcc = 204`바이트 memcpy → strlen이 비정상값

원인: `palloc_free_page`를 `argument_stack` **이전**에 호출 → `argv[i]` dangling → `strlen(argv[i])`이 쓰레기에서 큰 값 반환 → rsp가 매핑 밖으로 내려감.

수정: 해제 순서를 `argument_stack` **이후**로.

**교훈**: 포인터가 살아있는 동안 메모리를 해제하지 않는다. "파싱 후 해제"라는 주석이 맞아도 그 포인터를 **다시 읽는** 함수를 놓치면 버그.

### Bug 2 — TIMEOUT (수동 vs 프레임워크 간극)

수동 실행에선 `args-none: exit(0)` 출력 OK였는데 `make tests/userprog/args-none.result`에선 TIMEOUT.

원인: 출력은 정상이지만 `process_wait` stub의 영구 블록으로 부모가 `power_off()`까지 도달 못 함 → 테스트 프레임워크가 정해진 시간 내 종료를 못 봐서 FAIL.

**교훈**:
- 수동 실행은 "Kernel PANIC만 안 나면 OK"의 false positive 가능.
- 진짜 검증은 항상 정식 프레임워크 (`.result` 파일)로.

---

## 7. 변경 파일 4개 요약

| 파일 | 변경 |
|---|---|
| `pintos/include/threads/thread.h` | `synch.h` include + 5필드 추가 |
| `pintos/threads/thread.c` | `init_thread` list/sema 초기화, `thread_create` 부모-자식 연결 |
| `pintos/userprog/process.c` | `argument_stack` 신규, `process_exec` 파싱+name 갱신, `process_wait`/`process_exit` 정식 sema 동기화 |
| `pintos/userprog/syscall.c` | `init.h` include + SYS_HALT, SYS_EXIT 케이스 |
