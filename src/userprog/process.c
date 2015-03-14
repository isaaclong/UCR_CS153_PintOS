#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "lib/string.h"
#include "threads/synch.h"
/* PROJECT 2: include malloc as well (for our process_info structs) */
#include "threads/malloc.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmd_line, void (**eip) (void), void **esp);
static bool setup_stack_helper (const char * cmd_line, uint8_t * kpage, uint8_t * upage, void ** esp);
void free_info (struct process_info *pi);
/* 
 * Struct used to share between process_execute() in the
 * invoking thread and start_process() inside the newly 
 * invoked thread.
 */

struct exec_helper
{
    const char *file_name;    /* program to load */
    //add semaphore for loading (for resource race cases)
    struct semaphore load_done;
    //add bool for determining if program loaded successfully
    struct process_info *process_info;
    //add other stuff to transfer between process_execute and process_start
    bool success;
    //  hint: need a way to add to the child's list, wee [sic] below about thread's child list
};



// kpage is created in setup_stack
// x is all the values argv, argc, and null (need null on the stack!)
// be careful of the order of argv! Check stack example

/* Pushes the SIZE bytes in BUF onto the stack in KPAGE, whose page-
 * relative stack pointer is *OFS, and then adjusts *OFS appropriately.
 * The bytes pushed are rounded to a 32-bit boundary.
 *
 * If successful, returns a pointer to the newly pushed object.
 * On failure, returns a null pointer. */

static void *
push (uint8_t *kpage, size_t *ofs, const void *buf, size_t size)
{
    size_t padsize = ROUND_UP (size, sizeof (uint32_t));
    if (*ofs < padsize) return NULL;
    *ofs -= padsize;
    memcpy (kpage + *ofs + (padsize - size), buf, size);
    return kpage + *ofs + (padsize - size);
}

