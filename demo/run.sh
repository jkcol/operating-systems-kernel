#!/usr/bin/env bash
# Boot the demo kernel under QEMU. Runs inside the toolchain container.
#
#   TIMEOUT=<secs>   kill QEMU after this long (default: 30)
#
set -euo pipefail

cd /src/sys

exec timeout "${TIMEOUT:-30}" qemu-system-riscv64 \
    -global virtio-mmio.force-legacy=false \
    -machine virt -bios none -nographic -m 8M \
    -kernel demo-kernel.elf \
    -object rng-random,filename=/dev/urandom,id=rng0 \
    -device virtio-rng-device,rng=rng0 \
    -drive file=ktfs.raw,id=blk0,if=none,format=raw,readonly=false \
    -device virtio-blk-device,drive=blk0
