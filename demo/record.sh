#!/usr/bin/env bash
# Record the asciinema casts that the demo site plays back.
# Runs inside the toolchain container.
#
#   ./demo/record.sh [name ...]      default: all of them
#
set -euo pipefail

cd /src

CASTS=/src/docs/casts
mkdir -p "$CASTS"

record() {
    local name="$1"

    echo "==> building kernel with init=$name"
    PROG="$name" bash /src/demo/build.sh >/dev/null

    echo "==> recording $name"
    rm -f "$CASTS/$name.cast"

    # QEMU gets pipes on both ends; `cat` is what writes to the recorder's pty.
    asciinema rec \
        --cols 100 --rows 30 \
        --title "$name - RISC-V kernel demo" \
        --idle-time-limit 2 \
        --command "bash -c 'bash /src/demo/play/$name.sh | TIMEOUT=180 bash /src/demo/run.sh 2>/dev/null | cat'" \
        "$CASTS/$name.cast" >/dev/null

    echo "    $(wc -c < "$CASTS/$name.cast") bytes"
}

for name in "${@:-hello trek rogue zork}"; do
    record "$name"
done
