// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

char kmem_names[NCPU][7];

void
kinit()
{
  for (int i = 0; i < NCPU; i++)
  {
    snprintf(kmem_names[i], 6, "kmem%d", i);
    initlock(&kmem[i].lock, kmem_names[i]);
  }
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  // int nc = ((pa - (void *)end) / PGSIZE) % NCPU;
  push_off();
  int nc = cpuid();
  pop_off();

  acquire(&kmem[nc].lock);
  r->next = kmem[nc].freelist;
  kmem[nc].freelist = r;
  release(&kmem[nc].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  
  push_off();
  int nc = cpuid();
  pop_off();

  acquire(&kmem[nc].lock);
  r = kmem[nc].freelist;
  if(r)
  {
    kmem[nc].freelist = r->next;
    release(&kmem[nc].lock);
  }
  else
  {
    release(&kmem[nc].lock);
    for (int i = 1; i < NCPU; i++)
    {
      int newnc = (nc + i) % NCPU;
      acquire(&kmem[newnc].lock);
      r = kmem[newnc].freelist;
      if(r)
      {
        kmem[newnc].freelist = r->next;
        release(&kmem[newnc].lock);
        break;
      }
      release(&kmem[newnc].lock);
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
