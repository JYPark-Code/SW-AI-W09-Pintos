#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

static bool wakeup_priority(const struct list_elem *a,
							const struct list_elem *b,
							void *aux UNUSED);

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4		  /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread(thread_func *, void *aux);

static void idle(void *aux UNUSED);
static struct thread *next_thread_to_run(void);
static void init_thread(struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule(void);
static tid_t allocate_tid(void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *)(pg_round_down(rrsp())))

// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = {0, 0x00af9a000000ffff, 0x00cf92000000ffff};

static bool
wakeup_priority(const struct list_elem *a,
				const struct list_elem *b,
				void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, elem);
	const struct thread *tb = list_entry(b, struct thread, elem);

	return ta->priority > tb->priority;
}

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void)
{
	ASSERT(intr_get_level() == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	/**
	 * CPU의 Segment/Register 설정
	 * gdt : CPU가 사용할 GDT 테이블 본체
	 * desc_ptr : "그 GDT가 메모리 어디에 있고 크기가 얼마인지"를 알려주는 포맷
	 * lgdt() : 그 정보를 CPU의 GDTR 레지스터에 로드하는 함수
	 *
	 * gdt(global descriptor table) : 메모리를 어떻게 나눠서 쓸지 정의해놓은 표(테이블) - 세그먼트 기반 메모리 관리에서 사용하는 설명서
	 * -> 근데 최근 OS는 페이징을 써서 세그먼트를 거의 안 쓴다고 함.
	 * [ 전체 메모리 ] <= 옛날 x86 CPU 메모리 방식
	 * -> 코드 영역
	 * -> 데이터 영역
	 * -> 스택 영역
	 *
	 * 문제 (segment)
	 * 1. 아무 주소나 접근 가능하면 위험
	 * 2. 코드/데이터 구분 필요
	 * 3. 커널/유저 권한 분리 필요
	 * -> 생겨나게 된 개념이 세그먼트(segment) - gdt는 엄청 길다. (TODO 블로그에 작성해야 하는데..)
	 */
	struct desc_ptr gdt_ds = {
		.size = sizeof(gdt) - 1,
		.address = (uint64_t)gdt};
	/**
	 * static __inline void lgdt(const struct desc_ptr *dtr) {
	 *		__asm __volatile("lgdt %0" : : "m" (*dtr));
	 * }
	 *
	 * 사실상 lgdt, ltr, lidt, invlpg 같은 함수는 사실상 CPU 명령 1개짜리 래퍼 -> 함수 호출 오버헤드 발생하면 아까움
	 *
	 * Load Global Descriptor Table Register
	 * 즉, CPU 내부의 GDTR 레지스터에 GDT 정보 로드
	 *
	 * __attribute__ : GCC/Clang 계열 컴파일러 확장 문법 "이 함수/구조체/변수는 이런 성질이 있다"고 컴파일러에게 힌트나 졔약을 주는 기능
	 * __attribute__((packed)) : 구조체 패딩을 넣지 말라는 뜻
	 * __attritube__((always_inline)) : 가능한 한 반드시 인라인하라는 뜻
	 * __attribute__((unused)) : 안 써도 경고 내지 말라는 뜻
	 * __attribute__((noreturn)) : 이 함수는 반환하지 않는다는 뜻
	 *
	 * __inline : 함수 호출 오버헤드를 줄이기 위해 컴파일러가 함수 호출 대신 본문을 직접 펼쳐 넣도록 유도하는 키워드
	 * lgdt(&gdt_ds);를 진짜 함수 call로 만들기보다, 그 자리에 바로 어셈블리 한 줄을 넣게 하려는 의도
	 *
	 * __asm : C 코드 안에 직접 어셈블리어를 넣는 문법
	 * lgdt %0 : 실제 실행할 어셈블리 명령
	 * %0 : 아래 operand 목록의 0번째 피연산자
	 * "m" (*dtr) : *dtr를 메모리 operand로 쓰라는 뜻
	 * 메모리에 있는 desc_ptr 포맷 값을 읽어서 lgdt 명령의 피연산자로 써라 라는 뜻
	 *
	 * __volatile : "이 어셈블리 명령은 의미 있는 부작용이 있으니, 컴파일러가 멋대로 제거하거나 순서를 바꾸지 마라"라고 강제하는 것
	 */
	lgdt(&gdt_ds);

	/* Init the globla thread context */
	lock_init(&tid_lock);
	list_init(&ready_list);
	/**
	 * 지금 당장 free하면 위험한 죽은 스레드들을, 나중에 안전한 시점에 정리하려고 모아두는 리스트
	 * static struct list destruction_req;
	 *
	 * 스레드가 thread_exit() 할 때 그 스레드는 아직 자기 자신의 커널 스택 위에서 실행 중이기 때문
	 * 만약 종료하는 스레드가 자기 struct thread 페이지를 바로 free 해버리면,
	 * 지금 쓰고 있는 스택
	 * 지금 쓰고 있는 struct thread
	 * 를 스스로 치워버리는 셈이라 바로 망가짐
	 *
	 * 1. 종료 스레드가 thread_exit() 호출
	 * - 자기 상태를 THREAD_DYING으로 바꿈
	 * - 다른 스레드로 스케줄링
	 * 2. 설계 전환 직전 schedule()에서 이전 스레드가 죽는 중이면 destruction_req에 넣음
	 * 3. 다음에 어떤 스레드가 do_schedule()에 들어왔을 때, destruction_req 리스트를 돌면서 안전하게 palloc_free_page()
	 * 죽는 스레드 A
	 *	-> thread_exit()
	 *	-> status = THREAD_DYING
	 *	-> 다른 스레드로 전환
	 *	-> A는 destruction_req에 등록
	 *
	 * 다른 살아있는 스레드 B
	 *	-> do_schedule()
	 *	-> destruction_req에서 A를 꺼냄
	 *	-> A의 페이지를 free
	 */
	list_init(&destruction_req);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread();
	init_thread(initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
/**
 * idle thread는 지금 당장 실행할 스레드가 하나도 없을 때 CPU를 맡아주는 특수 스레드
 * ready queue가 비어있을 때를 위한 안전한 기본 대상이 하나 필요
 * 그래서 thread_start()가 미리 idle thread를 만들고, next_thread_to_run()을 호출해
 * idle_thread를 반환
 */
void thread_start(void)
{
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init(&idle_started, 0);
	thread_create("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
/**
 * 타이머 인터럽트가 올 때마다 스케줄링 관련 상태를 업데이트하고,
 * 필요하면 강제로 스레드를 변경하는 함수
 */
void thread_tick(void)
{
	struct thread *t = thread_current();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void)
{
	printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
		   idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char *name, int priority,
					thread_func *function, void *aux)
{
	struct thread *t;
	tid_t tid;

	ASSERT(function != NULL);

	/* Allocate thread. */
	/**
	 * 새 스레드 구조체와 스택 공간에 이전에 쓰이던 쓰레기 값이 남아 있으면 커널이
	 * 이상하게 동작할 수 있으니, 안전하게 0으로 밀고 시작
	 */
	t = palloc_get_page(PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread(t, name, priority);
	tid = t->tid = allocate_tid();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	/**
	 * trap frame (tf)
	 * CPU 레지스터 상태를 저장해두는 구조체
	 * (스레드가 실행되려면 CPU 레지스터에 값들이 세팅되어야 함)
	 *
	 * rip : 다음에 실행할 명령어 주소
	 * 이 스레드가 처음 스케줄링되면 kernel_thread 함수부터 실행하라
	 * rsp : 스택 포인터
	 * rdi : 첫번째 인자
	 * rsi : 두번째 인자
	 * cs : 코드 세그먼트
	 * ss : 스택 세그먼트
	 * eflags : CPU 플래그
	 *
	 * Pintos는 새 스레드를 만들 때 실제 CPU 레지스터를 바로 바꾸는 게 아니라
	 * t->tf 안에 미래의 CPU 상태를 미리 써둠
	 */
	t->tf.rip = (uintptr_t)kernel_thread;
	t->tf.R.rdi = (uint64_t)function;
	t->tf.R.rsi = (uint64_t)aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock(t);
	struct thread *curr = thread_current();
	if (curr->priority < t->priority)
	{
		thread_yield();
	}

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void)
{
	ASSERT(!intr_context());
	ASSERT(intr_get_level() == INTR_OFF);
	thread_current()->status = THREAD_BLOCKED;
	schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread *t)
{
	enum intr_level old_level;

	ASSERT(is_thread(t));

	old_level = intr_disable();
	ASSERT(t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, wakeup_priority, NULL);
	// list_push_front(&ready_list, &t->elem);
	t->status = THREAD_READY;
	intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name(void)
{
	return thread_current()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current(void)
{
	struct thread *t = running_thread();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT(is_thread(t));
	ASSERT(t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void)
{
	return thread_current()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void)
{
	ASSERT(!intr_context());

#ifdef USERPROG
	process_exit();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable();
	do_schedule(THREAD_DYING);
	NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void)
{
	struct thread *curr = thread_current();
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable();
	if (curr != idle_thread)
		// list_push_front(&ready_list, &curr->elem);
		list_insert_ordered(&ready_list, &curr->elem, wakeup_priority, NULL);
	do_schedule(THREAD_READY);
	intr_set_level(old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
/**
 * 여기에 메인 스레드는 priority 31인데,
 * priority-change
 * - main thread -> 32로 우선순위가 할당되어서 생성되고
 * - thread_set_priority에서 우선순위를 변경하기 전에 한번 실행되고 우선순위 변경
 * - 그런데 실행되기 전에는 우선순위 확인해서 메인스레드보다 커야 함 (그래야 가능)
 * priority-fifo
 * - 우선순위가 같을 경우, 먼저 들어간 애들이 먼저 나올 수 있도록 설계
 * priority-preempt
 * - 우선순위를 선점하는 로직
 */
void thread_set_priority(int new_priority)
{
	enum intr_level old_level;
	struct thread *curr;

	ASSERT(intr_get_level() == INTR_ON);

	old_level = intr_disable();
	curr = thread_current();
	curr->priority = new_priority;
	if (!list_empty(&ready_list))
	{
		struct thread *t = list_entry(list_front(&ready_list), struct thread, elem);

		if (curr->priority < t->priority)
		{
			thread_yield();
		}
	}

	// old_level = intr_disable();
	// curr = thread_current(); // main
	// if (main->priority > curr->priority)
	// {
	// 	thread_yield();
	// }
	// curr->priority = new_priority;
	// list_insert_ordered(&ready_list, &curr->elem, wakeup_priority, NULL);
	// thread_block();

	intr_set_level(old_level);
}

/* Returns the current thread's priority. */
int thread_get_priority(void)
{
	return thread_current()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED)
{
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void)
{
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle(void *idle_started_ UNUSED)
{
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current();
	sema_up(idle_started);

	for (;;)
	{
		/* Let someone else run. */
		intr_disable();
		thread_block();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread(thread_func *function, void *aux)
{
	ASSERT(function != NULL);

	intr_enable(); /* The scheduler runs with interrupts off. */
	function(aux); /* Execute the thread function. */
	thread_exit(); /* If function() returns, kill the thread. */
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread(struct thread *t, const char *name, int priority)
{
	ASSERT(t != NULL);
	ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT(name != NULL);

	memset(t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy(t->name, name, sizeof t->name);
	// 스택 포인터 초기화 (정확하게는 아직 모르겠다.)
	t->tf.rsp = (uint64_t)t + PGSIZE - sizeof(void *);
	t->original_priority = priority;
	t->priority = priority;
	t->wate_on_lock = NULL;
	// 디버깅용 : 스택 오버플로우 감지
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run(void)
{
	if (list_empty(&ready_list))
		return idle_thread;
	else
		return list_entry(list_pop_front(&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret(struct intr_frame *tf)
{
	__asm __volatile(
		"movq %0, %%rsp\n"
		"movq 0(%%rsp),%%r15\n"
		"movq 8(%%rsp),%%r14\n"
		"movq 16(%%rsp),%%r13\n"
		"movq 24(%%rsp),%%r12\n"
		"movq 32(%%rsp),%%r11\n"
		"movq 40(%%rsp),%%r10\n"
		"movq 48(%%rsp),%%r9\n"
		"movq 56(%%rsp),%%r8\n"
		"movq 64(%%rsp),%%rsi\n"
		"movq 72(%%rsp),%%rdi\n"
		"movq 80(%%rsp),%%rbp\n"
		"movq 88(%%rsp),%%rdx\n"
		"movq 96(%%rsp),%%rcx\n"
		"movq 104(%%rsp),%%rbx\n"
		"movq 112(%%rsp),%%rax\n"
		"addq $120,%%rsp\n"
		"movw 8(%%rsp),%%ds\n"
		"movw (%%rsp),%%es\n"
		"addq $32, %%rsp\n"
		"iretq"
		: : "g"((uint64_t)tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch(struct thread *th)
{
	uint64_t tf_cur = (uint64_t)&running_thread()->tf;
	uint64_t tf = (uint64_t)&th->tf;
	ASSERT(intr_get_level() == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile(
		/* Store registers that will be used. */
		"push %%rax\n"
		"push %%rbx\n"
		"push %%rcx\n"
		/* Fetch input once */
		"movq %0, %%rax\n"
		"movq %1, %%rcx\n"
		"movq %%r15, 0(%%rax)\n"
		"movq %%r14, 8(%%rax)\n"
		"movq %%r13, 16(%%rax)\n"
		"movq %%r12, 24(%%rax)\n"
		"movq %%r11, 32(%%rax)\n"
		"movq %%r10, 40(%%rax)\n"
		"movq %%r9, 48(%%rax)\n"
		"movq %%r8, 56(%%rax)\n"
		"movq %%rsi, 64(%%rax)\n"
		"movq %%rdi, 72(%%rax)\n"
		"movq %%rbp, 80(%%rax)\n"
		"movq %%rdx, 88(%%rax)\n"
		"pop %%rbx\n" // Saved rcx
		"movq %%rbx, 96(%%rax)\n"
		"pop %%rbx\n" // Saved rbx
		"movq %%rbx, 104(%%rax)\n"
		"pop %%rbx\n" // Saved rax
		"movq %%rbx, 112(%%rax)\n"
		"addq $120, %%rax\n"
		"movw %%es, (%%rax)\n"
		"movw %%ds, 8(%%rax)\n"
		"addq $32, %%rax\n"
		"call __next\n" // read the current rip.
		"__next:\n"
		"pop %%rbx\n"
		"addq $(out_iret -  __next), %%rbx\n"
		"movq %%rbx, 0(%%rax)\n" // rip
		"movw %%cs, 8(%%rax)\n"	 // cs
		"pushfq\n"
		"popq %%rbx\n"
		"mov %%rbx, 16(%%rax)\n" // eflags
		"mov %%rsp, 24(%%rax)\n" // rsp
		"movw %%ss, 32(%%rax)\n"
		"mov %%rcx, %%rdi\n"
		"call do_iret\n"
		"out_iret:\n"
		: : "g"(tf_cur), "g"(tf) : "memory");
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
/**
 * 현재 실행중인 스레드의 상태를 바꾸고, 실제 스케줄러 schedule()로 넘기는 함수
 * thread_exit()할 때, 스레드는 아직 자기 자신의 커널 스택 위에서 실행 중이기 때문에 destruction_req에서 정리할 스레드를 모아두고 정리
 */
static void
do_schedule(int status)
{
	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(thread_current()->status == THREAD_RUNNING);
	while (!list_empty(&destruction_req))
	{
		struct thread *victim =
			list_entry(list_pop_front(&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current()->status = status;
	schedule();
}

static void
schedule(void)
{
	struct thread *curr = running_thread();
	struct thread *next = next_thread_to_run();

	ASSERT(intr_get_level() == INTR_OFF);
	ASSERT(curr->status != THREAD_RUNNING);
	ASSERT(is_thread(next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate(next);
#endif

	if (curr != next)
	{
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread)
		{
			ASSERT(curr != next);
			list_push_back(&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch(next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid(void)
{
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire(&tid_lock);
	tid = next_tid++;
	lock_release(&tid_lock);

	return tid;
}
