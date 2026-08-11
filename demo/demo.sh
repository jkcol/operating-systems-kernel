#!/usr/bin/env bash
# One-command demo: builds the toolchain image, builds the kernel, boots it.
#
#   ./demo/demo.sh          build and boot
#   ./demo/demo.sh build    build only
#   ./demo/demo.sh run      boot only (reuses the last build)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=oskernel-demo

# Git Bash / MSYS rewrites anything that looks like a Unix path before handing
# it to a native .exe, which mangles the container-side paths below.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
MOUNT="$REPO"
command -v cygpath >/dev/null 2>&1 && MOUNT="$(cygpath -w "$REPO")"

docker build -q -t "$IMAGE" -f "$MOUNT/demo/Dockerfile" "$MOUNT" >/dev/null

# Interactive only when there is a terminal to attach (so CI/pipes still work).
TTY=(); [ -t 0 ] && TTY=(-it)

build() { docker run --rm -v "$MOUNT:/src" "$IMAGE" bash /src/demo/build.sh; }
boot()  { docker run --rm "${TTY[@]}" -v "$MOUNT:/src" "$IMAGE" bash /src/demo/run.sh; }

case "${1:-all}" in
    build) build ;;
    run)   boot ;;
    all)   build; boot ;;
    *)     echo "usage: $0 [build|run|all]" >&2; exit 2 ;;
esac
