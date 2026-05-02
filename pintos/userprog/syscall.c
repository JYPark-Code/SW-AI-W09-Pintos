#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/vaddr.h"      /* is_user_vaddr / KERN_BASE 사용 */
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* stage 0 최소 메모리 검증.
 *   - NULL 즉시 거절
 *   - is_user_vaddr(p)는 ((uint64_t)p < KERN_BASE)와 동치 →
 *     커널 영역 포인터를 유저가 넘긴 경우 차단
 * 언맵된 페이지/페이지 경계 걸침 등 정식 검증은 이후 단계에서 추가. */
static void
validate_user_addr (const void *uaddr) {
	if (uaddr == NULL || !is_user_vaddr(uaddr))
		thread_exit();
}

/* x86-64 syscall ABI (KAIST 기준)
 *   rax       = syscall 번호 / 반환값
 *   rdi, rsi, rdx, r10, r8, r9 = 인자 1~6
 * intr_frame->R 의 동명 필드에 그대로 들어 있다. */
void
syscall_handler (struct intr_frame *f UNUSED) {
	uint64_t sysno = f->R.rax;
	switch (sysno) {
		case SYS_WRITE: {
			int            fd     = (int) f->R.rdi;
			const void    *buffer = (const void *) f->R.rsi;
			unsigned       size   = (unsigned) f->R.rdx;

			/* stage 0: KERN_BASE 체크만 (요구사항대로 최소화) */
			validate_user_addr(buffer);

			if (fd == 1) {
				/* fd=1 (stdout): putbuf로 콘솔 출력.
				 *   - putbuf는 내부 console lock으로 한 호출분을 보호하여
				 *     다른 출력과 섞이지 않는다.
				 *   - 유저 모드 printf()의 최종 종착지가 바로 이 분기.
				 *     이게 없으면 유저 프로그램의 모든 출력이 사라진다. */
				putbuf(buffer, size);
				f->R.rax = size;     /* 쓴 바이트 수 반환 (stdout은 size 그대로) */
			} else {
				/* fd != 1 은 stage 0 범위 밖.
				 * 호출은 허용하되 의미 있는 동작 없이 -1 반환. */
				f->R.rax = (uint64_t) -1;
			}
			break;
		}
		default:
			/* 아직 라우팅 안 된 시스템 콜.
			 * 디버깅 가시성을 위해 한 줄 찍고 종료. */
			printf("[stage0] unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}
