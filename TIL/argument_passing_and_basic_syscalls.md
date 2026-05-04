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

### `load()` 내부 TODO를 비워둘 수 있었던 이유

KAIST 템플릿 `process.c:549-550`에는 다음 TODO가 있다:

```c
/* TODO: Your code goes here.
 * TODO: Implement argument passing (see project2/argument_passing.html). */
success = true;
```

직관적으로는 "여기에 argument_stack을 넣어야 하나?" 싶지만, 우리는 그 자리를
**비워두고** 바깥(`process_exec`)에서 헬퍼로 처리했다. 그래도 동작하는 이유:

#### 1. `load()`는 인자 푸시의 사전조건만 만들어놓고 끝낸다

`load()`가 리턴할 시점에 `_if`(intr_frame)는 이미 다음 상태다 (`process.c:543-547`):

```
_if.rip = ehdr.e_entry      ← 진입점 세팅
_if.rsp = USER_STACK         ← setup_stack이 빈 유저 스택을 만들어 둠
세그먼트/eflags               ← process_exec 시작부에서 채움
```

즉 **"엔트리 + 빈 유저 스택"** 이 준비된 상태로 리턴한다. 인자 푸시는 그
스택의 `rsp`만 더 내려서 문자열·포인터를 쓰는 작업이라, `load` 내부일 필요가
없다.

#### 2. `_if`의 소유자는 `process_exec`이지 `load`가 아니다

```c
struct intr_frame _if;          /* process_exec 스택 변수 */
...
success = load (argv[0], &_if); /* load는 _if에 rip/rsp만 써놓고 리턴 */
...
argument_stack (argv, argc, &_if);  /* 같은 _if를 다시 mutate */
do_iret (&_if);
```

`load`는 `_if`를 빌려 쓰는 쪽일 뿐, 리턴 후에도 `process_exec`가 자유롭게
조작할 수 있다. `do_iret` 직전까지 인자만 더 얹으면 동등한 효과.

#### 3. 토큰 파싱이 `load` **앞**에 끝나 있다

```c
for (token = strtok_r(file_name, " ", &save_ptr); ... )
    argv[argc++] = token;
...
success = load (argv[0], &_if);   /* load엔 프로그램명만 */
...
argument_stack (argv, argc, &_if);
palloc_free_page (file_name);
```

`argv[]`가 이미 만들어져 있으니 `load`에는 `argv[0]`만 주면 충분하다. ELF
파일을 여는 데 필요한 건 어차피 프로그램 이름뿐이라 `load`의 시그니처
(`static bool load(const char *file_name, ...)`)를 그대로 둘 수 있다.

#### 만약 TODO 자리에 직접 넣었다면?

- `load` 시그니처를 `load(cmdline, ...)` 또는 `load(argc, argv, ...)`로 변경
  필요 → ELF 로더가 인자 파싱·푸시까지 떠안는 SRP 위반.
- `setup_stack` 직후·`success = true` 직전이라는 순서 제약을 `load` 내부
  흐름에 묶게 됨 → `done:` 라벨 분기와 얽혀 가독성 저하.

#### 한 줄 요약

> `load()`는 "엔트리 + 빈 유저 스택"이라는 **포스트컨디션**을 약속한다.
> 그 약속이 지켜지면 인자 푸시는 `_if`를 보유한 `process_exec`에서
> `do_iret` 직전에 해도 동등하게 동작한다. 그래서 TODO 자리는 비워두고
> `argument_stack` 헬퍼로 외부에서 처리한 게 가능했다.

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

---

## 13. 전체 코드 흐름 도식 — 부팅부터 power_off까지

`pintos -- run "args-single onearg"` 한 줄이 어떻게 흘러가는지 한 사이클 추적.

### 13.1 부팅 + 자식 생성 단계

