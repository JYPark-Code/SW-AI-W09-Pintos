# Pintos `tests/threads` 테스트 분석 정리

## 1. 범위

이 문서는 아래 두 파일에 등록된 `threads` 계열 테스트를 기준으로 정리했다.

- `pintos/tests/threads/Make.tests`
- `pintos/tests/threads/mlfqs/Make.tests`

즉, 일반 `threads` 테스트와 `mlfqs` 테스트를 모두 포함한다.

## 2. 테스트 파일 구성 방식

Pintos의 `threads` 테스트는 보통 아래 3종류 파일로 구성된다.

- `*.c`
  - 실제 테스트 로직이 들어 있는 C 소스다.
  - `pintos/tests/threads/tests.c`의 `run_test()`가 테스트 이름에 맞는 함수를 호출한다.
- `*.ck`
  - 테스트가 끝난 뒤 `.output` 파일을 읽어 PASS/FAIL을 판정하는 Perl checker 스크립트다.
  - 어떤 테스트는 출력 전체를 정확히 비교하고, 어떤 테스트는 순서/수치/허용 오차만 검사한다.
- `*.pm`
  - 여러 `.ck` 파일이 공유하는 Perl 유틸 모듈이다.
  - 예: `alarm.pm`, `mlfqs.pm`

예를 들어 `alarm-multiple.ck`는 "테스트 본체"가 아니라 "출력 검증기"다.  
실제 테스트 본체는 `pintos/tests/threads/alarm-wait.c` 안의 `test_alarm_multiple()`이고,  
`alarm-multiple.ck`는 `tests::threads::alarm` 모듈의 `check_alarm(7)`을 호출해 출력 순서를 검증한다.

## 3. 공통 실행 방법

현재 워크스페이스 기준 빌드 디렉터리는 이미 `pintos/threads/build`에 있다.

### 전체 `threads` 테스트 실행

```bash
cd pintos/threads/build
make check
```

이 명령은 `tests/threads`와 `tests/threads/mlfqs`에 등록된 테스트를 모두 실행한다.

### 개별 테스트 실행

일반 `threads` 테스트:

```bash
cd pintos/threads/build
make tests/threads/alarm-multiple.result
```

MLFQS 테스트:

```bash
cd pintos/threads/build
make tests/threads/mlfqs/mlfqs-load-1.result
```

`.result`는 실행 + checker 판정까지 포함한다.  
`.output`은 실행 로그만 만든다.

예:

```bash
make tests/threads/alarm-multiple.output
```

## 4. 내부적으로 어떻게 실행되는가

`pintos/tests/Make.tests`를 보면 개별 테스트는 기본적으로 아래 형태로 실행된다.

```bash
pintos -v -k -T <TIMEOUT> -m <MEMORY> -- -q run <test-name>
```

그리고 그 결과로 생긴 `<test-name>.output`을 `<test-name>.ck`가 읽어서 PASS/FAIL을 결정한다.

즉, `alarm-multiple`의 핵심 흐름은 다음과 같다.

1. `make tests/threads/alarm-multiple.result`
2. Pintos가 `run alarm-multiple`로 테스트 실행
3. 출력이 `tests/threads/alarm-multiple.output`에 저장
4. `tests/threads/alarm-multiple.ck`가 그 출력을 읽어 검사

## 5. 테스트 목록 분석

---

## Alarm 계열

### `alarm-single`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-wait.c`
  - 체커: `pintos/tests/threads/alarm-single.ck`
  - 공용 체커 모듈: `pintos/tests/threads/alarm.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-single.result`
- `alarm-single.ck`는 무엇인가
  - `check_alarm(1)`을 호출하는 Perl checker다.
  - 5개 스레드가 각기 다른 sleep 시간을 가지고 1번씩 깨어나는 순서가 올바른지 검증한다.
- 무엇을 테스트하나
  - `timer_sleep()`이 서로 다른 wake-up 시점을 가진 스레드를 올바른 시간 순서대로 깨우는지 확인한다.
  - 바쁜 대기 없이 sleeping queue가 제대로 동작하는지를 보는 대표적인 기본 테스트다.

