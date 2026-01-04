/*! @file vioblk.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
@brief VirtIO block device
@copyright Copyright (c) 2024-2025 University of Illinois
*/


#include "devimpl.h"
#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include <limits.h>

#include "conf.h"
#include "console.h"
#include "device.h"
#include "error.h"
#include "heap.h"
#include "intr.h"
#include "misc.h"
#include "string.h"
#include "thread.h"
#include "uio.h"  // FCNTL
#include "virtio.h"

#include "stddef.h"

// COMPILE-TIME PARAMETERS
//
#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif


// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)
#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_TOPOLOGY 10
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14
#define VIRTIO_BLK_DESC_CHAIN_MAX_LEN 16 // newwwww
#define VIRTIO_MAX_SZ 64

#define VIRTIO_BLK_DESC_CHAIN_LEN 3
#define VIRTIO_BLK_S_OK 0

#define VIRTIO_BLK_T_IN           0
#define VIRTIO_BLK_T_OUT          1
#define VIRTIO_BLK_T_FLUSH        4
#define VIRTIO_BLK_T_GET_ID       8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD      11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE   14

// INTERNAL TYPE DEFINITIONS
//
struct vioblk_storage {
    struct storage base;
    volatile struct virtio_mmio_regs* regs;
    int irqno;
    char opened;

    unsigned int blksz;
    unsigned long long capacity;

    uint64_t enabled_features;

    struct condition desc_updated;
    // add lock
    struct lock vio_lck;

    struct virtq_desc * desc;
    struct virtq_avail * avail;
    struct virtq_used * used;

    uint16_t qlen;  // max number of descriptors in virtqueue

};

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector; // The sector number indicates the offset (multiplied by 512) where the read or write is to occur.
    uint8_t data[512];
    uint8_t status;
};

// INTERNAL FUNCTION DECLARATIONS
//

/**
* @brief Sets the virtq avail and virtq used queues such that they are available for use. (Hint,
* read virtio.h) Enables the interupt line for the virtio device and sets necessary flags in vioblk
* device.
* @param sto Storage IO struct for the storage device
* @return Return 0 on success or negative error code if error. If the given sto is already opened,
* then return -EBUSY.
*/
static int vioblk_storage_open(struct storage* sto);


/**
* @brief Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device. If
* the given sto is not opened, this function does nothing.
* @param sto Storage IO struct for the storage device
* @return None
*/
static void vioblk_storage_close(struct storage* sto);


/**
* @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
* repeatedly setting the appropriate registers to request a block from the disk, waiting until the
* data has been populated in block buffer cache, and then writes that data out to buf. Thread
* sleeps while waiting for the disk to service the request.
* @param sto Storage IO struct for the storage device
* @param pos The starting position for the read within the VirtIO device
* @param buf A pointer to the buffer to fill with the read data
* @param bytecnt The number of bytes to read from the VirtIO device into the buffer
* @return The number of bytes read from the device, or negative error code if error
*/
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                unsigned long bytecnt);


/**
* @brief Writes bytecnt number of bytes from the parameter buf to the disk. The size of the virtio
* device should not change. You should only overwrite existing data. Write should also not create
* any new files. Achieves this by filling up the block buffer cache and then setting the
* appropriate registers to request the disk write the contents of the cache to the specified block
* location. Thread sleeps while waiting for the disk to service the request.
* @param sto Storage IO struct for the storage device
* @param pos The starting position for the write within the VirtIO device
* @param buf A pointer to the buffer with the data to write
* @param bytecnt The number of bytes to write to the VirtIO device from the buffer
* @return The number of bytes written to the device, or negative error code if error
*/
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                unsigned long bytecnt);


/**
* @brief Given a file io object, a specific command, and possibly some arguments, execute the
* corresponding functions on the VirtIO block device.
* @details Any commands such as FCNTL_GETEND should pass back through the arg variable. Do not
* directly return the value.
* @details FCNTL_GETEND should return the capacity of the VirtIO block device in bytes.
* @param sto Storage IO struct for the storage device
* @param op Operation to execute. vioblk should support FCNTL_GETEND.
* @param arg Argument specific to the operation being performed
* @return Status code on the operation performed
*/
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg);


