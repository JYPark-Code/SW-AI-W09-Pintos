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
#include "filesys/file.h"
#include "devices/input.h"

struct lock filesys_lock;

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
	lock_init(&filesys_lock);
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
	if (uaddr == NULL || !is_user_vaddr(uaddr)
	    || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
		thread_current()->exit_status = -1;
		thread_exit();
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

		case SYS_CREATE: {
			const char *filename = (const char *) f->R.rdi;
			unsigned    size     = (unsigned)     f->R.rsi;

			validate_user_addr(filename);

			lock_acquire(&filesys_lock);
			bool success = filesys_create(filename, size);
			lock_release(&filesys_lock);

			f->R.rax = success;
			break;
		}

		case SYS_OPEN: {
			const char *filename = (const char *) f->R.rdi;

			validate_user_addr(filename);

			lock_acquire(&filesys_lock);
			struct file *file = filesys_open(filename);

			if (file == NULL) {
				f->R.rax = (uint64_t) -1;
				lock_release(&filesys_lock);
				break;
			}

			thread_current()->fd_table[thread_current()->fd_next] = file;
			f->R.rax = thread_current()->fd_next;
			thread_current()->fd_next++;
			lock_release(&filesys_lock);
			break;
		}

		case SYS_CLOSE: {
			uint64_t fd = (uint64_t) f->R.rdi;

			lock_acquire(&filesys_lock);

			if (fd < 2 || fd >= 128) {
				lock_release(&filesys_lock);
				break;
			}

			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL) {
				lock_release(&filesys_lock);
				break;
			}

			file_close(file);
			thread_current()->fd_table[fd] = NULL;
			lock_release(&filesys_lock);
			break;
		}

		case SYS_READ: {
			int      fd     = (int)      f->R.rdi;
			void    *buffer = (void *)   f->R.rsi;
			unsigned size   = (unsigned) f->R.rdx;

			validate_user_addr(buffer);
			lock_acquire(&filesys_lock);

			if (fd < 0 || fd >= 128) {
				f->R.rax = (uint64_t) -1;
			} else if (fd == 0) {
				for (unsigned i = 0; i < size; i++)
					((char *) buffer)[i] = input_getc();
				f->R.rax = size;
			} else if (fd == 1) {
				f->R.rax = (uint64_t) -1;
			} else {
				struct file *file = thread_current()->fd_table[fd];
				f->R.rax = (file == NULL) ? (uint64_t) -1
				                          : (uint64_t) file_read(file, buffer, size);
			}
			lock_release(&filesys_lock);
			break;
		}

		case SYS_FILESIZE: {
			int fd = (int) f->R.rdi;

			lock_acquire(&filesys_lock);
			if (fd < 2 || fd >= 128) {
				f->R.rax = (uint64_t) -1;
				lock_release(&filesys_lock);
				break;
			}
			struct file *file = thread_current()->fd_table[fd];
			if (file == NULL) {
				f->R.rax = (uint64_t) -1;
				lock_release(&filesys_lock);
				break;
			}
			f->R.rax = file_length(file);
			lock_release(&filesys_lock);
			break;
		}

		case SYS_WRITE: {
			int            fd     = (int)          f->R.rdi;
			const void    *buffer = (const void *) f->R.rsi;
			unsigned       size   = (unsigned)     f->R.rdx;

			validate_user_addr(buffer);
			lock_acquire(&filesys_lock);

			if (fd < 0 || fd >= 128) {
				f->R.rax = (uint64_t) -1;
			} else if (fd == 0) {
				f->R.rax = (uint64_t) -1;
			} else if (fd == 1) {
				putbuf(buffer, size);
				f->R.rax = size;
			} else {
				struct file *file = thread_current()->fd_table[fd];
				f->R.rax = (file == NULL) ? (uint64_t) -1
				                          : (uint64_t) file_write(file, buffer, size);
			}
			lock_release(&filesys_lock);
			break;
		}
		default:
			printf("unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}
