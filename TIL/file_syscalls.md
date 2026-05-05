# 파일 시스템 콜 회고 — CREATE / OPEN / CLOSE / READ / WRITE / FILESIZE

> KAIST 64bit Pintos Project 2 — userprog 단계의 **파일 시스템 콜** 구현
> 진행 기록. 콜이 추가될 때마다 이어서 정리한다.
>
> - **1차** (§0~§4): CREATE / OPEN / CLOSE — 인프라 설계 + 첫 3콜
> - **2차** (§5): READ / FILESIZE — fd_table 활용 + 분기 처리의 함정
> - **3차** (§6): WRITE 확장 — stage 0 stdout-only → fd_table 경로 추가, READ와 대칭
> - **다음 예정**: REMOVE / SEEK / TELL — fd_table 두 단계 가드 패턴 그대로
>
> 콜의 본체는 `filesys_*` / `file_*` 한 줄이지만, 그 한 줄을 안전하게 부르려면
> **유저 포인터 검증 / 전역 락 / per-thread fd 테이블** 세 인프라가 먼저
> 깔려 있어야 한다는 것이 시리즈를 관통하는 교훈.

---

## 0. 출발점에서의 문제 인식

Stage 0까지의 syscall 핸들러는 HALT/EXIT/WRITE만 라우팅됐고, `validate_user_addr`도
NULL과 `is_user_vaddr` 두 단계 체크만 있었다. 이 상태에서 파일 콜을 추가하면
다음이 곧바로 깨진다:

1. **bad-ptr 테스트** — 유저가 `0x20101234` 같은 "유저 영역 안이지만 매핑 안 된"
   주소를 인자로 넣으면 `is_user_vaddr`은 통과해 버린다 → `filesys_create`가
   그대로 따라가다 커널 패닉.
2. **동시성** — Pintos 기본 파일시스템은 thread-safe가 아니다. 여러 스레드가
   동시에 `filesys_open`/`filesys_create`를 부르면 free-map과 inode 캐시가
   망가진다.
3. **fd 관리** — `OPEN`이 반환할 정수 fd를 어디다 저장하고, `CLOSE`가 그걸로
   어떻게 `struct file *`을 역추적할지 결정해야 한다.

즉 콜 셋 자체보다 **그 콜들이 의지할 인프라 3개**부터 짜야 했다.

---

## 1. 해결 방향 — 공통 인프라 먼저, 콜은 그 위에

### 1.1 per-thread fd 테이블 (`thread.h`, `thread.c`)

```c
/* threads/thread.h */
struct thread {
    ...
    /* fd 테이블 */
    struct file *fd_table[128];
    int fd_next;   /* 다음 할당할 fd 번호, 2로 초기화 */
    ...
};

/* threads/thread.c — init_thread() */
t->fd_next = 2;   /* 0=stdin, 1=stdout 예약 */
```

- **per-process가 아니라 per-thread**에 둔 이유: Pintos는 process 1개 = thread
  1개라는 단순한 구조라 thread struct에 그대로 박는 게 자연스럽다. 추후 fork가
  생기면 `__do_fork`에서 부모 fd_table을 자식에 복제하는 식으로 확장.
- **고정 배열(`fd_table[128]`)**: 동적 할당하면 `thread_create`/`process_exit`
  마다 alloc/free가 생긴다. 일단은 단순함을 택하고, 한계가 드러나면 나중에
  확장.
- **`fd_next`만 단조 증가**: `CLOSE`로 비워진 슬롯을 재사용하지 않는다.
  fd 누수가 발생하지만 이번 단계 테스트에선 충분. 추후 빈 슬롯 스캔 또는
  free-list로 개선.

### 1.2 전역 파일시스템 락 (`syscall.c`)

```c
struct lock filesys_lock;

void
syscall_init (void) {
    lock_init(&filesys_lock);
    ...
}
```

- **굵은 락(coarse-grained)** 으로 시작. 모든 `filesys_*` 콜을 같은 락 하나로
  직렬화. 성능은 안 좋지만 정확성은 보장된다.
- **락 잡는 범위는 최소화**: `validate_user_addr` 같은 메모리 검증은 **락 밖**
  에서. 검증 실패가 곧 `thread_exit`인데, 락을 들고 죽으면 데드락.

### 1.3 유저 포인터 3단 검증 (`syscall.c`)

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

세 조건은 **순서가 의미**를 가진다.

| 단계 | 체크 | 차단 케이스 |
|---|---|---|
| ① | `uaddr == NULL` | NULL 역참조 즉시 차단 |
| ② | `!is_user_vaddr(uaddr)` | 유저가 커널 주소(KERN_BASE=`0x8004000000` 이상) 넘긴 경우 |
| ③ | `pml4_get_page(...) == NULL` | 유저 영역 안이지만 페이지 매핑이 없는 주소 |

**KAIST 64bit 특수성**: KERN_BASE가 32bit Pintos의 `0xc0000000`이 아니라
`0x8004000000`. `is_user_vaddr` 체크의 의미는 같지만, bad-ptr 테스트가 던지는
값(`0x20101234` 같은)이 64bit 유저 영역에 합법적으로 들어와 버린다 → ②까지
통과. 그래서 ③이 없으면 못 잡는다.

실패 시 `exit_status = -1` 후 `thread_exit` → `process_exit`이 "name: exit(-1)"
을 출력하고 부모 wait가 -1을 회수. 테스트 채점이 이 -1을 본다.

---

## 2. 콜 구현 — 3개 모두 같은 골격

세 콜의 공통 구조:

```
1. 인자 추출 (intr_frame.R.rdi 등)
2. 필요한 경우 유저 포인터 검증 (락 밖)
3. lock_acquire(&filesys_lock)
4. filesys_*  /  file_close 호출
5. 결과를 f->R.rax에 세팅 (반환값)
6. lock_release(&filesys_lock)
```

### 2.1 `SYS_CREATE`

```c
case SYS_CREATE: {
    const char *filename = (const void *) f->R.rdi;
    unsigned    size     = (unsigned)    f->R.rsi;

    validate_user_addr(filename);          /* 락 밖에서 검증 */

    lock_acquire(&filesys_lock);
    bool success = filesys_create(filename, size);
    lock_release(&filesys_lock);

    f->R.rax = success;
    break;
}
```

- 인자 단 두 개(filename, size). 검증 대상은 filename 포인터뿐.
- `filesys_create`가 부울을 그대로 돌려주므로 `rax`에 그대로 대입.
  (`bool` → `uint64_t` 암시 변환 — 0/1만 들어가니 안전.)

### 2.2 `SYS_OPEN`

