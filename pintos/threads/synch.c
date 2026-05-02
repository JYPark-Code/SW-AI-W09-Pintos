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

/* Maximum nested donation chain depth. */
#define DONATION_DEPTH 8

/* static 함수 선언 */
static void donate_priority(void);
static void remove_with_lock(struct lock *lock);
static bool cmp_donation_priority (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);


/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */

static bool
priority_more (const struct list_elem *a,
               const struct list_elem *b,
               void *aux UNUSED)
{
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);

	return ta->priority > tb->priority;
}

void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
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

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_insert_ordered (&sema->waiters, &thread_current ()->elem,
				priority_more, NULL);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
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

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;
	struct thread *unblocked = NULL;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
<<<<<<< HEAD
	if (!list_empty (&sema->waiters)) {
		list_sort (&sema->waiters, priority_more, NULL);
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
	}
=======
>>>>>>> accccb83401aa30138aac21a537a294f9f557b24
	sema->value++;
	if (!list_empty (&sema->waiters)) {
		struct list_elem *max_elem =
			list_min (&sema->waiters, cmp_priority, NULL);
		list_remove (max_elem);
		unblocked = list_entry (max_elem, struct thread, elem);
		thread_unblock (unblocked);
	}
	intr_set_level (old_level);

	/* 깨운 thread가 더 높을 때만 yield. interrupt 컨텍스트면
	   intr_yield_on_return으로 ISR 종료 시점에 양보 예약. */
	if (unblocked != NULL
			&& unblocked->priority > thread_current ()->priority) {
		if (intr_context ())
			intr_yield_on_return ();
		else
			thread_yield ();
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
	struct thread *cur = thread_current ();
	enum intr_level old_level;

	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	/* donation 체인 갱신은 atomic하게: holder의 donations 리스트와
	   priority 체인 업데이트가 인터럽트로 끊기면 일관성이 깨진다. */
	old_level = intr_disable ();
	if (lock->holder != NULL) {
		cur->wait_on_lock = lock;
		list_insert_ordered (&lock->holder->donations,
				&cur->donation_elem,
				cmp_donation_priority, NULL);
		donate_priority ();
	}
	intr_set_level (old_level);

	sema_down (&lock->semaphore);
	cur->wait_on_lock = NULL;
	lock->holder = cur;
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
	enum intr_level old_level;

	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	/* donations 정리와 priority 재계산을 atomic하게 처리 후
	   semaphore 반환은 락 밖에서. */
	old_level = intr_disable ();
	remove_with_lock (lock);
	refresh_priority ();
	lock->holder = NULL;
	intr_set_level (old_level);

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
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

<<<<<<< HEAD
static bool
cmp_sema_priority (const struct list_elem *a,
                   const struct list_elem *b,
                   void *aux UNUSED)
{
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
	struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	struct thread *ta = list_entry (list_front (&sa->semaphore.waiters),
			struct thread, elem);
	struct thread *tb = list_entry (list_front (&sb->semaphore.waiters),
			struct thread, elem);

	return ta->priority > tb->priority;
=======
/* semaphore_elem 안의 thread 우선순위를 비교하는 함수 */
static bool
cmp_sema_priority(const struct list_elem *a,
                  const struct list_elem *b,
                  void *aux UNUSED) {
    struct semaphore_elem *sa = list_entry(a, struct semaphore_elem, elem);
    struct semaphore_elem *sb = list_entry(b, struct semaphore_elem, elem);

    /* 각 semaphore의 waiters에서 thread 우선순위 비교 */
    struct thread *ta = list_entry(list_begin(&sa->semaphore.waiters),
                                   struct thread, elem);
    struct thread *tb = list_entry(list_begin(&sb->semaphore.waiters),
                                   struct thread, elem);
    return ta->priority > tb->priority;
>>>>>>> accccb83401aa30138aac21a537a294f9f557b24
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
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
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
<<<<<<< HEAD
		list_sort (&cond->waiters, cmp_sema_priority, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
=======

		/* cond->waiters는 semaphore_elem의 리스트.
		   각 semaphore_elem은 대기 중인 thread의 개별 semaphore를 갖고 있음.
		   list_pop_front()는 우선순위를 무시하고 맨 앞을 꺼내므로,
		   list_min() + cmp_sema_priority()로 가장 높은 우선순위 thread를
		   가진 semaphore_elem을 찾아서 꺼냄. */

		/* 1. cond->waiters에서 가장 높은 우선순위 semaphore_elem 찾기
		      cmp_sema_priority: semaphore_elem 안의 semaphore의 waiters에서
		      thread 우선순위를 비교하는 함수 */
		struct list_elem *max_sema_elem = list_min(&cond->waiters,
		                                           cmp_sema_priority, NULL);

		/* 2. 찾은 semaphore_elem을 waiters 리스트에서 제거 */
		list_remove(max_sema_elem);

		/* 3. 해당 semaphore_elem의 semaphore에 sema_up() 호출
		      → 대기 중인 thread를 깨움 */
		sema_up(&list_entry(max_sema_elem,
		                    struct semaphore_elem, elem)->semaphore);
>>>>>>> accccb83401aa30138aac21a537a294f9f557b24
	}
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


static void
donate_priority(void) {
    /* 1. 현재 thread 가져오기 */
	struct thread *cur = thread_current();
    /* 2. 최대 DONATION_DEPTH(8)단계까지 반복 */
    for (int depth = 0; depth < DONATION_DEPTH; depth++) {

        /* 3. 현재 thread가 기다리는 lock 확인 */
        /* wait_on_lock이 NULL이면 중단 */
		if(cur->wait_on_lock == NULL){
			break;
		}

        /* 4. lock의 holder 확인 */
        /* holder가 NULL이면 중단 */
		struct thread *holder = cur->wait_on_lock->holder;
		if(holder == NULL){
			break;
		}
        /* 5. holder 우선순위가 나보다 낮으면 올려줌 */
		if(holder->priority >= cur -> priority){
			break;
		}

		holder->priority = cur -> priority;	
        /* 6. 다음 체인으로 이동 */
        /* thread = lock->holder */
		cur = holder;
		
    }
}

static void
remove_with_lock(struct lock *lock) {

	struct thread *cur = thread_current();
	struct list_elem *e;

	/* 1. 현재 thread의 donations 리스트 순회 */
	for(e = list_begin(&cur->donations); 
		e != list_end(&cur->donations);
		){
		
		/* 2. 각 donation_elem에서 thread 꺼내기 */
		struct thread *t = list_entry(e, struct thread, donation_elem);

		/* 3. 그 thread의 wait_on_lock이 이 lock이면 */
		if (t->wait_on_lock == lock) {
			/* donations 리스트에서 제거 */
			e = list_remove(e);
		} else {
			e = list_next(e);
		}
	}
 
}

void
refresh_priority(void) {
    /* 1. 현재 thread 가져오기 */
	struct thread *cur = thread_current();
    /* 2. 우선순위를 original_priority로 초기화 */
	cur->priority = cur->original_priority;

    /* 3. donations 리스트가 비어있지 않으면 */
    /*    donations 중 가장 높은 우선순위 찾기 */
    /*    그 우선순위가 현재보다 높으면 업데이트 */
	if (!list_empty(&cur->donations)) {
		struct list_elem *e = list_min(&cur->donations, cmp_donation_priority, NULL);
		struct thread *top = list_entry(e, struct thread, donation_elem);
		if (top->priority > cur->priority)
			cur->priority = top->priority;
	}
}


static bool
cmp_donation_priority (const struct list_elem *a,
                       const struct list_elem *b,
                       void *aux UNUSED) {
    struct thread *ta = list_entry(a, struct thread, donation_elem);
    struct thread *tb = list_entry(b, struct thread, donation_elem);
    return ta->priority > tb->priority;   /* 내림차순 less */
}