```
[initial_thread (main)]                          [kernel space]
    │
    ├─ run_actions("args-single onearg")
    │
    ├─ process_create_initd("args-single onearg")
    │     │
    │     ├─ palloc_get_page(0) → fn_copy
    │     ├─ strlcpy(fn_copy, "args-single onearg", PGSIZE)
    │     │
    │     └─ thread_create("args-single onearg", initd, fn_copy)
    │            │                                 ┌─────────────────┐
    │            ├─ palloc_get_page(PAL_ZERO) → t  │ #ifdef USERPROG │
    │            ├─ init_thread(t, name, pri)      │  parent 연결    │
    │            ├─ t->parent = current  ◄─────────┤                 │
    │            ├─ list_push_back(&current        │  자동 children  │
    │            │     ->children, &t->child_elem) │  list 등록      │
    │            └─ thread_unblock(t)              └─────────────────┘
    │                  ↓                          [initd thread t]
    │                  └─────── ready_list로 ──────────┐
    │                                                  ▼
    └─ process_wait(initd_tid)                  (스케줄 대기)
          │
          ├─ children list에서 initd_tid 검색 → 발견
          └─ sema_down(&initd->wait_sema) ⏸ BLOCKED
```

### 13.2 자식 실행 단계 (process_exec)

```
[initd thread, 스케줄됨]                          [kernel space]
    │
    ├─ initd(f_name="args-single onearg")
    │     ├─ process_init()
    │     └─ process_exec(f_name)
    │            │
    │            ├─ process_cleanup()  ← 기존 pml4 회수
    │            │
    │            ├─ strtok_r 파싱
    │            │     argv = ["args-single", "onearg"]
    │            │     argc = 2
    │            │
    │            ├─ strlcpy(thread->name, argv[0])
    │            │     ── 16자 잘림 방지: "args-single"
    │            │
    │            ├─ load("args-single", &_if)
    │            │     ├─ pml4_create + process_activate
    │            │     ├─ ELF 세그먼트 로드
    │            │     ├─ setup_stack: USER_STACK 매핑
    │            │     │     _if.rsp = 0x47480000
    │            │     └─ _if.rip = ehdr.e_entry
    │            │
    │            ├─ argument_stack(argv, argc, &_if)
    │            │     ─ 6단계로 유저 스택 세팅
    │            │     ─ rsp ↓, rdi=2, rsi=&argv[0]
    │            │
    │            ├─ palloc_free_page(file_name)
    │            │     ─ argv[i] 사용 끝난 후
    │            │
    │            └─ do_iret(&_if) ────────┐
    │                                      │
    └────────────────────────────────────  │
                                            ▼
                                    [user mode 진입]
```

### 13.3 유저 모드 → exit() → 동기화 단계

```
[user mode: args-single]
    │
    ├─ _start → main(2, ["args-single","onearg"])
    │     ↑ rdi/rsi가 이대로 main 인자로 전달됨
    │
    ├─ printf 등 작업
    │
    └─ exit(0)
         │ syscall instruction (rax=SYS_EXIT, rdi=0)
         ▼
[kernel: syscall_handler]
    │
    ├─ case SYS_EXIT
    ├─ thread_current()->exit_status = 0
    └─ thread_exit()
         └─ process_exit()
              │
              ├─ printf("args-single: exit(0)\n")
              ├─ process_cleanup()  ← pml4 destroy
              │
              ├─ sema_up(&curr->wait_sema) ──────┐
              │                                   │ initial_thread
              │                                   │ 깨움
              └─ sema_down(&curr->exit_sema) ⏸   │
                  (BLOCKED, struct는 alive)       │
                                                  ▼
[initial_thread (main) 깨어남]
    │
    ├─ exit_status = child->exit_status  (=0)
    ├─ list_remove(&child->child_elem)
    ├─ sema_up(&child->exit_sema) ──────┐
    │                                    │ initd 깨움
    └─ return 0  (process_wait 복귀)     │
         │                               │
         ▼                               ▼
    main() 계속                    [initd 깨어남]
         │                               │
         └─ power_off() ◄── shutdown    └─ return → thread_exit
                                              └─ do_schedule(THREAD_DYING)
                                                   └─ 다음 스케줄 시 페이지 해제
```