```c
case SYS_OPEN: {
    const char *filename = (const void *) f->R.rdi;

    validate_user_addr(filename);
    lock_acquire(&filesys_lock);

    struct file *file = filesys_open(filename);

    if (file == NULL) {
        f->R.rax = -1;
        lock_release(&filesys_lock);
        break;
    } else {
        thread_current()->fd_table[thread_current()->fd_next] = file;
        f->R.rax = thread_current()->fd_next;
        thread_current()->fd_next++;
    }
    lock_release(&filesys_lock);
    break;
}
```

- `filesys_open`이 NULL이면 `-1`(에러) 반환. 정수 fd 규약에 맞춰 부호 있는
  값을 `rax`에 넣어도 64bit 레지스터에선 문제없다.
- 성공 시 슬롯에 `struct file *`을 저장하고, **저장한 fd 번호를 반환한
  뒤**에 `fd_next`를 증가. (먼저 증가 후 반환하면 첫 fd가 3이 되어버림.)

### 2.3 `SYS_CLOSE`

```c
case SYS_CLOSE: {
    uint64_t fd = (uint64_t) f->R.rdi;

    lock_acquire(&filesys_lock);

    if (fd < 2 || fd >= 128) {
        lock_release(&filesys_lock);
        break;
    } else {
        struct file *file = thread_current()->fd_table[fd];
        if (file == NULL) {
            lock_release(&filesys_lock);
            break;
        }
        file_close(file);
        thread_current()->fd_table[fd] = NULL;
    }
    lock_release(&filesys_lock);
    break;
}
```

- **포인터 검증이 필요 없다** — 인자가 정수 fd라서. 대신 **fd 범위 체크**가
  필요: `< 2`는 stdin/stdout 예약 슬롯이라 닫을 수 없고, `>= 128`은 배열
  out-of-bounds.
- **이중 close 방지**: 슬롯 NULL 체크. 같은 fd로 두 번 close가 와도 두
  번째는 그냥 빠진다.
- `file_close`가 inode 참조를 줄이고 free까지 해주므로, 여기선 슬롯에 NULL을
  쓰는 것 외에 추가 정리 없음.

---

## 3. 겪은 함정들

### 함정 1 — `is_user_vaddr`만으로는 bad-ptr를 못 잡는다

```c
/* 처음 작성한 형태 (불충분) */
if (uaddr == NULL || !is_user_vaddr(uaddr))
    thread_exit();
```

`is_user_vaddr(uaddr)`는 사실상 `(uint64_t)uaddr < KERN_BASE`이다.
bad-ptr 테스트가 던지는 `0x20101234`는 KERN_BASE보다 작으므로 **유저 영역
안**에 있다. 통과해 버림.

**해결**: `pml4_get_page`로 실제 페이지 테이블에 매핑이 있는지까지 확인.

```c
if (uaddr == NULL || !is_user_vaddr(uaddr)
    || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
    thread_current()->exit_status = -1;
    thread_exit();
}
```

> 추가로 깨달은 것: 단순히 `thread_exit()`만 부르면 종료 메시지의 status가
> 0으로 찍힌다. **`exit_status = -1`을 먼저 세팅**해야 채점이 통과한다.

### 함정 2 — `filesys_*` 함수의 implicit declaration 경고

`filesys_create` / `filesys_open` / `file_close`를 부르자마자 컴파일러가:

```
warning: implicit declaration of function 'filesys_open'
warning: implicit declaration of function 'file_close'
```

KAIST 템플릿 syscall.c에는 `filesys/*.h` include가 없다(애초에 stub만 있던
파일이라). 누락이 발생하면 64bit 환경에서 반환 포인터가 `int`로 추론되어
**상위 32비트가 잘려서** 잘못된 포인터가 된다 → 실행 시 무작위 패닉.

**해결**:

```c
#include "filesys/filesys.h"   /* filesys_create, filesys_open */
#include "filesys/file.h"      /* file_close, struct file */
```

> 32bit 시절엔 implicit-int가 그대로 동작해서 운 좋게 통과하는 일이 많았는데,
> 64bit에선 포인터 절단으로 즉시 깨진다. KAIST 64bit 환경의 함정.

### 함정 3 — fd를 `int`로 받으면 캐스팅이 어색해진다

처음엔:

```c
int fd = (int) f->R.rdi;
if (fd < 2 || fd >= 128) ...
```

이렇게 했더니 음수 fd가 들어왔을 때 의도대로 차단되긴 하는데, 이후 `fd_table[fd]`
로 인덱싱할 때 컴파일러 경고("array subscript has type 'int'")가 났고, 부호 비교가
신경 쓰였다.

**해결**: `uint64_t`로 받아서 부호 없이 한 번에 처리.

```c
uint64_t fd = (uint64_t) f->R.rdi;
if (fd < 2 || fd >= 128) { ... }   /* 음수가 들어오면 거대한 unsigned 값 → >=128에서 차단 */
```

x86-64 syscall ABI는 인자가 64bit 레지스터로 들어오므로, 받는 쪽도 그 폭에
맞추는 게 일관적이다.

### 함정 4 — `break`로 빠지면 락이 그대로 잡힌 채 남는다

OPEN의 NULL 케이스를 처음 짤 때:

```c
if (file == NULL) {
    f->R.rax = -1;
    break;        /* ← 락 안 풀고 break! */
}
lock_release(&filesys_lock);
```

이러면 다음 syscall에서 같은 락을 잡으려다 영원히 블록 → 데드락.

**해결**: 락 잡힌 상태에서 break 하기 전엔 **반드시** `lock_release`.
모든 분기에서 검토:

```c
if (file == NULL) {
    f->R.rax = -1;
    lock_release(&filesys_lock);   /* ← */
    break;
}
```

CLOSE의 fd 범위 체크 / NULL 슬롯 분기에도 같은 패턴이 반복된다.

> RAII가 없는 C에선 이 누락이 정말 자주 난다. **"early break/return 직전에
> 락 정리"**가 무조건 체크리스트.

---

## 4. 핵심 통찰

### (a) 시스템 콜의 본체는 한 줄, 그 한 줄을 안전히 부르는 게 본질

`SYS_CREATE`만 봐도 진짜 일하는 줄은 `filesys_create(filename, size)` 하나.
나머지는 전부 **그 한 줄을 호출 가능한 상태로 만드는 일**:

- 유저 포인터가 정말 유저 메모리를 가리키는가? (3단 검증)
- 다른 스레드가 같은 파일시스템을 동시에 만지지 않는가? (락)
- 락을 잡았다면 어떤 경로에서도 풀고 나가는가? (분기마다 release)

OS 코드의 70%는 이 "안전망"이라는 게 와닿았다.

### (b) 인프라 → 콜 순서가 정답

