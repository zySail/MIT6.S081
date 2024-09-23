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
} kmem;

int ref[32768] = {0}; // reference count array
#define INDEX(p) ((p-KERNBASE)/4096)

int initref(uint64 pa){
  if(pa > KERNBASE && pa < PHYSTOP){
    ref[INDEX(pa)] = 1;
  }
  else{
    return -1;
  }
  return 0;
}

int getref(uint64 pa){
  if(pa > KERNBASE && pa < PHYSTOP){
    return ref[INDEX(pa)];
  }
  else{
    return -1;
  }
}

int incrementref(uint64 pa){
  if(pa > KERNBASE && pa < PHYSTOP){
    if(ref[INDEX(pa)] < 65535){ // max ref count by myself
      acquire(&kmem.lock);
      ref[INDEX(pa)]++;
      release(&kmem.lock);
    }
    else 
      return -1;
  }
  else{
    return -1;
  }
  return 0;
}

int decrementref(uint64 pa){
  if(pa > KERNBASE && pa < PHYSTOP){
    if(ref[INDEX(pa)] > 0){
      acquire(&kmem.lock);
      ref[INDEX(pa)]--;
      release(&kmem.lock);
    }
    else  
      return -1;
  }
  else{
    return 0;
  }
  return 0;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    ref[INDEX((uint64)p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  if(getref((uint64)pa) < 0)
    panic("kfree panic");

  if(getref((uint64)pa) > 1){
    decrementref((uint64)pa);
    return;
  }

  decrementref((uint64)pa);
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE); // fill with junk
    if(getref((uint64)r) != 0) panic("kalloc");
    initref((uint64)r);
  }
  return (void*)r;
}
