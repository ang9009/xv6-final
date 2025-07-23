// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

static int pageref_count[PHYSTOP / PGSIZE];

void freerange(void* pa_start, void* pa_end);

extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

struct run {
  struct run* next;
};

struct {
  struct spinlock lock;
  struct run* freelist;
} kmem;

void kinit() {
  initlock(&kmem.lock, "kmem");

  freerange(end, (void*)PHYSTOP);
}

// Updates page reference table. If the last reference to a page is removed, the
// page will be freed.
void update_pageref(bool increment, uint64 pa, bool do_free) {
  if (pa % PGSIZE != 0) {
    panic("update_pageref(): address not page-aligned");
  }

  int i = pa / PGSIZE;
  if (i < 0 || i >= PHYSTOP / PGSIZE) {
    panic("update_pageref(): address out of bounds");
  } else if (!increment && pageref_count[i] <= 0) {
    return;
  }

  bool free;
  increment ? pageref_count[i]++ : pageref_count[i]--;
  free = pageref_count[i] == 0;

  if (do_free && free) {
    kfree((void*)pa);
  }
}

void freerange(void* pa_start, void* pa_end) {
  char* p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
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

  if (r) {
    memset((char*)r, 5, PGSIZE);  // fill with junk
    update_pageref(true, (uint64)r, false);
  }

  return (void*)r;
}