콜을 먼저 짜고 인프라를 끼워 맞추면 매번 콜 본체를 수정해야 한다. 반대로
**fd 테이블 / 락 / 검증 함수 3개**를 먼저 짜두면, 새 콜을 추가할 때 그
인프라를 호출하는 보일러플레이트만 작성하면 된다. CREATE/OPEN/CLOSE를
거의 같은 골격으로 짤 수 있었던 이유.

### (c) KAIST 64bit는 "32bit 직관"을 깨뜨린다

- KERN_BASE = `0x8004000000` → bad-ptr 테스트가 32bit 시절 가드를 우회한다.
- implicit declaration → 64bit에선 포인터 상위 비트가 잘려서 즉사.
- syscall ABI가 `rdi/rsi/rdx/r10/r8/r9` 6개 → 인자는 늘 64bit 폭으로 들어온다.

문서를 읽을 때 "32bit Pintos 자료를 그대로 가져다 쓰면 안 된다"는 점을
계속 의식해야 한다.

### (d) 락은 정확성, 분리는 성능 — 지금은 정확성

전역 `filesys_lock` 하나로 모든 파일 콜을 직렬화하는 건 **느리다**. 두 프로세스가
서로 다른 파일을 열어도 직렬화돼 버린다. 그래도 일단 이 굵은 락으로 시작한
이유:

1. Pintos 기본 파일시스템 자체가 internal locking이 없어서, 미세하게 쪼개려면
   파일시스템 코드를 같이 손봐야 함 → 다음 단계 작업량.
2. 지금 단계 테스트는 동시성보다 **정확성**(open이 정확한 fd를 돌려주는지,
   close가 슬롯을 비우는지)을 본다.

성능 최적화는 기능이 다 들어간 다음.

---

## 5. 후속 — SYS_READ / SYS_FILESIZE

CREATE/OPEN/CLOSE를 마무리하고 곧바로 READ로 넘어갔다. 인프라(fd_table /
filesys_lock / validate_user_addr 3단 검증)는 그대로 재사용. 콜 본체보다는
**fd 분기 처리의 함정**이 이번 단계의 핵심 학습 포인트였다.

### 5.1 SYS_READ — 4개 분기를 어떻게 가를 것인가

인자: `fd(rdi)`, `buffer(rsi)`, `size(rdx)`. 케이스가 4종류다.

| fd 값 | 동작 | 반환 |
|---|---|---|
| 음수 / 128 이상 | 잘못된 fd | -1 |
| 0 (stdin) | `input_getc()`를 size번 반복해 buffer에 채움 | size |
| 1 (stdout) | 읽기 불가 | -1 |
| 2 이상 | `fd_table[fd]`에서 file 꺼내 `file_read` | 실제 읽은 바이트 수 |

```c
case SYS_READ: {
    int      fd     = (int)      f->R.rdi;
    void    *buffer = (void *)   f->R.rsi;
    unsigned size   = (unsigned) f->R.rdx;

    validate_user_addr(buffer);
    lock_acquire(&filesys_lock);

    if (fd < 0 || fd >= 128) {              /* ← 범위 체크가 가장 먼저 */
        f->R.rax = -1;
    } else if (fd == 0) {                   /* stdin */
        for (unsigned i = 0; i < size; i++)
            ((char *) buffer)[i] = input_getc();
        f->R.rax = size;
    } else if (fd == 1) {                   /* stdout — 읽기 금지 */
        f->R.rax = -1;
    } else {
        struct file *file = thread_current()->fd_table[fd];
        f->R.rax = (file == NULL) ? -1 : file_read(file, buffer, size);
    }
    lock_release(&filesys_lock);
    break;
}
```

> 골격은 CLOSE와 동일하다 — **범위 체크 → 슬롯 NULL 가드 → 본 작업 → 반환값
> 세팅** 순. 다만 stdin/stdout 처리가 끼어든다는 점이 다르다.

### 5.2 SYS_FILESIZE — read-normal을 통과시키기 위한 선행 구현

처음 `read-normal`을 돌렸을 때 다음과 같이 실패했다:

```
[stage0] unhandled syscall: 8
```

**문제 추적 경로:**

1. "unhandled syscall: 8" → `syscall-nr.h`의 enum에서 8번째 = `SYS_FILESIZE`.
2. `read-normal` 테스트 본체가 `check_file()` 헬퍼를 부르고, 그 안에서
   `filesize()`를 호출 → 미구현 콜로 인해 `default` 분기로 떨어짐.
3. **READ 자체보다 FILESIZE를 먼저 구현해야** read-normal이 통과한다.

```c
case SYS_FILESIZE: {
    int fd = (int) f->R.rdi;
    lock_acquire(&filesys_lock);
    struct file *file = thread_current()->fd_table[fd];
    if (file == NULL) {
        lock_release(&filesys_lock);
        break;
    }
    f->R.rax = file_length(file);
    lock_release(&filesys_lock);
    break;
}
```

> 이 콜은 거의 보일러플레이트 — fd_table 조회 + `file_length` 한 줄. 그래도
> "테스트 의존성을 디버그 메시지에서 역추적하는 패턴"을 배운 게 큰 수확.

---

## 5.A 함정들 (READ 단계)

### 함정 5 — 음수 fd가 stdin/stdout 분기를 통과해 배열을 침범

처음에 범위 체크를 빼먹고 `if (fd == 0)` / `if (fd == 1)` 분기만 둔 뒤
`else`에서 `fd_table[fd]`로 갔다. `read-bad-fd` 테스트가 `-1`, `-1024`,
`INT_MIN`을 던지는데, 이들은 **0도 1도 아니므로 그대로 `fd_table[-1]`에
도달**한다 → 페이지 폴트.

**해결**: 범위 체크를 **분기 트리의 가장 위**로 올림.

```c
if (fd < 0 || fd >= 128) { f->R.rax = -1; ... }   /* ← 최상단 */
else if (fd == 0) { ... }
else if (fd == 1) { ... }
else { /* fd_table 접근 */ }
```

> 교훈: **배열 인덱스로 쓰일 값은 가드를 가장 먼저**. 그 뒤로는 "안전하다"고
> 가정하고 분기해도 된다. 분기 사이에 끼워두면 어느 한 분기가 우회 경로가
> 된다.

### 함정 6 — 테스트 실패 메시지 "unhandled syscall: N" 해석법

위 §5.2의 디버깅 흐름이 그대로 일반화 가능하다.

1. `default` 분기에서 `printf("unhandled syscall: %llu\n", ...)`을 찍어두면
2. `N`번째 `syscall-nr.h` enum과 1:1 매칭 가능 (HALT=0, EXIT=1, ..., FILESIZE=8)
3. 그 콜의 미구현이 곧 원인

> stage 0 디폴트 분기에 미리 print를 박아둔 게 큰 도움이 됐다. **`thread_exit()`
> 만 부르고 끝내면 어느 콜이 비었는지 알 수 없다.**