### `alarm-multiple`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-wait.c`
  - 체커: `pintos/tests/threads/alarm-multiple.ck`
  - 공용 체커 모듈: `pintos/tests/threads/alarm.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-multiple.result`
- `alarm-multiple.ck`는 무엇인가
  - Perl checker 스크립트다.
  - `use tests::threads::alarm;`로 공용 검사 로직을 불러오고 `check_alarm(7)`을 호출한다.
  - 여기서 `7`은 각 스레드가 7번씩 sleep/wakeup을 반복한다는 뜻이다.
- 무엇을 테스트하나
  - 5개 스레드가 각각 `10, 20, 30, 40, 50` tick 단위로 7번씩 잠들었다 깨어날 때, 전체 wake-up 순서가 비감소 순서인지 확인한다.
  - 테스트 코드에서는 각 스레드의 `iteration * duration` 값을 계산하고, 그 값이 뒤로 갈수록 작아지면 실패시킨다.
  - 즉 단순 1회성 wake-up이 아니라, 여러 번 반복되는 sleep 요청을 안정적으로 처리하는지를 검증한다.

### `alarm-simultaneous`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-simultaneous.c`
  - 체커: `pintos/tests/threads/alarm-simultaneous.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-simultaneous.result`
- `alarm-simultaneous.ck`는 무엇인가
  - 예상 출력 전체를 `check_expected`로 비교하는 checker다.
- 무엇을 테스트하나
  - 여러 스레드가 같은 tick에 깨어나야 할 때 정말 같은 tick에 깨는지 확인한다.
  - 테스트에서는 3개 스레드가 모두 10 tick씩 5번 sleep한다.
  - 각 iteration에서 첫 번째 스레드는 이전 iteration보다 10 tick 뒤에 깨어나고, 같은 iteration 안의 나머지 스레드는 `0 ticks later`여야 한다.
  - 즉 "동일 wake-up 시각을 가진 스레드들을 같은 tick에 깨우는가"를 테스트한다.

### `alarm-priority`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-priority.c`
  - 체커: `pintos/tests/threads/alarm-priority.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-priority.result`
- `alarm-priority.ck`는 무엇인가
  - 정확한 깨움 순서를 기대 출력으로 고정한 checker다.
- 무엇을 테스트하나
  - 여러 스레드가 같은 시각에 깨더라도, 실행 재개 순서는 우선순위가 높은 스레드부터여야 함을 검증한다.
  - 10개 스레드를 서로 다른 priority로 만들고 모두 같은 `wake_time`에 맞춰 `timer_sleep()` 시킨다.
  - wake-up 이후 출력이 `priority 30`부터 `priority 21`까지 순서대로 나와야 한다.
  - 즉 `timer_sleep()`에서 깨운 뒤 ready list 삽입과 스케줄링이 priority-aware한지 보는 테스트다.

### `alarm-zero`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-zero.c`
  - 체커: `pintos/tests/threads/alarm-zero.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-zero.result`
- `alarm-zero.ck`는 무엇인가
  - `(alarm-zero) PASS` 출력만 확인하는 단순 checker다.
- 무엇을 테스트하나
  - `timer_sleep(0)`이 즉시 반환되어야 함을 확인한다.
  - 0 tick sleep 요청을 sleeping 상태로 넣거나 불필요하게 block시키면 안 된다는 뜻이다.

### `alarm-negative`

- 관련 파일
  - 본체: `pintos/tests/threads/alarm-negative.c`
  - 체커: `pintos/tests/threads/alarm-negative.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/alarm-negative.result`
- `alarm-negative.ck`는 무엇인가
  - `(alarm-negative) PASS` 출력만 확인하는 단순 checker다.
- 무엇을 테스트하나
  - `timer_sleep(-100)` 같은 음수 인자가 들어와도 커널이 크래시하지 않아야 함을 확인한다.
  - 보통 이 테스트는 음수/0 입력을 즉시 반환 처리하는지 점검하는 용도다.

