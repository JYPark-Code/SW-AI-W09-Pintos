## 1 슬라이드 — 표지

"안녕하세요, 박지용입니다. 저는 Pintos Project 1에서 동기화 파트가 기억에 많이 남았고, 그 중에서 Semaphore와 Condition Variable의 우선순위 정렬 문제를 발표하겠습니다.
제목에 '참조 깊이'라는 단어가 있는데, 오늘 발표의 핵심 메시지가 여기 담겨 있습니다. 두 동기화 객체가 왜 다른 방식으로 수정되어야 하는지, 그 차이가 바로 참조 깊이에서 비롯됩니다."


## 2 슬라이드 — Semaphore: FIFO의 한계

"먼저 Semaphore입니다.
기본 Pintos의 sema_up()은 waiters 리스트에서 list_pop_front()를 호출합니다. waiters는 도착 순서, 즉 FIFO로만 정렬되어 있기 때문에, priority 63이 나중에 도착했다면 priority 1보다 나중에 깨어납니다. priority scheduler를 아무리 잘 만들어도 sema에서 막히면 의미가 없어집니다.
수정 방법은 list_pop_front() 대신 list_min()과 cmp_priority를 조합해서 waiters 중 가장 높은 priority를 가진 스레드를 찾아 thread_unblock()하는 겁니다.
여기서 한 가지 함정이 있었는데, 슬라이드 하단 주의사항에도 적어뒀습니다. cmp_priority는 내림차순 less 함수입니다. 그래서 list_max를 쓰면 의미가 뒤집혀서 가장 낮은 priority가 나옵니다. list_min을 써야 가장 높은 priority가 나옵니다. 처음에 이걸 반대로 써서 한참 헤맸던 부분입니다."


## 3 슬라이드 — 함정: Interrupt Context와 Kernel Panic

"sema_up()을 수정하면서 한 가지 더 고려해야 할 게 있었습니다.
sema_up()은 인터럽트 핸들러 내부에서도 빈번하게 호출됩니다. 스레드를 깨운 직후 바로 thread_yield()를 호출해서 CPU를 양보하려고 했는데, 인터럽트 컨텍스트에서 이걸 강제로 호출하면 시스템 패닉이 발생합니다.
그래서 슬라이드 오른쪽 흐름도처럼 두 경로로 나눴습니다. A 경로처럼 무조건 thread_yield()를 호출하면 Kernel Panic입니다. B 경로처럼 intr_context()로 지금이 인터럽트 상황인지 먼저 확인하고, 인터럽트가 아닐 때만 yield가 동작하도록 방어 로직을 추가했습니다."


# 4 슬라이드 — Condvar: 간접 참조의 장벽 돌파

"다음은 Condition Variable입니다.
sema_up()을 수정하고 나서 priority-sema 테스트는 PASS됐는데, priority-condvar 테스트는 여전히 FAIL이었습니다. 이유가 뭔지 찾아보니, cond_signal()이 cond->waiters에서 꺼내는 대상이 스레드가 아니라 semaphore_elem이었기 때문입니다.
슬라이드 오른쪽 그림을 보시면, 레이어가 3단으로 쌓여 있습니다. 각 레이어가 semaphore_elem 하나씩이고, 그 안에 thread가 1개씩 들어 있습니다. cond_wait()을 호출하는 스레드가 자기 전용 세마포어를 직접 만들고 그 안에서 잠들기 때문입니다.
그래서 priority에 접근하려면 semaphore_elem → semaphore.waiters → thread 순으로 두 단계를 거쳐야 합니다. cmp_priority를 그대로 쓸 수 없고, 이 2단계 경로를 탐색하는 전용 비교 함수 cmp_sem_priority를 새로 설계했습니다. 그리고 cond_signal() 안에서 list_min(cmp_sem_priority)을 호출해서, 돋보기처럼 가장 높은 priority인 63을 정확하게 찾아내도록 수정했습니다."


## 5 슬라이드 — 핵심 요약

"정리하면 이렇습니다.
Semaphore는 waiters에 스레드가 직접 있습니다. 1 Depth, 직접 참조입니다. list_min(cmp_priority) 하나로 해결됩니다.
Condition Variable은 waiters에 semaphore_elem이 있고, 그 안에 스레드가 있습니다. 2 Depth, 간접 참조입니다. list_min(cmp_sem_priority)이라는 별도 비교 함수가 필요합니다.
결국 동기화 객체가 대기열을 관리하는 내부 메모리 구조, 즉 참조 깊이를 완벽히 이해해야 정확한 우선순위 스케줄링 로직을 설계할 수 있다는 게 오늘 발표의 핵심입니다. 이상입니다."