### 함정 7 — stdin 읽기는 한 바이트씩 폴링

`input_getc()`는 키보드 입력을 **한 글자씩** 반환하는 블로킹 호출이다.
`memcpy`처럼 한 번에 N바이트를 받지 못한다.

```c
for (unsigned i = 0; i < size; i++)
    ((char *) buffer)[i] = input_getc();
```

`(char *)` 캐스팅이 필요한 이유: `buffer`는 `void *`로 받았기 때문에
`buffer[i]` 직접 인덱싱이 불가능. byte 단위 쓰기를 명시해야 한다.

---

## 5.B fd_table 접근의 일반 패턴

CLOSE / READ / FILESIZE 모두 같은 두 단계 가드가 들어간다 (이후 §6의 WRITE
fd_table 분기에서도 동일하게 적용):

```c
/* 1단: 범위 체크 — 배열 인덱스로 쓰기 전에 */
if (fd < 0 || fd >= 128) return -1;     /* 또는 break */

/* 2단: 슬롯 NULL 체크 — 닫힌/미할당 fd 가드 */
struct file *file = thread_current()->fd_table[fd];
if (file == NULL) return -1;            /* 또는 break */

/* 이제 안전하게 file_* 호출 */
```

추후 **SEEK / TELL / REMOVE**에도 그대로 복사 예정. 다섯 번째로 같은 코드를
쓸 때쯤이면 헬퍼로 뽑는 걸 고려.

---

## 6. 후속 — SYS_WRITE 확장

stage 0에서는 `fd == 1` (stdout)만 `putbuf`로 처리하고 나머지는 -1로 떨궜다.
이번 단계에서 **fd_table 경로**(`file_write`)를 붙여 완전한 구현으로 마무리.
READ 골격을 그대로 미러링하면 되는 작업이라 새로 배운 건 적지만, **READ와의
대칭 구조가 명확해진** 게 가장 큰 수확이다.

### 6.1 READ ↔ WRITE — 방향만 다른 대칭

| fd 케이스 | SYS_READ | SYS_WRITE |
|---|---|---|
| `fd < 0` 또는 `fd >= 128` | -1 (범위 차단) | -1 (범위 차단) |
| `fd == 0` (stdin) | `input_getc()` 폴링 → size | -1 (쓰기 금지) |
| `fd == 1` (stdout) | -1 (읽기 금지) | `putbuf()` → size |
| `fd >= 2` | `file_read(file, buffer, size)` | `file_write(file, buffer, size)` |

**읽기·쓰기는 방향만 다르고 골격이 동일하다.** 4단 분기, fd_table 두 단계
가드(`§5.B`), 락 잡는 위치까지 전부 같다. stdin/stdout 행은 서로 거울상이
되는데, 이게 POSIX의 "stream은 양방향이지만 stdin은 read-only / stdout은
write-only"라는 규약과 정확히 맞는다.

### 6.2 코드

```c
case SYS_WRITE: {
    int            fd     = (int)            f->R.rdi;
    const void    *buffer = (const void *)   f->R.rsi;
    unsigned       size   = (unsigned)       f->R.rdx;

    validate_user_addr(buffer);
    lock_acquire(&filesys_lock);

    if (fd < 0 || fd >= 128) {
        f->R.rax = -1;
    } else if (fd == 0) {                 /* stdin → 쓰기 금지 */
        f->R.rax = -1;
    } else if (fd == 1) {                 /* stdout → putbuf */
        putbuf(buffer, size);
        f->R.rax = size;
    } else {                              /* fd >= 2 → fd_table */
        struct file *file = thread_current()->fd_table[fd];
        f->R.rax = (file == NULL) ? -1 : file_write(file, buffer, size);
    }
    lock_release(&filesys_lock);
    break;
}
```

> stage 0에서 작성했던 `fd == 1`의 `putbuf` 한 줄이 그대로 분기 한 가지로
> 흡수됐다. 새 작업이 기존 코드를 폐기하지 않고 **완전한 구현의 일부**로
> 자리잡는 가장 깔끔한 시나리오.

### 6.3 분기 순서가 만드는 미세한 의미

`fd == 0`을 `fd < 0 || fd >= 128` 다음에 배치한 이유: 음수가 먼저 걸러지면
이후 분기에서 fd가 unsigned-안전한 양수임이 보장된다. 그 위에서 `fd == 0`
체크는 **stdin 의도가 분명한 케이스**만 잡는다. 이 순서를 뒤집으면(`fd == 0`
먼저), 음수 fd는 다음 분기로 흘러가 버리는 위험이 생긴다 — READ에서 겪었던
함정(§3 함정 5)과 같은 패턴.

### 6.4 락 위치 — 분기 전에 한 번, 분기마다 release

stage 0의 WRITE는 `fd == 1` 한 갈래만 있어 락이 필요 없었지만, fd_table
경로가 들어간 순간 락은 필수. 위치 선택지는:

1. ❌ 분기마다 따로 잡기 — 코드 중복, 누락 위험
2. ✅ **분기 전에 한 번 잡고 모든 분기에서 release** — 현재 구조

stdout 분기에서 `putbuf` 자체는 내부 console lock으로 보호되지만, **fd_table
조회와 같은 락 안에 두는 게 동시성 모델이 단순해진다** (다른 스레드가 같은
파일을 동시에 close/write할 가능성 차단).

---

## 6.A WRITE 확장의 짧은 함정 한 가지

### 함정 8 — fd == 0 분기에서 락 release를 빼먹기 쉽다

stage 0 시절 코드는 `if (fd == 1) {...} else { rax = -1; }` 두 갈래라 break
하나만 있어도 빠져나갔다. 4분기로 늘어나면서 **각 갈래에서 락 release를
재확인**해야 한다.

```c
} else if (fd == 0) {
    f->R.rax = -1;
    lock_release(&filesys_lock);     /* ← 빠뜨리기 쉬움 */
    break;
}
```

코드 양은 같지만 분기가 많아질수록 누락 가능성이 비례 증가. 차라리 **분기
끝에서 한 번에 release**하는 패턴이 안전:

```c
lock_acquire(&filesys_lock);
if (...) { f->R.rax = -1; }
else if (...) { ... }
...
lock_release(&filesys_lock);     /* 한 곳에서만 */
break;
```

WRITE 최종 코드(§6.2)가 이 형태. 각 분기에서 `break` 대신 **rax만 세팅하고
하나의 release를 통과**하게 만들면 누락이 구조적으로 불가능해진다.

---

## 7. 후속 — SYS_FORK / SYS_WAIT (프로세스 콜 진입)