### 13.4 핵심 invariant 정리

| 시점 | initial_thread | initd | child struct alive? |
|---|---|---|---|
| `sema_down(wait)` 직전 | RUNNING | READY | YES |
| 자식 `sema_up(wait)` 직후 | READY | RUNNING | YES |
| 자식 `sema_down(exit)` 진입 | RUNNING | BLOCKED | YES (★ 이 구간에서 부모가 안전하게 read) |
| 부모 `sema_up(exit)` 직후 | RUNNING | READY | YES |
| 자식 `do_schedule(DYING)` | RUNNING/READY | DYING | NO (다음 schedule에서 free) |

★ 표시 구간이 핵심. `exit_sema`가 없으면 자식이 즉시 죽을 수 있어 부모가 read한 `exit_status`가 dangling이 될 수 있음.

---

## 14. 어느 포인트로 작업을 잡고 개발했는가

### 14.1 출발점

stage 0 완료 상태:
- `SYS_WRITE` (fd=1) 만 동작
- `process_wait`는 `sema_down`으로 영구 블록 스텁 (자식 출력 보존 목적)
- 7개 테스트 모두 FAIL

### 14.2 작업 순서 (4 사이클로 분해)

**Cycle 1 — argument passing 골격**

| 작업 | 이유 |
|---|---|
| `argument_stack()` 신규 | KAIST 64-bit ABI에 맞춰 rdi=argc, rsi=&argv[0] 직접 세팅 |
| `process_exec` 인자 파싱 | strtok_r로 분해 → load(argv[0]) → argument_stack |
| `thread.h` `exit_status` | exit() 인자를 thread struct에 저장할 곳 필요 |
| `syscall.c` SYS_HALT/SYS_EXIT | 유저 프로그램이 종료할 입구 마련 |

→ 빌드 성공, 수동 실행 OK 같아 보임 (실제론 false positive)

**Cycle 2 — Page Fault 디버깅**

증상: `Page fault at 0x4747effe in kernel context`. 반복 실행 시 항상 같은 주소.

추적:
- `0x4747effe` < `USER_STACK - PGSIZE (0x4747F000)` → 매핑 외 영역
- `args-none`(arg 1개)도 `halt`(arg 1개)도 같은 주소 → 길이 의존성 없음
- `rdx = 0xcc = 204`바이트 memcpy → strlen이 비정상적으로 큰 값 반환

원인: `palloc_free_page`를 `argument_stack` **이전**에 호출 → `argv[i]`가 dangling pointer → `strlen(argv[i])`이 쓰레기 메모리에서 끝없이 0을 못 찾고 큰 길이 반환.

수정: 해제 순서를 `argument_stack` **이후**로.

**Cycle 3 — TIMEOUT 디버깅**

수동 실행에선 OK였지만 `make tests/userprog/<t>.result`에선 `halt`만 PASS, 나머지 6개 TIMEOUT.

추적: output을 읽어보니 `args-none: exit(0)` 까지 정상 출력 → 그 뒤로 시스템 종료 안 됨.

원인: `process_wait`이 `sema_down(stub)`으로 영구 블록 → 부모(initial_thread)가 `power_off()`까지 도달 못 함.

선택지:
- (A) `timer_sleep(100); return -1;` 꼼수 → 작업량 적지만 wait 계열 테스트는 어차피 못 풀고, 1초 타이밍에 의존하는 race 위험.
- (B) 정식 stage 1 wait 구현 → 30~40줄 추가지만 다음 단계 작업 활용 가능.

→ 팀 토론 후 (B) 채택.

**Cycle 4 — 정식 wait/exit 동기화 + thread name 잘림 수정**

| 작업 | 이유 |
|---|---|
| `thread.h` 5필드 추가 | parent/children/wait_sema/exit_sema 자료구조 |
| `thread_create` 부모 연결 | 모든 child가 자동 등록되도록 |
| `init_thread`에서 list/sema init | initial_thread 포함 모두 일관 |
| `process_wait` 검색+sema 패턴 | children 순회 → wait_sema down → exit 회수 → exit_sema up |
| `process_exit` sema 시퀀스 | 부모의 회수까지 thread struct 보존 |
| `process_exec` thread name 갱신 | `args-single onearg`(18자)가 16자 한계로 `args-single one`이 되어 종료 메시지 깨지는 부수 버그 |