---

## Priority 계열

### `priority-change`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-change.c`
  - 체커: `pintos/tests/threads/priority-change.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-change.result`
- `priority-change.ck`는 무엇인가
  - 메시지 순서를 정확히 비교하는 checker다.
- 무엇을 테스트하나
  - 현재 실행 중인 스레드가 자기 우선순위를 낮췄을 때, 더 높은 priority의 ready thread가 있으면 즉시 CPU를 양보하는지 확인한다.
  - 즉 `thread_set_priority()`가 단순히 숫자만 바꾸는 것이 아니라 preemption까지 유발하는지 보는 테스트다.

### `priority-donate-one`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-one.c`
  - 체커: `pintos/tests/threads/priority-donate-one.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-one.result`
- `priority-donate-one.ck`는 무엇인가
  - donation 전파와 lock release 이후 실행 순서를 기대 출력으로 비교하는 checker다.
- 무엇을 테스트하나
  - 메인 스레드가 lock을 잡고 있을 때, 더 높은 priority 스레드들이 그 lock 때문에 block되면 메인 스레드가 그 priority를 donation 받아야 한다.
  - 그리고 lock을 놓으면 가장 높은 priority waiters가 먼저 lock을 획득해야 한다.
  - 가장 기본적인 1단계 priority donation 테스트다.

### `priority-donate-multiple`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-multiple.c`
  - 체커: `pintos/tests/threads/priority-donate-multiple.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-multiple.result`
- `priority-donate-multiple.ck`는 무엇인가
  - 두 개 lock에 대해 donation이 누적되고, 각 lock을 해제할 때 donation이 단계적으로 사라지는지 검사하는 checker다.
- 무엇을 테스트하나
  - 메인 스레드가 lock A, B를 모두 들고 있고, 각 lock을 기다리는 더 높은 priority 스레드 둘이 donation을 보낸다.
  - 메인 스레드는 더 높은 donation 값으로 priority가 올라가야 하고, lock 하나를 해제하면 남은 donation만 유지해야 한다.
  - 즉 "다중 donation"과 "부분 release 시 priority 복원"을 테스트한다.

### `priority-donate-multiple2`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-multiple2.c`
  - 체커: `pintos/tests/threads/priority-donate-multiple2.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-multiple2.result`
- `priority-donate-multiple2.ck`는 무엇인가
  - `priority-donate-multiple`의 변형 시나리오에서 lock 해제 순서가 달라져도 올바른 결과가 나오는지 확인하는 checker다.
- 무엇을 테스트하나
  - 메인 스레드가 donation을 2개 받고, 중간에 donation과 무관한 thread `c`도 섞여 있다.
  - lock A를 먼저 풀고 lock B를 나중에 푸는 순서를 통해 ready queue와 donation 정리가 꼬이지 않는지 본다.
  - 기대 순서는 `b -> a -> c`다.

### `priority-donate-nest`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-nest.c`
  - 체커: `pintos/tests/threads/priority-donate-nest.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-nest.result`
- `priority-donate-nest.ck`는 무엇인가
  - 중첩 donation 상황에서 low/medium/high 스레드의 priority가 기대한 값으로 바뀌는지 확인하는 checker다.
- 무엇을 테스트하나
  - low thread가 lock A를 들고 있고, medium thread가 lock B를 들고 lock A를 기다리며, high thread가 lock B를 기다리는 구조다.
  - 따라서 high의 priority가 medium에게, 다시 low에게 연쇄적으로 전달되어야 한다.
  - 즉 nested donation, donation chain 2단계를 검증한다.

### `priority-donate-sema`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-sema.c`
  - 체커: `pintos/tests/threads/priority-donate-sema.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-sema.result`
- `priority-donate-sema.ck`는 무엇인가
  - semaphore와 lock이 섞인 상황에서 스레드 종료 순서가 기대와 같은지 보는 checker다.