파일 콜이 안정화된 직후 진입한 마지막 P2 영역. **세 파일에 걸친 변경**이고,
앞 §1~§6 의 "콜 한 줄 + 인프라" 구도와 달리 이번엔 **자료구조·동기화·
페이지 테이블 복제**가 한꺼번에 들어온다.

| 파일 | 무엇을 추가 |
|---|---|
| `thread.h` / `thread.c` | `fork_sema`, `fork_success` 멤버 + `init_thread` 초기화 |
| `process.c` | `struct fork_args`, `process_fork`, `__do_fork`, `duplicate_pte` |
| `syscall.c` | `SYS_FORK`, `SYS_WAIT` 라우팅 |

작업한 순서대로 풀어쓴다.

### 7.1 자료구조 — `struct thread` 확장 + `struct fork_args`

기존 `wait_sema` / `exit_sema` (§stage1 — `process_wait`) 옆에 fork 전용
세마포어를 한 줄 추가.

```c
/* thread.h, #ifdef USERPROG */
struct semaphore fork_sema;     /* fork: 자식이 메모리 복사 끝나면 up, 부모가 down */
bool fork_success;              /* 자식 메모리 복사 성공 여부 (실패 시 부모가 -1 반환) */
```

`init_thread` 에 `sema_init(&t->fork_sema, 0)` + `t->fork_success = false`.

다음으로 `__do_fork` 가 **부모 thread 와 부모 intr_frame 두 가지를 모두**
필요로 하는데, `thread_create` 의 `aux` 는 단일 포인터다.
→ 둘을 묶는 작은 구조체를 process.c 상단에 도입:

```c
/* process.c — __do_fork 가 부모 + parent_if 둘 다 필요해서 묶음 */
struct fork_args {
    struct thread *parent;
    struct intr_frame *parent_if;
};
```

### 7.2 SYS_FORK / SYS_WAIT 라우팅 (`syscall.c`)

`syscall_handler` switch 에 두 분기 추가. **§5.B 의 fd_table 가드 패턴은
적용 대상이 아니다** — fork/wait 는 fd 가 아니라 tid 를 다룬다.

```c
case SYS_FORK: {
    const char *name = (const void *) f->R.rdi;   /* 자식 이름 (유저가 넘김) */
    tid_t tid = process_fork(name, f);            /* f 가 곧 부모의 if_ */
    sema_down(&thread_current()->fork_sema);      /* 자식 메모리 복사 끝까지 대기 */
    f->R.rax = tid;
    break;
}

case SYS_WAIT: {
    tid_t pid = (tid_t) f->R.rdi;
    f->R.rax = process_wait(pid);                 /* §stage1 에서 만든 그것 그대로 */
    break;
}
```

WAIT 은 한 줄 라우팅 — `process_wait` 본체가 이미 `wait_sema` / `exit_sema`
로 정식 구현되어 있어서다 (§ stage1).

### 7.3 `process_fork` — fork_args 묶어서 thread_create

```c
tid_t process_fork(const char *name, struct intr_frame *if_) {
    struct fork_args *args = malloc(sizeof(struct fork_args));
    args->parent = thread_current();
    args->parent_if = if_;
    return thread_create(name, PRI_DEFAULT, __do_fork, args);
}
```

`malloc` 으로 힙에 둬야 한다 — 스택에 두면 부모가 `process_fork` 에서
return 한 뒤 사라지는데, `__do_fork` 는 그 이후에 schedule 되어 실행되기
때문이다. `args` 의 free 책임은 `__do_fork` 에 있다.

### 7.4 `__do_fork` — 자식의 메인 흐름

```c
static void __do_fork(void *aux) {
    struct intr_frame if_;
    struct thread *current = thread_current();

    /* 1. aux 캐스팅 + parent_if memcpy + args free */
    struct fork_args *args = (struct fork_args *) aux;
    struct thread *parent = args->parent;
    struct intr_frame *parent_if = args->parent_if;
    memcpy(&if_, parent_if, sizeof(struct intr_frame));
    free(args);

    /* 2. pml4 생성 + 부모 페이지 전체 복제 */
    current->pml4 = pml4_create();
    if (current->pml4 == NULL) goto error;
    process_activate(current);
    if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
        goto error;

    /* 3. fd_table 깊은 복사 (§7.5 함정 3) */
    for (int i = 2; i < 128; i++) {
        if (parent->fd_table[i] != NULL)
            current->fd_table[i] = file_duplicate(parent->fd_table[i]);
    }
    current->fd_next = parent->fd_next;

    /* 4. 자식 반환값 0 + 부모 깨우기 + 유저 모드 진입 */
    if_.R.rax = 0;
    sema_up(&parent->fork_sema);
    do_iret(&if_);

error:
    sema_up(&parent->fork_sema);   /* 실패해도 부모는 반드시 깨워야 함 */
    thread_exit();
}
```

흐름 요약:
1. `aux` → `fork_args` 캐스팅 → `parent_if` 를 자식 스택의 `if_` 로 memcpy → `free(args)`
2. `pml4_create()` → `pml4_for_each(parent->pml4, duplicate_pte, parent)` 로 페이지 테이블 전체 복제
3. `parent->fd_table` 을 슬롯별로 `file_duplicate` 해 자식의 `fd_table` 채우기, `fd_next` 도 복사
4. `if_.R.rax = 0` (자식의 fork 반환값) → `sema_up(parent->fork_sema)` → `do_iret(&if_)` 로 유저 모드 진입

### 7.5 `duplicate_pte` — 한 페이지씩 복사

`pml4_for_each` 가 부모의 모든 PTE 에 대해 호출하는 콜백. 템플릿에 TODO
가 박혀 있어 **이걸 안 채우면 `pml4_set_page` 에서 assertion fail** (함정 4).

```c
static bool duplicate_pte(uint64_t *pte, void *va, void *aux) {
    struct thread *parent = (struct thread *) aux;
    struct thread *current = thread_current();

    /* 1. 커널 주소면 자식 PT 에 매핑할 필요 없음 */
    if (is_kernel_vaddr(va)) return true;

    /* 2. 부모 페이지 해석 */
    void *parent_page = pml4_get_page(parent->pml4, va);

    /* 3. 자식용 새 페이지 (USER + ZERO) */
    void *newpage = palloc_get_page(PAL_USER | PAL_ZERO);
    if (newpage == NULL) return false;

    /* 4. 부모 페이지 내용 복사 + writable 결정 */
    memcpy(newpage, parent_page, PGSIZE);
    bool writable = is_writable(pte);

    /* 5. 자식 페이지 테이블에 등록 */
    if (!pml4_set_page(current->pml4, va, newpage, writable)) {
        palloc_free_page(newpage);
        return false;
    }
    return true;
}
```

핵심: **새 물리 페이지를 따로 할당해 내용만 복사**한다. 부모와 같은
물리 페이지를 가리키게 하면 안 된다 (CoW 가 아닌 정직한 복사 = Project 2
요구사항).