→ 7개 모두 PASS.

### 14.3 개발 사이클의 공통 패턴

```
1. 코드 작성
   ↓
2. make 빌드 (warning/error 0 확인)
   ↓
3. 수동 실행: pintos -v -k -T 60 ... -- -q -f run <test>
   (콘솔 출력 + Kernel PANIC 여부 확인)
   ↓
4. 정식 프레임워크: make tests/userprog/<t>.result
   (.result = PASS/FAIL, .output = 실제 출력)
   ↓
5. FAIL이면 .output 분석 → 원인 가설 → 수정 → 2로
```

특히 3과 4의 **간극**이 핵심 교훈:
- 3은 "Kernel PANIC만 안 나면 OK" → false positive 가능
- 4는 "출력이 expected와 일치 + 정상 종료" → 진짜 PASS

Cycle 1→2 에서 3만 보고 통과로 착각, Cycle 2→3 에서 4를 돌리면서 TIMEOUT 발견.
**진짜 검증은 항상 정식 프레임워크로**.

### 14.4 의사결정 트레이드오프

| 상황 | 선택지 | 채택 | 이유 |
|---|---|---|---|
| `rsi` 캡처 시점 | (a) `rsp+8`로 계산 (b) push 전 명시 저장 | (b) | 의도가 코드에 드러남, 오프셋 계산 실수 방지 |
| `palloc_free_page` 위치 | (a) `argument_stack` 전 (b) 후 | (b) | argv[i]가 file_name 내부를 가리켜 dangling 방지 |
| `process_wait` 임시 fix | (a) `timer_sleep` 꼼수 (b) 정식 sema | (b) | (a)는 wait 테스트 못 풀고 race 위험, (b)는 다음 단계 재사용 |
| thread name 갱신 위치 | (a) `process_create_initd` (b) `process_exec` | (b) | exec 후 프로그램 바뀌면 이름도 동반 변경, 미래 SYS_EXEC와 일관 |
| `exit_status` 초기값 | (a) 명시적 -1 (b) memset의 0 | (b) | 정상 종료 default=0 (Unix 관례), 비정상은 추후 kill 경로에서 -1 |

---

## 15. 코드 리뷰 Q&A — wait/exit 동기화 심화

머지 후 팀 리뷰에서 실제로 나온 질문들과 그에 대한 정리.
이 섹션은 "코드만 봐서는 즉시 안 보이는 의도"를 기록하는 것이 목적.

### 15.1 왜 `children`을 `struct list`로 선언했나

```c
struct list children;           /* thread.h:111 */
struct list_elem child_elem;    /* thread.h:112 */
```

**선택의 이유 4가지:**

1. **자식 개수가 가변** — `fork`/`exec`로 자식이 몇 개 생길지 컴파일 타임에 알 수 없음.
   단일 포인터·고정 배열로는 불충분.
2. **`process_wait(tid)`가 특정 자식을 식별해야 함** — 부모는 `child_tid`를 받아 자기
   children만 순회하며 일치하는 thread를 찾는다 (`process.c:309-316`). 이 한 자료구조로
   "내 자식인지 검증"과 "어느 자식인지 식별"을 동시에 해결.
3. **Pintos `struct list`가 intrusive doubly-linked** — 노드(`child_elem`)를 thread 본체에
   박아두므로 별도 malloc 없이 O(1) push/remove. 자식 lifetime과 잘 맞음.
4. **세마포어와 역할 분리** — `wait_sema`/`exit_sema`는 자식 1개당 1쌍의 동기화 채널이고,
   `children` 리스트는 그 채널들을 보유·탐색하는 인덱스 역할.

