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
thread_priority_more (const struct list_elem *a,
		const struct list_elem *b, void *aux UNUSED) {
	const struct thread *ta = list_entry (a, struct thread, elem); /* a elem의 주인 thread를 찾는다. */
	const struct thread *tb = list_entry (b, struct thread, elem); /* b elem의 주인 thread를 찾는다. */

	return ta->priority > tb->priority; /* true면 a가 b보다 앞에서 기다려야 한다. */
}

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL); /* 초기화할 semaphore가 실제로 있어야 한다. */

	sema->value = value; /* 현재 사용 가능한 허가권 개수를 설정한다. */
	list_init (&sema->waiters); /* 허가권이 없을 때 잠들 thread들의 대기열을 초기화한다. */
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable (); /* value와 waiters를 만지는 동안 interrupt를 막는다. */
	while (sema->value == 0) { /* 허가권이 하나도 없으면 */
		list_insert_ordered (&sema->waiters, &thread_current ()->elem,
				thread_priority_more, NULL); /* 현재 thread를 semaphore waiters에 priority 순서로 넣는다. */
		thread_block (); /* 현재 thread를 BLOCKED로 만들고 CPU에서 내려온다. */
	}
	sema->value--; /* 허가권 하나를 사용하고 통과한다. */
	intr_set_level (old_level); /* interrupt 상태를 원래대로 복구한다. */
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable (); /* value와 waiters를 만지는 동안 interrupt를 막는다. */
	if (sema->value > 0)
	{
		sema->value--; /* 허가권이 있으면 하나 사용한다. */
		success = true; /* 잠들지 않고 sema_down에 성공했다. */
	}
	else
		success = false; /* 허가권이 없으면 실패만 반환하고 BLOCKED 되지는 않는다. */
	intr_set_level (old_level); /* interrupt 상태를 원래대로 복구한다. */

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level; /* interrupt 상태를 저장해두는 변수다. */
	struct thread *unblocked = NULL; /* 이번 sema_up으로 깨울 thread를 담는다. */
	bool should_yield = false; /* 깨운 thread가 더 높으면 현재 thread가 양보해야 한다. */

	ASSERT (sema != NULL); /* up할 semaphore가 실제로 있어야 한다. */

	old_level = intr_disable (); /* value와 waiters를 만지는 동안 interrupt를 막는다. */
	if (!list_empty (&sema->waiters)) { /* 기다리는 thread가 있으면 하나 깨운다. */
		list_sort (&sema->waiters, thread_priority_more, NULL); /* 깨우기 직전에 priority 순서로 다시 정렬한다. */
		unblocked = list_entry (list_pop_front (&sema->waiters),
				struct thread, elem); /* waiters 맨 앞 elem을 꺼내 그 주인 thread를 찾는다. */
		thread_unblock (unblocked); /* 그 thread를 BLOCKED에서 READY로 깨운다. */
		should_yield = unblocked->priority > thread_current ()->priority; /* 깨운 thread가 현재 thread보다 높으면 선점 필요. */
	}
	sema->value++; /* 허가권 하나를 추가하거나, 깨운 thread가 소비할 신호를 만든다. */
	intr_set_level (old_level); /* interrupt 상태를 원래대로 복구한다. */

	if (should_yield) { /* 더 높은 priority thread를 깨웠다면 */
		if (intr_context ())
			intr_yield_on_return (); /* interrupt 안에서는 return 직전에 yield하도록 예약한다. */
		else
			thread_yield (); /* 일반 thread 문맥에서는 바로 CPU를 양보한다. */
	}
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
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
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	sema_down (&lock->semaphore);
	lock->holder = thread_current ();
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* cond->waiters에 연결될 list element. */
	struct semaphore semaphore;         /* 이 waiter를 재우고 깨우는 작은 semaphore. */
	int priority;                       /* 이 waiter를 만든 thread의 priority. */
};

static bool
semaphore_elem_priority_more (const struct list_elem *a,
		const struct list_elem *b, void *aux UNUSED) {
	const struct semaphore_elem *sa = list_entry (a,
			struct semaphore_elem, elem); /* a elem의 주인 semaphore_elem을 찾는다. */
	const struct semaphore_elem *sb = list_entry (b,
			struct semaphore_elem, elem); /* b elem의 주인 semaphore_elem을 찾는다. */

	return sa->priority > sb->priority; /* true면 a waiter가 b waiter보다 먼저 signal되어야 한다. */
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL); /* 초기화할 condition variable이 실제로 있어야 한다. */

	list_init (&cond->waiters); /* 조건을 기다리는 semaphore_elem들의 대기열을 초기화한다. */
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
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter; /* 현재 thread를 재울 작은 semaphore 포장지다. */

	ASSERT (cond != NULL); /* 기다릴 condition variable이 있어야 한다. */
	ASSERT (lock != NULL); /* condition과 함께 쓰는 lock이 있어야 한다. */
	ASSERT (!intr_context ()); /* 잠들 수 있으므로 interrupt context에서는 호출할 수 없다. */
	ASSERT (lock_held_by_current_thread (lock)); /* cond_wait는 lock을 잡은 상태에서만 호출한다. */

	sema_init (&waiter.semaphore, 0); /* signal 전까지 잠들도록 작은 semaphore value를 0으로 둔다. */
	waiter.priority = thread_current ()->priority; /* cond->waiters 정렬을 위해 현재 priority를 저장한다. */
	list_insert_ordered (&cond->waiters, &waiter.elem,
			semaphore_elem_priority_more, NULL); /* thread가 아니라 waiter 포장지를 priority 순으로 넣는다. */
	lock_release (lock); /* 조건을 기다리는 동안 다른 thread가 공유 상태를 바꿀 수 있게 lock을 놓는다. */
	sema_down (&waiter.semaphore); /* 작은 semaphore에서 BLOCKED 되어 signal을 기다린다. */
	lock_acquire (lock); /* 깨어난 뒤 조건을 다시 확인할 수 있도록 lock을 다시 잡는다. */
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL); /* signal할 condition variable이 있어야 한다. */
	ASSERT (lock != NULL); /* condition과 함께 쓰는 lock이 있어야 한다. */
	ASSERT (!intr_context ()); /* condition signal은 interrupt context에서 쓰지 않는다. */
	ASSERT (lock_held_by_current_thread (lock)); /* signal할 때도 같은 lock을 잡고 있어야 한다. */

	if (!list_empty (&cond->waiters))
		list_sort (&cond->waiters, semaphore_elem_priority_more, NULL); /* priority가 바뀌었을 수 있으니 다시 정렬한다. */
	if (!list_empty (&cond->waiters))
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore); /* 가장 높은 waiter의 작은 semaphore를 up해서 thread를 깨운다. */
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