### 7.6 FORK / WAIT 동기화 시퀀스

```
[부모 syscall_handler]                    [자식 __do_fork]
SYS_FORK 진입
process_fork(name, f)
  malloc(fork_args), parent_if 보관
  thread_create(__do_fork, args)  ──────►  thread 생성, ready 큐 진입
return from process_fork
sema_down(&parent->fork_sema)  ⏸          schedule 되어 시작
                                          aux → fork_args 캐스팅
                                          parent_if memcpy → if_, free(args)
                                          pml4_create + pml4_for_each
                                            (duplicate_pte 페이지마다)
                                          fd_table 복사 (file_duplicate)
                                          fd_next 복사
                                          if_.R.rax = 0
                                ◄──────   sema_up(&parent->fork_sema)
[부모 재개]                               do_iret(&if_)  → 유저 모드, fork()=0
f->R.rax = tid
return                                    (자식 사용자 코드 실행)
유저 모드 복귀, fork()=tid
```

핵심 포인트:
- **부모는 자식 메모리 복사가 끝날 때까지 BLOCKED** — 자식이 부모의
  주소공간을 참조하는 동안 부모가 syscall 을 통해 그 공간을 바꾸면 안 된다
- **error 케이스에도 `sema_up` 필수** (함정 5) — 그렇지 않으면 부모가
  `fork_sema` 에서 영원히 대기

---

## 7.A 함정들 (FORK 단계)

번호는 §3 (1~4), §5.A (5~7), §6.A (8) 의 연장.

### 함정 9 — `fork("child")` 의 이름은 부모 이름이 아니다

`process_fork` 시 thread 이름을 `thread_current()->name` 으로 쓰면
**자식이 부모 이름을 그대로 물려받는다**. 정작 유저는 `fork("child")`
처럼 새 이름을 인자로 넘긴다.

```c
case SYS_FORK: {
    const char *name = (const void *) f->R.rdi;   /* ← 유저가 넘긴 이름 */
    tid_t tid = process_fork(name, f);
    ...
}
```

`f->R.rdi` 의 시스템 콜 1번 인자에서 직접 가져와야 한다.
이걸 놓치면 `multi-child-fork` 같은 테스트의 출력 비교가 어긋난다.

### 함정 10 — `aux` 는 단일 포인터, parent_if 를 같이 못 보낸다

`thread_create(name, prio, fn, aux)` 의 `aux` 가 하나라
**parent + parent_if 두 개를 동시에 넘길 수 없다**.

해결: §7.1 의 `struct fork_args` 로 묶기. `malloc` 으로 힙에 두고
`__do_fork` 가 `free` 책임을 진다 (스택 변수로 두면 `process_fork`
return 후 사라져 use-after-free).

### 함정 11 — `fd_table` 을 포인터 복사하면 부모/자식이 같은 file 객체 공유

```c
/* 잘못 — 부모 fd 닫으면 자식 fd 도 깨짐 */
memcpy(current->fd_table, parent->fd_table, sizeof(parent->fd_table));

/* 올바름 — 슬롯마다 독립 객체 생성 */
for (int i = 2; i < 128; i++) {
    if (parent->fd_table[i] != NULL)
        current->fd_table[i] = file_duplicate(parent->fd_table[i]);
}
```

`file_duplicate` 는 `inode` 는 공유하되 file 객체 (포지션·deny_write 상태
등) 를 독립으로 떠준다 — 정확히 fork 시맨틱.

### 함정 12 — `duplicate_pte` 의 TODO 미구현 → kernel panic

템플릿이 비어 있어서 그냥 두면 `pml4_set_page(current->pml4, va, NULL, ...)`
같은 모양으로 호출되어 **assertion failed** 로 커널이 죽는다.

체크리스트 (§7.5 그대로):
1. `is_kernel_vaddr(va)` 면 즉시 `return true` (커널 페이지는 자식 PT 에 매핑 불요)
2. `palloc_get_page(PAL_USER | PAL_ZERO)` 로 새 물리 페이지
3. `memcpy(newpage, parent_page, PGSIZE)` 로 내용 복사
4. `is_writable(pte)` 로 쓰기 권한 판단
5. `pml4_set_page` 실패 시 `palloc_free_page(newpage)` 후 `return false`

이 5단을 다 채워야 fork 가 한 번이라도 성공한다.

### 함정 13 — error 레이블에서 `sema_up` 누락 시 부모 영구 대기

```c
error:
    sema_up(&parent->fork_sema);   /* ★ 누락하면 부모가 syscall 핸들러에서 영원히 BLOCKED */
    thread_exit();
```

성공 경로에서만 `sema_up` 하면 자식 메모리 복사가 실패한 순간 부모가
`sema_down(&parent->fork_sema)` 에서 깨어나지 못한다.
**fork_sema 는 성공·실패 모두 정확히 한 번 up 되어야 한다** — `wait_sema`
패턴 (§stage1) 과 동일한 invariant.

### 함정 14 — `SYS_WAIT` 라우팅을 안 깔면 `[stage0] unhandled syscall: 4`

`process_wait` 본체는 stage1 에서 이미 만들어 놨지만, syscall 디스패처에
SYS_WAIT 분기가 빠지면 **fork 테스트가 자식 종료 회수 단계에서 죽는다**.
한 줄짜리 라우팅이지만 누락하면 fork 의 정상성을 검증할 수가 없다 — §6.A
함정 8 ("WRITE 분기 누락" → unhandled syscall) 과 같은 패밀리의 실수.

```c
case SYS_WAIT: {
    tid_t pid = (tid_t) f->R.rdi;
    f->R.rax = process_wait(pid);
    break;
}
```

---

## 8. 후속 — SYS_EXEC (`exec-missing` 통과까지의 시행착오 7회)

FORK/WAIT 직후 진입한 마지막 P2 콜. 다른 콜과 달리 **테스트 한 개
(`exec-missing`) 통과에 압도적으로 오래 걸렸고**, 그 과정에서 `load()` —
`process_exec()` — `SYS_EXEC` — `initd()` 네 곳이 **동시에** 맞물려야
한다는 걸 알게 됐다. 시행착오 전체를 순서대로 남긴다.

### 8.1 문제 정의

```
exec("no-such-file")
  → load() 가 실패해야 한다 (파일 없음)
  → 그 결과로 현재 프로세스가 exit(-1) 로 종료되어야 한다
```

핵심 시맨틱: **exec 실패 = "현재 프로세스 교체 실패" = 호출자 자체가 죽음**.
유저에게 -1 을 반환하는 `read()` / `write()` 와 시맨틱이 다르다.

### 8.2 시행착오 연대기