> **한 줄 요약**: "여러 자식 중 특정 자식을 tid로 식별해 그 자식의 동기화 세마포어를
> 정확히 깨우기 위해" 리스트로 두었다.

---

### 15.2 `wait_sema`/`exit_sema`는 어디서 초기화되나

`pintos/threads/thread.c:474-479`의 `init_thread()` 안:

```c
#ifdef USERPROG
    list_init (&t->children);
    sema_init (&t->wait_sema, 0);
    sema_init (&t->exit_sema, 0);
    t->parent = NULL;
#endif
```

`init_thread()`는 **모든 thread가 만들어지는 순간 무조건 한 번 거치는** 진입점:
- 부팅 시 `thread_init()`에서 main thread
- 이후 `thread_create()`에서 모든 신규 thread

→ 누락 위험 0.

**왜 초기값 0인가** — 두 세마포어 모두 "상대가 신호 줄 때까지 기다리는 랑데부"이기 때문.
1로 했다면 부모 wait이 즉시 통과되어 status 유실, 또는 자식이 즉시 사라져 use-after-free.

---

### 15.3 `process_wait`의 for문에서 `e`는 언제 "채워지나"

질문이 두 층위로 해석됨. 둘 다 정리.

#### (a) 루프 변수 `e` 자체의 값 흐름

```c
for (e = list_begin(&cur->children);   /* 초기화: 첫 노드 주소  */
     e != list_end(&cur->children);    /* 조건: tail sentinel?  */
     e = list_next(e)) { ... }         /* 증가: 다음 노드로     */
```

`e`는 children 리스트의 head 다음 노드부터 tail sentinel 직전까지를 순차 방문하는 커서.
자식이 0명이면 `list_begin == list_end` → 본문 한 번도 실행 안 됨 → `child`는 NULL 그대로
→ `if (child == NULL) return -1;`로 빠짐.

#### (b) 그 children 리스트는 언제 채워지나 (실질적 질문)

**답: `thread_create()`에서.** `pintos/threads/thread.c:203-206`:

```c
#ifdef USERPROG
    t->parent = thread_current();
    list_push_back(&thread_current()->children, &t->child_elem);
#endif
```

시간 순:

```
t=0  부팅. 부모 thread.children = list_init() 상태(비어있음)
t=1  부모가 fork/exec_initd → thread_create() 호출
t=2    palloc_get_page로 자식 thread 구조체 할당
t=3    init_thread(자식): 자식의 children/sema/parent 0/빈 상태로 초기화
t=4    list_push_back(부모->children, &자식->child_elem)   ★ 여기서 채워짐
t=5    thread_unblock(자식)
...
t=N  부모가 process_wait(child_tid) 호출
       list_begin(&cur->children)이 t=4에서 push된 노드를 반환 → e가 받음
```

> 즉 **노드는 자식 생성 시점(`thread_create`)에 채워지고, `e`는 wait 호출 시점에 그 노드를
> 순회한다**. 두 시점이 분리되어 있는 게 핵심.

---

### 15.4 `exit_sema`의 마지막 `sema_up`은 누가 어디서?

`process_exit`에서 `sema_down(&curr->exit_sema)`로 자식이 블록되는데
(`process.c:350`), 그걸 풀어주는 마지막(=유일한) `sema_up`은:

→ **부모의 `process_wait()` 안 `process.c:323`** 에서 호출됨.

```c
sema_down (&child->wait_sema);            /* line 320: 자식 종료 대기   */
exit_status = child->exit_status;         /* line 321: status 회수      */
list_remove (&child->child_elem);         /* line 322: 중복 wait 차단   */
sema_up (&child->exit_sema);              /* line 323: ★ 마지막 sema_up */
return exit_status;                       /* line 325                   */
```

**이 순서가 중요한 이유 3가지:**

1. **status 안전 회수** — 자식 thread struct가 살아 있는 동안에만 `exit_status` 필드를 읽을
   수 있음. 자식이 먼저 free되면 use-after-free.
2. **중복 wait 차단** — `list_remove`까지 끝낸 다음 풀어주므로, 같은 tid로 다시 wait 와도
   children에서 못 찾고 -1.
