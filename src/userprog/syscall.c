#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>             /* defines the numbers for syscalls in an enum */
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

/* we have to make sure that we keep the syscalls in the order that they're defined */

//halt
static void sys_halt (void);
//exit
static void sys_exit (int status);
//exec
static int sys_exec (const char *cmd_line); /* this int should be pid_t but it wouldn't compile */
//wait
static int sys_wait (int pid);              /* so should this one */
//create
static bool sys_create (const char *file, unsigned initial_size);
//remove
static bool sys_remove (const char *file);
//open
static int sys_open (const char *file);
//filesize
static int sys_filesize (int fd);
//read
static int sys_read (int fd, void *buffer, unsigned size);
//write
static int sys_write (int fd, void *usrc_, unsigned size);
//seek
static void sys_seek (int fd, unsigned position);
//tell
static unsigned sys_tell (int fd);
//close
static void sys_close (int fd);

static void copy_in (void *dst_, const void *usrc_, size_t size);
static inline bool get_user (uint8_t *dst, const uint8_t *usrc);
static bool verify_user (const void *uaddr);
static inline bool get_user (uint8_t *dst, const uint8_t *usrc);

static void syscall_handler (struct intr_frame *);
struct lock filesys_lock;           /* we need something to ensure mutex on the file system */

struct file_desc
{
  struct list_elem elem;            /* keeps it in a list */
  struct file *f;                   /* keeps track of the file that the descriptor describes */
  int fd;                           /* the actual number of the file descriptor */
};

typedef int sc_func (int, int, int);
struct syscall
{
  sc_func *scf;     /* the function itself */
  int num_args;     /* the number of arguments */
};
  
/* keep these in the order as defined - that way we can just add to get the call */
const struct syscall all_syscalls[] = 
{
  {(sc_func *) sys_halt,     0},
  {(sc_func *) sys_exit,     1},
  {(sc_func *) sys_exec,     1},
  {(sc_func *) sys_wait,     1},
  {(sc_func *) sys_create,   2},
  {(sc_func *) sys_remove,   1},
  {(sc_func *) sys_open,     1},
  {(sc_func *) sys_filesize, 1},
  {(sc_func *) sys_read,     3},
  {(sc_func *) sys_write,    3},
  {(sc_func *) sys_seek,     2},
  {(sc_func *) sys_tell,     1},
  {(sc_func *) sys_close,    1},
};

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  printf ("system call!\n");

  unsigned callNum;
  int args[3];
  int numOfArgs;
  struct syscall *sc;

  /* get the actual system call that we need */
  /* get syscall number */
  copy_in (&callNum, f->esp, sizeof callNum);
  if (callNum >= sizeof (all_syscalls) / sizeof (*all_syscalls))
  {
    thread_exit ();
  }
  sc = all_syscalls + callNum;

  /* get the arguments to the syscall */
  memset (args, 0, sizeof (args));
  copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * numOfArgs);

  /* check that the pointer to the stack is valid */
  if (!verify_user (f->esp)) sys_exit (-1);
  
  //numOfArgs = number of args that the system call uses
  
  /* execute the syscall, set the eax in the interrupt frame */
  f->eax = sc->scf (args[0], args[1], args[2]);

  thread_exit ();
}

/* Copies SIZE bytes from user address USRC to kernel address
   DST.
   Call thread_exit() if any of the user accesses are invalid. */
static void
copy_in (void *dst_, const void *usrc_, size_t size) 
{
  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;
 
  for (; size > 0; size--, dst++, usrc++) 
    if (usrc >= (uint8_t *) PHYS_BASE || !get_user (dst, usrc)) 
      thread_exit ();
}




/* Creates a copy of user string US in kernel memory
   and returns it as a page that must be freed with
   palloc_free_page().
   Truncates the string at PGSIZE bytes in size.
   Call thread_exit() if any of the user accesses are invalid. */