#### 시행착오 1 — `process_exec()` 실패 시 단순 `return -1`

가장 먼저 시도한 안. load 실패하면 그냥 `return -1`.

```c
/* process_exec */
if (!success) return -1;
```

**증상**: page fault.

**원인**: `process_exec` 진입 시 이미 `process_cleanup()` 으로 **현재
페이지 테이블을 destroy 한 상태**다. 이 상태에서 `return -1` 로 유저
프로그램에 돌아가면 코드 페이지가 없어서 즉사.

#### 시행착오 2 — `process_cleanup()` 을 `load()` 이후로 이동

cleanup 시점을 늦춰서, load 가 성공한 다음에만 기존 pml4 를 정리하도록 변경.

**증상**: page fault (이번엔 do_iret 직후).

**원인**: `process_cleanup()` 이 **새로 load 한 pml4 까지 destroy** 해버린다.
어차피 같은 thread 의 pml4 슬롯이라, "기존" 과 "새것" 을 cleanup 이 구분 못함.

#### 시행착오 3 — `process_cleanup()` 완전 제거

cleanup 호출을 통째로 빼버림. load 가 내부적으로 pml4 를 교체하니 별도
cleanup 불필요한 게 맞다.

**증상**: 일반 exec 는 통과하지만, **`exec-missing` 은 여전히 page fault**.

**원인**: load 실패 시 **새 pml4 가 thread 에 남아있고**, 기존 유저
프로그램의 페이지가 거기엔 없다. 즉, load 가 성공했든 실패했든
`t->pml4` 가 새것을 가리키게 되어 있는 상태.

#### 시행착오 4 — `load()` 에서 `old_pml4` 백업/복원

```c
uint64_t *old_pml4 = t->pml4;
t->pml4 = pml4_create();
if (t->pml4 == NULL)        /* ← 잘못된 NULL 체크 */
    goto done;
...
done:
    if (!success) {
        t->pml4 = old_pml4;
        pml4_activate(old_pml4);
    }
```

**증상**: `initd` 첫 호출 시 즉시 `goto done` 으로 빠져 load 자체가
무조건 실패.

**원인**: NULL 체크를 `t->pml4` 로 했는데, **`initd` 단계에선 `old_pml4`
가 NULL** 이라 `t->pml4 = pml4_create()` 직후 그 새 pml4 가 NULL 이 아니어도
체크의 의미가 어긋난다 — 정확히는 코드 흐름상 새 pml4 의 NULL 여부를
판별하는 의도였는데 변수 분리가 안 돼서 의도와 다른 값을 보고 있었다.

#### 시행착오 5 — NULL 체크 변수 분리

```c
uint64_t *new_pml4 = pml4_create();
if (new_pml4 == NULL)       /* ← 새 pml4 자체를 검사 */
    goto done;
t->pml4 = new_pml4;
```

**증상**: `load()` 자체는 통과. 그러나 `exec("no-such-file")` 실패 후
`process_exec()` 이 `return -1` → `SYS_EXEC` 핸들러에서 `f->R.rax = -1`
세팅 후 유저 프로그램 복귀 → 유저 프로그램이 `exit(0)` 으로 종료.

**원인**: 테스트가 기대하는 건 `exit(-1)`. 시맨틱 불일치 — exec 실패는
**유저에게 -1 반환** 이 아니라 **호출자 프로세스 자체를 -1 로 종료** 다.

#### 시행착오 6 — `SYS_EXEC` 에서 `thread_exit()` 호출

```c
case SYS_EXEC: {
    int r = process_exec(...);
    thread_current()->exit_status = -1;
    thread_exit();
}
```

**증상**: `initd` 의 `PANIC("Fail to launch initd")` 발동, 커널 패닉.

**원인**: `exec-missing` 테스트 자체를 부팅할 때 **initd 가
`process_exec` 으로 사용자 프로그램을 띄운다**. 이 단계에서 실패하면
테스트가 시작도 못 하는데, 우리 변경이 initd 의 기존 PANIC 을 그대로
타격함.

#### 시행착오 7 — `initd()` PANIC 을 `NOT_REACHED()` 로 교체

PANIC 대신 NOT_REACHED 로 바꾸면 부드럽게 빠질 줄 알았음.

**증상**: 같은 자리에서 `NOT_REACHED()` PANIC 발동.

**원인**: `NOT_REACHED()` 는 매크로상 도달 시 패닉이 도지는 어서션이다
("이 라인까지 오면 안 된다"). PANIC 을 NOT_REACHED 로 바꾼 건 **이름만
바꾼 것이지 의미는 같다**. initd 단계의 실패는 **진짜로 비정상**이라
panic 이 맞고, 우리가 잡아야 할 건 **테스트 본체에서의 exec 실패**다.

### 8.3 최종 해결 — 4 곳 동시 수정

7회 시도가 가르쳐 준 것: **load / process_exec / SYS_EXEC / initd 가 한
사슬로 엮여 있어, 한 곳만 고치면 다른 곳에서 터진다**. 네 곳 동시 수정해야
정확히 하나의 시맨틱으로 수렴한다.

#### 1) `load()` — old_pml4 백업 + new_pml4 NULL 체크 + 실패 시 복원

```c
bool
load (const char *file_name, struct intr_frame *if_) {
    struct thread *t = thread_current();
    uint64_t *old_pml4 = t->pml4;            /* 백업 */
    uint64_t *new_pml4 = pml4_create();      /* 별도 변수에 받기 */
    if (new_pml4 == NULL)                    /* ★ new_pml4 기준 NULL 체크 */
        goto done;
    t->pml4 = new_pml4;
    process_activate(t);
    /* ... ELF 파싱 / 세그먼트 적재 ... */

done:
    file_close(file);
    if (!success) {
        if (t->pml4 != old_pml4)
            pml4_destroy(t->pml4);           /* 새 pml4 정리 */
        t->pml4 = old_pml4;                  /* 원복 */
        pml4_activate(old_pml4);
    }
    return success;
}
```

#### 2) `process_exec()` — `process_cleanup()` 제거, 실패 시 `return -1`

```c
int
process_exec (void *f_name) {
    /* ★ 기존의 process_cleanup() 호출 삭제 — load 가 pml4 를 교체한다 */
    /* ... 인자 파싱, load 호출 ... */
    if (!success) {
        palloc_free_page(file_name);
        return -1;
    }
    do_iret(&_if);   /* 성공하면 여기서 영원히 안 돌아옴 */
    NOT_REACHED();
}
```

#### 3) `SYS_EXEC` — 실패 시 `exit(-1)`