3. **자식의 진짜 소멸 트리거** — 자식의 `sema_down(exit_sema)` 통과 → `process_exit` 리턴
   → `thread_exit()` → `do_schedule(THREAD_DYING)` → 페이지 회수.

#### 시나리오별 sema_up 타이밍

| 상황 | wait_sema sema_up | exit_sema sema_up |
|---|---|---|
| **A. 자식이 먼저 끝남, 부모 늦게 wait** | 자식 `process_exit`(`process.c:347`)에서 (부모 도착 전 이미 up) | 부모 wait의 `process.c:323` |
| **B. 부모 먼저 wait, 자식 늦게 종료** | 자식이 종료하면서 up (부모는 그동안 sema_down에서 블록) | 부모 wait의 `process.c:323` |
| **C. 부모가 wait을 안 부름** | 자식 `process_exit`에서 up (낭비) | **아무도 안 올려줌 → 자식 영구 블록** |

C 케이스는 §10 "한계"에서 언급한 좀비 누수와 동일. 해결책은 `process_exit` 끝에서
자기 children을 순회하며 `sema_up(child->exit_sema)`를 해주는 정리 로직 추가.

---

### 15.5 핵심 invariant 재확인 (리뷰용 한 장 요약)

```
[자식 생성]   thread_create() → list_push_back(부모.children, 자식.child_elem)
[자식 초기화] init_thread()  → wait_sema=0, exit_sema=0
                                ↓
[부모 wait]   process_wait(tid)
   ├─ children 순회 → 못 찾으면 return -1
   ├─ sema_down(child.wait_sema)        ─────┐
   │                                          │
[자식 exit]   process_exit()                  │
   ├─ printf("name: exit(N)")                 │
   ├─ process_cleanup()                       │
   ├─ sema_up(self.wait_sema)        ────────┘ (부모 깨움)
   ├─ sema_down(self.exit_sema)   ⏸ ─────┐
   │                                       │
   ├─ exit_status 회수 (struct alive)      │
   ├─ list_remove(child.child_elem)        │
   ├─ sema_up(child.exit_sema)   ─────────┘ (자식 깨움) ★ 마지막 up
   └─ return exit_status
                                  자식: thread_exit → do_schedule(DYING)
```

---

## 16. `SYS_CREATE` 구현과 유저 포인터 검증 3단 체크

`syscall.c:70-90`에서 SYS_CREATE를 추가하면서 유저가 넘긴 `filename` 포인터를
**호출 전에 반드시 검증**해야 했다. 검증 없이 `filesys_create()`를 부르면
NULL/커널/언맵 주소를 그대로 따라가다가 커널 패닉이 난다.

```c
case SYS_CREATE: {
    const char  *filename = (const void *) f->R.rdi;
    unsigned       size   = (unsigned) f->R.rsi;

    validate_user_addr(filename);          /* ① 검증이 먼저 */

    lock_acquire(&filesys_lock);           /* ② 파일시스템 전역 락 */
    bool success = filesys_create(filename, size);
    lock_release(&filesys_lock);

    f->R.rax = success;                    /* ③ 부울 그대로 반환 */
    break;
}
```

### 16.1 `validate_user_addr` — 3단 체크의 의미 (`syscall.c:52-59`)

```c
static void
validate_user_addr (const void *uaddr) {
    if (uaddr == NULL || !is_user_vaddr(uaddr)
        || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
        thread_current()->exit_status = -1;
        thread_exit();
    }
}
```

세 조건은 **순서가 의미를 가진다**. 위에서 아래로 갈수록 비용이 커지고,
앞 조건이 통과해야 뒤 조건을 안전하게 평가할 수 있다.

#### 첫 번째: `uaddr == NULL`

포인터가 NULL인지 확인한다. NULL 포인터를 역참조하면 즉시 크래시가 나기 때문에
가장 먼저 확인한다. `is_user_vaddr(NULL)`은 `0 < KERN_BASE`이므로 true가 되어
이 가드가 없으면 다음 단계로 그냥 흘러가 버린다.