/**
* @brief The interrupt handler for the VirtIO device. When an interrupt occurs, the system will
* call this function.
* @param irqno The interrupt request number for the VirtIO device
* @param aux A generic pointer for auxiliary data.
* @return None
*/
static void vioblk_isr(int irqno, void* aux);


// INTERNAL GLOBAL VARIABLES
//
static const struct storage_intf vioblk_storage_intf = {
    .blksz = 512, // SHOULD THIS BE 512 BYTES OR 1...
    .open = &vioblk_storage_open,
    .close = &vioblk_storage_close, 
    .fetch = &vioblk_storage_fetch,
    .store = &vioblk_storage_store,
    .cntl = &vioblk_storage_cntl   
};


// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.
/**
* @brief Initializes virtio block device with the necessary IO operation functions and sets the
* required feature bits.
* @param regs Memory mapped register of Virtio
* @param irqno Interrupt request number of the device
* @return None
*/
// Inputs: volatile struct virtio_mmio_regs* regs - memory mapped registers 
// 		int irqno - interrupt request number for device
// Outputs: n/a
// Description: Initializes and attached a virtual i/o block storage device 
// Side Effects: allocates memory 
void vioblk_attach(volatile struct virtio_mmio_regs* regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_storage* vbd;
    unsigned int blksz;
    int result;

    trace("%s(regs=%p,irqno=%d)", __func__, regs, irqno);

    assert(regs->device_id == VIRTIO_ID_BLOCK);

    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();  // fence o,io

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.
    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;


    // blksz must be a power of two
    assert(((blksz - 1) & blksz) == 0);

    // FIXME
    vbd = kcalloc(1, sizeof(*vbd));
    if(!vbd){ //check if nullptr
        return;
    }  

    vbd->qlen = VIRTIO_BLK_DESC_CHAIN_LEN; // set qlen to max descriptors

    size_t descsz = (size_t)(sizeof(struct virtq_desc) * vbd->qlen); // get sizes of queues
    size_t availsz = (size_t)VIRTQ_AVAIL_SIZE(vbd->qlen);
    size_t usedsz = (size_t)VIRTQ_USED_SIZE(vbd->qlen);
    //size_t desc_freesz = (size_t)(sizeof(int) * vbd->qlen);
    size_t total = descsz + availsz + usedsz /*+ desc_freesz*/; // add sizes

    uint8_t * totv = kcalloc(1, total); // create continuous block

    if(!totv){ // if kcalloc fails
        kfree(vbd);
        return;
    }

    vbd->regs = regs;
    vbd->irqno = irqno;
    vbd->opened = 0;     

    vbd->blksz = blksz;
    vbd->capacity = regs->config.blk.capacity;

    vbd->desc = (struct virtq_desc*)totv; // place queues in correct memory
    vbd->avail = (struct virtq_avail*)(totv + descsz);
    vbd->used = (struct virtq_used*)(totv + descsz + availsz);
    // vbd->desc_free = (uint8_t *)(totv + descsz + availsz + usedsz);

    vbd->avail->idx = 0;
    vbd->used->idx = 0;

    virtio_attach_virtq(regs, 0, vbd->qlen, 
                        (uint64_t)(uintptr_t)vbd->desc, 
                        (uint64_t)(uintptr_t)vbd->used, 
                        (uint64_t)(uintptr_t)vbd->avail); // attatch vrng

    lock_init((&vbd->vio_lck)); // initialize lock
    condition_init(&vbd->desc_updated, "vioblk.desc_updated");

    __sync_synchronize();
    storage_init(&vbd->base, &vioblk_storage_intf, vbd->capacity);
    register_device(VIOBLK_NAME, DEV_STORAGE, vbd);

}



// Inputs: struct storage* sto - pointer to generic storage device 
// Outputs: int (0 - successfully opened) 
// Description: opens given virtio storage device and enables virtqueues & interupt sourc e
// Side Effects: edits opened flag 
static int vioblk_storage_open(struct storage* sto) {
    // FIXME
    struct vioblk_storage * const vbd =
        (void*)sto - offsetof(struct vioblk_storage, base);
    
    if (!vbd){ return -EINVAL;}
    if (vbd->opened){ return -EBUSY; }
    // enable virqueues
    // Enables the interupt line for the virtio device and sets necessary flags in vioblk * device.
    virtio_enable_virtq(vbd->regs, 0);
    enable_intr_source(vbd->irqno, VIOBLK_INTR_PRIO, vioblk_isr, vbd);
    vbd->opened = 1;
    return 0;
}


