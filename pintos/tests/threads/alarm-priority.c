/* Checks that when the alarm clock wakes up threads, the
   higher-priority threads run first. */

#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/timer.h"

static thread_func alarm_priority_thread;
static int64_t wake_time;
static struct semaphore wait_sema;

void test_alarm_priority(void)
{
  int i;

  /* This test does not work with the MLFQS. */
  ASSERT(!thread_mlfqs);

  // 현재 틱을 가져와서 500번을 더하는 시간을 깨어나는 시간으로 둔다.
  wake_time = timer_ticks() + 5 * TIMER_FREQ;
  // 세마포어를 쓰고있는 상태로 초기화 (lock 걸린 상태)
  sema_init(&wait_sema, 0);

  // name에 priority 32 - (0~10 + 5) % 10 - 1; -> 22 ~ 31
  for (i = 0; i < 10; i++)
  {
    int priority = PRI_DEFAULT - (i + 5) % 10 - 1; // 22 ~ 31
    char name[16];
    snprintf(name, sizeof name, "priority %d", priority); // priority 22 ~ 31
    thread_create(name, priority, alarm_priority_thread, NULL);
  }

  thread_set_priority(PRI_MIN);

  for (i = 0; i < 10; i++)
    sema_down(&wait_sema);
}

/**
 * 처음 틱은 넘기고 다음 틱부터 확인
 */
static void
alarm_priority_thread(void *aux UNUSED)
{
  /* Busy-wait until the current time changes. */
  /**
   * 한 틱의 Freq가 100이라고 할때,
   * 여러 스레드가 한번에 동작 시
   * 어떤 스레드는 현재 틱이 막 시작할 때 읽기를 시작
   * 어떤 스레드는 현재 틱이 막 끝날 때 읽기를 시작 할 수도 있음
   */
  /**
    메인 스레드가 thread A 생성
    메인 스레드가 thread B 생성
    아직 둘 다 READY일 뿐 실행은 안 됨
    스케줄러가 먼저 A를 실행시킴
    A가 timer_ticks() 읽음, 현재 tick = 100
    그 사이 timer interrupt 발생해서 tick = 101
    나중에 B가 실행돼서 timer_ticks() 읽음, 현재 tick = 101
    이러면 둘은 “같은 테스트 안의 스레드”지만 시작 기준이 이미 달라진 것.

    특히 Pintos 테스트는 단일 CPU라서 “동시에” 여러 스레드가 실제로 병렬 실행되는 건 아닙니다.
    정확히는 번갈아가며 실행되기 때문에, 읽는 순간이 조금씩 어긋나는 게 자연스럽다.
   */
  int64_t start_time = timer_ticks();
  while (timer_elapsed(start_time) == 0)
    continue;

  /* Now we know we're at the very beginning of a timer tick, so
     we can call timer_sleep() without worrying about races
     between checking the time and a timer interrupt. */
  timer_sleep(wake_time - timer_ticks());

  /* Print a message on wake-up. */
  msg("Thread %s woke up.", thread_name());

  sema_up(&wait_sema);
}
