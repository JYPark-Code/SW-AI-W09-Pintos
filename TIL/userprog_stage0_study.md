# Pintos Project 2 — Userprog Stage 0 학습 정리

## 1. 목적 — 디버깅 환경 세팅을 위한 최소 구현

Project 2 본격 진입 전 **유저 프로그램의 출력을 콘솔에서 볼 수 있도록** 만드는 단계.
syscall, wait, exit, fork 같은 큰 주제를 한꺼번에 건드리기 전에, 다음 두 가지만 먼저 살리면 이후 디버깅이 훨씬 수월해진다.

1. **`write(fd=1)` 시스템 콜** — 유저 모드 `printf()`의 최종 종착지
2. **`process_wait()` 임시 스텁** — 부모(init)가 자식 종료 후 즉시 power_off하지 않도록 영구 블록

이게 빠지면 유저 프로그램이 아무리 출력해도 콘솔에 도달 전에 머신이 꺼져버려 "왜 안 되지?"가 곧 "보이지를 않는다"가 되어 디버깅 자체가 불가능하다.

---

## 2. 수정한 것들

### `pintos/userprog/syscall.c` — `syscall_handler()` + `validate_user_addr()`

KAIST 64bit Pintos는 x86-64 `syscall` 명령어를 쓰므로 ABI가 32bit 시절과 다르다:

| 레지스터 | 의미 |
|---|---|
| `rax` | syscall 번호 / 반환값 |
| `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` | 인자 1~6 |

`intr_frame->R` 의 동명 필드에 그대로 매핑된다.

```c
void
syscall_handler (struct intr_frame *f UNUSED) {
    uint64_t sysno = f->R.rax;

    switch (sysno) {
        case SYS_WRITE: {
            int            fd     = (int) f->R.rdi;
            const void    *buffer = (const void *) f->R.rsi;
            unsigned       size   = (unsigned) f->R.rdx;

            validate_user_addr(buffer);   /* KERN_BASE 체크 (최소) */

            if (fd == 1) {
                putbuf(buffer, size);
                f->R.rax = size;
            } else {
                f->R.rax = (uint64_t) -1;  /* fd≠1은 stage 0 범위 밖 */
            }
            break;
        }
        default:
            printf("[stage0] unhandled syscall: %llu\n",
                   (unsigned long long) sysno);
            thread_exit();
    }
}
```

**왜 putbuf?** 내부 console lock으로 한 호출 분량을 보호하므로 다른 출력과 섞이지 않는다.
**왜 검증은 `is_user_vaddr` 한 줄?** 정식 검증(언맵 페이지/페이지 경계 걸침)은 syscall 인자 전체 검증 단계에서 도입할 예정. stage 0은 "커널 영역 포인터를 유저가 넘겼을 때만" 거절.

### `pintos/userprog/process.c` — `process_wait()` 스텁

기존 코드는 `return -1` 즉시 반환이라, init이 `process_wait(initd)`에서 곧바로 복귀 → main이 `power_off()`로 머신을 꺼버린다. 자식의 stdout이 콘솔에 도달하기 전에 꺼지므로 출력이 통째로 사라진다.

```c
int
process_wait (tid_t child_tid UNUSED) {
    struct semaphore stub;
    sema_init(&stub, 0);
    sema_down(&stub);   /* 영구 블록 */
    return -1;
}
```

**왜 세마포어로?** 단순 `for(;;)` 무한루프는 CPU를 점유한다. 값 0 sema의 sema_down은 thread를 BLOCKED 상태로 만들어 스케줄러에서 빠진다.
**한계:** 진짜로 child_tid를 추적하지는 않는다. 자식 list / exit_status 회수 / 좀비 정리는 정식 wait 구현 단계에서 추가.

추가로 `threads/synch.h` include 빠져있어서 컴파일 에러 → 한 줄 추가.

---

## 3. 검증용 테스트 — `tests/userprog/stage0/`

기존 공식 테스트 (`args-none`, `halt` 등)는 stage 0에서 모두 timeout으로 죽는다. process_wait이 영구 블록이라 kernel이 power_off에 도달하지 못하기 때문에, 공식 .ck의 `common_checks`가 "Powering off" 부재로 자동 fail시킨다.

→ stage 0 전용 테스트 디렉토리를 별도로 만들어 자체 .ck로 판정.

### 디렉토리 구조

```
pintos/tests/userprog/stage0/
├── Make.tests           ← 빌드 등록
├── write-fd1.c          ← write(fd=1) 검증 유저 프로그램
├── write-fd1.ck         ← Perl 검사기
├── wait-blocks.c        ← process_wait 영구 블록 검증
└── wait-blocks.ck
```

`pintos/userprog/Make.vars`의 `TEST_SUBDIRS`에 `tests/userprog/stage0`를 추가하여 빌드 파이프라인에 흡수.

### 테스트 1: `write-fd1`