// Inputs: struct storage* sto - pointer to generic storage device
// Outputs: n/a
// Description: closes given virtio storage device and disables virtqueues & interrupts
// Side Effects: edits opened flag 
static void vioblk_storage_close(struct storage* sto) {
    // FIXME
    // Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device.
    // If * the given sto is not opened, this function does nothing.

    struct vioblk_storage * const vbd =
        (void*)sto - offsetof(struct vioblk_storage, base);
        if (!vbd || !vbd->opened){ return; }
    
    disable_intr_source(vbd->irqno);
    virtio_reset_virtq(vbd->regs, 0);
    vbd->opened = 0;
    return;
}



// Inputs: struct storage* sto - pointer to storage struct
// 		unsigned long long pos - starting position in virtio device (byte address)
// 		void* buf - pointer to buffer that recieves data 
// 		unsigned long bytecnt - number of bytes to read from virtio device 
// Outputs: long (number of bytes read) 
// Description: read specified number of bytes (bytecount) from vioblk storage device 
// Side Effects: edits avail_idx and used_idx, may cause current thread to suspend, disable & restore interrupts 
static long vioblk_storage_fetch(struct storage* sto, unsigned long long pos, void* buf,
                                unsigned long bytecnt) {
    // FIXME (from vioblk -> buf)
    /**
        * @brief Reads bytecnt number of bytes from the disk and writes them to buf. Achieves this by
        * repeatedly setting the appropriate registers to request a block from the disk, waiting until the
        * data has been populated in block buffer cache, and then writes that data out to buf. Thread
        * sleeps while waiting for the disk to service the request.
        * @param sto Storage IO struct for the storage device
        * @param pos The starting position for the read within the VirtIO device
        * @param buf A pointer to the buffer to fill with the read data
        * @param bytecnt The number of bytes to read from the VirtIO device into the buffer
        * @return The number of bytes read from the device, or negative error code if error
        */
    struct vioblk_storage * const vbd =
        (void*)sto - offsetof(struct vioblk_storage, base);
    // create block request
    // unsigned long blkcnt = bytecnt/vbd->blksz;
    unsigned long long sect = pos / vbd->blksz;
    int pie = disable_interrupts();
    lock_acquire(&vbd->vio_lck);

    // kprintf("sect is: %llu\n", sect);
    // kprintf("lock acquired and interrupt disabled\n");

    struct virtio_blk_req * req = kmalloc(sizeof(struct virtio_blk_req));
    if (!req){
        lock_release(&vbd->vio_lck);
        restore_interrupts(pie);
        return -ENOMEM;
    }

    // kprintf("request blk address: %p\n", req);

    req->type = VIRTIO_BLK_T_IN;
    req->sector = sect;

    // initialize chained descriptors
    // uint16_t group_three = vbd->avail->idx % (vbd->qlen/3);
    // uint16_t req_idx = group_three * 3;
    // uint16_t data_idx = group_three * 3 + 1;
    // uint16_t stat_idx = group_three * 3 + 2;


    // struct virtq_desc * request = &vbd->desc[req_idx];
    // struct virtq_desc * data = &vbd->desc[data_idx];
    // struct virtq_desc * status = &vbd->desc[stat_idx];

    struct virtq_desc * request = &vbd->desc[0];
    struct virtq_desc * data = &vbd->desc[1];
    struct virtq_desc * status = &vbd->desc[2];

    // kprintf("descriptor req address: %p\n", request);  
    // kprintf("descriptor data address: %p\n", data);
    // kprintf("descriptor status address: %p\n", status);  

    // request->addr = (uint64_t)req; // virtio_blk_req structure
    request->addr = (uint64_t)(uintptr_t)req; // set desc addr to request struct 
    request->flags = VIRTQ_DESC_F_NEXT; // write to this
    // request->len = sizeof(req->type) + sizeof(req->reserved) + sizeof(req->sector); 
    request->len = offsetof(struct virtio_blk_req, data);  
    request->next = 1;
    // request->next = data_idx;

    // data->addr = (uint64_t)(&req->data);    // data
    data->addr = (uint64_t)(uintptr_t)req->data; // set data desc address to request data buffer
    // data->flags |= VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
    data->flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE; // there is a next descriptor, write to this
    data->len = (uint32_t)512;
    data->next = 2;
    // data->next = stat_idx;

    // status->addr = (uint64_t)&req->status;     // status bit
    status->addr = (uint64_t)(uintptr_t)&req->status;  // set desc address to status struct
    status->flags = VIRTQ_DESC_F_WRITE; // write to this
    status->len = (uint32_t)sizeof(req->status);

    // kprintf("descriptors all initialized\n");

    // put descriptor into avail index
    uint16_t availidx = vbd->avail->idx % vbd->qlen; // get ring idx
    uint16_t last_used = vbd->used->idx; // get comparison index

    // kprintf("next avail idx is: %u\n", availidx);
    // kprintf("last used idx is: %u\n", last_used);

    // vbd->avail->ring[availidx] = req_idx; // put descriptor idx in avail
    vbd->avail->ring[availidx] = 0; // put descriptor idx in avail

    vbd->avail->idx++; // increment avail idx

    virtio_notify_avail(vbd->regs, 0);

    // kprintf("avail notified\n");

    while (vbd->used->idx == last_used){
        // lock_release(&vbd->vio_lck);
        // condition_wait(&vbd->desc_updated);
        // lock_acquire(&vbd->vio_lck);
        continue;
    }

    int bytes = (bytecnt > vbd->blksz) ? vbd->blksz : bytecnt;

    // kprintf("# of bytes to read: %d\n", bytes);

    if (req->status != VIRTIO_BLK_S_OK){
        kfree(req);
        lock_release(&vbd->vio_lck);
        restore_interrupts(pie);
        return -EIO;
    }

    // read over the proper number of bits
    // kprintf("copying ...\n");
    memcpy(buf, req->data, bytes);
    kfree(req);
    lock_release(&vbd->vio_lck); // release lock
    restore_interrupts(pie);

    // kprintf("done\n");

    return bytes;
}



