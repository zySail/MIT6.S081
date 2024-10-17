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
static void insert(uint key, struct buf *b){
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
static void delete(uint key, struct buf *b){
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
  // make buf into a double link and add to bucket 0
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
  acquire(&bcache.hashtbl.bucket_lock[key]);
  // Is the block already cached?
  for(b = bcache.hashtbl.bucket[key]; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashtbl.bucket_lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // first search in its bucket for a free buf
  for(b = bcache.hashtbl.bucket[key]; b; b = b->next){
    if(b->refcnt == 0){
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.hashtbl.bucket_lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // bucket[key] has no free buf, search in other buckets

  release(&bcache.hashtbl.bucket_lock[key]);
  acquire(&bcache.lock);
  acquire(&bcache.hashtbl.bucket_lock[key]);
  for(b = bcache.hashtbl.bucket[key]; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.hashtbl.bucket_lock[key]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // look for the lru buf
  uint lru_key;
  struct buf *t;
  b = 0;
  for(int i = 0; i < NR_HASH; i++){ // iterate over every bucket
    if(i == key)
      continue;
    acquire(&bcache.hashtbl.bucket_lock[i]);
    for(t = bcache.hashtbl.bucket[i]; t; t = t->next){ // search in bucket[i]
      if(t->refcnt == 0 && (b == 0 || t->timestamp < b->timestamp)){
        b = t;
      }
    }
    if(b){
      // move buf
      lru_key = hash(b->dev, b->blockno);
      delete(lru_key, b); // remove LRU buf from old bucket
      release(&bcache.hashtbl.bucket_lock[lru_key]);
      // modify buf
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      insert(key, b); // insert into new bucket
      release(&bcache.hashtbl.bucket_lock[key]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
    else{
      release(&bcache.hashtbl.bucket_lock[i]);
    }
  }
  panic("bget: no buffer can be recyled");
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

  acquire(&tickslock);
  uint timestamp = ticks;
  release(&tickslock);

  uint key = hash(b->dev, b->blockno);
  acquire(&bcache.hashtbl.bucket_lock[key]);
  if (b->refcnt > 0) {
    b->refcnt--;
    b->timestamp = timestamp;
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


