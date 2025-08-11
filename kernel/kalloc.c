// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void* pa_start, void* pa_end);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
  struct run* next;
};

struct cpu_kmem {
  struct spinlock lock;
  struct run* freelist;
} kmem[NCPU];

void kinit() {
#ifdef LAB_LOCK
  for (int i = 0; i < NCPU; i++) {
    char buf[16];  // You only really need 6 I think but whatever
    snprintf(buf, sizeof(buf), "kmem%d", i);
    initlock(&kmem[i].lock, buf);
  }
#endif
  freerange(end, (void*)PHYSTOP);
}

void freerange(void* pa_start, void* pa_end) {
  char* p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

struct run* steal_mem() {
  struct run* node = 0;

  for (int i = 0; i < NCPU; i++) {
    struct cpu_kmem* mem_info = &kmem[i];
    acquire(&mem_info->lock);
    if (mem_info->freelist) {
      node = mem_info->freelist;
      mem_info->freelist = mem_info->freelist->next;
      release(&mem_info->lock);
      break;
    }
    release(&mem_info->lock);
  }

  return node;
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void* pa) {
  struct run* r;

  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  release(&kmem[cpu].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void* kalloc(void) {
  struct run* r;

  push_off();
  int cpu = cpuid();
  pop_off();

  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if (r) {
    kmem[cpu].freelist = r->next;
  }
  release(&kmem[cpu].lock);

  if (!r) {
    r = steal_mem(cpu);
  }

  if (r)
    memset((char*)r, 5, PGSIZE);  // fill with junk
  return (void*)r;
}
