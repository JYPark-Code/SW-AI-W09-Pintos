# Pintos Project 2 — Argument Passing & 기본 시스템 콜

## 수정 파일 목록

| 파일 | 변경 내용 |
|---|---|
| `include/threads/thread.h` | `exit_status` 필드 추가 |
| `userprog/process.c` | `argument_stack()` 신규 작성, `process_exec()` 수정, `process_exit()` 종료 메시지 추가 |
| `userprog/syscall.c` | `SYS_HALT`, `SYS_EXIT` 케이스 추가 |

---

## 1. 왜 수정했는가

`process_exec()`는 ELF 바이너리를 메모리에 올리고 `do_iret()`으로 유저 모드로 전환한다.
그런데 전환 시점에 유저 스택에 아무것도 없으면 `main(argc, argv)` 의 인자가 쓰레기 값이 되어
argv 관련 테스트 전체(`args-*`)가 실패한다.

또한 유저 프로그램이 종료할 때 `exit()` 시스템 콜을 호출하는데,
`SYS_EXIT`가 없으면 핸들러가 `thread_exit()`만 하고 종료 코드를 버린다.
`SYS_HALT`가 없으면 `halt` 테스트가 unhandled syscall 로 죽는다.

---

## 2. `thread.h` — `exit_status` 필드 추가

```c
#ifdef USERPROG
    uint64_t *pml4;
    int exit_status;   /* SYS_EXIT로 전달받은 종료 코드 */
#endif
```

`init_thread()`가 `memset(t, 0, sizeof *t)`을 호출하므로
별도 초기화 없이 기본값 0이 보장된다.

`#ifdef USERPROG` 안에 넣는 이유:
커널 전용 스레드(idle, timer interrupt handler 등)에는 유저 프로세스 개념이 없으므로
`USERPROG` 빌드일 때만 구조체 크기를 늘린다.

---

## 3. `process.c` — `argument_stack()`

### 스택 레이아웃 (낮은 주소 방향으로 성장)

```
높은 주소
┌──────────────────────────────┐
│ argv[0] 문자열 \0            │  ← 가장 먼저 push
│ argv[1] 문자열 \0            │
│ ...                          │
│ argv[argc-1] 문자열 \0       │
├──────────────────────────────┤
│ (word-align 패딩, 0~7 bytes) │
├──────────────────────────────┤
│ NULL sentinel  (8 bytes)     │  ← argv[argc] = 0 (C 표준)
├──────────────────────────────┤
│ argv[argc-1] 포인터 (8 bytes)│
│ ...                          │
│ argv[0] 포인터   (8 bytes)   │  ← rsi 여기를 가리킨다
├──────────────────────────────┤
│ fake return address = 0      │  ← rsp 여기
└──────────────────────────────┘
낮은 주소
```

### 6단계 구현 순서

```c
static void
argument_stack (char **argv, int argc, struct intr_frame *_if) {
    uintptr_t arg_addr[ARGV_MAX];
    uint8_t *rsp = (uint8_t *) _if->rsp;

    /* 1. 문자열 데이터 push (argv[argc-1] → argv[0]) */
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen (argv[i]) + 1;
        rsp -= len;
        memcpy (rsp, argv[i], len);
        arg_addr[i] = (uintptr_t) rsp;   /* 나중에 포인터로 재사용 */
    }

    /* 2. word-align: x86-64 ABI는 call 직전 rsp가 16-byte 정렬을 요구 */
    rsp = (uint8_t *) ((uintptr_t) rsp & ~(uintptr_t) 7);

    /* 3. NULL sentinel: argv[argc] = 0 */
    rsp -= sizeof (uintptr_t);
    *(uintptr_t *) rsp = 0;

    /* 4. argv 포인터 배열 역순 push (argv[argc-1] → argv[0]) */
    for (int i = argc - 1; i >= 0; i--) {
        rsp -= sizeof (uintptr_t);
        *(uintptr_t *) rsp = arg_addr[i];
    }

    /* rsi = argv[] 배열 시작 주소 — fake return addr push 이전에 캡처 */
    _if->R.rsi = (uint64_t) rsp;
    _if->R.rdi = (uint64_t) argc;

    /* 5. fake return address */
    rsp -= sizeof (uintptr_t);
    *(uintptr_t *) rsp = 0;

    /* 6. rsp 최종 반영 */
    _if->rsp = (uintptr_t) rsp;
}
```

