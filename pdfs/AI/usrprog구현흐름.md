[introduction.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/introduction.html) 기준으로 보면, Project 2는 “유저 프로그램이 어떻게 시작되고, 커널과 어떻게 대화하고, 잘못된 포인터나 파일 접근을 어떻게 견고하게 막을 것인가”를 배우는 단계예요. 그래서 학습도 기능별이 아니라 `실행 흐름` 기준으로 잡는 게 제일 잘 남습니다.

**추천 학습 흐름**

1. 먼저 [introduction.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/introduction.html)에서 `배경`, `소스 파일`, `가상 메모리 배치`, `사용자 메모리 접근`까지만 읽으세요. 여기서 Project 2의 본질이 “시스템 콜 구현”보다 “유저/커널 경계 관리”라는 걸 먼저 잡는 게 중요합니다.
2. 그 다음 문서가 강하게 권장한 [동기화 부록](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/appendix/synchronization.html)과 [가상 주소 부록](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/appendix/virtual_address.html)을 읽으세요. Project 2에서 제일 많이 터지는 버그가 `파일시스템 동기화`와 `잘못된 사용자 포인터`라서, 이 둘을 먼저 알아야 구현이 덜 흔들립니다.
3. 이제 문서보다 코드를 먼저 따라가세요. 시작점은 [init.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/threads/init.c:391)의 `process_wait(process_create_initd(task))`이고, 이어서 [process.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/process.c:42)의 `process_create_initd()` -> `initd()` -> `process_exec()` -> `load()` -> `setup_stack()` 흐름을 보세요. 이걸 이해하면 “유저 프로그램이 어디서 태어나는지”가 잡힙니다.
4. 그 다음 [argument_passing.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/argument_passing.html)을 읽고, 바로 [process.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/process.c:411)의 argument passing TODO를 연결하세요. 이 파트는 구현도 작고, `유저 스택`, `argc/argv`, `USER_STACK`, `레지스터 초기화`를 한 번에 익히기 좋아서 Project 2의 최고의 첫 진입점입니다.
5. 그 다음 [user_memory.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/user_memory.html)과 [exception.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/exception.c:120)의 `page_fault()`를 보세요. 여기서 먼저 “포인터를 역참조하기 전에 검증할지” 아니면 “fault를 유도하고 처리할지” 정책을 정해야 합니다. 처음 구현이면 `검증 후 접근` 쪽이 훨씬 덜 아픕니다.
6. 이제 [system_call.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/system_call.html)로 가서 [syscall.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/syscall.c:28)의 `syscall_init()`와 `syscall_handler()`를 연결해서 보세요. 여기서는 “시스템 콜 번호를 어디서 꺼내는지”, “인자가 어떤 레지스터로 오는지”, “반환값을 어디에 넣는지”만 먼저 이해하면 됩니다.
7. 그 다음에야 `wait/exit/fork/exec`와 파일 디스크립터 테이블을 보세요. 특히 문서도 권장하듯 [process.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/process.c:203)의 `process_wait()`를 비교적 초반에 설계하는 게 좋습니다. `init.c`가 초기 유저 프로세스를 기다리는 구조라서, 이걸 너무 뒤로 미루면 전체 흐름이 계속 찝찝하게 남아요.
8. 마지막에 [process_termination.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/process_termination.html)과 [deny_write.html](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pdfs/pintos-kaist-ko/project2/deny_write.html)을 붙이세요. 종료 메시지와 실행 파일 쓰기 거부는 핵심 구조를 다 만든 뒤 넣는 마무리 성격이 강합니다.

**코드로 볼 때의 권장 순서**

- [process.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/process.c:42)를 먼저 보세요. “유저 프로그램 시작”의 전체 그림이 여기 있습니다.
- [process.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/process.c:411)의 argument passing TODO를 먼저 잡으세요.
- [exception.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/exception.c:120)로 넘어가 사용자 포인터 실패 경로를 보세요.
- [syscall.c](/D:/jungleCamp/SW-AI-pintos_22.04_lab_docker/pintos/userprog/syscall.c:42)에서 시스템 콜 디스패처를 만드세요.
- 그 뒤에 `fd table`, `wait`, `exit`, `exec`, `fork` 순으로 확장하세요.
- `gdt.c`, `tss.c`, `syscall-entry.S`는 지금 당장은 깊게 안 봐도 됩니다. 문서도 수정이 필요 없다고 말하는 쪽입니다.

**구현 순서도 이렇게 가면 좋아요**

- `argument passing`
- `halt`, `exit`, `write(fd=1)`, `read(fd=0)` 같은 최소 시스템 콜
- `process_wait`와 종료 상태 전달
- `create/open/close/filesize/read/write/seek/tell/remove`
- 잘못된 포인터/경계값/NULL 처리
- `exec`
- `fork`
- `deny_write`

**테스트도 이 순서가 좋습니다**

- `args-single`, `args-none`, `args-multiple`, `args-dbl-space`, `args-many`
- `halt`, `exit`, `wait-simple`, `wait-twice`, `wait-bad-pid`, `wait-killed`
- `create-normal`, `open-normal`, `close-normal`, `read-normal`, `write-normal`
- `bad-read`, `bad-write`, `open-bad-ptr`, `create-bad-ptr`, `exec-bad-ptr`
- `exec-once`, `exec-arg`, `exec-missing`
- `fork-once`, `fork-multiple`, `fork-recursive`
- `rox-simple`, `rox-child`, `rox-multichild`

한 줄로 요약하면, Project 2는 `시스템 콜 목록`부터 외우는 것보다 `유저 프로그램 생성 -> 유저 스택 세팅 -> 유저 포인터 검증 -> 시스템 콜 진입 -> 프로세스 생명주기 -> 파일 디스크립터 관리` 순서로 이해하는 게 가장 잘 남습니다.

원하면 다음 답변에서 이 흐름을 기준으로 “첫 3일 학습/구현 플랜”처럼 더 실전적으로 쪼개드릴게요.