- 무엇을 테스트하나
  - low thread는 lock을 잡은 채 semaphore에서 block되고, high thread는 그 lock 때문에 low에게 donation한다.
  - 이후 semaphore up/down 흐름 속에서 `H -> M -> L -> main` 순으로 진행되어야 한다.
  - donation이 semaphore 대기와 섞여도 깨지지 않는지 보는 테스트다.

### `priority-donate-lower`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-lower.c`
  - 체커: `pintos/tests/threads/priority-donate-lower.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-lower.result`
- `priority-donate-lower.ck`는 무엇인가
  - donation 중인 스레드가 자신의 base priority를 낮춰도 effective priority가 유지되는지 확인하는 checker다.
- 무엇을 테스트하나
  - 메인 스레드가 donation을 받고 있는 상태에서 `thread_set_priority()`로 base priority를 낮춘다.
  - 이때 donation이 살아 있는 동안에는 실제 priority가 떨어지면 안 되고, lock을 놓아 donation이 사라진 뒤에야 낮아진 base priority가 반영되어야 한다.

### `priority-donate-chain`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-donate-chain.c`
  - 체커: `pintos/tests/threads/priority-donate-chain.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-donate-chain.result`
- `priority-donate-chain.ck`는 무엇인가
  - 긴 donation chain과 interloper thread들의 실행 순서를 통째로 기대 출력과 비교하는 checker다.
- 무엇을 테스트하나
  - main thread가 lock 0을 잡고, thread 1..7이 lock을 사슬처럼 기다리도록 만든다.
  - donation이 `thread 7 -> ... -> thread 1 -> main`까지 연쇄 전파되어야 한다.
  - 동시에 각 단계보다 priority가 1 낮은 interloper thread들을 넣어, donation chain이 끝나기 전에는 이들이 끼어들지 못해야 한다.
  - 즉 deep donation chain과 strict scheduling order를 함께 보는 강한 테스트다.

### `priority-fifo`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-fifo.c`
  - 체커: `pintos/tests/threads/priority-fifo.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-fifo.result`
- `priority-fifo.ck`는 무엇인가
  - 16개 같은 priority 스레드의 각 iteration 출력이 항상 동일한 순열인지 파싱해서 확인하는 custom checker다.
  - 중요한 점은 반드시 `0 1 2 ... 15`일 필요는 없고, 첫 줄과 이후 줄이 모두 동일하면 된다.
- 무엇을 테스트하나
  - 같은 우선순위의 스레드들이 round-robin/FIFO 순서를 일관되게 유지하는지 확인한다.
  - ready list가 같은 priority 안에서 삽입 순서를 깨뜨리면 이 테스트가 실패한다.

### `priority-preempt`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-preempt.c`
  - 체커: `pintos/tests/threads/priority-preempt.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-preempt.result`
- `priority-preempt.ck`는 무엇인가
  - high-priority thread가 먼저 전부 실행되고 난 뒤에 main thread의 마지막 메시지가 찍혀야 한다는 점을 비교하는 checker다.
- 무엇을 테스트하나
  - 더 높은 priority 스레드가 생성되면 현재 실행 중인 lower-priority thread를 즉시 preempt해야 한다.
  - 즉 `thread_create()` 이후 scheduling decision이 바로 일어나는지 테스트한다.

### `priority-sema`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-sema.c`
  - 체커: `pintos/tests/threads/priority-sema.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-sema.result`
- `priority-sema.ck`는 무엇인가
  - `sema_up()` 때 깨어나는 순서가 높은 priority부터인지 기대 출력으로 비교하는 checker다.
- 무엇을 테스트하나
  - semaphore waiters 목록이 FIFO가 아니라 priority 순서로 관리되어야 함을 검증한다.
  - 메인 스레드가 `sema_up()`를 10번 호출할 때, 매번 가장 높은 priority waiter가 먼저 깨어나야 한다.

### `priority-condvar`

