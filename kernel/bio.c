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

#define NBUCKET 13
#define HASH(key) ((uint64)(key) % NBUCKET)

struct {
  struct buf buf[NBUF];

  struct spinlock bucket_lock[NBUCKET];
  struct buf bucket[NBUCKET]; 
} bcache;


static char bcache_name[NBUCKET][18];

static uint dhash(uint dev, uint blockno)
{
  return HASH(dev + blockno);
}

void
binit(void)
{
  struct buf *b;

  for (int i = 0; i< NBUCKET; i++)
  {
    snprintf(bcache_name[i], sizeof(bcache_name[0]), "bcache.bucket%d", i);
    initlock(&bcache.bucket_lock[i], bcache_name[i]);

    bcache.bucket[i].prev = &bcache.bucket[i];
    bcache.bucket[i].next = &bcache.bucket[i];
  }

  uint bucketid;
  for (b = bcache.buf; b != bcache.buf + NBUF; b++)
  {
    bucketid = (uint)HASH(b);
    b->prev = &bcache.bucket[bucketid];
    b->next = bcache.bucket[bucketid].next;
    initsleeplock(&b->lock, "buffer");
    bcache.bucket[bucketid].next->prev = b;
    bcache.bucket[bucketid].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint bucketid = dhash(dev, blockno);

  acquire(&bcache.bucket_lock[bucketid]);

  // Is the block already cached?
  for (b = bcache.bucket[bucketid].next; b != &bcache.bucket[bucketid]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.bucket_lock[bucketid]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached. Find unused buffer in current bucket.
  for (b = bcache.bucket[bucketid].next; b != &bcache.bucket[bucketid]; b = b->next)
  {
    if (b->refcnt == 0)
    {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      
      release(&bcache.bucket_lock[bucketid]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // No free buffer found in current bucket. Find in other buckets.
  for (int i = 1; i < NBUCKET; i++)
  {
    int old_bucketid = (bucketid + i) % NBUCKET;

    acquire(&bcache.bucket_lock[old_bucketid]);
    for (b = bcache.bucket[old_bucketid].next; b != &bcache.bucket[old_bucketid]; b = b->next)
    {
      if (b->refcnt == 0)
      {
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // Remove buffer from old bucket.
        b->prev->next = b->next;
        b->next->prev = b->prev;

        // Insert new buffer at head of current bucket.
        b->next = bcache.bucket[bucketid].next;
        b->prev = bcache.bucket;
        bcache.bucket[bucketid].next->prev = b;
        bcache.bucket[bucketid].next = b;
        
        release(&bcache.bucket_lock[old_bucketid]);
        release(&bcache.bucket_lock[bucketid]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.bucket_lock[old_bucketid]);
  }
  
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint bucketid = dhash(b->dev, b->blockno);
  acquire(&bcache.bucket_lock[bucketid]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
  }
  
  release(&bcache.bucket_lock[bucketid]);
}

void
bpin(struct buf *b) {
  uint bucketid = dhash(b->dev, b->blockno);
  acquire(&bcache.bucket_lock[bucketid]);
  b->refcnt++;
  release(&bcache.bucket_lock[bucketid]);
}

void
bunpin(struct buf *b) {
  uint bucketid = dhash(b->dev, b->blockno);
  acquire(&bcache.bucket_lock[bucketid]);
  b->refcnt--;
  release(&bcache.bucket_lock[bucketid]);
}