### 핵심 포인트: rsi를 왜 rsp+8로 계산하면 안 되는가

`do_iret()` 이후 %rsi 레지스터가 곧바로 `main(int argc, char **argv)`의
두 번째 인자로 전달된다.
즉, **rsi = argv 배열의 시작 주소 (argv[0] 포인터가 있는 스택 위치)** 여야 한다.

fake return address를 push한 뒤 `rsp + 8`로 계산하면 틀리지 않지만,
fake return address를 push하기 **전에** 현재 rsp 값을 rsi에 직접 저장하는 방식이
의도를 더 명확히 표현한다.

### word-align이 필요한 이유

x86-64 System V ABI는 `call` 명령 직전 rsp가 16-byte 정렬 상태임을 보장한다.
문자열 데이터 길이는 임의적이므로 push 후 rsp가 정렬에서 어긋날 수 있다.
정렬되지 않은 상태로 SSE 명령(`movaps` 등)이 실행되면 #GP fault가 발생한다.

---

## 4. `process.c` — `process_exec()` 수정

### 변경 전후 비교

```c
/* 변경 전 */
success = load (file_name, &_if);
palloc_free_page (file_name);

/* 변경 후 */
// 1. strtok_r로 파싱 (file_name 페이지 살아있는 동안)
for (token = strtok_r (file_name, " ", &save_ptr); ...)
    argv[argc++] = token;

// 2. argv[0]만 load에 넘김
success = load (argv[0], &_if);

// 3. load 실패 시에만 즉시 해제
if (!success) { palloc_free_page (file_name); return -1; }

// 4. 스택 세팅 완료 후 palloc 해제 (argv[]가 file_name 내부를 가리키므로)
argument_stack (argv, argc, &_if);
palloc_free_page (file_name);

do_iret (&_if);
```

### palloc_free_page 타이밍이 중요한 이유

`strtok_r`은 `file_name` 버퍼를 in-place로 수정한다(공백을 `\0`으로 치환).
`argv[i]` 포인터들은 이 버퍼 내부의 각 토큰을 가리킨다.
`argument_stack()`이 `strlen(argv[i])`로 길이를 계산하고 `memcpy`로 스택에 복사하므로,
**스택 세팅이 완전히 끝난 뒤에** 페이지를 해제해야 한다.

따라서 순서는 반드시 `strtok_r 파싱 → load → argument_stack → palloc_free_page`이어야 한다.

---

## 5. `process.c` — `process_exit()` 종료 메시지

```c
void
process_exit (void) {
    struct thread *curr = thread_current ();

    if (curr->pml4 != NULL)
        printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

    process_cleanup ();
}
```

`pml4 != NULL` 조건으로 유저 프로세스만 메시지를 출력한다.
커널 스레드(idle 등)는 `pml4`가 0으로 초기화되어 있으므로 자동으로 걸러진다.

테스트 프레임워크의 `.ck` 파일은 이 형식을 파싱한다:
```
프로그램이름: exit(종료코드)
```

---

## 6. `syscall.c` — `SYS_HALT` / `SYS_EXIT`

### 번호 확인 (`syscall-nr.h`)

```c
SYS_HALT = 0,   /* Halt the operating system. */
SYS_EXIT = 1,   /* Terminate this process.    */
```

### 구현

```c
case SYS_HALT:
    power_off ();       /* QEMU/Bochs에 shutdown 신호 — NO_RETURN */
    NOT_REACHED ();

case SYS_EXIT: {
    int status = (int) f->R.rdi;    /* x86-64 ABI: 첫 번째 인자 = rdi */
    thread_current ()->exit_status = status;
    thread_exit ();     /* 내부에서 process_exit() 호출 */
    NOT_REACHED ();
}
```

