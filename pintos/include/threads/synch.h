#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/* A counting semaphore. */
/**
 * value : 값이 0,1인 binary semaphore
 * list_init(&sema->waiters) -> head <-> tail
 * 초기화 후 실제로 값이 들어가게 되면 head <-> thread <-> tail 이런 형태로 저장
 */
struct semaphore
{
	unsigned value;		 /* Current value. */
	struct list waiters; /* List of waiting threads. */
};

void sema_init(struct semaphore *, unsigned value);
void sema_down(struct semaphore *);
bool sema_try_down(struct semaphore *);
void sema_up(struct semaphore *);
void sema_self_test(void);

/* Lock. */
/**
 * lock->holder = NULL; -> 아무도 이 락을 들고 있지 않다는 뜻 (현재 락 소유자)
 * lock은 내부적으로 값이 1 인 binary semaphore를 써서 구현 (0, 1)
 * semaphore->value = 1; -> 지금 한 스레드가 들어갈 수 있다는 의미
 * 누군가 lock_acquire()하면 내부적으로 sema_down() 하면서 1이 0이 되고, 그 순간부터 다른 스레드는 들어갈 수 없게 됨.
 * 나중에 lock_release()하면 sema_up()해서 다시 1이 되어 다음 스레드가 들어갈 수 있는 상태가 됨.
 */
struct lock
{
	struct thread *holder;		/* Thread holding lock (for debugging). */
	struct semaphore semaphore; /* Binary semaphore controlling access. */
};

void lock_init(struct lock *);
void lock_acquire(struct lock *);
bool lock_try_acquire(struct lock *);
void lock_release(struct lock *);
bool lock_held_by_current_thread(const struct lock *);

/* Condition variable. */
struct condition
{
	struct list waiters; /* List of waiting threads. */
};

void cond_init(struct condition *);
void cond_wait(struct condition *, struct lock *);
void cond_signal(struct condition *, struct lock *);
void cond_broadcast(struct condition *, struct lock *);

/* Optimization barrier.
 *
 * The compiler will not reorder operations across an
 * optimization barrier.  See "Optimization Barriers" in the
 * reference guide for more information.*/
/**
 * 함수는 호출 오버헤드가 존재 => 함수가 아니라 매크로
 * 함수는 인라인 보장 안됨
 * 함수는 컴파일러가 다시 최적화할 수도 있음
 *
 * asm : C 코드 안에 직접 어셈블리 코드 삽입
 * asm volatile ("") -> 아무 CPU 명령도 실행 안 함
 * 근데 volatile을 붙이면 컴파일러는 절대 지우지 않게 됨
 * : : : "memory" -> 이 코드가 메모리를 건드릴 수도 있으니 앞뒤 메모리 순서 절대 바꾸지 않도록 강제
 *
 * 만약 이게 없으면
 * intr_disable();
 * t = ticks;
 * intr_set_level(old_level);
 * 		↓
 * t - ticks;
 * intr_disable();
 * intr_set_level(old_level);
 * 로 컴파일러가 최적화해버려 내부적인 인터럽트 보호 역할이 완전 깨질 수 있음
 */
#define barrier() asm volatile("" : : : "memory")

#endif /* threads/synch.h */