#### 두 번째: `!is_user_vaddr(uaddr)`

이 주소가 유저 영역에 있는지 확인한다. `is_user_vaddr`은 사실상
`(uint64_t)vaddr < KERN_BASE`(=`0x8004000000`)이고, 유저 프로그램이
커널 주소를 인자로 넘기면 여기서 잡힌다.

| 예시 주소        | KERN_BASE 비교        | 판정       |
|------------------|----------------------|-----------|
| `0x20101234`     | KERN_BASE보다 낮음   | 유저 영역 → 통과 |
| `0x9000000000`   | KERN_BASE보다 높음   | 커널 영역 → 차단 |

#### 세 번째: `pml4_get_page(thread_current()->pml4, uaddr) == NULL`

유저 영역 안에 있지만 **실제 매핑이 없는 주소**를 잡는다. 예컨대
`0x20101234`는 유저 영역(KERN_BASE 미만)이지만 페이지 테이블에 등록이
안 되어 있으면 `pml4_get_page`가 NULL을 반환한다. 이 단계가 없으면
유저가 아무 임의의 유저 주소나 넘겨도 1차·2차 검증은 통과시켜 버린다.

> 즉 ①은 **NULL 차단**, ②는 **커널 영역 차단**, ③은 **언맵 페이지 차단**.
> 셋이 합쳐져야 "유저가 합법적으로 매핑한 주소"라는 보증이 된다.

### 16.2 검증 실패 시 동작

세 조건 중 하나라도 걸리면:

1. `thread_current()->exit_status = -1`  ← bad-ptr 테스트가 기대하는 코드
2. `thread_exit()` 호출 → `process_exit()` → "name: exit(-1)" 출력 + cleanup
3. `process_wait`을 부른 부모는 자식의 -1을 회수해서 그대로 반환

`filesys_create`를 호출하기 **전에** 끝내므로 락도 잡지 않는다 = 데드락
가능성 0.

### 16.3 왜 `filesys_lock`이 필요한가

Pintos 기본 파일시스템(`pintos/filesys/`)은 thread-safe하지 않다. 여러
프로세스가 동시에 `filesys_create / filesys_open / filesys_remove`를
호출하면 free-map과 inode 캐시가 깨진다. 그래서 `syscall_init`에서
`lock_init(&filesys_lock)`으로 한 번 만들어 두고, 파일시스템 진입
직전·직후로만 짧게 잡는다.

`validate_user_addr`이 락 **밖**에서 호출되는 점이 중요하다. 검증에서
`thread_exit`이 일어나도 잡힌 락이 없으니 정리 부담이 없다.

### 16.4 stage 0 검증의 한계 (다음 단계로 미룬 것들)

현재 `validate_user_addr`은 **포인터 한 개**만 본다. 다음 케이스들은 아직
못 잡는다:

- **문자열의 끝까지 매핑 보장**: `filename`이 페이지 경계를 걸쳐 있을 때
  뒤쪽 페이지가 언맵일 수 있다. 바이트 단위로 NUL 만날 때까지 페이지마다
  재검증이 필요.
- **버퍼 범위 검증**: SYS_WRITE/READ가 받는 `(buffer, size)`에서 buffer ~
  buffer+size-1 전 구간이 매핑돼야 한다.
- **쓰기 가능 여부**: read 류 syscall은 유저 버퍼가 RW여야 함
  (`pml4e_walk`로 PTE_W 검사).
- **fault 기반 lazy 검증**: KAIST 권장 방식은 page fault 핸들러에서
  유저 fault를 -1로 변환하는 것. stage 1 이후 도입.

### 16.5 한 줄 요약

> SYS_CREATE의 본체는 `filesys_create` 한 줄이지만, 그 한 줄을 안전하게
> 부르려면 **NULL → 커널영역 → 언맵 페이지** 3단 가드 + **filesys 전역
> 락**이 같이 와야 한다. 검증은 락 밖에서, filesys 호출은 락 안에서.