### x86-64 syscall ABI 요약

| 레지스터 | 역할 |
|---|---|
| `rax` | syscall 번호 / 반환값 |
| `rdi` | 인자 1 |
| `rsi` | 인자 2 |
| `rdx` | 인자 3 |

`thread_exit()`가 `process_exit()`를 호출하므로 별도로 `process_exit()`를 직접 호출하지 않는다.

---

## 7. 핵심 개념 요약

| 용어 | 설명 |
|---|---|
| `argument_stack()` | load 이후 유저 스택에 argc/argv를 배치하는 helper |
| `strtok_r` | thread-safe 문자열 토크나이저. 버퍼를 in-place 수정 |
| `word-align` | rsp를 8-byte 경계에 맞추는 작업. SSE 계열 명령 fault 방지 |
| `NULL sentinel` | `argv[argc] = 0`. C 표준이 요구하는 argv 끝 표시 |
| `fake return address` | main()의 반환 주소 자리를 0으로 채움. 실제로 리턴되면 SYS_EXIT를 거침 |
| `exit_status` | SYS_EXIT 인자를 thread 구조체에 저장. process_wait()와 종료 메시지에 사용 |
| `pml4` | Page Map Level 4. NULL이면 커널 스레드로 판별 |
| `power_off()` | `threads/init.h` 선언. QEMU에 shutdown 신호를 보냄 |

---

## 8. 버그 수정 — `palloc_free_page` 순서 오류

### 증상

```
Page fault at 0x4747effe: not present error writing page in kernel context.
Kernel PANIC at userprog/exception.c: Kernel bug - unexpected interrupt in kernel
```

`args-none`, `halt`, `exit` 등 args / halt / exit 테스트 전부 동일한 주소(`0x4747effe`)에서 커널 패닉.

### 원인 분석

초기 구현에서 `palloc_free_page(file_name)`을 `argument_stack()` **이전**에 호출했다.

```
strtok_r 파싱  →  load()  →  palloc_free_page()  →  argument_stack()  ← 버그
```

`argv[i]` 포인터들은 `file_name` palloc 페이지 내부를 가리킨다.
페이지가 해제된 뒤 `strlen(argv[i])`를 호출하면 해제된 메모리의 쓰레기 값을 읽어
길이가 203바이트처럼 비정상적으로 커진다.
그 결과 `rsp`가 `USER_STACK - PGSIZE(0x4747F000)` 보다 낮은 주소까지 내려가
매핑되지 않은 페이지에 쓰기를 시도하여 커널 Page Fault 발생.

### 수정

```
strtok_r 파싱  →  load()  →  argument_stack()  →  palloc_free_page()  ← 수정
```

`argument_stack()` 완료 후 `palloc_free_page()` 호출로 순서를 변경했다.

### 교훈

포인터가 살아있는 동안은 메모리를 해제하지 않는다.
"파싱 후 해제"라는 주석이 맞아도, `argument_stack()`이 그 포인터를 **다시 읽는다**는 점을 놓쳤다.

---

## 9. 두 번째 버그 — TIMEOUT (process_wait 미구현 + thread name 잘림)

### 증상

`make tests/userprog/<name>.result`로 정식 프레임워크 검증 시:

| 테스트 | 결과 | 원인 |
|---|---|---|
| `halt` | PASS | 유저 프로그램이 직접 `power_off()` |
| `exit`, `args-none` | FAIL · TIMEOUT | 출력은 정상이나 커널이 종료 못 함 |
| `args-single`, `args-multiple`, `args-many`, `args-dbl-space` | FAIL · 출력 mismatch + TIMEOUT | 종료 메시지의 thread name이 잘림 + TIMEOUT |

`args-single`의 mismatch:
```
- args-single: exit(0)        ← expected
+ args-single one: exit(0)    ← actual
```

