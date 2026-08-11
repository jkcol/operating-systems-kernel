// demo_main.c - kernel entry point for the standalone demo build
//
// Boots the kernel, mounts the KTFS image off the VirtIO block device, and
// execs a user binary out of the filesystem in user mode.
//
// This differs from main.c and tests/test_main.c in one respect: it runs on an
// unmodified qemu-system-riscv64, whose `virt` machine provides a single UART
// at 0x10000000. The course QEMU had a second UART at 0x10000100 that user
// programs wrote to; here the kernel console and the user program share uart0,
// so everything lands on one terminal. Built with -DDEMO_CONSOLE_UART so that
// attach_uart() registers uart0 as an openable device.
//

#include "cache.h"
#include "conf.h"
#include "console.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "dev/virtio.h"
#include "device.h"
#include "error.h"
#include "filesys.h"
#include "heap.h"
#include "intr.h"
#include "memory.h"
#include "process.h"
#include "string.h"
#include "thread.h"
#include "timer.h"
#include "uio.h"

#define CMNTNAME "c"
#define DEVMNTNAME "dev"
#define CDEVNAME "vioblk"
#define CDEVINST 0
#define CONSDEVNAME "uart0"

#ifndef INITEXE
#define INITEXE "hello"
#endif

#define NUART 1     // stock QEMU virt has exactly one
#define NVIODEV 8

static void attach_devices(void);
static void mount_cdrive(void);
static void run_init(void);

void main(void) {
    extern char _kimg_end[];  // provided by kernel.ld

    console_init();

    kprintf("\n");
    kprintf("================================================\n");
    kprintf("  RISC-V kernel demo - rv64 / qemu virt / Sv39\n");
    kprintf("================================================\n");

    intrmgr_init();
    devmgr_init();
    thrmgr_init();
    fsmgr_init();
    heap_init(_kimg_end, RAM_END);
    memory_init();
    procmgr_init();
    kprintf("[boot] subsystems up: intr, dev, thread, fs, heap, memory, proc\n");

    attach_devices();
    enable_interrupts();
    kprintf("[boot] interrupts enabled\n");

    mount_cdrive();
    run_init();
}

void attach_devices(void) {
    int i;
    int result;

    rtc_attach((void*)RTC_MMIO_BASE);

    for (i = 0; i < NUART; i++) attach_uart((void*)UART_MMIO_BASE(i), UART0_INTR_SRCNO + i);

    for (i = 0; i < NVIODEV; i++) attach_virtio((void*)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);

    result = mount_devfs(DEVMNTNAME);

    if (result != 0) {
        kprintf("mount_devfs(%s) failed: %s\n", DEVMNTNAME, error_name(result));
        halt_failure();
    }

    kprintf("[boot] devices attached, devfs mounted at /%s\n", DEVMNTNAME);
}

void mount_cdrive(void) {
    struct storage* hd;
    struct cache* cache;
    int result;

    hd = find_storage(CDEVNAME, CDEVINST);

    if (hd == NULL) {
        kprintf("Storage device %s%d not found\n", CDEVNAME, CDEVINST);
        halt_failure();
    }

    result = storage_open(hd);

    if (result != 0) {
        kprintf("storage_open failed on %s%d: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = create_cache(hd, &cache);

    if (result != 0) {
        kprintf("create_cache(%s%d) failed: %s\n", CDEVNAME, CDEVINST, error_name(result));
        halt_failure();
    }

    result = mount_ktfs(CMNTNAME, cache);

    if (result != 0) {
        kprintf("mount_ktfs(%s, cache(%s%d)) failed: %s\n", CMNTNAME, CDEVNAME, CDEVINST,
                error_name(result));
        halt_failure();
    }

    kprintf("[boot] KTFS on %s%d mounted at /%s (write-back cache)\n", CDEVNAME, CDEVINST,
            CMNTNAME);
}

void run_init(void) {
    struct uio* initexe;
    struct uio* termio;
    struct process* proc;
    char* argv[] = {INITEXE, NULL};
    int result;

    result = open_file(CMNTNAME, INITEXE, &initexe);

    if (result != 0) {
        kprintf("open %s/%s: %s; terminating\n", CMNTNAME, INITEXE, error_name(result));
        halt_failure();
    }

    kprintf("[boot] exec /%s/%s in user mode\n", CMNTNAME, INITEXE);
    kprintf("------------------------------------------------\n");

    // Open the console UART *after* the last kprintf. uart0 is the polled
    // kernel console as well as the device backing the user program's stdio,
    // and once uart_serial_open() turns on the TX interrupt the two writers
    // race: on a pty, where the host applies backpressure, interleaving a
    // polled kputc with the interrupt-driven driver wedges output entirely.

    result = open_file(DEVMNTNAME, CONSDEVNAME, &termio);

    if (result != 0) {
        kprintf("open %s/%s: %s; terminating\n", DEVMNTNAME, CONSDEVNAME, error_name(result));
        halt_failure();
    }

    // Hand the user program stdin/stdout/stderr on the console UART.

    proc = current_process();
    proc->uiotab[0] = termio;
    proc->uiotab[1] = termio;
    proc->uiotab[2] = termio;

    process_exec(initexe, 1, argv);

    kprintf("process_exec returned; terminating\n");
    halt_failure();
}
