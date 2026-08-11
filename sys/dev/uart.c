// uart.c -  NS8250-compatible serial port
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef UART_TRACE
#define TRACE
#endif

#ifdef UART_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "misc.h"
#include "uart.h"
#include "devimpl.h"
#include "intr.h"
#include "heap.h"
#include "thread.h"
#include "console.h"

#include "error.h"

#include <stdint.h>

// COMPILE-TIME CONSTANT DEFINITIONS
//

#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif

#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif

#ifndef UART_DEVNAME
#define UART_DEVNAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 

struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
    
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
    
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };

    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};

#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)

// Simple fixed-size ring buffer

struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};

// UART device structure

struct uart_serial {
    struct serial base;
    volatile struct uart_regs * regs;
    int irqno;
    char opened;

    unsigned long rxovrcnt; ///< number of times OE was set
    
    struct condition rxbnotempty; ///< signalled when rxbuf becomes not empty
    struct condition txbnotfull;  ///< signalled when txbuf becomes not full

    struct ringbuf rxbuf;
    struct ringbuf txbuf;

    struct lock uart_lock;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int uart_serial_open(struct serial * ser);
static void uart_serial_close(struct serial * ser);
static int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz);
static int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz);

static void uart_isr(int srcno, void * aux);

// Ring buffer (struct rbuf) functions

static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);

// INTERNAL GLOBAL VARIABLES
//

static const struct serial_intf uart_serial_intf = {
    .blksz = 1,
    .open = &uart_serial_open,
    .close = &uart_serial_close,
    .recv = &uart_serial_recv,
    .send = &uart_serial_send
};

// EXPORTED FUNCTION DEFINITIONS
// 


void attach_uart(void * mmio_base, int irqno) {
    struct uart_serial * uart;

    trace("%s(%p,%d)", __func__, mmio_base, irqno);
    
    // UART0 is used for the console and should not be attached as a normal
    // device. It should already be initialized by console_init(). We still
    // register the device (to reserve the name uart0), but pass a NULL device
    // pointer, so that find_serial("uart", 0) returns NULL.

    // The demo build runs on stock QEMU, which has only uart0, so the console
    // UART has to double as an openable device there.

#ifndef DEMO_CONSOLE_UART
    if (mmio_base == (void*)UART0_MMIO_BASE) {
        register_device(UART_DEVNAME, DEV_SERIAL, NULL);
        return;
    }
#endif
    
    uart = kcalloc(1, sizeof(struct uart_serial));

    uart->regs = mmio_base;
    uart->irqno = irqno;
    uart->opened = 0;

    // Initialize condition variables. The ISR is registered when our interrupt
    // source is enabled in uart_serial_open().

    condition_init(&uart->rxbnotempty, "uart.rxnotempty");
    condition_init(&uart->txbnotfull, "uart.txnotfull");


    // Initialize hardware

    uart->regs->ier = 0;
    uart->regs->lcr = LCR_DLAB;
    // fence o,o ?
    uart->regs->dll = 0x01;
    uart->regs->dlm = 0x00;
    // fence o,o ?
    uart->regs->lcr = 0; // DLAB=0

    lock_init(&uart->uart_lock);
    serial_init(&uart->base, &uart_serial_intf);
    register_device(UART_DEVNAME, DEV_SERIAL, uart);
}

// int uart_serial_open(struct serial * ser)
// Inputs: struct serial * ser - Pointer to the serial structure   
// Outputs: Returns 0 if successful, -EBUSY if not  
// Description: Prepares the UART for communication by opening the device   
// Side Effects: None

int uart_serial_open(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    if (uart->opened)
        return -EBUSY;
    
    // Reset receive and transmit buffers
    
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);

    // Read receive buffer register to flush any stale data in hardware buffer

    uart->regs->rbr; // forces a read because uart->regs is volatile

    // Enable interrupts when data ready (DR) status asserted

    // FIXME your code goes here 
        
    uart->regs->ier |= IER_DRIE; // enable interrupt whenever DR

    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart); // enable interrupt 

    uart->opened = 1; // opened flag

    return 0;
}

// void uart_serial_close(struct serial * ser)
// Inputs: struct serial * ser - Pointer to the serial structure   
// Outputs: None  
// Description: Closes the serial device and disables interrupts   
// Side Effects: None

void uart_serial_close(struct serial * ser) {
    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    trace("%s()", __func__);

    // FIXME your code goes here

    if(!uart->opened){ // if not opened, do nothing
        return;
    }

    uart->regs->ier = 0; // disable interrupts

    disable_intr_source(uart->irqno); // disable source 

    uart->opened = 0; // indicate closed
}