### 원인 1: thread name이 cmdline 전체로 들어가 16자에서 잘림

`struct thread`의 `name[16]` 한계 때문에 `"args-single onearg"`(18자)가
`"args-single one\0"`(15자 + null)으로 잘렸다. `process_create_initd`에서
cmdline 전체를 `thread_create`에 넘긴 게 원인.

**수정**: `process_exec`에서 인자 파싱 후 thread 이름을 `argv[0]`로 갱신.

```c
strlcpy (thread_current ()->name, argv[0],
         sizeof thread_current ()->name);
```

### 원인 2: stage 0 영구 블록 스텁이 main을 power_off에 도달 못 하게 함

stage 0의 `process_wait`가 자식 종료와 무관하게 `sema_down`으로 영구 블록 →
부모(initial_thread)가 `power_off()`까지 도달 못 함 → 테스트 프레임워크 TIMEOUT.

**수정**: stage 1 wait/exit 정식 구현 (semaphore 동기화).

---

## 10. stage 1 wait/exit 동기화 구현

### 자료구조 (thread.h, `#ifdef USERPROG`)

```c
struct thread *parent;          /* 나를 만든 스레드 */
struct list children;           /* 내가 만든 자식들 */
struct list_elem child_elem;    /* 부모의 children에 들어가는 노드 */
struct semaphore wait_sema;     /* 자식이 up, 부모가 down */
struct semaphore exit_sema;     /* 부모가 up, 자식이 down */
```

### 부모-자식 연결 (thread.c)

`thread_create`가 새 스레드를 만들 때 호출 스레드의 children list에 자동 등록:

```c
#ifdef USERPROG
    t->parent = thread_current ();
    list_push_back (&thread_current ()->children, &t->child_elem);
#endif
```

`init_thread`에서 모든 스레드의 list/sema 초기화 (initial_thread 포함).

### 두 세마포어의 역할

| 세마포어 | 누가 down | 누가 up | 목적 |
|---|---|---|---|
| `wait_sema` | 부모 (process_wait) | 자식 (process_exit) | 자식 종료 신호 |
| `exit_sema` | 자식 (process_exit) | 부모 (process_wait) | exit_status 회수 동안 자식 thread 보존 |

### 동기화 시퀀스

```
[부모: process_wait]              [자식: process_exit]
sema_down(child->wait_sema) ⏸    
                                  printf("name: exit(N)")
                                  process_cleanup()
                                  sema_up(self->wait_sema)  ──┐
   ⏸ ← ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ── ──┘
                                  sema_down(self->exit_sema) ⏸
status = child->exit_status     (BLOCKED — struct alive)
list_remove(&child->child_elem)
sema_up(child->exit_sema) ──┐
                              └→ ⏸ 깨어남
return status                     return → thread_exit
                                  → do_schedule(DYING)
                                  → struct 해제
```

### 핵심 invariant

- 부모가 `child->exit_status`를 읽는 시점에 자식은 `exit_sema`에서 BLOCKED 상태로
  thread struct가 살아있음 (do_schedule(DYING)에 진입하지 않았으므로)
- 부모의 `sema_up(exit_sema)` 이후에야 자식이 die할 수 있음
- 동일 `child_tid`로 두 번째 `process_wait` 호출 시 list_remove로 인해 검색 실패 → -1

### 한계 (이후 단계 작업)

- 부모가 wait 없이 먼저 죽으면 자식이 `exit_sema`에서 영구 블록 → 좀비 누수.
  부모 process_exit 시 children 정리 로직 필요.
- 예외(bad memory access 등)로 죽는 프로세스의 `exit_status = -1` 설정 필요
  (현재는 `init_thread`의 memset으로 0이 되어 있음).

---

## 11. 테스트 결과

```
PASS  args-none
PASS  args-single
PASS  args-multiple
PASS  args-many
PASS  args-dbl-space
PASS  halt
PASS  exit
```

---

## 12. wait 계열 테스트는 왜 아직 못 도는가

