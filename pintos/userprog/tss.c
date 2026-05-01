#include "userprog/tss.h"
#include <debug.h>
#include <stddef.h>
#include "userprog/gdt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "intrinsic.h"

/* The Task-State Segment (TSS).
 *
 *  Instances of the TSS, an x86-64 specific structure, are used to
 *  define "tasks", a form of support for multitasking built right
 *  into the processor.  However, for various reasons including
 *  portability, speed, and flexibility, most x86-64 OSes almost
 *  completely ignore the TSS.  We are no exception.
 *
 *  Unfortunately, there is one thing that can only be done using
 *  a TSS: stack switching for interrupts that occur in user mode.
 *  When an interrupt occurs in user mode (ring 3), the processor
 *  consults the rsp0 members of the current TSS to determine the
 *  stack to use for handling the interrupt.  Thus, we must create
 *  a TSS and initialize at least these fields, and this is
 *  precisely what this file does.
 *
 *  When an interrupt is handled by an interrupt or trap gate
 *  (which applies to all interrupts we handle), an x86-64 processor
 *  works like this:
 *
 *    - If the code interrupted by the interrupt is in the same
 *      ring as the interrupt handler, then no stack switch takes
 *      place.  This is the case for interrupts that happen when
 *      we're running in the kernel.  The contents of the TSS are
 *      irrelevant for this case.
 *
 *    - If the interrupted code is in a different ring from the
 *      handler, then the processor switches to the stack
 *      specified in the TSS for the new ring.  This is the case
 *      for interrupts that happen when we're in user space.  It's
 *      important that we switch to a stack that's not already in
 *      use, to avoid corruption.  Because we're running in user
 *      space, we know that the current process's kernel stack is
 *      not in use, so we can always use that.  Thus, when the
 *      scheduler switches threads, it also changes the TSS's
 *      stack pointer to point to the new thread's kernel stack.
 *      (The call is in schedule in thread.c.) */

/* Kernel TSS. */
struct task_state *tss;

/* Initializes the kernel TSS. */
/**
 * TSS (Task State Segment)
 * CPU가 "인터럽트나 권한 변경 시 사용할 커널 스택 정보"를 저장해두는 구조체
 * TSS = CPU가 사용자 모드 -> 커널 모드로 넘어올 때 사용하 스택 위치를 알려주는 구조
 *
 * - 핀토스는 유저 프로그램을 실행할 때, 권한이 다른 두 모드를 사용
 * User Mode (Ring 3)
 * Kernel Mode (Ring 0)
 *
 * - 문제
 * 유저 프로그램에서 인터럽트(ex. system call, page fault)가 발생하면
 * 커널 코드로 넘어가야 하는데, 기존 유저 스택을 그대로 쓰면 위험(보안 + 안정성)
 * 그래서 CPU는 TSS에 저장된 커널 스택으로 스택을 교체 -> 그 위에서 인터럽트 핸들러 실행
 *
 * rsp0 (esp0) : 커널 모드에서 사용할 스택 포인터
 */
void tss_init(void)
{
	/* Our TSS is never used in a call gate or task gate, so only a
	 * few fields of it are ever referenced, and those are the only
	 * ones we initialize. */
	tss = palloc_get_page(PAL_ASSERT | PAL_ZERO);
	tss_update(thread_current());
}

/* Returns the kernel TSS. */
struct task_state *
tss_get(void)
{
	ASSERT(tss != NULL);
	return tss;
}

/* Sets the ring 0 stack pointer in the TSS to point to the end
 * of the thread stack. */
void tss_update(struct thread *next)
{
	ASSERT(tss != NULL);
	tss->rsp0 = (uint64_t)next + PGSIZE;
}
