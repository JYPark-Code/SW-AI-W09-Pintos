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

// 3. 파싱 완료 후 palloc 해제
palloc_free_page (file_name);

// 4. 스택 세팅 후 do_iret
argument_stack (argv, argc, &_if);
do_iret (&_if);
```

### palloc_free_page 타이밍이 중요한 이유

`strtok_r`은 `file_name` 버퍼를 in-place로 수정한다(공백을 `\0`으로 치환).
`argv[i]` 포인터들은 이 버퍼 내부의 각 토큰을 가리킨다.
`argument_stack()`이 `memcpy`로 스택에 복사하기 전에 페이지를 해제하면
dangling pointer가 되어 undefined behavior가 발생한다.

따라서 순서는 반드시 `strtok_r 파싱 → load → palloc_free_page → argument_stack`이어야 한다.

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

## 8. 통과 예상 테스트

```
pass  args-none
pass  args-single
pass  args-multiple
pass  args-many
pass  args-dbl-space
pass  halt
pass  exit
```