- 관련 파일
  - 본체: `pintos/tests/threads/priority-condvar.c`
  - 체커: `pintos/tests/threads/priority-condvar.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/priority-condvar.result`
- `priority-condvar.ck`는 무엇인가
  - `cond_signal()`이 깨우는 스레드 순서를 기대 출력으로 비교하는 checker다.
- 무엇을 테스트하나
  - condition variable에서 기다리는 waiter들 중 가장 높은 priority thread가 먼저 깨어나야 한다.
  - 즉 condvar 구현 내부에서 waiter semaphore들을 priority 기준으로 정렬했는지 확인한다.

---

## MLFQS 계열

MLFQS 테스트는 `pintos/tests/threads/mlfqs/Make.tests`에서 `.output` 타깃에 자동으로 `-mlfqs` 플래그를 붙인다.  
즉 이 테스트들은 advanced scheduler 모드에서 실행된다.

### `mlfqs-load-1`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-load-1.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-load-1.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-load-1.result`
- `mlfqs-load-1.ck`는 무엇인가
  - 출력 안에 최종 `PASS`가 존재하는지만 확인하는 checker다.
  - 실제 수치 범위 검사는 C 테스트 코드 내부에서 직접 수행한다.
- 무엇을 테스트하나
  - busy thread 1개만 있을 때 load average가 약 42초 전후에 0.5를 넘고, 그 후 10초 idle이면 다시 0.5 아래로 내려오는지 본다.
  - 즉 `load_avg` 계산식이 대체로 맞는지 보는 기본 테스트다.

### `mlfqs-load-60`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-load-60.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-load-60.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-load-60.result`
- `mlfqs-load-60.ck`는 무엇인가
  - 60개 부하 스레드를 기준으로 시간대별 `load average` 기대 곡선을 계산해서 실제 출력과 비교하는 checker다.
  - 허용 오차는 3.5다.
- 무엇을 테스트하나
  - 60개 thread가 일정 시간 sleeping/spinning/sleeping 하며 ready thread 수가 크게 변하는 상황에서 `load_avg`가 기대식대로 올라가고 내려가는지 검증한다.
  - `load_avg`의 장기 추세와 decay 동작을 보는 테스트다.

### `mlfqs-load-avg`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-load-avg.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-load-avg.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-load-avg.result`
- `mlfqs-load-avg.ck`는 무엇인가
  - 시간대별 `load average`를 이론값과 비교하는 checker다.
  - 허용 오차는 2.5다.
- 무엇을 테스트하나
  - 60개 스레드가 각기 다른 시점에 부하를 만들어 ready thread 수가 선형적으로 증가했다가 다시 감소하는 상황을 만든다.
  - 이때 `load_avg`가 그 패턴을 올바르게 따라가는지 본다.
  - 주석에도 나오듯, timer interrupt에서 너무 많은 일을 해 main thread가 충분히 sleep하지 못하면 왜곡될 수 있는 테스트다.

### `mlfqs-recent-1`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-recent-1.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-recent-1.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-recent-1.result`
- `mlfqs-recent-1.ck`는 무엇인가
  - 2초 간격으로 출력된 `recent_cpu` 값을 이론값과 비교하는 checker다.
  - 허용 오차는 2.5다.
- 무엇을 테스트하나
  - ready process가 사실상 하나뿐인 경우 `recent_cpu`와 `load_avg`가 시간에 따라 적절히 증가/감쇠하는지 확인한다.
  - 특히 초 단위 경계에서 `recent_cpu` 갱신 시점이 정확해야 통과하기 쉽다.

### `mlfqs-fair-2`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-fair.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-fair-2.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-fair-2.result`
- `mlfqs-fair-2.ck`는 무엇인가
  - nice 값이 모두 0인 스레드 2개가 받은 tick 수를 이론값과 비교하는 checker다.
  - 허용 오차는 50 tick이다.
- 무엇을 테스트하나
  - 동일한 nice 값의 스레드들은 CPU를 대체로 공평하게 나눠 가져야 함을 검증한다.
  - 즉 MLFQS fairness의 가장 단순한 형태다.

