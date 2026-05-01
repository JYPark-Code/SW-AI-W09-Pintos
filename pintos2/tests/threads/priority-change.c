/* Verifies that lowering a thread's priority so that it is no
   longer the highest-priority thread in the system causes it to
   yield immediately. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/thread.h"

static thread_func changing_thread;

void test_priority_change(void)
{
  /* This test does not work with the MLFQS. */
  ASSERT(!thread_mlfqs);

  msg("Creating a high-priority thread 2.");
  /**
   * 스레드 생성 시 32
   */
  thread_create("thread 2", PRI_DEFAULT + 1, changing_thread, NULL);
  msg("Thread 2 should have just lowered its priority.");
  /**
   * 스레드의 우선순위 변경 및 바로 동작 29
   */
  thread_set_priority(PRI_DEFAULT - 2);
  msg("Thread 2 should have just exited.");
}

/**
 * 스레드의 우선순위를 0으로 변경
 */
static void
changing_thread(void *aux UNUSED)
{
  msg("Thread 2 now lowering priority.");
  /**
   * 작동 중 일때, 우선순위 변경 30
   */
  thread_set_priority(PRI_DEFAULT - 1);
  msg("Thread 2 exiting.");
}
