/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static bool
wakeup_priority(const struct list_elem *a,
				const struct list_elem *b,
				void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, elem);
	const struct thread *tb = list_entry(b, struct thread, elem);

	return ta->priority > tb->priority;
}

static bool
doner_priority(const struct list_elem *a,
			   const struct list_elem *b,
			   void *aux UNUSED)
{
	const struct thread *ta = list_entry(a, struct thread, donation_elem);
	const struct thread *tb = list_entry(b, struct thread, donation_elem);

	return ta->priority > tb->priority;
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
/**
 * 세마포어를 초기화 하는 함수
 * 의미
 * ---------------------------
 * 세마포어 값 설정
 * 기다리는 스레드 목록 초기화
 * ---------------------------
 * value = 1 이면 한 스레드만 통과 가능하고, value = 0이면 누군가 sema_up() 해주기 전까지 대기
 */
void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
/**
 * 세마포어를 "획득"하는 함수
 * value가 0이면 현재 스레드를 waiters에 넣고 잠재움
 * 현재 실행중인 스레드를 sema->waiters에 넣는다.
 *
 * value == 0이면 현재 스레드를 waiters에 넣고 block한 뒤, 깨어나서 value-- 함
 * 반대로 sema_up은 waiters가 있으면 하나를 깨우고 value++
 * +1의 의미는 자원을 하나 반납했다.
 *
 * value : 남아 있는 통과권 수
 * waiters : 통과권이 없어서 자고 있는 스레드 목록
 */
void sema_down(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());

	// 인터럽트를 끄고
	old_level = intr_disable();
	// value가 0이면
	while (sema->value == 0)
	{
		// 현재 스레드를 waiters에 넣고 잠재움
		// value가 양수면 value를 1 감소시키고 통과
		// list_push_back(&sema->waiters, &thread_current()->elem);
		list_insert_ordered(&sema->waiters, &thread_current()->elem, wakeup_priority, NULL);
		thread_block();
	}
	sema->value--;
	intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
/**
 * 기다리지 않고 세마포어 획득을 시도하는 함수
 * 가능하면 바로 획득
 * 불가능하면 잠들지 않고 false 반환
 */
bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
/**
 * 세마포어를 "반납"하는 함수
 * 기다리는 스레드가 있으면 하나 깨움
 * sema->value를 1 증가시킴
 */
void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;
	struct thread *wakeup;
	bool should_yield = false;
	bool in_intr;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	in_intr = intr_context();
	// ready에 바로 넣지 말고 깨워야할 애를 바로 받아서 바로 비교 후 깨우기
	if (!list_empty(&sema->waiters))
	{
		wakeup = list_entry(list_pop_front(&sema->waiters),
							struct thread, elem);
		thread_unblock(wakeup);

		// 깨운 스레드가 sema_up()이 끝나기 전에 먼저 실행되면 문제
		// struct thread *curr = thread_current();
		// if (curr->priority < wakeup->priority)
		// {
		// 	thread_yield();
		// }
		struct thread *curr = thread_current();
		if (curr->priority < wakeup->priority)
		{
			should_yield = true;
		}
	}
	sema->value++;

	intr_set_level(old_level);

	if (should_yield)
	{
		thread_yield();
	}
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock));

	// struct list *donation;
	thread_current()->wait_on_lock = lock;
	if (lock->holder != NULL)
	{
		if (lock->holder->priority < thread_current()->priority)
		{
			lock->holder->priority = thread_current()->priority;
		}
		list_insert_ordered(&lock->holder->donations, &thread_current()->donation_elem, doner_priority, &lock);
	}

	sema_down(&lock->semaphore);
	thread_current()->wait_on_lock = NULL;
	lock->holder = thread_current();
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	struct thread *curr = thread_current();
	int max = curr->original_priority;
	struct list_elem *e;

	for (e = list_begin(&curr->donations); e != list_end(&curr->donations);)
	{
		struct thread *donor = list_entry(e, struct thread, donation_elem);

		if (donor->wait_on_lock == lock)
		{
			e = list_remove(e);
		}
		else
		{
			if (donor->priority > max)
			{
				max = donor->priority;
			}
			e = list_next(e);
		}
	}

	curr->priority = max;

	lock->holder = NULL;
	sema_up(&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* One semaphore in a list. */
/**
 * Pintos는 대기실을 구현할 때, 기다리는 사람마다 개인용 세마포어 하나씩 붙여 둠
 * 그 개인용 세마포어를 담는 껍데기가 struct semaphore_elem
 * 대기열에 들어갈 수 있고, 동시에 자기만의 잠금벨도 가진 대기표
 *
 * 어떤 스레드 A는 "지금은 못 한다. 어떤 조건이 참이 될 때까지 기다려야 한다."
 * 어떤 스레드 b는 나중에 그 조건을 참으로 만든다.
 * 그때 B가 A를 깨워준다.
 *
 * 누가 기다리고 있으면 깨워라
 * 기다리는 사람이 없으면 signal은 사라짐
 * 즉 상태를 저장하지 않음
 */
struct semaphore_elem
{
	struct list_elem elem;		/* List element. */
	struct semaphore semaphore; /* This semaphore. */
	int priority;
};

static bool
sema_elem_priority_more(const struct list_elem *a,
						const struct list_elem *b,
						void *aux UNUSED)
{
	const struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
	const struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

	return sa->priority > sb->priority;
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
/**
 * 1. 현재 스레드가 waiter 라는 semaphore_elem 만듬
 * 2. waiter.semaphore를 0으로 초기화
 * 3. cond->waiters 리스트에 waiter.elem을 넣음
 * 4. 가지고 있던 lock을 놓음
 * 5. sema_down(&waiter.semaphore)를 호출해서 잠듬
 * 6. 나중에 누군가 cond_signal()을 부름
 * 7. cond_signal()은 cond->waiters에서 waiter 하나를 꺼냄
 * 8. 그 waiter 안의 semaphore에 sema_up()을 해줌
 * 9. 잠들어 있던 스레드가 깸
 * 10. 깨어난 뒤 다시 lock_acquire(lock) 해서 락을 잡고 돌아옴
 */
void cond_wait(struct condition *cond, struct lock *lock)
{
	// 조건변수 대기자 한명 당, 깨울 수 있는 개인용 세마포어를 하나씩 붙여두는 것
	struct semaphore_elem waiter;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	// 락을 확인해서 현재 스레드가 lock 걸린 상태여야만 한다.
	ASSERT(lock_held_by_current_thread(lock));

	// 현재 semaphore_elem의 세마포어를 0으로
	/**
	 * cond_wait에서 각 스레드는 자기만의 지역변수 struct semaphore_elem waiter; 를 하나 만듬
	 * waiter.semaphore를 0으로 초기화한 뒤 cond->waiters 리스트에 waiter.elem을 넣음
	 * 그 다음 sema_down(&waiter.semaphore)로 잠듬
	 * 나중에 cond_signal()이 cond->waiters에서 하나 꺼내 그 안의 semaphore에 sema_up()을 호출해 그 대기자를 깨움
	 */
	sema_init(&waiter.semaphore, 0);
	waiter.priority = thread_current()->priority;
	list_insert_ordered(&cond->waiters, &waiter.elem, sema_elem_priority_more, NULL);
	// list_push_back(&cond->waiters, &waiter.elem);
	lock_release(lock);
	sema_down(&waiter.semaphore);
	lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	struct thread *ready;
	struct list_elem waiters;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	// 세마포어를 하나 받아서
	// 세마 업(이 세마는 스레드 하나가 갖고 있는 세마포어)
	if (!list_empty(&cond->waiters))
	{
		// 이미 semaphore_elem 안에 cond.waiters 리스트가 들어가 있음
		struct semaphore_elem *sema_elem = list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem);

		sema_up(&sema_elem->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}
