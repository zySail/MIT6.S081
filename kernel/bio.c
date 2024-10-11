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
*/

struct hash_table{
  struct spinlock bucket_lock[NR_HASH];
  struct buf *bucket[NR_HASH];
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct hash_table hashtbl;
} bcache;

// hash_table's functions
// --------------------------------------------------------------
uint64 hash(uint dev, uint blockno){
  return (dev ^ blockno) % NR_HASH;
}

// insert buf node b into hash bucket[key]
void insert(uint64 key, struct buf *b){
  if(!b || key >= NR_HASH )
    panic("insert");

  if(!bcache.hashtbl.bucket[key]){ // bucket has no buf node
    b->next = 0;
    b->prev = 0;
    bcache.hashtbl.bucket[key] = b;
  }
  else{
    b->next = bcache.hashtbl.bucket[key];
    b->prev = 0;
    bcache.hashtbl.bucket[key]->prev = b;
    bcache.hashtbl.bucket[key] = b;
  }
}

// delete buf node b from hash bucket[key]
void delete(uint64 key, struct buf *b){
  if(!b || key >= NR_HASH )
    panic("delete");

  if(b->prev == 0){ // b is the first buf node
    if(b->next)
      b->next->prev = 0;
    bcache.hashtbl.bucket[key] = b->next;
    b->next = 0;
  }
  else{
    if(b->next) 
      b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = 0;
    b->prev = 0;
  }
}
//-----------------------------------------------------------

void
binit(void)
{
  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NR_HASH; i++){
    initlock(&bcache.hashtbl.bucket_lock[i], "bcache");
    bcache.hashtbl.bucket[i] = 0;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint64 lastest_time = 0;
  struct buf *LRU_buf;
  uint64 LRU_buf_key;

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
  
  // iterate over buf[], look for LRU_buf
  for(int i = 0; i < NBUF; i++){
    if(bcache.buf[i].refcnt == 0) {
      if(bcache.buf[i].last_used_time > lastest_time){
        LRU_buf = &bcache.buf[i];
        lastest_time = bcache.buf[i].last_used_time;
      }
    }
  }
  if(!LRU_buf)
    panic("bget: no buffers");
  
  // remove LRU_buf from old hash bucket
  LRU_buf_key = hash(LRU_buf->dev, LRU_buf->blockno);
  acquire(&bcache.hash_bucket_lock[LRU_buf_key]);
  if(bcache.hash_table[LRU_buf_key] == LRU_buf){ // check if the head is the target
    bcache.hash_table[LRU_buf_key] = LRU_buf->next;
  }
  else{
    b = bcache.hash_table[LRU_buf_key];
    while(b != 0){
        if(b->next == LRU_buf){
          b->next = LRU_buf->next;
        }
        b++;
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

  releasesleep(&b->lock); // release buf b's sleeplock

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


