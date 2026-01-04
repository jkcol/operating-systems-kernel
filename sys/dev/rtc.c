// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "thread.h"
#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "rtc.h"
#include "conf.h"
#include "misc.h"
#include "devimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t time_low;  // read first, latches time_high
    uint32_t time_high; //
};

struct rtc_device {
    struct serial base; // must be first
    volatile struct rtc_regs * regs;
    struct lock rtc_lock;
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct serial * ser);
static void rtc_close(struct serial * ser);
static int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz);

static uint64_t read_real_time(volatile struct rtc_regs * regs);

// INTERNAL GLOBAL VARIABLES AND CONSTANTS
//

static const struct serial_intf rtc_serial_intf = {
    .blksz = 8,
    .open = &rtc_open,
    .close = &rtc_close,
    .recv = &rtc_recv
};

// EXPORTED FUNCTION DEFINITIONS
// 

// void rtc_attatch(void * mmio_base)
// Inputs: void * mmio_base - base address for IO
// Outputs: none
// Description: Registers the RTC device to the system
// Side Effects: None

void rtc_attach(void * mmio_base) {
    // FIXME your code goes here
    struct rtc_device* rtc;
	rtc = kcalloc(1, sizeof(struct rtc_device));
	rtc->regs = mmio_base;
    lock_init(&rtc->rtc_lock);
	serial_init(&rtc->base, &rtc_serial_intf);
	register_device("rtc", DEV_SERIAL, rtc);
}

int rtc_open(struct serial * ser) {
    trace("%s()", __func__);
    return 0;
}

void rtc_close(struct serial * ser) {
    trace("%s()", __func__);
}

// void rtc_recv(struct serial * ser, void * buf, unsigned int bufsz)
// Inputs: struct serial * ser - pointer to serial device structure
//         void * buf - buffer to write timestamp to 
//         unisigned int bufsz - size of buffer to write to 
// Outputs: none
// Description: Gets the current time of the clock and copies it into buf 
// Side Effects: None

int rtc_recv(struct serial * ser, void * buf, unsigned int bufsz) {
    // FIXME your code goes here
    struct rtc_device * const rtc = (void *)ser - offsetof(struct rtc_device, base);
	if(bufsz == 0 || bufsz < 0){
		return 0;
	}

    lock_acquire(&rtc->rtc_lock); // lock to prevent multi-thread recieves 

	uint64_t time_now = read_real_time(rtc->regs);
	memcpy(buf, &time_now, sizeof(uint64_t));

    lock_release(&rtc->rtc_lock);

    return sizeof(uint64_t);
}

// uint64_t read_real_time(volatile struct rtc_regs * regs)
// Inputs: volatile struct rtc_regs * regs - pointer to memory-mapped registers
// Outputs: Returns the current time 
// Description: Helper funciton that reads 64-bit timestamp
// Side Effects: None

uint64_t read_real_time(volatile struct rtc_regs * regs) {
    // FIXME your code goes here
    uint32_t low, high;
	low = regs->time_low;
	high = regs->time_high;
	uint64_t time = (uint64_t)high<<(32)|low;
	return time;
}