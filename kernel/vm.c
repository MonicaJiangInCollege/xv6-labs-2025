#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "vm.h"

#define SUPERPG_SIZE (1 << 21)    // 2MB
#define SUPERPG_MASK (SUPERPG_SIZE - 1)

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
#ifdef LAB_PGTBL
      if(PTE_LEAF(*pte)) {
        return pte;
      }
#endif
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// walkpte: 走到指定level，不分配下级页表
// level=2:L2, level=1:L1, level=0:L0
pte_t *
walkpte(pagetable_t pagetable, uint64 va, int target_level)
{
  if(va >= MAXVA)
    panic("walkpte");

  pagetable_t pt = pagetable;
  for(int level = 2; level > target_level; level--){
    pte_t *pte = &pt[PX(level, va)];
    if(!(*pte & PTE_V)){
      return 0;
    }
#ifdef LAB_PGTBL
    if(PTE_LEAF(*pte)){
      return pte;
    }
#endif
    pt = (pagetable_t)PTE2PA(*pte);
  }
  return &pt[PX(target_level, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
#ifdef LAB_PGTBL
  if(walkpte(pagetable, va, 1) == pte)
    pa += va & SUPERPG_MASK;
#endif
  return pa;
}

// va/pa必须2MB对齐；在pagetable中安装L1超级页
int
install_superpage(pagetable_t pagetable, uint64 va, uint64 pa)
{
  if((va & SUPERPG_MASK) != 0 || (pa & SUPERPG_MASK) != 0)
    panic("install_superpage unaligned");

  pte_t *l1pte = walkpte(pagetable, va, 1);
  if(l1pte == 0)
    return -1;
  if(*l1pte & PTE_V)
    panic("install_superpage: already mapped");

  *l1pte = PA2PTE(pa) | PTE_V | PTE_R | PTE_W | PTE_U;
  return 0;
}

static int
demote_superpage(pagetable_t pagetable, uint64 va)
{
  pte_t *l1pte = walkpte(pagetable, va, 1);

  if(l1pte == 0)
    return -1;

  if(!(*l1pte & PTE_V) || !PTE_LEAF(*l1pte))
    return -1;


  uint64 oldpa = PTE2PA(*l1pte);
  int flags = PTE_FLAGS(*l1pte);


  // 创建新的L0页表
  pagetable_t l0 = (pagetable_t)kalloc();
  if(l0 == 0)
    return -1;

  memset(l0, 0, PGSIZE);


  // 把superpage复制成512个普通4KB页
  for(int i = 0; i < 512; i++){

    char *mem = kalloc();

    if(mem == 0){
      // 简单处理：失败直接panic
      panic("demote kalloc");
    }

    // 复制原superpage中的这一页
    memmove(mem,
            (char*)(oldpa + i * PGSIZE),
            PGSIZE);


    l0[i] = PA2PTE((uint64)mem)
          | (flags & ~PTE_V)
          | PTE_V;
  }


  // 释放原来的2MB superpage
  superfree((void*)oldpa);


  // L1现在指向普通L0页表
  *l1pte = PA2PTE(l0) | PTE_V;


  return 0;
}

#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
static void
print_helper(pagetable_t pt, int level, int depth, uint64 va_prefix)
{
  pte_t *pgtab = pt;


  for(int i=0;i<512;i++){

    pte_t pte=pgtab[i];

    if(!(pte&PTE_V))
      continue;


    uint64 va =
        va_prefix |
        ((uint64)i << (PGSHIFT + level*9));


    for(int d=0;d<depth;d++)
      printf(" ..");


    printf("%p: pte %p pa %p\n",
           (void*)va,
           (void*)pte,
           (void*)PTE2PA(pte));


    if(level>0 &&
       ((pte&(PTE_R|PTE_W|PTE_X))==0)){

      print_helper(
          (pagetable_t)PTE2PA(pte),
          level-1,
          depth+1,
          va
      );
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);

  print_helper(pagetable,2,1,0);
}
#endif

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; ){

    // 先检查是不是superpage
    pte_t *l1pte = walkpte(pagetable, a, 1);

    if(l1pte && (*l1pte & PTE_V) && PTE_LEAF(*l1pte)){

      uint64 super_start = a & ~SUPERPG_MASK;
      uint64 super_end = super_start + SUPERPG_SIZE;


      // 情况1：整个superpage释放
      if(a == super_start &&
         va + npages*PGSIZE >= super_end){

        if(do_free)
          superfree((void*)PTE2PA(*l1pte));

        *l1pte = 0;

        a += SUPERPG_SIZE;
        continue;
      }


      // 情况2：只释放一部分，需要降级
      if(demote_superpage(pagetable, a) < 0)
        panic("demote_superpage");

      // 降级后重新处理当前4KB页
      continue;
    }


    // 普通4KB页处理
    pte_t *pte = walk(pagetable, a, 0);

    // Lazy allocation may leave holes in the address range.
    if(pte == 0 || !(*pte & PTE_V)){
      a += PGSIZE;
      continue;
    }


    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: leaf");


    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }

    *pte = 0;

    a += PGSIZE;
  }
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += sz){
    sz = PGSIZE;
    // VA 2MB对齐，且剩余空间 >=2MB → 尝试分配超级页
    if(((a & SUPERPG_MASK) == 0) && ((newsz - a) >= SUPERPG_SIZE)){
      mem = superalloc();
      if(mem != 0){
        if(install_superpage(pagetable, a, (uint64)mem) == 0){
          sz = SUPERPG_SIZE;
          continue;
        }
        superfree(mem);
      }
    }
    // 超级页失败，回退4KB小页
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 i;
  uint flags;
  char *mem;
  uint64 step;

  for(i = 0; i < sz; ){
    step = PGSIZE;
    pte = walk(old, i, 0);
    if(pte == 0 || !(*pte & PTE_V)){
      i += step;
      continue;
    }

    pte_t *l1pte = walkpte(old, i, 1);
    if(l1pte && (*l1pte & PTE_V) && PTE_LEAF(*l1pte)){
      uint64 sva = i & ~SUPERPG_MASK;
      // 如果当前i不在超级页起始，先对齐到起点，下一轮正式处理
      if(i != sva){
        i = sva;
        continue;
      }

    printf("uvmcopy: hit superpage sva=%lx\n", sva);

      // i已经对齐到超级页起始，执行拷贝
      uint64 spa = PTE2PA(*l1pte);
      void *newspa = superalloc();


    if(newspa == 0){
      printf("uvmcopy: superalloc FAILED sva=%lx\n", sva);
      goto err;
    }

    memmove(newspa, (void*)spa, SUPERPG_SIZE);

    if(install_superpage(new, sva, (uint64)newspa) < 0){
      printf("uvmcopy: install_superpage FAILED sva=%lx\n", sva);
      superfree(newspa);
      goto err;
    }
      step = SUPERPG_SIZE;
    } else {
      // 普通4KB页逻辑
      uint64 pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      mem = kalloc();
      if(mem == 0)
        goto err;
      memmove(mem, (char*)pa, PGSIZE);
      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }
    }
    i += step;
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist %lx %ld\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}




// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif

uint64
sys_kpgtbl(void)
{
  vmprint(myproc()->pagetable);
  return 0;
}
