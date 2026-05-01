/* Tests that cond_signal() wakes up the highest-priority thread
   waiting in cond_wait(). */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static thread_func priority_condvar_thread;
static struct lock lock;
static struct condition condition;

void test_priority_condvar(void)
{
  int i;

  /* This test does not work with the MLFQS. */
  ASSERT(!thread_mlfqs);

  lock_init(&lock);      // 들어갈 수 있는 자원이 있다는 의미
  cond_init(&condition); // 컨디션 스트럭처에 waiters라는 리스트를 초기화

  thread_set_priority(PRI_MIN); // 메인 스레드를 0으로 우선순위 설정
  for (i = 0; i < 10; i++)
  {
    // 우선순위 21 ~ 30
    int priority = PRI_DEFAULT - (i + 7) % 10 - 1;
    char name[16];
    snprintf(name, sizeof name, "priority %d", priority);
    thread_create(name, priority, priority_condvar_thread, NULL);
  }

  for (i = 0; i < 10; i++)
  {
    lock_acquire(&lock); // lock을 초기화 하면서 들어올 수 있는 공간이 있는상태
    msg("Signaling...");
    cond_signal(&condition, &lock);
    lock_release(&lock); // 작업 후 록 릴리즈
  }
}

static void
priority_condvar_thread(void *aux UNUSED)
{
  msg("Thread %s starting.", thread_name());
  lock_acquire(&lock);          // 록을 획득
  cond_wait(&condition, &lock); // 기다리고
  msg("Thread %s woke up.", thread_name());
  lock_release(&lock); // 록 릴리즈
}
