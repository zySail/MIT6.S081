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


/*
define head: "struct buf hash_table[NR_HASH];" will be more convenient
but head buf will cost a lot of space due to buf's "uchar data[BSIZE]";
*/
#define NR_HASH 13

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
uint hash(uint dev, uint blockno){
  return (dev ^ blockno) % NR_HASH;
}

// insert buf node b into hash bucket[key]
void insert(uint key, struct buf *b){
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
void delete(uint key, struct buf *b){
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
  // make buf to a link and add to bucket 0
  for(int i = 0; i < NBUF; i++){ 
    bcache.buf[i].timestamp = 0;
    // set next
    if(i == NBUF - 1)
      bcache.buf[i].next = 0;
    else
      bcache.buf[i].next = &bcache.buf[i+1];
    // set prev
    if(i == 0)
      bcache.buf[i].prev = 0;
    else  
      bcache.buf[i].prev = &bcache.buf[i-1];
  }
  bcache.hashtbl.bucket[0] = &bcache.buf[0];
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint key;

  key = hash(dev, blockno);
  acquire(&bcache.hashtbl.bucket_lock[key]); // acquire bucket lock
  // Is the block already cached?
  for(b = bcache.hashtbl.bucket[key]; b; b = b->next){ // search in the bucket
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashtbl.bucket_lock[key]);
      // record last used time
      acquire(&tickslock);
      b->timestamp = ticks; 
      release(&tickslock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  uint min_timestap = 9999999;
  uint lru_key = NR_HASH; // the lru buf's bucket key
  int findbetter = 0; // record whether find a better buf in the bucket
  struct buf *t; // used in for loop to iterate bucket

  release(&bcache.hashtbl.bucket_lock[key]); // release bucket.lock first to avoid dead lock
  acquire(&bcache.lock);
  
  // part1: find the lru buf
  b = 0;
  for(int i = 0; i < NR_HASH; i++){ // iterate over every bucket
    findbetter = 0;
    acquire(&bcache.hashtbl.bucket_lock[i]);
    for(t = bcache.hashtbl.bucket[i]; t; t = t->next){ // search in bucket[i]
      if(t->refcnt == 0 && t->timestamp <= min_timestap){
        b = t;
        min_timestap = t->timestamp;
        findbetter = 1;
      }
    }
    if(!findbetter){ // not find a better buf in bucket[i]
      release(&bcache.hashtbl.bucket_lock[i]);
    }
    else{ // find a better buf in bucket[i]
      if(lru_key < NR_HASH) // release the old lru buf's bucket.lock if there is old record
        release(&bcache.hashtbl.bucket_lock[lru_key]);
      lru_key = i;
    }
  }
  if(!b)
    panic("bget: no buffer can be recyled");
  
  // part2: move lru buf
  // remove LRU buf from old bucket
  // bucket_lock[lru_key] is being holded
  delete(lru_key, b); 
  release(&bcache.hashtbl.bucket_lock[lru_key]);

  // modify buf
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  acquire(&tickslock);
  b->timestamp = ticks;
  release(&tickslock);
  // insert into new bucket
  acquire(&bcache.hashtbl.bucket_lock[key]);
  insert(key, b);
  release(&bcache.hashtbl.bucket_lock[key]);

  release(&bcache.lock);

  acquiresleep(&b->lock);
  return b;
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
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock); // release buf b's sleeplock

  uint key = hash(b->dev, b->blockno);
  acquire(&bcache.hashtbl.bucket_lock[key]);
  if (b->refcnt > 0) {
    b->refcnt--;
  }
  release(&bcache.hashtbl.bucket_lock[key]);
}

void
bpin(struct buf *b) {
  uint key = hash(b->dev, b->blockno);
  acquire(&bcache.hashtbl.bucket_lock[key]);
  b->refcnt++;
  release(&bcache.hashtbl.bucket_lock[key]);
}

void
bunpin(struct buf *b) {
  uint key = hash(b->dev, b->blockno);
  acquire(&bcache.hashtbl.bucket_lock[key]);
  b->refcnt--;
  release(&bcache.hashtbl.bucket_lock[key]);
}


