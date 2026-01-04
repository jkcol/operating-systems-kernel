/*! @file cache.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
    @brief Block cache for a storage device.
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifdef CACHE_TRACE
#define TRACE
#endif

#ifdef CACHE_DEBUG
#define DEBUG
#endif

#define CACHE_NUM_BLOCKS 64

#include "cache.h"

#include "conf.h"
#include "console.h"
#include "device.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "limits.h"

// INTERNAL TYPE DEFINITIONS

/**
 * struct cache_block
 * Input: None
 * Output: None
 * Description: The internal representation of a block in the cache
 * Side Effect: None
 */
struct cache_block { // this is the individual block in the cache
    void * data; // pointer to actual block data
    unsigned long long block_num; // position in backing storage device / can think of this as the index from pointer to storage struct
    // block 0 is bytes 0-511, block 1 is bytes 512-1023, block 2 is bytes 1024-1535, etc.
    int dirty; // dirty bit -> 1 means modified/written to, 0 means not modified
    int valid; // if block contains valid data -> 1 means valid (flushed to backing store/clean cached data), 0 means invalid
    unsigned long timestamp; // least recently used (LRU) tracking/counter
    int refcount; // reference count to track number of active users of this block
};

/**
 * TT of dirty and valid bits:
 * * Dirty | Valid | Meaning
 *     1   |  1    | Block has been modified and contains valid data
 *     1   |  0    | Invalid state - should not occur in normal operation
 *     0   |  1    | Block contains valid data that has not been modified
 *     0   |  0    | Block does not contain valid data
 * 
 * We have to keep track of modifications and the empty slots of cached data
 */   


/**
 * struct cache
 * Input: None
 * Output: None
 * Description: The internal representation of the cache
 */
struct cache {
    struct storage* backing_store; // backing storage device from virtio
    struct cache_block blocks[CACHE_NUM_BLOCKS]; // array of cache blocks size of 64
    struct lock cache_lock; // for safety!
    unsigned long counter; // for global LRU tracking for all blocks in cache
};


/**
 * @brief Creates/initializes a cache with the passed backing storage device (disk) and makes it
 * available through cptr.
 * @param disk Pointer to the backing storage device.
 * @param cptr Pointer to the cache to create.
 * @return 0 on success, negative error code if error
 */
int create_cache(struct storage* disk, struct cache** cptr) {
    // FIXME
    if (!disk || !cptr) {
        return -EINVAL; // invalid arguments
    }
    struct cache *cache = (struct cache *) kmalloc(sizeof(struct cache)); // very very similar to create_thread
    if (!cache) {
        return -ENOTSUP; // memory allocation failure -> given error so I'm using this ig
    }
    cache->backing_store = disk;
    cache->counter = 0;
    lock_init(&cache->cache_lock);
    for (int i = 0; i < CACHE_NUM_BLOCKS; i++) { // initialize every single block in the cache :(
        cache->blocks[i].data = kmalloc(CACHE_BLKSZ);
        if (!cache->blocks[i].data) {
            // Free previously allocated blocks
            for (int j = 0; j < i; j++) {
                kfree(cache->blocks[j].data);
            }
            kfree(cache);
            return -ENOTSUP;
        }
        cache->blocks[i].block_num = 0;
        cache->blocks[i].dirty = 0;
        cache->blocks[i].valid = 0;
        cache->blocks[i].timestamp = 0;
        cache->blocks[i].refcount = 0; 

    }
    *cptr = cache;
    return 0;

}

/**
 * @brief Reads a CACHE_BLKSZ sized block from the backing interface into the cache.
 * @param cache Pointer to the cache.
 * @param pos Position in the backing storage device. Must be aligned to a multiple of the block
 * size of the backing interface.
 * @param pptr Pointer to the block pointer read from the cache. Assume that CACHE_BLKSZ will always
 * be equal to the block size of the storage disk. Any replacement policy is permitted, as long as
 * your design meets the above specifications.
 * @return 0 on success, negative error code if error
 */