// Inputs: struct storage* sto - pointer to storage struct
// 		unsigned long long pos - starting position in virtio device (byte address)
// 		void* buf - pointer to buffer that gives data 
// 		unsigned long bytecnt - number of bytes given from buf 
// Outputs: long (number of bytes written to vioblk device) 
// Description: write specified number of bytes (bytecount) to vioblk storage device from given buffer 
// Side Effects: edit avail_idx, & used_idx, may cause current thread to suspend, disable & restore interrupts 
static long vioblk_storage_store(struct storage* sto, unsigned long long pos, const void* buf,
                                unsigned long bytecnt) {
    // FIXME (from buf -> vioblk)

    /**
    * @brief Writes bytecnt number of bytes from the parameter buf to the disk.
    * The size of the virtio device should not change. You should only
    * overwrite existing data. Write should also not create any new files.
    * Achieves this by filling up the block buffer cache and then setting the
    * appropriate registers to request the disk write the contents of the cache
    * to the specified block location.
    * !!! Thread sleeps while waiting for the disk
    * to service the request.
    * @param sto Storage IO struct for the storage device
    * @param pos The starting position for the write within the VirtIO device
    * @param buf A pointer to the buffer with the data to write
    * @param bytecnt The number of bytes to write to the VirtIO device from the
    * buffer
    * @return The number of bytes written to the device, or negative error code
    * if error
    */

    struct vioblk_storage * const vbd = 
        (void*)sto - offsetof(struct vioblk_storage, base);

    // create block request
    // unsigned long blkcnt = bytecnt/vbd->blksz;
    unsigned long long sect = pos / vbd->blksz;
    int pie = disable_interrupts();
    lock_acquire(&vbd->vio_lck); // acquire lock

    struct virtio_blk_req * req = kcalloc(1, sizeof(struct virtio_blk_req));
    if (!req){
        lock_release(&vbd->vio_lck);
        restore_interrupts(pie);
        return -ENOMEM;
    }

    req->type = VIRTIO_BLK_T_OUT;
    req->sector = sect;

    // initialize chained descriptors
    struct virtq_desc * request = &vbd->desc[0];
    struct virtq_desc * data = &vbd->desc[1];
    struct virtq_desc * status = &vbd->desc[2];

    // request->addr = (uint64_t)req;               // virtio_blk_req structure
    request->addr = (uint64_t)(uintptr_t)req;
    request->flags = VIRTQ_DESC_F_NEXT;
    request->len = offsetof(struct virtio_blk_req, data);
    // request->len = sizeof(req->type) + sizeof(req->reserved) + sizeof(req->sector); 
    request->next = 1;

    // data->addr = (uint64_t)(&req->data);    // data
    data->addr = (uint64_t)(uintptr_t)req->data;  
    // data->flags |= VIRTQ_DESC_F_NEXT;
    data->flags = VIRTQ_DESC_F_NEXT;
    data->len = (uint32_t)512;
    data->next = 2;

    // status->addr = (uint64_t)&req->status;       // status bit
    status->addr = (uint64_t)(uintptr_t)&req->status; 
    status->flags = VIRTQ_DESC_F_WRITE;
    status->len = (uint32_t)sizeof(req->status);

    // put descriptor into avail index

    int bytes = (bytecnt > vbd->blksz) ? vbd->blksz : bytecnt;
    // if (bytecnt > vbd->blksz){
    //     bytes = vbd->blksz;
    // }
    
    memcpy(req->data, buf, bytes);

    uint16_t availidx = vbd->avail->idx % vbd->qlen; // get ring idx
    uint16_t last_used = vbd->used->idx; // get comparison index

    vbd->avail->ring[availidx] = 0; // put descriptor idx in avail
    vbd->avail->idx++; // increment avail idx

    virtio_notify_avail(vbd->regs, 0);

    while (vbd->used->idx == last_used){
        // lock_release(&vbd->vio_lck);
        // condition_wait(&vbd->desc_updated);
        // lock_acquire(&vbd->vio_lck);
        continue;
    }
    if (req->status != VIRTIO_BLK_S_OK){
        kfree(req);
        lock_release(&vbd->vio_lck);
        restore_interrupts(pie);
        return -EIO;
    }
    // if (req->status != VIRTIO_BLK_S_OK){  return -EINVAL; }
    
    kfree(req);
    lock_release(&vbd->vio_lck); /// release lock
    restore_interrupts(pie);

    return bytes;
}


