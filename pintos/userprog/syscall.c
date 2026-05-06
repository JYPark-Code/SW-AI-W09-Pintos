#include "filesys/file.h"
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
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/mmu.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "filesys/file.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
static struct lock filesys_lock;
struct fd_entry {
	int fd;
	struct file *file;
	struct list_elem elem;
};

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
#define FD_MAX 128

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
	if (uaddr == NULL || !is_user_vaddr(uaddr) || pml4_get_page(thread_current()->pml4, uaddr) == NULL) {
		thread_current()->exit_status = -1;
		thread_exit();
	}
}

static int
add_file_to_fdt(struct file *file) {
	struct thread *curr = thread_current();

	if (curr->fd_idx >= FD_MAX)
		return -1;

	struct fd_entry *entry = malloc(sizeof *entry);
	if (entry == NULL) {
		return -1;
	}

	entry->fd = curr->fd_idx++;
	entry->file = file;

	list_push_back(&curr->fd_table, &entry->elem);

	return entry->fd;
}

static struct fd_entry *
find_fd(int fd) {
	struct thread *curr = thread_current();
	struct list *fdt = &curr->fd_table;
	struct list_elem *e;

	for (e = list_begin(fdt); e != list_end(fdt); e = list_next(e)) {
		struct fd_entry *entry = list_entry(e, struct fd_entry, elem);

		if (entry->fd == fd) {
			return entry;
		}
	}

	return NULL;
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
			char *filename = (char *)f->R.rdi;

			validate_user_addr(filename);

			lock_acquire (&filesys_lock);
			struct file *file = filesys_open(filename);
			if (file == NULL) {
				f->R.rax = -1;
			} else {
				int fd = add_file_to_fdt(file);
				if (fd == -1)
					file_close(file);
				f->R.rax = fd;
			}
			lock_release (&filesys_lock);

			break;
		}
		case SYS_CLOSE: {
			uint64_t fd = (uint64_t)f->R.rdi;

			lock_acquire (&filesys_lock);

			struct fd_entry *entry = find_fd(fd);
			if (entry == NULL) {
				lock_release(&filesys_lock);
				break;
			}

			file_close(entry->file);
			list_remove(&entry->elem);
			free(entry);

			lock_release (&filesys_lock);

			break;
		}

		case SYS_OPEN: {
			const char *file = (const char *) f->R.rdi;
		
			validate_user_string (file);
		
			lock_acquire (&filesys_lock);
			struct file *opened_file = filesys_open (file);
			lock_release (&filesys_lock);
		
			if (opened_file == NULL) {
				f->R.rax = -1;
				break;
			}
		
			struct thread *cur = thread_current ();
			int fd = cur->next_fd;
		
			if (fd >= 128) {
				file_close (opened_file);
				f->R.rax = -1;
				break;
			}
		
			cur->fd_table[fd] = opened_file;
			cur->next_fd++;
		
			f->R.rax = fd;
			break;
		}		

		case SYS_OPEN: {
			const char *file = (const char *) f->R.rdi;
			struct thread *cur = thread_current ();
			int fd = -1;

			validate_user_string (file);

			for (int i = 2; i < FD_MAX; i++) {
				if (cur->fd_table[i] == NULL) {
					fd = i;
					break;
				}
			}

			if (fd == -1) {
				f->R.rax = (uint64_t) -1;
				break;
			}

			cur->fd_table[fd] = filesys_open (file);
			if (cur->fd_table[fd] == NULL) {
				f->R.rax = (uint64_t) -1;
				break;
			}

			f->R.rax = fd;
			break;
		}
		case SYS_CLOSE: {
			int fd = (int) f->R.rdi;
			struct thread *cur = thread_current ();

			if (fd < 0 || fd >= FD_MAX) {
				break;
			}

			if (cur->fd_table[fd] == NULL) {
				break;
			}

			struct file *file = cur->fd_table[fd];
			cur->fd_table[fd] = NULL;
			file_close (file);

			break;
		}

		default:

			printf("unhandled syscall: %llu\n",
			       (unsigned long long) sysno);
			thread_exit();
	}
}