세마포어 동기화로 stage 1 wait는 정식 구현했지만, **`wait-simple` 같은 wait 테스트는 여전히 실행조차 안 된다**. 팀 설명용 정리.

### 12.1 우리가 만든 건 "커널 함수"이지 "유저 syscall"이 아니다

```
┌─────────────────────────────┐
│ 유저 프로그램 (wait-simple.c)│
│  fork("child-simple");      │   ← 이걸 호출하면
│  wait(pid);                 │
└──────────┬──────────────────┘
           │ syscall instruction
           ▼
┌─────────────────────────────┐
│ syscall_handler             │
│  case SYS_FORK:  ?          │   ← 라우팅 없음
│  case SYS_WAIT:  ?          │   ← 라우팅 없음
│  case SYS_EXEC:  ?          │   ← 라우팅 없음
└─────────────────────────────┘
           │
           ▼
┌─────────────────────────────┐
│ process_wait()  ✅ 구현 완료 │
│ process_exit()  ✅ 구현 완료 │
│ process_fork()  ❌ stub      │
│ __do_fork()     ❌ TODO 천지 │
└─────────────────────────────┘
```

`process_wait()` 자체는 작동하지만 **유저가 부를 입구가 막혀 있다**.

### 12.2 wait 테스트는 자식 생성을 fork/exec에 의존

`wait-simple.c`:
```c
if ((pid = fork ("child-simple"))) {
    msg ("wait(exec()) = %d", wait (pid));
} else {
    exec ("child-simple");
}
```

**3개 syscall이 모두 필요**:
- `fork` → 자식 만들기
- `exec` → 자식이 다른 프로그램 로드
- `wait` → 부모가 자식 회수

하나라도 없으면 테스트 진행 자체가 안 됨.

### 12.3 `__do_fork`는 미완성 덩어리

`process.c`에 골격만 있고 **TODO 천지**:

```c
/* 1. TODO: If the parent_page is kernel page, then return immediately. */
/* 3. TODO: Allocate new PAL_USER page for the child */
/* 4. TODO: Duplicate parent's page */
/* 6. TODO: if fail to insert page, do error handling. */
/* TODO: somehow pass the parent_if. */
/* TODO: Hint) To duplicate the file object, use file_duplicate */
```

페이지 테이블 복제, 파일 디스크립터 복제, 부모 `intr_frame` 전달 — 전부 미구현.

### 12.4 그럼 args/halt/exit는 어떻게 통과했나

이 7개 테스트는 **fork/exec/wait 없이도** 동작하는 구조다:

```
kernel main
  → process_create_initd("args-none")
  → thread_create(initd, ...)         ← 커널 내부 API
  → initd 스레드 실행
  → process_exec("args-none")         ← ELF 로드만, 자식 안 만듦
  → 유저 프로그램 실행 → exit(0)
  → kernel main의 process_wait(initd_tid)가 회수
```

자식 생성이 **kernel main 한 곳에서만**, 그것도 `thread_create` 직접 호출이라
우리 동기화 인프라로 충분했다.

`wait-*` 테스트는 **유저 → fork → 자식 생성** 경로라 `__do_fork`가 필수.

### 12.5 한 줄 요약

> wait의 **동기화 배관**은 깔았지만, 유저가 접근할 **수도꼭지(syscall handler)와 자식 생성기(fork)**가 아직 없어서 wait 테스트는 못 돈다.

### 12.6 다음 단계 작업량 추정

| 항목 | 예상 분량 |
|---|---|
| `SYS_WAIT` 라우팅 | 5줄 (`process_wait(f->R.rdi)` 호출) |
| `SYS_EXEC` 라우팅 + 구현 | 30줄 (유저 포인터 검증 + filesys + load) |
| `SYS_FORK` + `__do_fork` 완성 | 80~100줄 (page table 복제 + fd 복제 + parent_if 전달) |

지금 7개 통과 → 다음으로 wait 계열 풀려면 **fork 구현이 가장 큰 고비**.
