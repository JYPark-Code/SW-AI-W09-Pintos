#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/init.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "filesys/filesys.h"
#include "threads/mmu.h"


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

static void
validate_user_addr (const void *uaddr) {
    if (uaddr == NULL || !is_user_vaddr(uaddr) || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
        thread_current ()->exit_status = -1;
        thread_exit ();
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
			const char *file = (const char *) f->R.rdi;
			unsigned initial_size = (unsigned) f->R.rsi;

			validate_user_addr (file);
			f->R.rax = filesys_create (file, initial_size);
			break;
		}
 
		default:
			printf("unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}