```c
case SYS_EXEC: {
    const char *filename = (const void *) f->R.rdi;
    validate_user_addr(filename);

    /* process_exec 는 palloc 페이지를 기대 — 유저 포인터를 그대로 넘기면 안 됨 */
    char *fn_copy = palloc_get_page(PAL_ZERO);
    strlcpy(fn_copy, filename, PGSIZE);

    int result = process_exec(fn_copy);
    /* 여기까지 왔으면 무조건 실패 (성공이면 do_iret 에서 안 돌아옴) */
    thread_current()->exit_status = -1;
    thread_exit();
    NOT_REACHED();
}
```

#### 4) `initd()` — PANIC 유지

```c
if (process_exec(f_name) < 0)
    PANIC("Fail to launch initd\n");
```

initd 의 exec 실패는 진짜 부팅 실패이므로 panic 이 맞다.
**`exec-missing` 은 initd 단계에선 성공하고, 그 위 사용자 프로그램 안에서
`exec("no-such-file")` 이 실패하는 시나리오** — 두 단계의 exec 가
구분되어야 한다.

### 8.4 핵심 통찰

1. **`process_exec` 가 성공하면 절대 돌아오지 않는다 (do_iret).**
   따라서 호출자 (`SYS_EXEC`, `initd`) 의 "그 다음 코드" 는 **실패 경로
   전용**이다. 이 사실을 못 받아들이면 SYS_EXEC 의 `thread_exit()` 가
   불필요해 보인다.

2. **exec 실패 = 호출자 프로세스 종료 = `exit(-1)`.**
   `read`/`write` 처럼 "유저에게 -1 반환" 이 아니다. 시행착오 5 의
   본질이 이 시맨틱 차이를 못 보던 것.

3. **load 가 새 pml4 를 만들면서 기존 pml4 를 교체한다.**
   따라서 실패 시 **원본으로 되돌릴 책임도 load 에 있다** —
   `old_pml4` 백업·복원·activate 가 한 묶음.

4. **NULL 체크 대상의 변수는 명시적으로 분리.**
   `t->pml4 == NULL` 처럼 멤버 변수로 검사하면 initd 의 `old_pml4 == NULL`
   상황과 의미가 섞인다. `new_pml4 = pml4_create()` 후 `new_pml4 == NULL`
   로 검사하는 게 의도가 한 줄로 드러나는 코드.

### 8.5 함정 15 — 네 곳 사이의 의존성을 한 번에 고려해야 한다

기존 함정 1~14 는 **한 함수 안의 한 분기 / 한 자료구조** 수준이었다.
EXEC 의 함정은 다르다 — **load 의 NULL 체크 변수**, **process_exec 의
cleanup 시점**, **SYS_EXEC 의 실패 시 thread_exit**, **initd 의
PANIC 유지** 가 한 체인이라, 한 개만 고치면 다음 시도에서 새 증상이
튀어나온다 (시행착오 1→2→3 의 page fault → 4→5 의 load 실패 → 6→7
의 PANIC).

→ 이 함정에 대한 디버깅 전략: **한 시도 후 같은 자리가 깨끗해지면
다음으로 넘어가지 말고, 다른 자리에서 새 증상이 안 나오는지를 같이 확인.**
exec 처럼 "한 콜이 4개 함수에 걸쳐 책임을 나눠 가진" 케이스에선
국소 수정이 항상 부분 해결로 끝난다.

---

## 9. 다음 단계로 미룬 것들

- **REMOVE / SEEK / TELL** — `filesys_remove` / `file_seek` / `file_tell`
  한 줄 래퍼. §5.B의 fd_table 두 단계 가드 패턴 그대로 적용.
- **fd 슬롯 재사용** — `fd_next` 단조 증가는 누수 발생. 빈 슬롯 스캔으로 변경.
- **per-thread 락 / inode 단위 락** — 현재 굵은 락의 성능 이슈.
- **buffer 검증의 페이지 경계 처리** — `validate_user_addr`은 한 바이트만
  검사. READ/WRITE의 `(buffer, size)` 전 구간을 페이지마다 재검증해야 함.
- **`fork_success` 활용** — 현재는 멤버만 추가하고 실제 분기는 안 깔림.
  `__do_fork` 에서 success/fail 을 기록 → 부모가 그 값을 읽고 fork 반환값을
  `-1` 로 정정하는 패턴으로 확장 예정.
- **EXEC 의 fn_copy 누수** — `process_exec` 성공 경로 (`do_iret` 으로
  안 돌아옴) 에서 `palloc_free_page(fn_copy)` 가 불릴 자리가 없다.
  현재는 그냥 누수 — 다음 단계에서 `process_exec` 안에서 free 하도록 정리.

---

## 10. 한 줄 요약

> 파일 시스템 콜 구현의 무게중심은 **`filesys_*` 한 줄**이 아니라
> **그 한 줄을 부르기 전·후의 인프라**(유저 포인터 3단 검증 / 전역 락 /
> per-thread fd 테이블)에 있다. 인프라를 먼저 깔면 콜은 같은 골격으로
> 찍어낼 수 있고, 64bit Pintos의 함정(KERN_BASE / implicit-decl /
> 분기별 락 누수 / 음수 fd의 배열 침범)은 이 인프라 단계에서 한꺼번에 잡힌다.
> 시리즈 전체의 교훈 셋: **(a) 배열 인덱스 가드는 분기 트리의 가장 위에**,
> **(b) unhandled syscall: N → `syscall-nr.h` enum 역추적**, **(c) READ와
> WRITE는 방향만 다르고 골격이 동일** — 한 콜을 잘 짜놓으면 다음 콜은
> 미러링이 된다.
>
> FORK/WAIT (§7) 로 가면 무게중심이 **자료구조 + 동기화** 로 이동한다.
> `fork_sema` 한 개가 "부모는 자식 메모리 복사가 끝나야만 깨어난다" 라는
> 시맨틱을 한 줄로 표현하고, 그 invariant 가 깨지는 모든 경로 (성공·실패)
> 에서 `sema_up` 이 한 번씩 보장되어야 한다. fork 의 함정 6개 (§7.A
> 9~14) 는 결국 **단일 포인터 aux / 깊은 복사 / 페이지 테이블 복제 /
> 신호 invariant** 네 가지 축에서 발생한다.
>
> EXEC (§8) 는 다시 한 번 무게중심을 옮긴다 — 이번엔 **함수 간 책임 분배**다.
> `exec-missing` 테스트 하나를 위해 `load` / `process_exec` / `SYS_EXEC` /
> `initd` 네 곳이 **동시에 한 시맨틱** ("실패하면 호출자가 exit(-1)") 을
> 향해 정렬되어야 한다. 시행착오 7 회 (§8.2) 는 모두 "한 곳만 고치면
> 다른 곳에서 터진다" 의 변주이고, 시리즈를 통틀어 **국소 수정의 한계**
> 를 가장 명확히 보여 준 사건이다 (§8.5 함정 15).
