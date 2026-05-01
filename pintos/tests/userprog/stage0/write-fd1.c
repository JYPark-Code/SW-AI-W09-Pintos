/* stage 0: write(fd=1) 검증.
 *
 * 의도:
 *   유저 모드에서 write(1, buf, n) 한번 호출했을 때 buf 내용이
 *   콘솔에 그대로 등장하는지를 확인한다. 즉 SYS_WRITE 라우팅 +
 *   putbuf 경로가 살아있는지 검증.
 *
 * tests/main.c를 쓰지 않는 이유:
 *   tests/main.c 는 argv[0]을 test_name으로 사용하는데, stage 0에서는
 *   argument passing이 아직 미구현이라 argv가 garbage이다.
 *   _start (lib/user/entry.c) → main(argc, argv) → exit(main_ret) 흐름이라
 *   argv를 건드리지 않으면 안전.
 *
 * 종료 흐름:
 *   main return → _start의 exit() 호출 → SYS_EXIT → 우리 핸들러의
 *   default 분기에서 thread_exit(). 부모(init)는 process_wait stub의
 *   sema_down에서 영구 블록 → kernel은 외부 timeout으로만 종료. */

#include <syscall.h>

int
main (void) {
	static const char marker[] = "STAGE0_WRITE_OK\n";
	/* sizeof(marker) - 1 = '\n' 포함 16자 (NUL 제외) */
	write (1, marker, sizeof marker - 1);
	return 0;
}
