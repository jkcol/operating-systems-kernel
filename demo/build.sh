#!/usr/bin/env bash
# Build the kernel demo. Runs inside the toolchain container (see Dockerfile).
set -euo pipefail

cd /src

# The course toolchain shipped newlib headers; the Ubuntu bare-metal cross
# compiler does not. These sources need a few declarations, not the library.
COMPAT=/tmp/compat
mkdir -p "$COMPAT/sys"
: > "$COMPAT/stdlib.h"
: > "$COMPAT/stdio.h"
printf '#include "error.h"\n' > "$COMPAT/sys/errno.h"

# elf.c and memory.c use the BSD alignment helpers that <sys/cdefs.h> supplied.
cat > "$COMPAT/sys/cdefs.h" <<'EOF'
#ifndef _COMPAT_SYS_CDEFS_H_
#define _COMPAT_SYS_CDEFS_H_
#include <stdint.h>
/* Used on both integers and pointers by the kernel sources. */
#define __align_down(n, k) ((__typeof__(n))(((uintptr_t)(n) / (k)) * (k)))
#define __align_up(n, k) ((__typeof__(n))((((uintptr_t)(n) + (k) - 1) / (k)) * (k)))
#endif
EOF

echo "==> building userspace"
make -C usr bin/hello >/dev/null

echo "==> building KTFS image"
chmod +x util/mkfs_ktfs
cp usr/bin/hello /tmp/hello
( cd /tmp && /src/util/mkfs_ktfs /src/sys/ktfs.raw 4M 64 hello >/dev/null )

echo "==> building kernel"
make -C sys clean >/dev/null
: > sys/blob.raw            # ramdisk is unused by the demo; keep the blob empty
make -C sys CPPFLAGS="-I$COMPAT" demo-kernel.elf >/dev/null

ls -l sys/demo-kernel.elf sys/ktfs.raw
