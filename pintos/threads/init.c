#include "threads/init.h"
#include <console.h>
#include <debug.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/kbd.h"
#include "devices/input.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#endif
#include "tests/threads/tests.h"
#ifdef VM
#include "vm/vm.h"
#endif
#ifdef FILESYS
#include "devices/disk.h"
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/* Page-map-level-4 with kernel mappings only. */
uint64_t *base_pml4;

#ifdef FILESYS
/* -f: Format the file system? */
static bool format_filesys;
#endif

/* -q: Power off after kernel tasks complete? */
bool power_off_when_done;

bool thread_tests;

static void bss_init(void);
static void paging_init(uint64_t mem_end);

static char **read_command_line(void);
static char **parse_options(char **argv);
static void run_actions(char **argv);
static void usage(void);

static void print_stats(void);

int main(void) NO_RETURN;

/* Pintos main program. */
int main(void)
{
	uint64_t mem_end;
	char **argv;

	/* Clear BSS and get machine's RAM size. */
	/**
	 * bss : block started by symbol
	 * 초기값이 없는 전역 변수 / static 변수가 저장되는 영역
	 *
	 * 프로그램 실행 시
	 * [ Code (text) ] -> 실행코드
	 * [ Data ] -> 초기값 있는 전역 변수
	 * [ BSS ] -> 초기값 없는 전역 변수
	 * [ Heap ] -> 동적 할당 (malloc)
	 * [ Stack ] -> 함수 호출, 지역 변수
	 *
	 * BSS가 따로 있는 이유
	 * 1. 파일 크기 줄이기 위해
	 * 		int a; 같은 변수는 굳이 파일에 "0 값"을 저장할 필요 없음
	 * 		실행할 때 그냥 OS가 0으로 초기화하면 됨
	 */
	bss_init();

	/* Break command line into arguments and parse options. */
	/**
	 * 커맨드라인을 읽고 printf()로 현재 받은 인자들 출력
	 *
	 * -q -mlfqs run alarm-single
	 * argv = { "-q", "-mlfqs", "run", "alarm-single", NULL }
	 */
	argv = read_command_line();
	/**
	 * -h : 도움말 출력
	 * -q : 끝나면 power off
	 * -f : filesys 포맷
	 * -rs=SEED : 랜덤 시드 설정
	 * -mlfqs : MLFQS 스케줄러 사용
	 */
	argv = parse_options(argv);

	/* Initialize ourselves as a thread so we can use locks,
	   then enable console locking. */
	/**
	 * 인터럽트 꺼진 상태인지 확인 후
	 * 임시 커널 GDT를 설명하는 desc_ptr 생성
	 * lgdt로 CPU에 "이제 이 GDT를 써"락 로드
	 * 스레드 시스템용 tid락/ 레디 리스트, destruction_req 리스트 초기화
	 * 지금 이미 실행 중인 부트 흐름을 main 스레드로 변환
	 */
	thread_init();
	/**
	 * 스레드 시스템용 콘솔 락 초기화
	 */
	console_init();

	/* Initialize memory system. */
	/**
	 * 1. resolve_area_info
	 * base mem (1MB 미만) ext mem (1MB 이상) 메모리 영역을 나눠
	 * 2. populate_pools
	 * base mem과 ext mem 둘 다 더해 전체 사이즈에서 커널 풀과 유저 풀을 2등분 하고
	 * 페이징 처리 + 비트맵 작업
	 */
	mem_end = palloc_init();
	/**
	 * 블록 크기들 16 32 64 ... 를 나눠
	 * malloc(20) 같은 요청이 오면 가장 가까운 크기 클래스에서 하나를 주는 식으로 처리하기 위해 나누는 작업
	 */
	malloc_init();

	/**
	 * 페이지 테이블을 만들고, 커널 가상주소가 물리메모리를 어떻게 매핑할지 설정한 뒤, CPU가 그 페이징 설정을 실제로 쓰게 만드는 초기화
	 */
	paging_init(mem_end);

#ifdef USERPROG
	tss_init();
	gdt_init();
#endif

	/* Initialize interrupt handlers. */
	/**
	 * CPU가 인터럽트/예외가 발생했을 때 어디로 들어가야 하는지 IDT를 만들고, PIC를 그 규칙에 맞게 재설정하는 함수
	 */
	intr_init();
	/**
	 * 타이머를 설정하고
	 * 외부 하드웨어 인터럽트 0x20이 들어오면 timer_interrupt()를 발생
	 */
	timer_init();
	/**
	 * 외부 하드웨어 인터럽트 0x21이 들어오면 keyboard_interrupt()를 발생
	 */
	kbd_init();
	/**
	 * 키보드/시리얼 입력을 담아둘 공용 입력 버퍼를 초기화하는 함수
	 * 실제로는 intq_init(&buffer) 한 줄만 호출하고, 이 buffer는 input.c의 struct intq
	 */
	input_init();
#ifdef USERPROG
	exception_init();
	syscall_init();
#endif
	/* Start thread scheduler and enable interrupts. */
	/**
	 * idle_thread를 만드는 것 뿐
	 */
	thread_start();
	/**
	 * 시리얼 포트(COM1, UART)에서 입출력 이벤트가 생겼을 때 오는 하드웨어 인터럽트
	 * - 시리얼로 들어온 입력이 있으면 RBR_REG에서 읽어 input_putc()로 input buffer에 넣음
	 * - 출력할 문자가 대기 중이면 txq에서 꺼내 THR_REG로 내보냄
	 * 모르겠다..
	 * 시리얼 콘솔을 본격적으로 인터럽트 기반으로 쓰기 시작하는 초기화, Pintos가 출력/테스트에서 시리얼을
	 * 많이 쓰기 때문에 기본적으로 꼭 켜두는 것
	 * 즉, IRQ4 -> 벡터 0x24에 등록 후 busy waiting 대시 큐 기반 인터럽트 방식으로 바꾸는 것
	 */
	serial_init_queue();
	/**
	 * CPU가 1 tick 동안 대략 몇 번의 빈 루프를 돌 수 있는지를 측정해서
	 * loops_per_ticks 를 구하는 함수
	 * 1tick당 대략 10ms인데, 내 머신에서 10ms보다 살짝 덜 걸리는 busy-wait 루프 횟수를
	 * 부팅 시 한 번 보정하는 것
	 *
	 * timer_sleep() 처럼 1 tick이상 기다릴 때는 스레드를 block 시키면 되지만,
	 * 1 tick보다 짧은 아주 짧은 지연은 그렇게 못 맞춘다.
	 * 그래서 read_time_sleep()은
	 * 충분히 길면 timer_sleep() 사용
	 * 너무 짧으면 busy_wait() 사용
	 *
	 * 이때 busy_wait()에 몇 번 루프를 줘야 하는지 기준이 바로 loops_per_tick
	 * 실제로 timer.c에서 그 값을 써서 timer_msleep, timer_usleep, timer_nsleep의 짧은 지연을 구함
	 * 1. 먼저 2의 거듭제곱으로 대충 큰 값을 찾음
	 * too_many_loops()로 이 루프 수가 1 tick을 넘는가?를 검사함
	 * 그 다음 상위 비트 아래 몇 비트를 더 켜 보면서 값을 조금 더 정밀하게 맞춤
	 */
	timer_calibrate();

#ifdef FILESYS
	/* Initialize file system. */
	disk_init();
	filesys_init(format_filesys);
#endif

#ifdef VM
	vm_init();
#endif

	printf("Boot complete.\n");

	/* Run actions specified on kernel command line. */
	run_actions(argv);

	/* Finish up. */
	if (power_off_when_done)
		power_off();
	thread_exit();
}