// int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: struct serial * ser - Pointer to the serial structure   
//         void * buf - Buffer to read data into 
//         unsigned int bufsz - Size of buffer reading to 
// Outputs: Returns number of bytes read   
// Description: Reads data from the ring buffer into provided buffer  
// Side Effects: None

int uart_serial_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here

    int bread = 0; // initializations
    int pie; 

    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    if(!uart->opened){ // return if unopened
        return -EINVAL;
    }    

    if(bufsz == 0){
        return 0;
    }

    lock_acquire(&uart->uart_lock);

    while(bread < bufsz){

        pie = disable_interrupts(); // disable interrupts 

        while(rbuf_empty(&uart->rxbuf)){ // wait until data available using conditions, then read 
            uart->regs->ier |= IER_DRIE; // enable interrupts 
            condition_wait(&uart->rxbnotempty); 
            pie = disable_interrupts(); // disable 
        }

        while(!rbuf_empty(&uart->rxbuf) && bread < bufsz){ // read data 
            ((char*)buf)[bread++] = rbuf_getc(&uart->rxbuf);
        }

        uart->regs->ier |= IER_DRIE; // enable interrupt
        restore_interrupts(pie);
    }

    lock_release(&uart->uart_lock);

    return bread;
}


// int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz)
// Inputs: struct serial * ser - Pointer to the serial structure   
//         const void * buf - Buffer to write data out of 
//         unsigned int bufsz - Size of buffer writing to 
// Outputs: Returns number of bytes written  
// Description: Sends data from given buffer to write buffer    
// Side Effects: None

int uart_serial_send(struct serial * ser, const void * buf, unsigned int bufsz) {
    // FIXME your code goes here

    int bwrit = 0; // initializations 
    int pie;

    struct uart_serial * const uart =
        (void*)ser - offsetof(struct uart_serial, base);

    if(!uart->opened){ // return if unopened
        return -EINVAL;
    }    

    if(bufsz == 0){
        return 0;
    }

    lock_acquire(&uart->uart_lock);

    while(bwrit < bufsz){ // write to buffer when not full

        pie = disable_interrupts(); // disable interrupts 
 
        while(rbuf_full(&uart->txbuf)){ // wait on condition variable 
            uart->regs->ier |= IER_THREIE; // enable interrupts
            condition_wait(&uart->txbnotfull);
            pie = disable_interrupts(); // disable
        }

        while(!rbuf_full(&uart->txbuf) && bwrit < bufsz){
            rbuf_putc(&uart->txbuf, ((const char *)buf)[bwrit++]);
        }

        uart->regs->ier |= IER_THREIE; // enable interrupt
        restore_interrupts(pie);
    }

    lock_release(&uart->uart_lock);

    return bwrit;
}

// void uart_isr(int srcno, void * aux)
// Inputs: int srcno - Source of interrupt 
//         void * aux - Uart pointer 
// Outputs: None 
// Description: Handles the UART interrupt service routine   
// Side Effects: None

void uart_isr(int srcno, void * aux) {
    // FIXME your code goes here

    struct uart_serial * const uart = aux;  //get uart 

    while(!rbuf_full(&uart->rxbuf) && (uart->regs->lsr & LSR_DR)){ // if space in read buf, and DR
        rbuf_putc(&uart->rxbuf, uart->regs->rbr); // read to ring buf 
    }
    if(rbuf_full(&uart->rxbuf)){
        uart->regs->ier &= ~IER_DRIE; // disable interrupts if full 
    }

    while(!rbuf_empty(&uart->txbuf) && (uart->regs->lsr & LSR_THRE)){ // if there are charcters to write, and THRIE
        uart->regs->thr = rbuf_getc(&uart->txbuf); // write to thr buf 
    }
    if(rbuf_empty(&uart->txbuf)){
        uart->regs->ier &= ~IER_THREIE; // disable interrupts of empty 
    }

    if(!rbuf_empty(&uart->rxbuf)){ // broadcast if rxbuf not empty
        condition_broadcast(&uart->rxbnotempty);
    }
    if(!rbuf_full(&uart->txbuf)){ // broadcast if txbuf not full
        condition_broadcast(&uart->txbnotfull);
    }
}


void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}



int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return (rbuf->tpos - rbuf->hpos == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;

    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}

char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;

    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}

// The functions below provide polled uart input and output for the console.

#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)

void console_device_init(void) {
    UART0.ier = 0x00;

    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
    
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;

    // The com0_putc and com0_getc functions assume DLAB=0.

    UART0.lcr = 0;
}

void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;

    UART0.thr = c;
}

char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
    
    return UART0.rbr;
}