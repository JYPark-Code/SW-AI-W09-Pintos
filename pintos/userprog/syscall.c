#include "filesys/file.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "userprog/gdt.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "threads/mmu.h"
#include "threads/synch.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static struct lock filesys_lock;

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
	
	lock_init (&filesys_lock);
}

static void
validate_user_addr (const void *uaddr) {
	/* 유저가 넘긴 주소가 커널 주소이거나 실제 매핑되지 않은 주소면 종료한다. */
	if (uaddr == NULL || !is_user_vaddr(uaddr) || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
		thread_current()->exit_status = -1;
		thread_exit();
	}
}

static void
validate_user_string (const char *str) {
	/* 문자열 인자는 첫 주소만으로는 부족하므로 '\0'까지 모든 글자 주소를 검사한다. */
	while (true) {
		validate_user_addr (str);
		if (*str == '\0')
			return;
		str++;
	}
}

void
syscall_handler (struct intr_frame *f UNUSED) {
	uint64_t sysno = f->R.rax;

	switch (sysno) {
		case SYS_HALT:
			power_off ();
			NOT_REACHED ();

		case SYS_EXIT: {
			int status = (int) f->R.rdi;
			thread_current ()->exit_status = status;
			thread_exit ();
			NOT_REACHED ();
		}

		case SYS_WRITE: {
			int            fd     = (int) f->R.rdi;
			const void    *buffer = (const void *) f->R.rsi;
			unsigned       size   = (unsigned) f->R.rdx;

			validate_user_addr(buffer);

			if (fd == 1) {
				putbuf(buffer, size);
				f->R.rax = size;
			} else {
				f->R.rax = (uint64_t) -1;
			}
			break;
		}
		case SYS_CREATE: {
			char* file = (char *)f->R.rdi;
			unsigned initial_size = (unsigned) f->R.rsi;

			validate_user_addr(file);

			lock_acquire (&filesys_lock);
			bool success = filesys_create (file, initial_size);
			lock_release (&filesys_lock);
		
			f->R.rax = success;
			break;
		}

		case SYS_OPEN: {
			const char *file = (const char *) f->R.rdi;
		
			/* open()의 file 인자는 문자열 포인터라서 문자열 전체를 검증한다. */
			validate_user_string (file);
		
			lock_acquire (&filesys_lock);
			struct file *opened_file = filesys_open (file);
			lock_release (&filesys_lock);
		
			if (opened_file == NULL) {
				/* 없는 파일 또는 열기 실패는 fd 대신 -1을 반환한다. */
				f->R.rax = -1;
				break;
			}
		
			struct thread *cur = thread_current ();
			int fd = cur->next_fd;
		
			if (fd >= 128) {
				/* fd 테이블이 가득 찼다면 방금 연 파일을 닫고 실패 처리한다. */
				file_close (opened_file);
				f->R.rax = -1;
				break;
			}
		
			/* 커널 내부 파일 포인터는 fd 테이블에 숨기고, 유저에게는 fd 번호만 반환한다. */
			cur->fd_table[fd] = opened_file;
			cur->next_fd++;
		
			f->R.rax = fd;
			break;
		}		

		default:
			printf("unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}
