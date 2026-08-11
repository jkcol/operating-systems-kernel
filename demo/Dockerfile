FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc-riscv64-unknown-elf \
        binutils-riscv64-unknown-elf \
        qemu-system-misc \
        asciinema \
        file \
        xxd \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