**유저 프로그램:**
```c
#include <syscall.h>

int
main (void) {
    static const char marker[] = "STAGE0_WRITE_OK\n";
    write (1, marker, sizeof marker - 1);
    return 0;
}
```

`tests/main.c`를 일부러 사용하지 않았다. 그 안의 `main`이 `argv[0]`을 `test_name`으로 쓰는데 stage 0에서는 argument passing 미구현이라 argv가 garbage이기 때문. `_start → main → exit(ret)` 흐름에서 argv를 건드리지 않으면 안전.

**검사기 (`.ck`):**
```perl
fail "STAGE0_WRITE_OK marker missing — write(fd=1) didn't reach console\n"
  if !grep (/STAGE0_WRITE_OK/, @output);
pass;
```

`common_checks` 호출 안 함. 마커 등장 여부만 본다.

**통과 기준:** 출력에 `STAGE0_WRITE_OK` 문자열 등장.

### 테스트 2: `wait-blocks`

**유저 프로그램:** 본체는 write-fd1과 거의 동일 (`STAGE0_WAIT_CHILD_DONE` 마커만 다름). 검사 관점이 다르다.

**검사기 (`.ck`):**
```perl
fail "child marker missing — write failed or kernel terminated too early\n"
  if !grep (/STAGE0_WAIT_CHILD_DONE/, @output);

fail "kernel powered off — process_wait did NOT block (stub broken)\n"
  if grep (/Powering off/, @output);

pass;
```

**두 조건 동시 충족 시 통과:**
1. 자식 마커 등장 (자식이 정상 실행됨)
2. `Powering off` **부재** (kernel이 power_off로 가지 못함 = 부모가 process_wait에서 블록)

### TIMEOUT 단축

```makefile
tests/userprog/stage0/write-fd1.output:   TIMEOUT = 10
tests/userprog/stage0/wait-blocks.output: TIMEOUT = 10
```

기본 60초인데 stage 0은 어차피 timeout으로만 끝나므로 10초로 단축. 빠른 피드백.

---

## 4. 실행 결과

```bash
cd pintos/userprog
make build/tests/userprog/stage0/write-fd1.result
make build/tests/userprog/stage0/wait-blocks.result
```

| 테스트 | 결과 | 핵심 출력 |
|---|---|---|
| `write-fd1`   | **PASS** | `STAGE0_WRITE_OK` + `[stage0] unhandled syscall: 1` + `TIMEOUT` |
| `wait-blocks` | **PASS** | `STAGE0_WAIT_CHILD_DONE` + `[stage0] unhandled syscall: 1` + `TIMEOUT` |

출력 끝에 `Powering off`가 **없고** `TIMEOUT`만 있다는 점이 process_wait stub이 의도대로 동작하고 있다는 결정적 증거다.

`[stage0] unhandled syscall: 1`은 `_start`가 `main` return 후 호출하는 `exit(0)` (= SYS_EXIT, 번호 1) 가 default 분기로 가서 찍은 디버그 라인. 이번 단계에서 의도된 동작.

---

## 5. 회고 — Stage 0의 가치

| 안 했을 때 | 했을 때 |
|---|---|
| `printf()`가 콘솔에 안 보임 → 모든 디버깅이 GDB 의존 | 유저 프로그램이 자기 상태를 콘솔로 보고할 수 있음 |
| `process_wait` 즉시 반환 → 자식 출력이 잘림 | 자식 출력이 끝까지 보임 |
| 다음 단계 구현 중 버그 원인이 "내 코드인지 출력 사라진 것인지" 구분 불가 | "보이지 않으면 코드가 안 돈 것"이 명확 |

**핵심 교훈:** OS 구현은 점진적이라 *디버깅 가시성을 가장 먼저 확보*하는 게 중요하다. 그래서 halt/exit/argument-passing 등의 실제 syscall보다 `write(fd=1)` 한 분기와 `process_wait` 영구 블록 두 가지를 먼저 묶어 하나의 "stage 0"으로 처리.

---

## 6. 다음 단계로 넘어가기 전 정리

stage 0의 코드는 **모두 임시 스캐폴딩**이다. 정식 구현 단계에서:

| 항목 | stage 0 | 정식 |
|---|---|---|
| `process_wait` | 무조건 sema 영구 블록 | 자식 list 추적 + child별 wait_sema + exit_status 회수 |
| `validate_user_addr` | NULL + KERN_BASE만 | 언맵 페이지 + 페이지 경계 걸침 검증 |
| `syscall_handler` default | `[stage0]` 라벨 + thread_exit | 적절한 종료 코드(-1)로 exit |
| `tests/userprog/stage0/` | 자체 .ck로 timeout 정상 처리 | 디렉토리 통째로 삭제 + Make.vars 한 줄 원복 |

`Make.vars` 한 줄 + 디렉토리 삭제만으로 깔끔하게 폐기 가능하도록 설계한 이유가 여기에 있다.
