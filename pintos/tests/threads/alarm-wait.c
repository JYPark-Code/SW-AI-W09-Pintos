/* Creates N threads, each of which sleeps a different, fixed
   duration, M times.  Records the wake-up order and verifies
   that it is valid. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static void test_sleep(int thread_cnt, int iterations);

void test_alarm_single(void)
{
  test_sleep(5, 1);
}

void test_alarm_multiple(void)
{
  test_sleep(5, 7);
}

/* Information about the test. */
/**
 * start : 테스트 시작 시각 (기준 시간 역할 - 모든 thread가 "같은 기준 시간"을 공유)
 * iterations : 각 thread가 몇 번 반복할지 - 테스트 조건을 통일하기 위한 값
 *
 * output_lock : 여러 thread가 동시에 output을 건드리면 출력이 깨진다.
 * 따라서 lock_acquire(*output_lock); , lock_release(*output_lock);으로 출력 영역 보호
 * *output_pos : 출력 배열의 "현재 쓰기 위치 포인터"
 * output buffer = [_,_,_,_,_]
 * output_pos -> index 0
 * thread가 쓰면
 * thread A -> output[0] = 100
 * output_pos -> 1
 *
 * thread B -> output[1] = 200
 * output_pos -> 2
 * 여러 thread가 결과를 순서대로 기록
 */
struct sleep_test
{
  int64_t start;  /* Current time at start of test. */
  int iterations; /* Number of iterations per thread. */

  /* Output. */
  struct lock output_lock; /* Lock protecting output buffer. */
  int *output_pos;         /* Current position in output buffer. */
};

/* Information about an individual thread in the test. */
struct sleep_thread
{
  struct sleep_test *test; /* Info shared between all threads. */
  int id;                  /* Sleeper ID. */
  int duration;            /* Number of ticks to sleep. */
  int iterations;          /* Iterations counted so far. */
};

static void sleeper(void *);

/* Runs THREAD_CNT threads thread sleep ITERATIONS times each. */
static void
test_sleep(int thread_cnt, int iterations)
{
  struct sleep_test test;
  struct sleep_thread *threads;
  int *output, *op;
  int product;
  int i;

  /* This test does not work with the MLFQS. */
  ASSERT(!thread_mlfqs);

  msg("Creating %d threads to sleep %d times each.", thread_cnt, iterations);
  msg("Thread 0 sleeps 10 ticks each time,");
  msg("thread 1 sleeps 20 ticks each time, and so on.");
  msg("If successful, product of iteration count and");
  msg("sleep duration will appear in nondescending order.");

  /* Allocate memory. */
  threads = malloc(sizeof *threads * thread_cnt);                // 스레드 5개의 공간을 만듬
  output = malloc(sizeof *output * iterations * thread_cnt * 2); // 반복할 횟수 * 스레드 개수 * 2의 공간을 할당
  if (threads == NULL || output == NULL)
    PANIC("couldn't allocate memory for test");

  /* Initialize test. */
  /**
   * 현재 타이머 틱을 가져와서 100을 더하는 이유
   * - 테스트가 보고 싶은건 "스레드 생성 속도"가 아니라 timer_sleep()이 약속한 시점에 제대로 깨우는가 이기 때문
   * - 모든 worker가 충분히 생성되고, sleep list에 들어간 뒤에 첫 wakeup 시각이 오도록 여유를 주는 것
   */
  test.start = timer_ticks() + 100;
  test.iterations = iterations; // 현재 반복횟수를 저장
  lock_init(&test.output_lock); // 쓰레드의 세마포어와 락 초기화 설정
  test.output_pos = output;     // 현재 공유된 상태 위치값

  /* Start threads. */
  ASSERT(output != NULL);
  for (i = 0; i < thread_cnt; i++)
  {
    struct sleep_thread *t = threads + i;
    char name[16];

    t->test = &test;
    t->id = i;
    t->duration = (i + 1) * 10;
    t->iterations = 0;

    snprintf(name, sizeof name, "thread %d", i);
    thread_create(name, PRI_DEFAULT, sleeper, t);
  }

  /* Wait long enough for all the threads to finish. */
  timer_sleep(100 + thread_cnt * iterations * 10 + 100);

  /* Acquire the output lock in case some rogue thread is still
     running. */
  lock_acquire(&test.output_lock);

  /* Print completion order. */
  product = 0;
  for (op = output; op < test.output_pos; op++)
  {
    struct sleep_thread *t;
    int new_prod;

    ASSERT(*op >= 0 && *op < thread_cnt);
    t = threads + *op;

    new_prod = ++t->iterations * t->duration;

    msg("thread %d: duration=%d, iteration=%d, product=%d",
        t->id, t->duration, t->iterations, new_prod);

    if (new_prod >= product)
      product = new_prod;
    else
      fail("thread %d woke up out of order (%d > %d)!",
           t->id, product, new_prod);
  }

  /* Verify that we had the proper number of wakeups. */
  for (i = 0; i < thread_cnt; i++)
    if (threads[i].iterations != iterations)
      fail("thread %d woke up %d times instead of %d",
           i, threads[i].iterations, iterations);

  lock_release(&test.output_lock);
  free(output);
  free(threads);
}

/* Sleeper thread. */
static void
sleeper(void *t_)
{
  struct sleep_thread *t = t_;
  struct sleep_test *test = t->test;
  int i;

  for (i = 1; i <= test->iterations; i++)
  {
    int64_t sleep_until = test->start + i * t->duration;
    timer_sleep(sleep_until - timer_ticks());
    lock_acquire(&test->output_lock);
    *test->output_pos++ = t->id;
    lock_release(&test->output_lock);
  }
}
