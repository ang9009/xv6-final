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

struct {
  struct spinlock lock;
  struct run* freelist;
  struct run* superfreelist;
} kmem;

void kinit() {
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void freerange(void* pa_start, void* pa_end) {
  char* p;
  p = (char*)PGROUNDUP((uint64)pa_start);

  int superpg_count = 0;
  while (p + PGSIZE <= (char*)pa_end) {
    if (superpg_count < 4 && (uint64)p % SUPERPAGE_SIZE == 0 &&
        (p + SUPERPAGE_SIZE) <= (char*)pa_end) {
      superfree(p);
      p += SUPERPAGE_SIZE;
      superpg_count++;
    } else if (p + PGSIZE <= (char*)pa_end) {
      kfree(p);
      p += PGSIZE;
    }
  }
}

static void freerun(void* pa, uint64 size, struct run** freelist) {
  if (((uint64)pa % size) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP) {
    printf(
        "freerun panic\n divisible: %d, less than start: %d, greater than end "
        "of mem "
        "%d\n",
        ((uint64)pa % size) != 0, (char*)pa < end, (uint64)pa >= PHYSTOP);
    panic("freerun");
  }

  // Fill with junk to catch dangling refs.
  memset(pa, 1, size);

  struct run* r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = *freelist;
  *freelist = r;
  release(&kmem.lock);
}

void kfree(void* pa) {
  freerun(pa, PGSIZE, &kmem.freelist);
}

void superfree(void* pa) {
  freerun(pa, SUPERPAGE_SIZE, &kmem.superfreelist);
}

void* superalloc(void) {
  struct run* r;

  acquire(&kmem.lock);
  r = kmem.superfreelist;
  if (r)
    kmem.superfreelist = r->next;
  release(&kmem.lock);

  if (r)
    memset((char*)r, 5, PGSIZE);  // fill with junk
  return (void*)r;
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void* kalloc(void) {
  struct run* r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
    memset((char*)r, 5, PGSIZE);  // fill with junk
  return (void*)r;
}
