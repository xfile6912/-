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
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
	  return TID_ERROR;

  strlcpy (fn_copy, file_name, PGSIZE);
  struct file *file = NULL;
  char *command;
  char *copiedName=(char*)malloc(sizeof(char)*PGSIZE);//file name을 복사함 strtok_r을 filename으로 하게되면 file_name을 변경해버리므로, copied본을 이용해 strtok.
  char *next;
  struct thread *cur;
  strlcpy(copiedName, file_name, PGSIZE);
  command=strtok_r(copiedName, " ", &next);
  file = filesys_open (command);//file이 있는지 없는지 확인해줌.
  if (file == NULL)//없으면 -1리턴
  {
	  free(copiedName);
	  return -1;
  }


  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (command, PRI_DEFAULT, start_process, fn_copy);
  //printf("%d", tid);
  free(copiedName);
  cur=thread_current();
  sema_down(&(cur->load));//child가 load되기 전에 부모가 끝나는 경우가 생길수 있으므로 이를 막아주기위해서
  if (tid == TID_ERROR)
	  palloc_free_page (fn_copy); 

  struct list *childList=&cur->childList;
  struct thread *children;
  struct list_elem *elePtr;
  int flag=0;
  elePtr=list_prev(list_end(childList));
  children=list_entry(elePtr, struct thread, current);
  if(children->tid==tid && children->exitStatus==-1)
	  flag=1;
  if(flag==1)
	tid=process_wait(tid);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
	static void
start_process (void *file_name_)
{
	char *file_name = file_name_;
	struct intr_frame if_;
	bool success;
	/* Initialize interrupt frame and load executable. */
	memset (&if_, 0, sizeof if_);
	if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
	if_.cs = SEL_UCSEG;
	if_.eflags = FLAG_IF | FLAG_MBS;
	success = load (file_name, &if_.eip, &if_.esp);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	struct thread *cur=thread_current();
	sema_up(&(cur->parent->load));//child가 load 되었으므로부모는 이제 실행가능
	if (!success)
	{
		exit(-1);
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
	int
process_wait (tid_t child_tid UNUSED) 
{
	struct thread *cur=thread_current();//현재 thread의 자식들 전부 검사함. tid와 같은 것이 있는지
	struct list *childList=&cur->childList;
	struct thread *children;
	struct list_elem *elePtr;
	int retStatus=-1;
	int flag=0;
	for(elePtr=list_begin(childList); elePtr!=list_end(childList); elePtr=list_next(elePtr))
	{
		children=list_entry(elePtr, struct thread, current);
		tid_t currentid=children->tid;
		if(child_tid==currentid)//자식이 종료 되었다면 wait을 끝내도 되지만 아니면 양보해야함.
		{
			flag=1;//child tid와 같은 것을 발견함.
			break;
		}
	}
	if(flag==1)
	{
		while(children->exitFlag==0)//종료되지 않았다ㅏ면
		{
			thread_yield();
		}
		retStatus=children->exitStatus;
		list_remove(&(children->current));//수행이 끝난 children은 부모의 child list에서 제거해줌
		//부모가 시체 처리해줌.이러고 난 이후에 child는 확실하게 exit할수 있음.
		children->exitFlag=2;//부모가 시체까지 처리해준 상태.

	}
	return retStatus;
}

/* Free the current process's resources. */
	void
process_exit (void)
{
	struct thread *cur = thread_current ();
	uint32_t *pd;
	/* Destroy the current process's page directory and switch back
	   to the kernel-only page directory. */

	cur->exitFlag=1;//부모에게  끝난 것을 처리해줘도 괜찮다는 메시지 보냄.
	while(cur->exitFlag!=2)//부모가 끝났ㄴ것을 처리까지해 주면 비로소 프로세스가 종료가능.
	{
		thread_yield();
	}

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

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
	bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
	struct thread *t = thread_current ();
	struct Elf32_Ehdr ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	char *command;
	char *next;
	int argCnt=0;//argument count->numberOfArgument;
	char *arg[100];//parsing한 argument들이 들어가 있음. arg[0]에는 command가 들어가 잇음
	uint32_t *address[100];//stack 속의 argument의 address를 저장

	/* Allocate and activate page directory. */
	t->pagedir = pagedir_create ();
	if (t->pagedir == NULL) 
		goto done;
	process_activate ();
	//todo:parsing
	command=strtok_r(file_name, " ", &next);
	arg[argCnt++]=command;
	while(arg[argCnt-1])//parsing argument
	{
		arg[argCnt++]=strtok_r(NULL," ", &next);
	}
	argCnt-=1;//실제 argCnt는 -1을 해줘야함.
	/* Open executable file. */
	file = filesys_open (command);
	if (file == NULL) 
	{
		printf ("load: %s: open failed\n", command);
		goto done; 
	}
	//	file_deny_write(file);
	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 3
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
			|| ehdr.e_phnum > 1024) 
	{
		//printf("%s", t->name);
		/*		if(ehdr.e_type != 2)
				{
				printf("1\n");
				}
				if(ehdr.e_machine != 3)
				{
				printf("2\n");
				}
				if( ehdr.e_version != 1)
				{
				printf("3\n");
				}
				if(ehdr.e_phentsize != sizeof (struct Elf32_Phdr))
				{
				printf("4\n");
				}
				if(ehdr.e_phnum > 1024)
				{
				printf("5\n");

				}
				if(file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr)
				{
				printf("6\n");
				}
		 */
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
	if (!setup_stack (esp))
		goto done;

	//contruct stack
	for(i=argCnt-1; i>=0; i--)//push argv[argCnt-1]~argv[0]
	{
		int argLength=strlen(arg[i])+1;
		*esp-=argLength;//make the space for argument
		strlcpy(*esp, arg[i], argLength);//stack에 arg넣어줌.
		address[i]=*esp;
	}
	while(1)//word alignment
	{
		int temp=*esp;
		if(temp % sizeof(uint32_t) ==0)//4로 나뉘어 지면 wordalignment 필요없음
			break;
		*esp-=sizeof(uint8_t);
		*(uint8_t *)*esp=0;

	}
	for(i=argCnt; i>=0; i--)//push stack a address of argv[argCnt]~argv[0];
	{
		*esp-=sizeof(uint32_t*);
		if(i==argCnt)
			*(uint32_t*)*esp=0;//argv[argCnt] address is null.
		else
			*(uint32_t*)*esp=address[i];
	}

	uint32_t *argv=*esp;//push stack address of argv
	*esp-=sizeof(uint32_t);
	*(uint32_t *)*esp=argv;

	*esp-=sizeof(uint32_t);//push stack argc.
	*(uint32_t*)*esp=argCnt;

	*esp-=sizeof(uint32_t);
	*(uint32_t*)*esp=0;//push stack null;
	//  hex_dump(*esp, *esp, 200, true);
	/* Start address. */
	*eip = (void (*) (void)) ehdr.e_entry;
	success = true;


	//free memory allocation things
	// free(command);
done:
	/* We arrive here whether the load is successful or not. */
	file_close (file);
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

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
	static bool
setup_stack (void **esp) 
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) 
	{
		success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
		if (success)
			*esp = PHYS_BASE;
		else
			palloc_free_page (kpage);
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