/* stage 0: process_wait 영구 블록 검증.
 *
 * 의도:
 *   자식이 마커 출력 후 즉시 종료. 부모(init)가 process_wait stub의
 *   sema_down 에서 막혀 있으면 kernel은 power_off에 도달하지 않아야 한다.
 *   .ck에서 "마커 등장" + "Powering off 부재"로 두 신호 모두 검사.
 *
 * 주의:
 *   write-fd1과 본체는 거의 동일하다(둘 다 write 후 exit). 차이는 검사
 *   관점이다 — 이 테스트는 출력 자체가 아니라 kernel이 살아남는지를 본다. */

#include <syscall.h>

int
main (void) {
	static const char marker[] = "STAGE0_WAIT_CHILD_DONE\n";
	write (1, marker, sizeof marker - 1);
	return 0;
}
