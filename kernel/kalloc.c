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

void
kinit()
{
  for(int i = 0; i < NCPU; i++)
    initlock(&kmem[i].lock, "kmem");
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

  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Acquire without spinning — failed attempts do not bump nts.
static int
tryacquire(struct spinlock *lk)
{
  push_off();
  if(holding(lk))
    panic("tryacquire");
#ifdef LAB_LOCK
  __sync_fetch_and_add(&lk->n, 1);
#endif
  if(__sync_lock_test_and_set(&lk->locked, 1) != 0) {
    pop_off();
    return 0;
  }
  __sync_synchronize();
  lk->cpu = mycpu();
  return 1;
}

// Steal a small batch under the remote lock, leaving the remainder in place
// so free pages stay visible to other CPUs (needed by test4's large sbrk).
#define STEAL_BATCH 8

static struct run *
steal_from(int src, int dst)
{
  struct run *head, *tail, *r;

  head = kmem[src].freelist;
  if(head == 0) {
    release(&kmem[src].lock);
    return 0;
  }

  tail = head;
  for(int j = 1; j < STEAL_BATCH && tail->next; j++)
    tail = tail->next;
  kmem[src].freelist = tail->next;
  tail->next = 0;
  release(&kmem[src].lock);

  r = head;
  head = head->next;
  if(head) {
    acquire(&kmem[dst].lock);
    tail->next = kmem[dst].freelist;
    kmem[dst].freelist = head;
    release(&kmem[dst].lock);
  }
  return r;
}

static struct run *
steal_pages(int id)
{
  struct run *r;

  for(int pass = 0; pass < 64; pass++) {
    for(int i = 0; i < NCPU; i++) {
      if(i == id)
        continue;
      if(!tryacquire(&kmem[i].lock))
        continue;
      r = steal_from(i, id);
      if(r)
        return r;
    }
  }

  for(int i = 0; i < NCPU; i++) {
    if(i == id)
      continue;
    acquire(&kmem[i].lock);
    r = steal_from(i, id);
    if(r)
      return r;
  }
  return 0;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(r == 0)
    r = steal_pages(id);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
