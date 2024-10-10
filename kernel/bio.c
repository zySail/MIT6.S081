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

#define NR_HASH 13

/*
define head: "struct buf hash_table[NR_HASH];" will be more convenient
but head buf will cost a lot of space due to buf's "uchar data[BSIZE]";
not define head buf need to check the first buf when looking for LRU_buf
specifically design a head without data is complicated and unreadable
another plan is to design a struct only cotain buf *next , buf *pointer
and store the struct in hashtable
*/
struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct spinlock hash_bucket_lock[NR_HASH];
  struct buf *hash_table[NR_HASH];
} bcache;

uint64 hash(uint dev, uint blockno){
  return (dev ^ blockno) % NR_HASH;
}

void
binit(void)
{
  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NR_HASH; i++){
    initlock(&bcache.hash_bucket_lock[i], "bcache");
    bcache.hash_table[i] = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint64 key = hash(dev, blockno);
  acquire(&bcache.hash_bucket_lock[key]); // get bucket lock

  // Is the block already cached?
  for(b = bcache.hash_table[key]; b != 0; b = b->next){ // search in the bucket
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hash_bucket_lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  /*
    must release bucket.lock first to avoid dead lock
    if current proc acquire bcache.lock without release bucket.lock(*)
    unfortunately, another proc iterate over buf[] with holding bcache.lock
    when it acquire bucket.lock(*), dead lock
  */
  release(&bcache.hash_bucket_lock[key]);
  acquire(&bcache.lock);
  acquire(&bcache.hash_bucket_lock[key]);
  uint64 lastest_time = 0;
  struct buf *LRU_buf;
  uint64 LRU_buf_key;
  // iterate over buf[], look for LRU_buf
  for(b = bcache.buf; b != &bcache.buf[NBUF]; b++){
    if(b->refcnt == 0) {
      if(b->last_used_time > lastest_time){
        LRU_buf = b;
      }
    }
  }
  if(!LRU_buf)
    panic("bget: no buffers");
  
  // remove LRU_buf from old hash bucket
  LRU_buf_key = hash(LRU_buf->dev, LRU_buf->blockno);
  acquire(&bcache.hash_bucket_lock[LRU_buf_key]);
  if(bcache.hash_table[LRU_buf_key] == LRU_buf){ // check if the head is the target
    b = bcache.hash_table[LRU_buf_key];
    bcache.hash_table[LRU_buf_key] = LRU_buf->next;
  }
  else{
    for(b = bcache.hash_table[LRU_buf_key]; b != 0; b++){
        if(b->next == LRU_buf){
          b->next = LRU_buf->next;
        }
      }
  }
  
  // modify LRU_buf, and move it to new bucket
  LRU_buf->dev = dev;
  LRU_buf->blockno = blockno;
  LRU_buf->valid = 0;
  LRU_buf->refcnt = 1;
  LRU_buf->next = bcache.hash_table[key]->next;
  bcache.hash_table[key]->next = LRU_buf;

  release(&bcache.hash_bucket_lock[LRU_buf_key]);
  release(&bcache.hash_bucket_lock[key]);
  release(&bcache.lock);
  // return LRU_buf
  acquiresleep(&LRU_buf->lock);
  return LRU_buf;
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

  // record last used time
  acquire(&tickslock);
  b->last_used_time = ticks; 
  release(&tickslock);
  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


