/*! @file ramdisk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
    @brief Memory-backed storage implementation
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include "intr.h"
#ifdef RAMDISK_DEBUG
#define DEBUG
#endif

#ifdef RAMDISK_TRACE
#define TRACE
#endif

#include <stddef.h>

#include "console.h"
#include "devimpl.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "string.h"
#include "uio.h"
#include "thread.h"

#ifndef RAMDISK_NAME
#define RAMDISK_NAME "ramdisk"
#endif

// INTERNAL TYPE DEFINITIONS
//

/**
 * @brief Storage device backed by a block of memory. Allows modification of the backing memory
 * block.
 */
struct ramdisk {
    struct storage storage;  ///< Storage struct of memory storage
    void *buf;               ///< Block of memory
    size_t size;             ///< Size of memory block
    struct lock lock; // lock
};

// INTERNAL FUNCTION DECLARATIONS
//

static int ramdisk_open(struct storage *sto);
static void ramdisk_close(struct storage *sto);
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt);
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg);

// INTERNAL GLOBAL CONSTANTS
//

static const struct storage_intf ramdisk_intf = {
    .blksz = 1,
    .open = &ramdisk_open,
    .close = &ramdisk_close,
    .fetch = &ramdisk_fetch,
    .store = NULL,  // Read-only storage (blob data in .rodata)
    .cntl = &ramdisk_cntl};

// EXPORTED FUNCTION DEFINITIONS
//

/**
 * @brief Creates and registers a memory-backed storage device
 * @return None
 */

// void ramdisk_attach() 
// Inputs: None  
// Outputs: None
// Description: Creates and registers a memory-backed storage device 
// Side Effects: None 
void ramdisk_attach() {
    // External symbols from linker script for embedded blob data
    extern char _kimg_blob_start[], _kimg_blob_end[];

    // FIXME

    struct ramdisk * ramdisk; // create ramdisk
    ramdisk = kcalloc(1, sizeof(struct ramdisk)); // alloocate memory for ramdisk 

    if(!ramdisk){ // if kcalloc fails, return
        return;
    }
 
    size_t size = (size_t)(_kimg_blob_end - _kimg_blob_start); // compute size of blob
    ramdisk->buf = (void*)_kimg_blob_start; // buffer to write to is the start of the blob
    ramdisk->size = size; // init size 

    lock_init(&ramdisk->lock); // initialize a lock
    storage_init(&ramdisk->storage, &ramdisk_intf, ramdisk->size); // initialize storage device interface 
    register_device(RAMDISK_NAME, DEV_STORAGE, ramdisk); // register ramdisk
}



// INTERNAL FUNCTION DEFINITIONS
//

/**
 * @brief Opens the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 * @return 0 on success
 */
 
// static int ramdisk_open(struct storage *sto)
// Inputs: struct storage *sto - storage struct pointer  
// Outputs: Returns 0 if successful, -ENOTSUP if not
// Description: Opens the ramdisk device 
// Side Effects: None 
static int ramdisk_open(struct storage *sto) {
    // FIXME
    if(!sto){ // invalid args, return
        return -EINVAL;
    }

    struct ramdisk * const ramdisk = (void*)sto - offsetof(struct ramdisk, storage); // create ramdisk 

    if(!ramdisk){ // if ramdisk does not exist, return
        return -ENOTSUP;
    }

    return 0;
}

/**
 * @brief Closes the _ramdisk_ device.
 * @param sto Storage struct pointer for memory storage
 */

// static void ramdisk_close(struct storage *sto)
// Inputs: struct storage *sto - storage struct pointer  
// Outputs: None
// Description: Closes the ramdisk device 
// Side Effects: None
static void ramdisk_close(struct storage *sto) {
    // FIXME
    if(!sto){ // invalid args, return
        return;
    }

    struct ramdisk * const ramdisk = (void*)sto - offsetof(struct ramdisk, storage); // create ramdisk 

    if(!ramdisk){ // if ramdisk does not exist, return
        return;
    }
    
    return;
}

/**
 * @brief Reads bytecnt number of bytes from the disk and writes them to buf.
 * @details Performs proper bounds checks, then copies data from memory block to passed buffer
 * @param sto Storage struct pointer for memory storage
 * @param pos Position in storage to read from
 * @param buf Buffer to copy data from memory to
 * @param bytecnt Number of bytes to read from memory
 * @return Number of bytes successfully read
 */

// static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf, unsigned long bytecnt)
// Inputs: struct storage *sto - storage struct pointer  
//         unsigned long long pos - position in storage to read 
//         void *buf - buffer to copy data to
//         bytecnt - number of bytes to read 
// Outputs: # of bytes successfully read 
// Description: fetches bytecnt data from storage at pos to buf
// Side Effects: None
static long ramdisk_fetch(struct storage *sto, unsigned long long pos, void *buf,
                          unsigned long bytecnt) {
    // FIXME

    int pie;

    if(!sto || !buf){ // if storage device or buffer DNE, return
        return -EINVAL;
    }

    struct ramdisk * const ramdisk = (void*)sto - offsetof(struct ramdisk, storage); // create ramdisk 

    if(!ramdisk){ // if ramdisk DNE, return
        return -ENOTSUP;
    }

    if(bytecnt == 0 || pos >= ramdisk->size){ // if bufsz is zero, dont read anything 
        return 0;
    }

    if(pos+bytecnt > ramdisk->size){ // if # of bytes is too large, truncate 
        bytecnt = ramdisk->size - pos;
    }

    lock_acquire(&ramdisk->lock); // acquire the lock
    pie = disable_interrupts();
        
    memcpy(buf, ramdisk->buf + pos, bytecnt); // copy data from ramdisk to buf 
    
    restore_interrupts(pie);
    lock_release(&ramdisk->lock); // release the lock

    return bytecnt;
}

/**
 * @brief _cntl_ functions for memory storage.
 * @details Memory storage supports basic control operations
 * @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
 * directly return the value.
 * @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
 * @param sto Storage struct pointer for memory storage
 * @param cmd command to execute. ramdisk should support FCNTL_GETEND.
 * @param arg Argument for commands
 * @return 0 on success, error on failure or unsupported command
 */

// static int ramdisk_cntl(struct storage *sto, int cmd, void *arg)
// Inputs: struct storage *sto - storage struct pointer  
//         int cmd - command 
//         void *arg - arguments if needed for command 
// Outputs: 0 on success, error on fail
// Description: returns capacity of virtio in bytes  
// Side Effects: None
static int ramdisk_cntl(struct storage *sto, int cmd, void *arg) {
    // FIXME

    if(!sto){ // invalid args, return
        return -EINVAL;
    }

    if(cmd != 0){ // if invalid command, return (not FCNTL_GETEND)
        return -ENOTSUP;
    }

    if(!arg){ // if invalid arg ptr, return
        return -EINVAL;
    }

    *(unsigned long long *)arg = sto->capacity; // return capacity of VirtIO block through the arg

    return 0;
}