// Inputs: int irqno - interrupt request number 
// 		void* aux - pointer for vioblk device
// Outputs: n/a
// Description: interrupt handler for virtio block storage device, called when interrupt occurs
// Side Effects: wakes up other threads waiting on a condition, edits interrupt_status and interrupt_ack registers
static int vioblk_storage_cntl(struct storage* sto, int op, void* arg) {
    // FIXME
    /**
    * @brief Given a file io object, a specific command, and possibly some
    * arguments, execute the corresponding functions on the VirtIO block
    * device.
    * @details Any commands such as FCNTL_GETEND should pass back through the
    * arg variable. Do not directly return the value.
    * @details FCNTL_GETEND should return the capacity of the VirtIO block
    * device in bytes.
    * @param sto Storage IO struct for the storage device
    * @param op Operation to execute. vioblk should support FCNTL_GETEND.
    * @param arg Argument specific to the operation being performed
    * @return Status code on the operation performed
    */

    struct vioblk_storage * const vbd =
        (void*)sto - offsetof(struct vioblk_storage, base);
        // #define FCNTL_GETEND 0 // arg is unsigned long long * **only this one!
        // #define FCNTL_SETEND 1 // arg is unsigned long long *
        // #define FCNTL_GETPOS 2 // arg is unsigned long long *
        // #define FCNTL_SETPOS 3 // arg is unsigned long long *

    if (arg == NULL) {return -EINVAL; }

    if (op == FCNTL_GETEND){
        *(unsigned long long*)arg = vbd->capacity * vbd->blksz;
        return 0;
    }

    return -ENOTSUP;
}


static void vioblk_isr(int irqno, void* aux) {
    // FIXME    
    /**
        * @brief The interrupt handler for the VirtIO device. When an interrupt
        * occurs, the system will call this function.
        * @param irqno The interrupt request number for the VirtIO device
        * @param aux A generic pointer for auxiliary data.
        * @return None
        */

    struct vioblk_storage * const vbd = aux;
    if (!vbd) { return; }
    uint32_t inter = vbd->regs->interrupt_status;
    vbd->regs->interrupt_ack = inter;

    condition_broadcast(&vbd->desc_updated);
   
    return;
}