/* Clear BSS */
static void
bss_init(void)
{
	/* The "BSS" is a segment that should be initialized to zeros.
	   It isn't actually stored on disk or zeroed by the kernel
	   loader, so we have to zero it ourselves.

	   The start and end of the BSS segment is recorded by the
	   linker as _start_bss and _end_bss.  See kernel.lds. */
	extern char _start_bss, _end_bss;
	memset(&_start_bss, 0, &_end_bss - &_start_bss);
}

/* Populates the page table with the kernel virtual mapping,
 * and then sets up the CPU to use the new page directory.
 * Points base_pml4 to the pml4 it creates. */
static void
paging_init(uint64_t mem_end)
{
	uint64_t *pml4, *pte;
	int perm;
	/**
	 * pml4는 페이지 테이블의 가장 앞부분을 설정하는 것
	 * 이 때 0으로 다 초기화 하는 이유는 보안적인 문제
	 * 다른 프로세스가 사용 후 free를 한 곳이기 때문에 이전 데이터가 남아있을 수 있음
	 * 그 데이터를 초기화 하기 위해
	 * pml4 : 새로 쓸 최상위 페이지 테이블 한 페이지를 받음
	 * base_pml4 : 그 주소를 전역으로도 저장
	 * PAL_ZERO : 그 4KB 페이지를 전부 0으로 채움
	 * PAL_ASSERT : 할당 실패하면 그냥 NULL 반환하지 말고 커널을 panic 시킴
	 */
	pml4 = base_pml4 = palloc_get_page(PAL_ASSERT | PAL_ZERO);

	extern char start, _end_kernel_text;
	// Maps physical address [0 ~ mem_end] to
	//   [LOADER_KERN_BASE ~ LOADER_KERN_BASE + mem_end].
	for (uint64_t pa = 0; pa < mem_end; pa += PGSIZE)
	{
		uint64_t va = (uint64_t)ptov(pa);

		/**
		 * 페이지 테이블 엔트리의 권한 비트 2개를 켠다는 뜻
		 * 0x1 : present
		 * 0x2 : read/write
		 */
		perm = PTE_P | PTE_W;
		if ((uint64_t)&start <= va && va < (uint64_t)&_end_kernel_text)
			perm &= ~PTE_W;
		/**
		 * pml4 (Page Map Level 4) : x86-64에서 쓰는 최상위 페이지 테이블
		 * 즉, 운영체제가 가상주소를 물리주소로 바꿀 때, 제일 먼저 보는 1단계 테이블
		 *
		 * [ PML4 index ][ PDPT index ][ PD index ][ PT index ][ offset ]
		 * 즉, 주소 변환 순서
		 * 1. PML4
		 * 2. PDPT
		 * 3. PD
		 * 4. PT
		 * 5. 실제 물리 페이지
		 *
		 * pml4e_walk : PML4 엔트리를 따라 내려가면서 필요한 하위 테이블을 찾거나 생성
		 */
		if ((pte = pml4e_walk(pml4, va, 1)) != NULL)
			// pa : 상위 비트 : 물리 페이지 주소 pa
			// perm : 하위 비트 : 권한 플래그 perm
			*pte = pa | perm;
	}

	// reload cr3
	pml4_activate(0);
}