### `mlfqs-fair-20`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-fair.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-fair-20.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-fair-20.result`
- `mlfqs-fair-20.ck`는 무엇인가
  - nice 값이 모두 0인 20개 스레드의 tick 분배를 이론값과 비교하는 checker다.
  - 허용 오차는 20 tick이다.
- 무엇을 테스트하나
  - 많은 수의 동일 조건 스레드가 있을 때도 CPU 분배가 공정해야 함을 확인한다.
  - `mlfqs-fair-2`보다 더 빡빡한 fairness 검증이다.

### `mlfqs-nice-2`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-fair.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-nice-2.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-nice-2.result`
- `mlfqs-nice-2.ck`는 무엇인가
  - nice 값이 `0`과 `5`인 두 스레드가 받은 tick 수를 이론값과 비교하는 checker다.
  - 허용 오차는 50 tick이다.
- 무엇을 테스트하나
  - 더 높은 nice 값(덜 공격적인 스레드)이 실제로 CPU를 덜 받아야 함을 검증한다.
  - 즉 fairness뿐 아니라 nice 값이 priority 계산에 제대로 반영되는지 본다.

### `mlfqs-nice-10`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-fair.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-nice-10.ck`
  - 공용 체커 모듈: `pintos/tests/threads/mlfqs.pm`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-nice-10.result`
- `mlfqs-nice-10.ck`는 무엇인가
  - nice 값 `0`부터 `9`까지 10개 스레드의 tick 분배를 이론값과 비교하는 checker다.
  - 허용 오차는 25 tick이다.
- 무엇을 테스트하나
  - nice 값이 커질수록 CPU 점유량이 단계적으로 감소해야 함을 검증한다.
  - MLFQS의 priority 재계산 결과가 실제 스케줄링에 반영되는지 보는 테스트다.

### `mlfqs-block`

- 관련 파일
  - 본체: `pintos/tests/threads/mlfqs/mlfqs-block.c`
  - 체커: `pintos/tests/threads/mlfqs/mlfqs-block.ck`
- 실행
  - `cd pintos/threads/build`
  - `make tests/threads/mlfqs/mlfqs-block.result`
- `mlfqs-block.ck`는 무엇인가
  - block thread가 lock을 얻는 시점이 기대 출력과 같은지 검사하는 checker다.
- 무엇을 테스트하나
  - blocked 상태의 스레드도 `recent_cpu`와 priority가 적절히 갱신되어야 함을 확인한다.
  - block thread는 20초 spin 후 lock에서 10초 block되고, main thread는 25초 sleep 후 5초 spin 후 lock을 release한다.
  - 만약 blocked 동안 `recent_cpu` decay가 제대로 되면, lock release 직후 block thread가 곧바로 실행되어 lock을 얻어야 한다.

## 6. 빠르게 핵심만 요약하면

- `alarm-*`
  - `timer_sleep()`의 정확한 wake-up 시점, 동시 wake-up 처리, priority-aware wake-up, 0/음수 인자 처리를 본다.
- `priority-*`
  - priority scheduling, 즉시 preemption, semaphore/condvar의 priority waiter 처리, 그리고 donation의 단일/다중/중첩/연쇄/복원 로직을 본다.
- `mlfqs-*`
  - advanced scheduler에서 `load_avg`, `recent_cpu`, `nice`, priority 재계산, blocked thread 갱신, 공정한 CPU 분배를 본다.

## 7. 특히 `alarm-multiple.ck`를 한 줄로 정리하면

`alarm-multiple.ck`는 `alarm-multiple` 테스트의 "실행 파일"이 아니라 "출력 검증 스크립트"이며,  
`alarm-wait.c`의 `test_alarm_multiple()`가 만든 wake-up 로그를 읽어서  
"5개 스레드가 서로 다른 sleep 주기로 7번씩 깨어날 때 순서가 올바른가"를 검사한다.
