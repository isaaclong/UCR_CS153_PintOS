#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "devices/pit.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
// for lists
#include "lib/kernel/list.h"
  
/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);
static void real_time_delay (int64_t num, int32_t denom);

/*
 * PROJECT 1
 *
 * List of processes/threads that are waiting.
 * Using in project 1 to hold sleeping threads. Look in 
 * lib/kernal/list.h for documentation
 */ 

// list of sleeping threads
static struct list sleeping_thread_list; 

/* Sets up the timer to interrupt TIMER_FREQ times per second,
   and registers the corresponding interrupt. */
void
timer_init (void) 
{
  pit_configure_channel (0, 2, TIMER_FREQ);
  intr_register_ext (0x20, timer_interrupt, "8254 Timer");

  // initializing sleeping_thread_list here because it seems appropriate. 
  list_init (&sleeping_thread_list);
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) 
{
  unsigned high_bit, test_bit;

  ASSERT (intr_get_level () == INTR_ON);
  printf ("Calibrating timer...  ");

  /* Approximate loops_per_tick as the largest power-of-two
     still less than one timer tick. */
  loops_per_tick = 1u << 10;
  while (!too_many_loops (loops_per_tick << 1)) 
    {
      loops_per_tick <<= 1;
      ASSERT (loops_per_tick != 0);
    }

  /* Refine the next 8 bits of loops_per_tick. */
  high_bit = loops_per_tick;
  for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
    if (!too_many_loops (high_bit | test_bit))
      loops_per_tick |= test_bit;

  printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) 
{
  enum intr_level old_level = intr_disable ();
  int64_t t = ticks;
  intr_set_level (old_level);
  return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) 
{
  return timer_ticks () - then;
}

bool
sleep_list_compare (const struct list_elem *a, const struct list_elem *b, void *aux)
{
  struct thread *ta = list_entry(a, struct thread, sleep_elem);
  struct thread *tb = list_entry(b, struct thread, sleep_elem);
  return ta->wakeup_tick < tb->wakeup_tick ? true : false; 
}

void *aux;
/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{

  // Get current thread
  struct thread *current_thread = thread_current ();
  
  // Get currrent ticks
  int64_t start = timer_ticks ();
  //printf("start ticks:" "%" PRId64 "\n",start);
  //printf("passed in ticks: %d\n",ticks);
  current_thread->wakeup_tick = start + ticks;

  // make sure interrupts are on up until this point
  ASSERT (intr_get_level () == INTR_ON);

  enum intr_level old_level;

  // disable interrupts while inserting into list
  old_level = intr_disable ();
  list_insert_ordered (&sleeping_thread_list, &(current_thread->sleep_elem), &sleep_list_compare, aux);
  // reenable interrupts after insertion
  intr_set_level (old_level);

  // sleep the current thread
  sema_down (&(current_thread->is_sleeping));

  //printf ("after sema_down\n");
  //fflush (stdout);

  //struct intr_frame *args;
  
  /* 
  unsigned i = 0;
  for (i; i < ticks; i++)
  {
    timer_interrupt (args);
  }
  */
  
  // a time for when the thread should wake up; lib/kernal
  // FYI: timer ticks 100 times / second
  // FYI: int64_t is a signed int; could be negative (and I remember there being a test for it)

  // tell thread to sleep (function for this will also stop interrupts)
  // use thread_block as discussed in lecture - look in sync.c file for how to properly use
  // ^ if we use this we have to add semaphore into thread; use semma_up and semma_down
  
  // (lecture) assign the thread into some waiting queue made with the
  // existing doubly linked list implementation kernal/list; must turn
  // interrupts off while putting into list; should return to ready_list after 
  // appropriate amount of time waited
  //
  // have list of sleeping thread; each element in list should store when it has to wake up
  //
  // use timer_interrupt {timer_ticks++; thread_tick(); } to wake it up 
  // look at the list of sleeping threads, compare with current tick, if time to wake up
  // then semma_up, then remove from sleeping list
  //
  // partb) will need to run highest priority thread function here? 

  // Original implementation; busy-waits
  //while (timer_elapsed (start) < ticks) 
    //thread_yield ();
}

