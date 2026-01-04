/*! @file uio.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‌‌‍‍‍‌​​‌​⁠⁠‌
    @brief Uniform I/O interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#include <stdio.h>
#ifdef UIO_DEBUG
#define DEBUG
#endif

#ifdef UIO_TRACE
#define TRACE
#endif

#include "uio.h"

#include <stddef.h>  // for NULL and offsetof

#include "error.h"
#include "heap.h"
#include "memory.h"
#include "misc.h"
#include "thread.h"
#include "uioimpl.h"

static void nulluio_close(struct uio* uio);

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz);

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    void * data;
};

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// pipe helpers

void pipe_reader_close(struct uio* uio);
long pipe_reader_read(struct uio* uio, void* buf, unsigned long len);
void pipe_writer_close(struct uio* uio);
long pipe_writer_write(struct uio* uio, const void* buf, unsigned long len);

struct pipe {
    struct uio wio;
    struct uio rio;
    struct ringbuf rbuf;
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when tx becomes not full
    struct lock lock;
};

static const struct uio_intf pipe_reader_intf = { 
    .close = pipe_reader_close,
    .read = pipe_reader_read
};

static const struct uio_intf pipe_writer_intf = { 
    .close = pipe_writer_close,
    .write = pipe_writer_write
};

void uio_close(struct uio* uio) {
    debug("uio_close: refcnt=%d, has_close=%d", uio->refcnt, (uio->intf->close != NULL));

    // Decrement reference count if it's greater than 0
    if (uio->refcnt > 0) {
        uio->refcnt--;
        debug("uio_close: decremented refcnt to %d", uio->refcnt);
    }

    // Only call the actual close method when refcnt reaches 0
    if (uio->refcnt == 0 && uio->intf->close != NULL) {
        debug("uio_close: calling close method");
        uio->intf->close(uio);
    } else if (uio->refcnt > 0) {
        debug("uio_close: NOT calling close (refcnt=%d still has references)", uio->refcnt);
    }
}

long uio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    if (uio->intf->read != NULL) {
        if (0 <= (long)bufsz)
            return uio->intf->read(uio, buf, bufsz);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

long uio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    if (uio->intf->write != NULL) {
        if (0 <= (long)buflen)
            return uio->intf->write(uio, buf, buflen);
        else
            return -EINVAL;
    } else
        return -ENOTSUP;
}

int uio_cntl(struct uio* uio, int op, void* arg) {
    if (uio->intf->cntl != NULL)
        return uio->intf->cntl(uio, op, arg);
    else
        return -ENOTSUP;
}

unsigned long uio_refcnt(const struct uio* uio) {
    assert(uio != NULL);
    return uio->refcnt;
}

int uio_addref(struct uio* uio) { return ++uio->refcnt; }

struct uio* create_null_uio(void) {
    static const struct uio_intf nulluio_intf = {
        .close = &nulluio_close, .read = &nulluio_read, .write = &nulluio_write};

    static struct uio nulluio = {.intf = &nulluio_intf, .refcnt = 0};

    return &nulluio;
}

static void nulluio_close(struct uio* uio) {
    // ...
}

static long nulluio_read(struct uio* uio, void* buf, unsigned long bufsz) {
    // ...
    return -ENOTSUP;
}

static long nulluio_write(struct uio* uio, const void* buf, unsigned long buflen) {
    // ...
    return -ENOTSUP;
}

// PIPE IS BASED HEAVILY ON UART IMPLEMENTATION

// allocates a pipe and sets the args to be in the pipe
void create_pipe(struct uio **wptr, struct uio **rptr){

    if(!wptr || !rptr){
        return;
    }
    
    // allocate space for pipe and buffer
    struct pipe * pipe = kmalloc(sizeof(struct pipe));
    void * pp = alloc_phys_page();
    
    // initialize buffer
    pipe->rbuf.data = pp; 
    rbuf_init(&pipe->rbuf);
    
    // create conditions
    condition_init(&pipe->rxbnotempty, "rbtxnotempty");
    condition_init(&pipe->txbnotfull, "txbnotfull");
    
    // populate uios with correct wrappers
    uio_init1(&pipe->wio, &pipe_writer_intf);
    uio_init1(&pipe->rio, &pipe_reader_intf);

    // init lock
    lock_init(&pipe->lock);
    
    // place each uio into args
    *rptr = &pipe->rio;
    *wptr = &pipe->wio;
}   

// RING BUFFER FUNCTIONS FROM UART
void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}

int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == PAGE_SIZE);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    ((char*)rbuf->data)[tpos % PAGE_SIZE] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = ((char*)rbuf->data)[hpos % PAGE_SIZE];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// closes the reader end of the pipe
void pipe_reader_close(struct uio* uio){

    if(!uio){
        return;
    }

    struct pipe * pipe = (void*)uio - offsetof(struct pipe, rio);
    
    lock_acquire(&pipe->lock);

    // need to handle two cases:
    // 1. only the reader is closed -> signal writer that reader is closed
    // 2. both reader and writer are closed -> free everything
    if(pipe->rio.refcnt <= 0){
        if(pipe->wio.refcnt <= 0){
            lock_release(&pipe->lock);
            free_phys_page(pipe->rbuf.data);
            kfree(pipe);
            return;
        }

        condition_broadcast(&pipe->txbnotfull);
    }

    lock_release(&pipe->lock);
}

// reads from the pipe
long pipe_reader_read(struct uio* uio, void* buf, unsigned long len){

    if(!uio || !buf || len < 0){
        return -EIO;
    }

    if(len == 0){
        return 0;
    }

    struct pipe * pipe = (void*)uio - offsetof(struct pipe, rio);

    int bread = 0;

    lock_acquire(&pipe->lock);

    // if the pipe is empty, account for two conditions:
    // 1. If the writer still exists -> sleep until something is written
    // 2. If writer is gone -> return 0
    while(rbuf_empty(&pipe->rbuf)){
        if(pipe->wio.refcnt == 0){
            lock_release(&pipe->lock);
            return 0;
        }

        // unlock to prevent deadlock
        lock_release(&pipe->lock);
        condition_wait(&pipe->rxbnotempty);
        lock_acquire(&pipe->lock);
    }

    // read data from pipe buffer to buf && signal that the buffer is not full
    while(!rbuf_empty(&pipe->rbuf) && bread < len){ 
            ((char*)buf)[bread++] = rbuf_getc(&pipe->rbuf);
    }
    condition_broadcast(&pipe->txbnotfull);

    lock_release(&pipe->lock);

    return bread;
}

// closes the writer end of the pipe
void pipe_writer_close(struct uio* uio){

    if(!uio){
        return;
    }

    struct pipe * pipe = (void*)uio - offsetof(struct pipe, wio);

    lock_acquire(&pipe->lock);

    // need to handle two cases:
    // 1. only the reader is closed -> signal writer that reader is closed
    // 2. both reader and writer are closed -> free everything
    if(pipe->wio.refcnt <= 0){
        if(pipe->rio.refcnt <= 0){
            lock_release(&pipe->lock);
            free_phys_page(pipe->rbuf.data);
            kfree(pipe);
            return;
        }

        condition_broadcast(&pipe->rxbnotempty);
    }

    lock_release(&pipe->lock);
}

// writes to the pipe
long pipe_writer_write(struct uio* uio, const void* buf, unsigned long len){

    if(!uio || !buf || len < 0){
        return -EIO;
    }

    if(len == 0){
        return 0;
    }

    struct pipe * pipe = (void*)uio - offsetof(struct pipe, wio);

    int bwrit = 0;

    lock_acquire(&pipe->lock);

    // again, account for two cases
    // 1. buffer is full, but reader still exists -> wait for buffer to not be full ansd return 0
    // 2. reader doesn't exist -> return EPIPE
    while(rbuf_full(&pipe->rbuf)){
        if(pipe->rio.refcnt == 0){
            lock_release(&pipe->lock);
            return -EPIPE;
        }
        
        // unlock to prevent deadlock
        lock_release(&pipe->lock);
        condition_wait(&pipe->txbnotfull);
        lock_acquire(&pipe->lock);
    }

    // write from the buffer to the internal buffer && signal that the internal buffer is not empty
    while(!rbuf_full(&pipe->rbuf) && bwrit < len){
            rbuf_putc(&pipe->rbuf, ((const char *)buf)[bwrit++]);
    }
    condition_broadcast(&pipe->rxbnotempty);

    lock_release(&pipe->lock);

    return bwrit;
}