/* Breaks the kernel command line into words and returns them as
   an argv-like array. */
/**
 *
 */
static char **
read_command_line(void)
{
	// 65 크기로 둔 이유 : a/0b/0 처럼 한 글자당 2바이트씩 필요로 하기 때문 그리고 마지막 + 1 은 마지막에 NULL이 무조건 들어가서
	static char *argv[LOADER_ARGS_LEN / 2 + 1];
	char *p, *end;
	int argc;
	int i;

	// 인자 개수
	argc = *(uint32_t *)ptov(LOADER_ARG_CNT);
	// 현재 커맨드라인 문자열들이 저장된 버퍼의 시작 주소
	p = ptov(LOADER_ARGS);
	end = p + LOADER_ARGS_LEN;
	for (i = 0; i < argc; i++)
	{
		if (p >= end)
			PANIC("command line arguments overflow");

		argv[i] = p;
		p += strnlen(p, end - p) + 1;
	}
	argv[argc] = NULL;

	/* Print kernel command line. */
	printf("Kernel command line:");
	for (i = 0; i < argc; i++)
		if (strchr(argv[i], ' ') == NULL)
			printf(" %s", argv[i]);
		else
			printf(" '%s'", argv[i]);
	printf("\n");

	return argv;
}

/* Parses options in ARGV[]
   and returns the first non-option argument. */
