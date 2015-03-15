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

static struct file_desc* get_fd_struct (int fd);
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
  /* before we do anything, we should check that the stack pointer is in user space 
   * using the function provided by the TA. I think is_kernel_vaddr (f->esp) would
   * work equivalently. Maybe it's less safe. */
  if (!verify_user (f->esp)) sys_exit(-1);

  unsigned callNum;
  int args[3];
  int numOfArgs;
  struct syscall *sc_struct;

  /* get the actual system call that we need */
  /* get syscall number */
  copy_in (&callNum, f->esp, sizeof callNum);

  /* this line accesses the correct struct in our table by adding: it's like a case statement
   * but without the overhead of a case statement - pretty cool right? */
  sc_struct = all_syscalls + callNum;

  /* get the arguments to the syscall */
  copy_in (args, (uint32_t *) f->esp + 1, sizeof *args * sc_struct->num_args);
  
  /* execute the syscall, set the eax in the interrupt frame */
  f->eax = sc_struct->scf (args[0], args[1], args[2]);
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
  /* here is where we set the exit_status of a process, since every one will call exit */
  thread_current ()->process_info->exit_status = status;
  /* we're done, let's get out of here */
  thread_exit ();
}

/* per specs: Writes size bytes from buffer to open file fd. Right now, just writes
 * everything to STDOUT with putbuf (as suggested in the specs) */
static int sys_write (int fd, void *buffer, unsigned size)
{
  /* check that we're not STDOUT - if not, get the correct fd struct with get_fd_struct */
  //something like struct file_desc fd_struct = get_fd_struct (fd);  ??
  //after that, I don't yet know how to write to that file

  /* get mutex on the file system - not entirely sure if this is necessary for putbuf but
   * it should be when we try to write to files that aren't STDOUT */
  lock_acquire (&filesys_lock);
  
  /* we're good to go, let's write. NOTE: pufbuf (defined in lib/kernel/console.c writes
   * exclusively to the console so there's no chance that this is gonna write to a file. ever. */
  putbuf (buffer, size);

  lock_release (&filesys_lock);
  return size;
}

static void sys_halt (void)
{
}
//exec
static int sys_exec (const char *cmd_line) /* this int should be pid_t but it wouldn't compile */
{
  if(cmd_line == NULL || cmd_line == "" || !verify_user(cmd_line)) sys_exit(-1);

  lock_acquire (&filesys_lock);
  int pid = process_execute(cmd_line);
  lock_release (&filesys_lock);
  return pid;
}
//wait
static int sys_wait (int pid)              /* so should this one */
{
}
//create
static bool sys_create (const char *file, unsigned initial_size)
{
  if (file == NULL || file == "" || !verify_user(file)) sys_exit(-1); 

  lock_acquire (&filesys_lock);
  bool wasCreated = filesys_create (file, (off_t) initial_size);
  lock_release (&filesys_lock);
  return wasCreated;
}
//remove
static bool sys_remove (const char *file)
{
}
//open
static int sys_open (const char *file)
{
  if (file == NULL || !verify_user(file)) sys_exit(-1);
  if (file == "") sys_exit(0);

  lock_acquire (&filesys_lock);
  struct file *f = filesys_open(file);

  if (f == NULL) sys_exit(0);

  //int fd = &(&(*f).inode).magic; // is magic the correct number to return?
  struct thread *cur = thread_current ();
  int fd = cur->next_fd;
  fd++;
  
  struct file_desc fd_struct;
  fd_struct.fd = fd;
  fd_struct.f = f;
 // fds
  
  struct list *fd_list = &(cur->fds);
  struct list_elem *e = &(fd_struct.elem);
  list_push_back (fd_list, e);

  lock_release (&filesys_lock); 
  return fd;
}
//filesize
static int sys_filesize (int fd)
{
  /* use file_length defined in file.c */
  int filesize = 0;
  struct file_desc *fd_struct = get_fd_struct (fd);
  lock_acquire (&filesys_lock);
  /* the following line goes file_length -> inode_length -> inode -> inode_disk (data) -> length member */
  filesize = file_length (fd_struct->f);
  lock_release (&filesys_lock);

  return filesize;
}
//read
static int sys_read (int fd, void *buffer, unsigned size)
{
  
}
//seek
/* check out file_seek, probably does what we want here */
static void sys_seek (int fd, unsigned position)
{
}
//tell
/* notes: file_tell in file.c seems to do exactly what we need here (maybe file_tell(...) + 1? */
/* I'm not convinced that this is even tested */
static unsigned sys_tell (int fd)
{
  struct file_desc *fd_struct = get_fd_struct (fd);
  return file_tell (fd_struct->f);
}
//close
static void sys_close (int fd)
{
  
}


/* this function searches through the current thread's fd list and returns the one
 * that matches the given argument fd (file descriptor) */
static struct file_desc* get_fd_struct (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct file_desc* temp_fd_struct;
  int i = 0;
  /* iterate through the file descriptor (of current thread) list and return the correct struct */
  for (e = list_begin (&cur->fds); e != list_end (&cur->fds); e = list_next (e))
  {
    temp_fd_struct = list_entry (e, struct file_desc, elem);
    if (temp_fd_struct->fd == fd) return temp_fd_struct;
  }
  /* if we get this far, we shouldn't return, we should just exit since we can't do what we intended */
  thread_exit ();
}
