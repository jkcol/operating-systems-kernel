// viorng.c - VirtIO rng device
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "devimpl.h"
#include "misc.h"
#include "conf.h"
#include "intr.h"
#include "console.h"

// INTERNAL CONSTANT DEFINITIONS
//

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "viorng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

// INTERNAL TYPE DEFINITIONS
//

// viorng serial struct 

struct viorng_serial {
    // FIXME your code goes here
    struct serial base; // struct members from uart 
    volatile struct virtio_mmio_regs* regs;
    int irqno;
    char opened;

    volatile struct virtq_desc * desc; // virtq queues
    volatile struct virtq_avail * avail;
    volatile struct virtq_used * used;

    uint16_t qlen; // queue length

    struct condition usedbnotempty; // condition

    struct lock vio_lock; // lock for recv

    uint8_t vbuf[256];
};

// INTERNAL FUNCTION DECLARATIONS
//

static int viorng_serial_open(struct serial * ser);

static void viorng_serial_close(struct serial * ser);
static int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);

static void viorng_isr(int irqno, void * aux);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf viorng_serial_intf = {
    .blksz = 1,
    .open = &viorng_serial_open,
    .close = &viorng_serial_close,
    .recv = &viorng_serial_recv
};

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO rng device. Declared and called directly from virtio.c.

// void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
// Inputs: volatile struct virtio_mmio_regs * regs - mmio register pointer 
//         int irqno - interrupt source number
// Outputs: None  
// Description: Attatches the viorng device. Sets feature bits, fills out viorng struct, and registers the device. 
// Side Effects: None

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct viorng_serial * vrng;
    int result;
    
    assert (regs->device_id == VIRTIO_ID_RNG);

    // Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    // fence o,io
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // Allocate and initialize device struct

    // FIXME your code goes here 

    vrng = kcalloc(1, sizeof(*vrng)); // allocate memory for vrng and its queues 
    
    if(!vrng){ //check if nullptr
        return;
    }

    size_t descsz = (size_t)sizeof(struct virtq_desc); // get sizes of queues 
    size_t availsz = (size_t)VIRTQ_AVAIL_SIZE(1);
    size_t usedsz = (size_t)VIRTQ_USED_SIZE(1);

    size_t total = descsz + availsz + usedsz; // add sizes 

    uint8_t * totv = kcalloc(1, total); // create continuous block

    if(!totv){ // if kcalloc fails
        kfree(vrng);
        return;
    }

    vrng->desc = (struct virtq_desc*)totv; // place queues in correct memory 
    vrng->avail = (struct virtq_avail*)(totv + descsz);
    vrng->used = (struct virtq_used*)(totv + descsz + availsz);

    vrng->regs = regs; // init struct vars
    vrng->irqno = irqno;
    vrng->opened = 0;

    vrng->qlen = 1;

    vrng->desc[0].addr = (uint64_t)(uintptr_t)&vrng->vbuf; // set address to address of buffer 
    vrng->desc[0].len = sizeof(vrng->vbuf); // init length of buffer
    vrng->desc[0].flags = VIRTQ_DESC_F_WRITE; // set so that queue is WRITE ONLY
    vrng->desc[0].next = 0; // no chain  

    vrng->avail->flags = 0; // init avail and used just in case 
    vrng->avail->idx = 0;
    vrng->used->flags = 0;
    vrng->used->idx = 0; 

    condition_init(&vrng->usedbnotempty, "vrng.usedbnotempty"); // initialize condition variable 

    lock_init(&vrng->vio_lock); // initialize lock

    virtio_attach_virtq(regs, 0, 1, (uint64_t)(uintptr_t)vrng->desc, (uint64_t)(uintptr_t)vrng->used, (uint64_t)(uintptr_t)vrng->avail); // attatch vrng 

    regs->status |= VIRTIO_STAT_DRIVER_OK; //set the driver to OK
    // fence o,oi
    __sync_synchronize();

    // FIXME your code goes here

    serial_init(&vrng->base, &viorng_serial_intf); // serial struct init

    register_device(VIORNG_NAME, DEV_SERIAL, vrng); // device is registered
}

