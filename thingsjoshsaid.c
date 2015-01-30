/*
QUESTION LIST:

1. are we sleeping the thread correctly? (are we waking it up correctly?)
2. do we need two semaphores?

make changes to the thread class instead of making a contrived struct for sleeping threads

manually do the interrupt disables, rather than using the semaphore (also, use the save state stuff
that's essentially found in the semaphore code)

use list_insert_ordered and just check that one and pop them off

don't initialize the semaphore every time the function is called
call sema_init within init_threads and move all the stuff to threads.c
*/

struct sleeping_thread {
    struct list_elem elem;
    struct thread *t;

    int64_t wakeup_tick;
    struct semaphore is_sleeping;
};

/* Returns true is A is less than B, or false if A is greater than or equal to B. */
typedef bool list_less_func (const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux);