static char **
parse_options(char **argv)
{
	for (; *argv != NULL && **argv == '-'; argv++)
	{
		char *save_ptr;
		char *name = strtok_r(*argv, "=", &save_ptr);
		char *value = strtok_r(NULL, "", &save_ptr);

		if (!strcmp(name, "-h"))
			// 도움말 출력
			usage();
		else if (!strcmp(name, "-q"))
			power_off_when_done = true;
#ifdef FILESYS
		else if (!strcmp(name, "-f"))
			format_filesys = true;
#endif
		else if (!strcmp(name, "-rs"))
			random_init(atoi(value));
		else if (!strcmp(name, "-mlfqs"))
			thread_mlfqs = true;
#ifdef USERPROG
		else if (!strcmp(name, "-ul"))
			user_page_limit = atoi(value);
		else if (!strcmp(name, "-threads-tests"))
			thread_tests = true;
#endif
		else
			PANIC("unknown option `%s' (use -h for help)", name);
	}

	return argv;
}

/* Runs the task specified in ARGV[1]. */
static void
run_task(char **argv)
{
	const char *task = argv[1];

	printf("Executing '%s':\n", task);
#ifdef USERPROG
	if (thread_tests)
	{
		run_test(task);
	}
	else
	{
		process_wait(process_create_initd(task));
	}
#else
	run_test(task);
#endif
	printf("Execution of '%s' complete.\n", task);
}

/* Executes all of the actions specified in ARGV[]
   up to the null pointer sentinel. */
static void
run_actions(char **argv)
{
	/* An action. */
	struct action
	{
		char *name;					   /* Action name. */
		int argc;					   /* # of args, including action name. */
		void (*function)(char **argv); /* Function to execute action. */
	};

	/* Table of supported actions. */
	static const struct action actions[] = {
		{"run", 2, run_task},
#ifdef FILESYS
		{"ls", 1, fsutil_ls},
		{"cat", 2, fsutil_cat},
		{"rm", 2, fsutil_rm},
		{"put", 2, fsutil_put},
		{"get", 2, fsutil_get},
#endif
		{NULL, 0, NULL},
	};

	while (*argv != NULL)
	{
		const struct action *a;
		int i;

		/* Find action name. */
		for (a = actions;; a++)
			if (a->name == NULL)
				PANIC("unknown action `%s' (use -h for help)", *argv);
			else if (!strcmp(*argv, a->name))
				break;

		/* Check for required arguments. */
		for (i = 1; i < a->argc; i++)
			if (argv[i] == NULL)
				PANIC("action `%s' requires %d argument(s)", *argv, a->argc - 1);

		/* Invoke action and advance. */
		a->function(argv);
		argv += a->argc;
	}
}

/* Prints a kernel command line help message and powers off the
   machine. */
static void
usage(void)
{
	printf("\nCommand line syntax: [OPTION...] [ACTION...]\n"
		   "Options must precede actions.\n"
		   "Actions are executed in the order specified.\n"
		   "\nAvailable actions:\n"
#ifdef USERPROG
		   "  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
		   "  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
		   "  ls                 List files in the root directory.\n"
		   "  cat FILE           Print FILE to the console.\n"
		   "  rm FILE            Delete FILE.\n"
		   "Use these actions indirectly via `pintos' -g and -p options:\n"
		   "  put FILE           Put FILE into file system from scratch disk.\n"
		   "  get FILE           Get FILE from file system into scratch disk.\n"
#endif
		   "\nOptions:\n"
		   "  -h                 Print this help message and power off.\n"
		   "  -q                 Power off VM after actions or on panic.\n"
		   "  -f                 Format file system disk during startup.\n"
		   "  -rs=SEED           Set random number seed to SEED.\n"
		   "  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
		   "  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
	);
	power_off();
}

/* Powers down the machine we're running on,
   as long as we're running on Bochs or QEMU. */
void power_off(void)
{
#ifdef FILESYS
	filesys_done();
#endif

	print_stats();

	printf("Powering off...\n");
	outw(0x604, 0x2000); /* Poweroff command for qemu */
	for (;;)
		;
}

/* Print statistics about Pintos execution. */
static void
print_stats(void)
{
	timer_print_stats();
	thread_print_stats();
#ifdef FILESYS
	disk_print_stats();
#endif
	console_print_stats();
	kbd_print_stats();
#ifdef USERPROG
	exception_print_stats();
#endif
}
