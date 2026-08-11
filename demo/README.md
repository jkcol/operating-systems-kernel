# Demo

Boots the kernel end to end on an **unmodified** `qemu-system-riscv64`, with no
host toolchain required beyond Docker.

```bash
./demo/demo.sh
```

Output:

```
================================================
  RISC-V kernel demo - rv64 / qemu virt / Sv39
================================================
[boot] subsystems up: intr, dev, thread, fs, heap, memory, proc
[boot] devices attached, devfs mounted at /dev
[boot] interrupts enabled
[boot] KTFS on vioblk0 mounted at /c (write-back cache)
[boot] exec /c/hello in user mode
------------------------------------------------
I am the parent!
Hello, world! (child)
Hello, world (parent)!
```

The last three lines come from a user-mode process, so reaching them exercises
Sv39 page-table setup, the VirtIO block driver, the block cache, KTFS, the ELF
loader, the trap path, and `fork` / `wait` / `write` across the syscall
boundary.

## What it does

`demo/build.sh` links `usr/bin/hello` into a KTFS image with `util/mkfs_ktfs`,
builds `sys/demo-kernel.elf`, and `demo/run.sh` boots it with the image attached
as a VirtIO block device.

## Why a separate kernel entry point

`sys/demo/demo_main.c` exists because neither `sys/main.c` nor
`sys/tests/test_main.c` boots on stock QEMU:

- `sys/main.c` — `run_init()` opens the init binary but never execs it, so the
  kernel mounts the filesystem and then falls off the end of `main`.
- `sys/tests/test_main.c` — attaches `NUART 6` UARTs, and the active test opens
  `dev/uart1`. `conf.h` places UART1 at `0x10000100`, which only exists on the
  course's patched QEMU; the stock `virt` machine has a single UART at
  `0x10000000`, so the second `attach_uart()` takes a store access fault.

`demo_main.c` attaches one UART and, via `-DDEMO_CONSOLE_UART`, lets `uart0`
double as both the kernel console and the user program's terminal — so kernel
and user output land on the same stdout. Everything else is the unmodified
kernel.

## Toolchain notes

`demo/build.sh` synthesizes three headers the Ubuntu bare-metal cross compiler
does not ship but the sources include: `<stdlib.h>`, `<stdio.h>`, and
`<sys/cdefs.h>` (for `__align_up` / `__align_down`, used by `elf.c` and
`memory.c`). The course toolchain supplied these via newlib.

`usr/progs/date.c` includes `<stdio.h>` and does not build here, so the demo
builds only `usr/bin/hello`.
