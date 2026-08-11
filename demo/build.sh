#!/usr/bin/env bash
# Build the kernel demo. Runs inside the toolchain container (see Dockerfile).
#
#   PROG=<name>   program the kernel execs at boot (default: hello)
#                 one of: hello, trek, rogue, zork
#
set -euo pipefail

cd /src

PROG="${PROG:-hello}"

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

echo "==> building KTFS image (init=$PROG)"
chmod +x util/mkfs_ktfs
STAGE=$(mktemp -d)
cp usr/bin/hello "$STAGE/hello"
cp usr/games/trek usr/games/rogue usr/games/zork usr/games/dtextc.dat "$STAGE/"
( cd "$STAGE" && /src/util/mkfs_ktfs /src/sys/ktfs.raw 8M 64 hello trek rogue zork dtextc.dat >/dev/null )
rm -rf "$STAGE"

echo "==> building kernel"
make -C sys clean >/dev/null
: > sys/blob.raw            # ramdisk is unused by the demo; keep the blob empty
make -C sys CPPFLAGS="-I$COMPAT -DINITEXE=\"\\\"$PROG\\\"\"" demo-kernel.elf >/dev/null

ls -l sys/demo-kernel.elf sys/ktfs.raw
