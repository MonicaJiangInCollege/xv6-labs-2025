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

#define SUPERPG_SIZE (1 << 21)    // 2MB
#define SUPERPG_PAGES (SUPERPG_SIZE / PGSIZE) // 512 × 4KB

// 预留16块2MB、强制2MB对齐的物理内存；数量足够跑 pgtbltest
char super_pool[16][SUPERPG_SIZE] __attribute__((aligned(SUPERPG_SIZE)));
int super_used[16] = {0};  // 0=空闲，1=已分配
struct spinlock superlock; // 保护超级页池并发

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&superlock, "superpool"); // 新增锁
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

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// 返回2MB对齐的整块物理内存；失败返回0
void*
superalloc(void)
{
  acquire(&superlock);
  for(int i = 0; i < 16; i++){
    if(super_used[i] == 0){
      super_used[i] = 1;
      release(&superlock);
      memset(super_pool[i], 5, SUPERPG_SIZE); // 填充垃圾，模仿kalloc
      return super_pool[i];
    }
  }
  release(&superlock);
  return 0; // 池耗尽
}

// 只能释放superalloc返回的整块2MB；禁止部分释放
void
superfree(void *pa)
{
  // 地址必须落在super_pool内、2MB对齐
  uint64 p = (uint64)pa;
  if(p % SUPERPG_SIZE != 0){
    panic("superfree: unaligned");
  }
  acquire(&superlock);
  for(int i = 0; i < 16; i++){
    if(pa == super_pool[i]){
      if(super_used[i] == 0)
        panic("superfree double free");
      memset(pa, 1, SUPERPG_SIZE); // 模仿kfree填充垃圾
      super_used[i] = 0;
      release(&superlock);
      return;
    }
  }
  release(&superlock);
  panic("superfree: not a superpage");
}