/* reverse the order of argc pointers, esentially reversing the order of argv */
void reverse_args (int argc, char **argv)
{
  int i;
  /* from the front [0] to the middle [(argc - 1) / 2], change the order of the args */
  for (i = 0; i <= (argc - 1) / 2; ++i) {
    /* simple swap function, but it looks sorta complex */
    char *tmp = argv[i];
    argv[i]   = argv[(argc - 1) - i];
    argv[(argc - 1) - i] = tmp;
  }
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  struct exec_helper exec;
  char thread_name[16];
  char *fn_copy; //suggested deletion on this line
  tid_t tid;
  char* saveptr;

  //set exec file name here
  exec.file_name = file_name;
  //initialize a semaphore for loading here
  sema_init (&exec.load_done, 0);


  /* SUGGESTED: REMOVE THE BELOW FROM HERE ----------------*/
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
//  fn_copy = palloc_get_page (0);
//  if (fn_copy == NULL)
//    return TID_ERROR;
//  strlcpy (fn_copy, file_name, PGSIZE);

  /* UNTIL HERE ------------------- */

  //##add program name to thread_name, watch out for the size, strtok_r....
  /* copy file name into thread_name to init the thread (make it 16 as specified in thread.h) */
  strlcpy (thread_name, file_name, sizeof (thread_name));
  /* tokenize the first arg */
  strtok_r (thread_name, " ", &saveptr);
  tid = thread_create (thread_name, PRI_DEFAULT, start_process, &exec);

  if (tid != TID_ERROR) // change to !=
  {
    //down a semaphore for loading (where should you up it?) --> in start_process!
    /* this is upped in start_process after the process has been, well, started. */
    sema_down (&exec.load_done); 

    //if program loaded successfully, add new child to the list of this thread's children (mind list_elems)...we need to check this list in process wait, when children are done, process wait can finish, see process wait
    /* if it successfully loaded, push the info struct onto the children list */
    if (exec.success)
    {
      list_push_back (&(thread_current ()->children), &(exec.process_info->elem));
    }
    else
    {
      tid = TID_ERROR;
    }
    //palloc_free_page (fn_copy); //suggested: remove this line
  }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
/* PROJECT 2: changed how this function works so that it uses the exec_helper structs 
 *  (it's just one more layer of indirection, what could go wrong?) */
static void
start_process (void *exec_)
{
  struct exec_helper *exec = exec_;  //added
  //char *file_name = file_name_; //no longer necessary
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (exec->file_name, &if_.eip, &if_.esp);  //file_name became exec->file_name

  /* PROJECT 2 */
  /* since we create the process with thread_create in process_execute, we have to 
   * deal with allocating memory for process_info here */
  if (success)
  {
    /* the following lines allocate memory for the process_info struct of the executing thread */
    thread_current ()->process_info = malloc (sizeof (*exec->process_info));
    exec->process_info = thread_current ()->process_info;

    /* since we initialize process_info to NULL, if it's still NULL then it's not a success */
    success = (NULL != exec->process_info);
  }
  
  /* we also have to initialize process_into since it's the first time it is in use */
  if (success)
  {
    lock_init (&exec->process_info->lock);
    exec->process_info->tid = thread_current ()->tid;
    exec->process_info->alive_count = 2;         /* 2 -> both parent and child are alive at this point */
    exec->process_info->exit_status = -1;        /* -1 indicates failure (it'll be set later) */
    sema_init (&exec->process_info->alive, 0);
  }

  /* alert that parent thread that the loading has happened. */
  exec->success = success;
  /* up the semaphore that we downed in process_execute */
  sema_up (&exec->load_done);       /* the helper really helps here */

  /* If load failed, quit. */
  //palloc_free_page (exec->file_name);  //turns out we don't need this line because we allocated already
  if (!success) 
  {
    thread_exit ();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
/* UPDATE: now this goes through every child of the current process
 * and finds the one matching the passed-in child_tid. If it is found,
 * it indicates that the child is no longer alive and removes it from
 * the child list. This differs from the suggested implementation in
 * that is uses a semaphore to tell when the kid has exited. */

int
process_wait (tid_t child_tid) 
{
  /* PROJECT 2: added psuedo code */
  //struct process_info* pi = get_child_process (child_tid);
  //the above line will mean we have to write a function to do that...probably in thread.c?
  //probably just a for loop through the all list?
  //while (1);
  struct thread *cur = thread_current ();
  struct list_elem *kids;       /* holds a process's list of children as we iterate */
  for (kids = list_begin (&(cur->children)); kids != list_end (&(cur->children)); kids = list_next (kids))
  {
    struct process_info *kid = list_entry (kids, struct process_info, elem);
    if (child_tid == kid->tid)
    {
      /* tell the parent that the child is now not running */
      sema_down (&kid->alive);
      /* remove it from the child list */
      list_remove (kids);
      return kid->exit_status;
    }
  }
  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  /* PROJECT 2: get some list elems going */
  struct list_elem *le, *next;

  /* close the exec so that we can write */
  file_close (cur->executable);

  if (cur->process_info != NULL)
  {
    /* grab the struct temporarily so we can free it */
    struct process_info *pi = cur->process_info;
    /* need this for tests to pass (gives exit status of processes */
    printf ("%s: exit(%d)\n", cur->name, pi->exit_status);
    /* release the process_info struct */
    free_info (pi);
  }

  /* destroy all the child_list entries */
  for (le = list_begin (&cur->children); le != list_end (&cur->children); le = next)
  {
    /* get the process info of each child, advance next with list_remove, and free the entry */
    struct process_info *pi = list_entry (le, struct process_info, elem);
    next = list_remove (le);
    free_info (pi);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* PROJECT 2 */
#define MAX_ARGS 3

static bool setup_stack (void **esp, char *cmd_line);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmd_line, void (**eip) (void), void **esp)  //change file name to cmd_line
{
  struct thread *t = thread_current ();
  char file_name[NAME_MAX+2]; //add a file name variable here, the file_name and cmd_line are different
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
//  char* charPointer;  //added for parsing
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  
  /* PROJECT 2 */
  //use strtok_r to remove file_name from cmd_line
  char *token, *save_ptr;
  char *parsed_args[MAX_ARGS];
  i = 0;

  /* copy all of cmd_line into file_name */
  strlcpy (file_name, cmd_line, sizeof (file_name));
  /* get the pointer to the first space character in the string */
  char *charPointer = strchr (file_name, ' ');
  /* set the pointer that we found */
  if (charPointer != NULL) *charPointer = '\0';

  file = filesys_open (file_name);
  t->executable = file;

  // first call to strtok_r returns first argument
  // subsequent calls return subsequent arguments until NULL which 
  // means done
  /* note: this method below has been commented because it caused undefined behavior */

/*
  for (token = strtok_r (cmd_line, " ", &save_ptr); token != NULL && i < MAX_ARGS;
       token = strtok_r (NULL, " ", &save_ptr))
  {
    parsed_args[i] = token;
    i++;
  }

  char *prog_name = parsed_args[0];
  printf ("prog_name: %s\n", prog_name);
*/  

  //end of implied blank space

//  file = filesys_open (prog_name); // set the thread's bin file to this as well! it is super helpful to have

//  printf ("@@@@@@@@@@@@@@@@@@@@@@@@@load (after filesys_open)\n");

//  t->executable = file;
  // each thread have a pointer to the file they are using for when you need to close it in process_exit

  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  //disable file write for 'file' here. GO TO BOTTOM. DON'T CHANGE ANYTHING IN THESE IF AND FOR STATEMENTS
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  //if (!setup_stack (esp))  // add cmd_line to setup_stack param here, also change setup_stack
  if (!setup_stack (esp, cmd_line))  // add cmd_line to setup_stack param here, also change setup_stack
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */

  // TODO: push all arguments on to stack 

//  file_close (file);        //remove this!!!!!!!! (yes eight) since
                            //thread has its own file, close it when process is done (hint: in process exit)
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* PROJECT 2 */

static bool setup_stack_helper (const char * cmd_line, uint8_t * kpage, uint8_t * upage, void ** esp) 
{
  size_t ofs = PGSIZE; //##Used in push!
  char * const null = NULL; //##Used for pushing nulls
  //char *saveptr; //##strtok_r usage
  //##Probably need some other variables here as well...
  char *temp_cmd_line;
  int argc;  
  char **argv;
  
  //##Parse and put in command line arguments, push each value
  temp_cmd_line = push (kpage, &ofs, cmd_line, strlen (cmd_line) + 1);
  //##if any push() returns NULL, return false
  if (temp_cmd_line == NULL) return false;
  
  //##push() a null (more precisely &null).
  //##if push returned NULL, return false
  if (push (kpage, &ofs, &null, sizeof (null)) == NULL) return false;

  //use strtok_r to remove file_name from cmd_line
  char *token, *save_ptr;
  char *parsed_args[MAX_ARGS];
  int i = 0;

  // first call to strtok_r returns first argument
  // subsequent calls return subsequent arguments until NULL which 
  // means done
  for (token = strtok_r (temp_cmd_line, " ", &save_ptr); token != NULL && i < MAX_ARGS;
       token = strtok_r (NULL, " ", &save_ptr))
  {
    void *cur_uarg = upage + (token - (char *) kpage);
    if (push (kpage, &ofs, &cur_uarg, sizeof (cur_uarg)) == NULL) return false;
    //parsed_args[i] = token;
    i++;
  }

  /* after iterating through the list, argc should be i */
  argc = i;
  argv = (char **) (upage + ofs);
  reverse_args (argc, (char **) (kpage + ofs));

  /* push argv, argc, null to indicate end */
  if (push (kpage, &ofs, &argv, sizeof (argv)) == NULL
   || push (kpage, &ofs, &argc, sizeof (argc)) == NULL
   || push (kpage, &ofs, &null, sizeof (null)) == NULL)
  {
    return false;
  }

/*

  *esp=*esp-4+(sizeof(null) + 1)%4; // do this for every push?
  //##Push argv addresses (i.e. for the cmd_line added above) in reverse order
  //size_t bufsize = sizeof (parsed_args[0]);
  //push (kpage, &ofs, &parsed_args[0], bufsize); 

  //##Push argc, how can we determine argc?
  push (kpage, &ofs, &i, sizeof (i));
  *esp=*esp-4+(sizeof(i) + 1)%4;

  //##See the stack example on documentation for what "reversed" means
  //##Push &null
  push (kpage, &ofs, &null, sizeof(null));
  //##Should you check for NULL returns?
 
*/

  //##Set the stack pointer. IMPORTANT! Make sure you use the right value here...
  *esp = upage + ofs;

  //##If you made it this far, everything seems good, return true
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */

// added parameter for parsed command line args
static bool
setup_stack (void **esp, char *cmd_line) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      //success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      uint8_t *upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
      if (install_page (upage, kpage, true))
      {
        success = setup_stack_helper (cmd_line, kpage, upage, esp);
      }
      else
      {
        palloc_free_page (kpage);
      }
      // TODO: push all arguments onto stack in reverse order, i.e. push "y\0", push "x\0" push "echo\0"
      // remember the addresses on stack where these strings are pushed as later we need to push those 
      // addresses too
      /*
      if (success)
      {
        *esp = PHYS_BASE;
        setup_stack_helper (&cmd_line, kpage, ((uint8_t *) PHYS_BASE) - PGSIZE, esp);
      }
      */
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/* PROJECT 2: since we used malloc for our process_info, we have to free it as well */
void free_info (struct process_info *pi)
{
  /* before we can free it, we have to check if the processes are alive or not */
  int temp_alive_count;
  /* ensure mutex on the count */
  lock_acquire (&pi->lock);
  /* we have to use this temp variable to work with the locks */
  temp_alive_count = --pi->alive_count;
  lock_release (&pi->lock);
  sema_up (&pi->alive);          /* it was initialized to 0, so we up it here */

  if (temp_alive_count == 0) free (pi);
}
