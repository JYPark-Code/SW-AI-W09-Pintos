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

## 7. 다음 단계로 미룬 것들

- **REMOVE / SEEK / TELL** — `filesys_remove` / `file_seek` / `file_tell`
  한 줄 래퍼. §5.B의 fd_table 두 단계 가드 패턴 그대로 적용.
- **fd 슬롯 재사용** — `fd_next` 단조 증가는 누수 발생. 빈 슬롯 스캔으로 변경.
- **per-thread 락 / inode 단위 락** — 현재 굵은 락의 성능 이슈.
- **buffer 검증의 페이지 경계 처리** — `validate_user_addr`은 한 바이트만
  검사. READ/WRITE의 `(buffer, size)` 전 구간을 페이지마다 재검증해야 함.
- **WAIT 라우팅 / EXEC / FORK** — 파일 콜이 끝났으니 프로세스 콜로 진입.
  Project 3 진입 전 마무리해야 할 마지막 P2 영역.

---

## 8. 한 줄 요약

> 파일 시스템 콜 구현의 무게중심은 **`filesys_*` 한 줄**이 아니라
> **그 한 줄을 부르기 전·후의 인프라**(유저 포인터 3단 검증 / 전역 락 /
> per-thread fd 테이블)에 있다. 인프라를 먼저 깔면 콜은 같은 골격으로
> 찍어낼 수 있고, 64bit Pintos의 함정(KERN_BASE / implicit-decl /
> 분기별 락 누수 / 음수 fd의 배열 침범)은 이 인프라 단계에서 한꺼번에 잡힌다.
> 시리즈 전체의 교훈 셋: **(a) 배열 인덱스 가드는 분기 트리의 가장 위에**,
> **(b) unhandled syscall: N → `syscall-nr.h` enum 역추적**, **(c) READ와
> WRITE는 방향만 다르고 골격이 동일** — 한 콜을 잘 짜놓으면 다음 콜은
> 미러링이 된다.
