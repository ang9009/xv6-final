// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 29

struct bucket {
  struct spinlock lock;
  struct buf head;  // Sentinel
};

struct bucket buckets[NBUCKETS];

struct {
  struct buf bufs[NBUF];
  struct spinlock lock;
} bcache;

void binit(void) {
  initlock(&bcache.lock, "bcache");

  //  Initialize buckets and sentinel nodes
#ifdef LAB_LOCK
  for (int i = 0; i < NBUCKETS; i++) {
    char buf[16];
    snprintf(buf, sizeof(buf), "bcache%d", i);
    initlock(&buckets[i].lock, buf);

    buckets[i].head.prev = &buckets[i].head;
    buckets[i].head.next = &buckets[i].head;
  }
#endif

  for (int i = 0; i < NBUF; i++) {
    struct buf* buffer = &bcache.bufs[i];
    initsleeplock(&buffer->lock, "buffer");
    int bucket_idx = i % NBUCKETS;

    // Insert buffer into bucket linked list
    struct buf* head = &buckets[bucket_idx].head;
    buffer->next = head->next;
    buffer->prev = head;

    head->next->prev = buffer;
    head->next = buffer;
  }
}

struct bucket* get_bucket(uint blockno) {
  return &buckets[blockno % NBUCKETS];
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf* bget(uint dev, uint blockno) {
  struct buf* b;
  struct bucket* bucket = get_bucket(blockno);

  acquire(&bucket->lock);
  // Is the block already cached?
  for (b = bucket->head.next; b != &bucket->head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bucket->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bucket->lock);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for (int i = 0; i < NBUF; i++) {
    b = &bcache.bufs[i];
    struct bucket* b_bucket = get_bucket(b->blockno);

    acquire(&b_bucket->lock);
    if (b->refcnt == 0) {
      struct bucket* new_bucket = get_bucket(blockno);

      if (new_bucket != b_bucket) {
        acquire(&new_bucket->lock);
        // Remove element from list
        b->prev->next = b->next;
        b->next->prev = b->prev;

        // Insert element into new list
        struct buf* head = &new_bucket->head;
        b->next = head->next;
        b->prev = head;

        head->next->prev = b;
        head->next = b;
        release(&new_bucket->lock);
      }

      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;

      release(&b_bucket->lock);
      acquiresleep(&b->lock);

      return b;
    }
    release(&b_bucket->lock);
  }

  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf* bread(uint dev, uint blockno) {
  struct buf* b;

  b = bget(dev, blockno);
  if (!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf* b) {
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf* b) {
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  struct bucket* bucket = get_bucket(b->blockno);
  acquire(&bucket->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    if (b->prev) {
      // Move behind head
      b->next->prev = b->prev;
      b->prev->next = b->next;
      b->next = bucket->head.next;
      b->prev = &bucket->head;
      bucket->head.next->prev = b;
      bucket->head.next = b;
    }
  }
  release(&bucket->lock);
}

void bpin(struct buf* b) {
  struct bucket* bucket = get_bucket(b->blockno);
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
}

void bunpin(struct buf* b) {
  struct bucket* bucket = get_bucket(b->blockno);
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}