/* Sleeps for approximately MS milliseconds.  Interrupts must be
   turned on. */
void
timer_msleep (int64_t ms) 
{
  real_time_sleep (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts must be
   turned on. */
void
timer_usleep (int64_t us) 
{
  real_time_sleep (us, 1000 * 1000);
}

/* Sleeps for approximately NS nanoseconds.  Interrupts must be
   turned on. */
void
timer_nsleep (int64_t ns) 
{
  real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Busy-waits for approximately MS milliseconds.  Interrupts need
   not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_msleep()
   instead if interrupts are enabled. */
void
timer_mdelay (int64_t ms) 
{
  real_time_delay (ms, 1000);
}

/* Sleeps for approximately US microseconds.  Interrupts need not
   be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_usleep()
   instead if interrupts are enabled. */
void
timer_udelay (int64_t us) 
{
  real_time_delay (us, 1000 * 1000);
}

/* Sleeps execution for approximately NS nanoseconds.  Interrupts
   need not be turned on.

   Busy waiting wastes CPU cycles, and busy waiting with
   interrupts off for the interval between timer ticks or longer
   will cause timer ticks to be lost.  Thus, use timer_nsleep()
   instead if interrupts are enabled.*/
void
timer_ndelay (int64_t ns) 
{
  real_time_delay (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) 
{
  printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */

/*
 * PROJECT 1
 *
 * This is what should be used to wake up the threads in the sleep queue.
 * It should iterate through the list of sleeping threads and look at their wake-up
 * tick time and compare that value to the current time (tick). If it's time to wake up, then
 * semma_up() to wake thread and remove it from the list of sleeping threads.
 *
 * Threads will eventually need to be woken up in order of priority?
 */
static void
timer_interrupt (struct intr_frame *args UNUSED)
{
  ticks++;
  //printf("incremented ticks\n");
  thread_tick ();

  struct list_elem *e = list_begin (&sleeping_thread_list);
  struct thread *t = list_entry (e, struct thread, sleep_elem);
  
  if (t->wakeup_tick >= ticks) 
  {
    //printf("before wakeup check; ticks:%d\n", ticks);
    // sema up to wake up
    sema_up (&(t->is_sleeping));
    
    // remove from sleeping_thread_list
    
    enum intr_level old_level;
    old_level = intr_disable ();

    list_remove (e);

    intr_set_level (old_level);
  }

  
  //go through the sleeping list and check everything that needs to be woken up
  //wake if necessary
  //either yield normally, or, if latter part of B is done, yield higher priority
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) 
{
  /* Wait for a timer tick. */
  int64_t start = ticks;
  while (ticks == start)
    barrier ();

  /* Run LOOPS loops. */
  start = ticks;
  busy_wait (loops);

  /* If the tick count changed, we iterated too long. */
  barrier ();
  return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) 
{
  while (loops-- > 0)
    barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) 
{
  /* Convert NUM/DENOM seconds into timer ticks, rounding down.
          
        (NUM / DENOM) s          
     ---------------------- = NUM * TIMER_FREQ / DENOM ticks. 
     1 s / TIMER_FREQ ticks
  */
  int64_t ticks = num * TIMER_FREQ / denom;

  ASSERT (intr_get_level () == INTR_ON);
  if (ticks > 0)
    {
      /* We're waiting for at least one full timer tick.  Use
         timer_sleep() because it will yield the CPU to other
         processes. */                
      timer_sleep (ticks); 
    }
  else 
    {
      /* Otherwise, use a busy-wait loop for more accurate
         sub-tick timing. */
      real_time_delay (num, denom); 
    }
}

/* Busy-wait for approximately NUM/DENOM seconds. */
static void
real_time_delay (int64_t num, int32_t denom)
{
  /* Scale the numerator and denominator down by 1000 to avoid
     the possibility of overflow. */
  ASSERT (denom % 1000 == 0);
  busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000)); 
}