static char *
copy_in_string (const char *us) 
{
  char *ks;
  size_t length;
 
  ks = palloc_get_page (0);
  if (ks == NULL) 
    thread_exit ();
 
  for (length = 0; length < PGSIZE; length++)
    {
      if (us >= (char *) PHYS_BASE || !get_user (ks + length, us++)) 
        {
          palloc_free_page (ks);
          thread_exit (); 
        }
       
      if (ks[length] == '\0')
        return ks;
    }
  ks[PGSIZE - 1] = '\0';
  return ks;
}


/* Copies a byte from user address USRC to kernel address DST.
   USRC must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static inline bool
get_user (uint8_t *dst, const uint8_t *usrc)
{
  int eax;
  asm ("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:"
       : "=m" (*dst), "=&a" (eax) : "m" (*usrc));
  return eax != 0;
}




/* Returns true if UADDR is a valid, mapped user address,
   false otherwise. */
static bool
verify_user (const void *uaddr) 
{
  return (uaddr < PHYS_BASE
          && pagedir_get_page (thread_current ()->pagedir, uaddr) != NULL);
}

static void sys_exit (int status)
{
  thread_current ()->process_info->exit_status = status;
  thread_exit ();
  NOT_REACHED ();
}

static struct file_desc* find_fd (int fd)
{
  printf ("we're in find_fd after a call from sys_write\n");
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct file_desc* temp_fd_struct;
  int i = 0;
  /* iterate through the file descriptor (of current thread) list and return the correct struct */
  for (e = list_begin (&cur->fds); e != list_end (&cur->fds); e = list_next (e))
  {
    printf ("i = %d\n", i);
    temp_fd_struct = list_entry (e, struct file_desc, elem);
    if (temp_fd_struct->fd == fd) printf ("ayyyy lmao we found the right one apparently\n");
    if (temp_fd_struct->fd == fd) return temp_fd_struct;
  }
  /* if we get this far, we shouldn't return, we should just exit since we can't do what we intended */
  thread_exit ();
}

static int sys_write (int fd, void *usrc_, unsigned size)
{

  printf ("sys_write happens so don't even worry about it bro\n");
  int byte_count = 0;             /* keeps track of how many bytes we've written */
  struct file_desc *fd_struct = NULL;
  uint8_t *usrc = usrc_;

  /* check that we're not STDOUT - if not, get the correct fd struct with find_fd */
  //if (fd != 1) fd_struct = find_fd (fd);

  printf ("just making sure - we don't get this far in sys_write (past find_id)\n");

  /* get mutex on the file system */
  lock_acquire (&filesys_lock);
  
  while (size > 0)
  {
    /* get the amount of pages left */
    size_t page_left = PGSIZE - pg_ofs (usrc);
    size_t write_amt = size < page_left ? size : page_left;
    off_t retval;

    /* check that we're allowed to write to this page (as described in design doc */
    if (!verify_user (usrc))
    {
      /* if we fail this check, we better get out of here quick */
      lock_release (&filesys_lock);
      thread_exit ();
    }
    
    /* we're good to go, let's write */
//    if (fd == 1)
//    {
//      putbuf (usrc, write_amt);
//      retval = write_amt;
//    }
//    else
//    {
      retval = file_write (fd_struct->f, usrc, write_amt);
//    }
    if (retval < 0)
    {
      if (byte_count == 0) byte_count = -1;
      break;
    }
    byte_count += retval;

    /* if it was a short write we're done here */
    if (retval != (off_t) write_amt) break;

    /* advance to the next block to write */
    usrc += retval;
    size -= retval;
  }
  lock_release (&filesys_lock);
  return byte_count;
}

static void sys_halt (void)
{
}
//exec
static int sys_exec (const char *cmd_line) /* this int should be pid_t but it wouldn't compile */
{
}
//wait
static int sys_wait (int pid)              /* so should this one */
{
}
//create
static bool sys_create (const char *file, unsigned initial_size)
{
}
//remove
static bool sys_remove (const char *file)
{
}
//open
static int sys_open (const char *file)
{
}
//filesize
static int sys_filesize (int fd)
{
}
//read
static int sys_read (int fd, void *buffer, unsigned size)
{
}
//seek
static void sys_seek (int fd, unsigned position)
{
}
//tell
static unsigned sys_tell (int fd)
{
}
//close
static void sys_close (int fd)
{
}