int cache_get_block(struct cache* cache, unsigned long long pos, void** pptr) {
    // FIXME

    if(!cache || !pptr){
        return -EINVAL; // return error if args are invalid 
    }
    
    // unsigned long long blknum = pos;
    unsigned long long blknum = pos / CACHE_BLKSZ; // determine block number from position


    // if(pos % CACHE_BLKSZ){ // if position in backing device is not a multiple of blksz, return format error
    //     return -EBADFMT;
    // }

    lock_acquire(&cache->cache_lock); // lock while accessing modifiable data

    for(int i = 0; i < CACHE_NUM_BLOCKS; i++){ // iterate through cache data
        if(cache->blocks[i].valid == 1 && cache->blocks[i].block_num == blknum){ // if data is valid and matches (cache hit)
            cache->counter = (cache->counter) + 1; // update block to indicate a recent access
            cache->blocks[i].timestamp = cache->counter;
            cache->blocks[i].refcount += 1; // increment reference count for this block
            *pptr = cache->blocks[i].data; // return the data of the cache block through arg
            lock_release(&cache->cache_lock); 
            return 0;
        }
    }

    // cache miss, find block to store data (may need to evict least used data)

    int oldest_blk_idx = -1; // initialize the "oldest" block to most recent use 
    unsigned long oldest_timestamp = ULONG_MAX;

    for(int i = 0; i < CACHE_NUM_BLOCKS; i++){ // loop through cache and find oldest accessed block
        if(cache->blocks[i].refcount > 0){ // if block is currently being used, skip it
            continue;
        }
        if(!cache->blocks[i].valid){ // if invalid data (can write to it), just use this block
            oldest_blk_idx = i;
            break;
        }
        
        if(cache->blocks[i].timestamp < oldest_timestamp){ // if current least used is less than global least used, update
            oldest_timestamp = cache->blocks[i].timestamp;
            oldest_blk_idx = i;
        }
    }
    
    if (oldest_blk_idx == -1) { // all blocks are in use
        lock_release(&cache->cache_lock);
        return -EBUSY; // return busy error
    }
    if(cache->blocks[oldest_blk_idx].dirty && cache->blocks[oldest_blk_idx].valid){ // if all blocks are in use, return busy error
        int res = storage_store(cache->backing_store, cache->blocks[oldest_blk_idx].block_num * CACHE_BLKSZ, cache->blocks[oldest_blk_idx].data, CACHE_BLKSZ);
        if(res <0) { // if store failed, return bad format 
            lock_release(&cache->cache_lock);
            return res;
        }  
    }
    int res = storage_fetch(cache->backing_store, pos,  cache->blocks[oldest_blk_idx].data, CACHE_BLKSZ);
    if(res < 0){
        lock_release(&cache->cache_lock);
        return res;
    }

    // i/o operations

    cache->blocks[oldest_blk_idx].block_num = blknum; // initialize the rest of the members 
    // cache->blocks[oldest_blk_idx].block_num = pos; // initialize the rest of the members 
    cache->blocks[oldest_blk_idx].dirty = 0;
    cache->blocks[oldest_blk_idx].valid = 1;
    cache->blocks[oldest_blk_idx].timestamp = ++cache->counter;
    cache->blocks[oldest_blk_idx].refcount = 1; // one active user of this block now

    // cache->counter = (cache->counter) + 1; // accesses this most recent block, so update counter 
    *pptr = cache->blocks[oldest_blk_idx].data; // return the new and freshly loaded data to arg

    lock_release(&cache->cache_lock);
    return 0;
}

/**
 * @brief Releases a block previously obtained from cache_get_block().
 * @param cache Pointer to the cache.
 * @param pblk Pointer to a block that was made available in cache_get_block() (which means that
 * pblk == *pptr for some pptr).
 * @param dirty Indicates whether the block has been modified (1) or not (0). If dirty == 1, the
 * block has been written to. If dirty == 0, the block has not been written to.
 * @return 0 on success, negative error code if error
 */
void cache_release_block(struct cache* cache, void* pblk, int dirty) {
    // FIXME
    if(!cache || !pblk){
        return; // return if invalid args
    }

    lock_acquire(&cache->cache_lock); // get lock

    for(int i = 0; i < CACHE_NUM_BLOCKS; i++){ // iterate through cache list
        if(cache->blocks[i].data == pblk){ // if data ptr to current block matches pblk
            if(dirty) {
                cache->blocks[i].dirty = 1; // block is modifed, so mark as dirty
            }
            cache->blocks[i].refcount -= 1; // decrement reference count for this block
            break;
        }
    }

    lock_release(&cache->cache_lock);
    
    return;
}

/**
 * @brief Flushes the cache to the backing device
 * @param cache Pointer to the cache to flush
 * @return 0 on success, error code if error
 */
int cache_flush(struct cache* cache) {
    // FIXME
    if(!cache){ // if arg is invalid, return
        return -EINVAL;
    }

    int res = 0;

    lock_acquire(&cache->cache_lock);

    for(int i = 0; i < CACHE_NUM_BLOCKS; i++){
        if(cache->blocks[i].dirty && cache->blocks[i].valid){ // if the file is dirty and valid
            
            res = storage_store(cache->backing_store, cache->blocks[i].block_num*CACHE_BLKSZ, cache->blocks[i].data, CACHE_BLKSZ); // flush the dat

            if(res < 0){ // if store failed, return bad format
                lock_release(&cache->cache_lock);
                return res;
            }
            cache->blocks[i].dirty = 0; // mark block as clean
        } 
    }

    lock_release(&cache->cache_lock);
    return 0;
}