// int viorng_serial_open(struct serial * ser)
// Inputs: struct serial * ser - pointer to serial device 
// Outputs: returns 0 upon success, -EBUSY if not 
// Description: Creates serial vrng struct, enables the queues for use, enables 
//              the vrng isr, and opens the serial dvice  
// Side Effects: None

int viorng_serial_open(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial * const vrng =
        (void*)ser - offsetof(struct viorng_serial, base); // create vrng 

    if(!vrng){ // return if nonexistent
        return -EINVAL;
    }
    if(vrng->opened){ // return if already opened
        return -EBUSY;
    }

    virtio_enable_virtq(vrng->regs, 0); // enable virqueues 

    enable_intr_source(vrng->irqno, VIORNG_IRQ_PRIO, viorng_isr, vrng); // enable vrng interrupt source

    vrng->opened = 1; // set opened flag 

    return 0;
}

// void viorng_serial_close(struct serial * ser)
// Inputs: struct serial * ser - pointer to serial device 
// Outputs: None 
// Description: Closes the device by setting the proper flags and resetting the queues 
// Side Effects: None

void viorng_serial_close(struct serial * ser) {
    // FIXME your code goes here
    struct viorng_serial * const vrng =
        (void*)ser - offsetof(struct viorng_serial, base); // create vrng

    if(!vrng || !vrng->opened){ // if unopened or nonexistent, dont close
        return;
    }

    disable_intr_source(vrng->irqno); // disable interrupts 
    virtio_reset_virtq(vrng->regs, 0); // reset virtuques which disables interrupts 
    vrng->opened = 0; // close
}


// int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: struct serial * ser - pointer to serial device 
//         void * buf - buffer to write random bits into 
//         unsigned int bufsz - size of given buffer
// Outputs: Number of bits obtained  
// Description: Requests the entropy device to provide random bits, waits until
//              bits are given, then writes them into buf 
// Side Effects: None

int viorng_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    int pie;

    struct viorng_serial * const vrng =
        (void*)ser - offsetof(struct viorng_serial, base); // create vrng 

    if(bufsz == 0){ // check edge case
        return 0;
    }
        
    if(!vrng || !vrng->opened || !buf){ // check other edge cases
        return -EINVAL;
    }

    lock_acquire(&vrng->vio_lock);

    pie = disable_interrupts();

    uint16_t availidx = vrng->avail->idx % vrng->qlen; // get ring idx

    uint16_t last_used = vrng->used->idx; // get comparison index 

    vrng->avail->ring[availidx] = 0; // put descriptor idx in avail
    __sync_synchronize();

    vrng->avail->idx ++; // increment idx
    __sync_synchronize();

    virtio_notify_avail(vrng->regs, 0); // notify device that it can process queue 
    
    while(vrng->used->idx == last_used){ // wait with condition variable
        condition_wait(&vrng->usedbnotempty);
        pie = disable_interrupts();
    }

    unsigned int bread = vrng->used->ring[last_used % vrng->qlen].len; // read how many bytes was written to used 

    if(bread > bufsz){ // if wrote more bytes than size, just read size 
        bread = bufsz;
    }

    memcpy(buf, vrng->vbuf, bread); // write entropy numbers into buf 

    lock_release(&vrng->vio_lock);

    restore_interrupts(pie);

    return bread; // return how many bytes read
}

// void viorng_isr(int irqno, void * aux)
// Inputs: int irqno - interrupt source # 
//         void * aux - extra pointer 
// Outputs: None   
// Description: Sets device registers for interrupt and then services interrupt request 
// Side Effects: None

void viorng_isr(int irqno, void * aux) {
    // FIXME your code goes here
    struct viorng_serial * const vrng = aux;  //get vrng

    if(!vrng){ // nullptr check
        return;
    }

    uint32_t inter = vrng->regs->interrupt_status; // read interrupt status

    vrng->regs->interrupt_ack = inter; // acknowledge that interrupt has been handled 

    condition_broadcast(&vrng->usedbnotempty); // wake all waiting threads 
}
