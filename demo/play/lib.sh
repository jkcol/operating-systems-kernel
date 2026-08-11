# Helpers for the scripted play-throughs.
#
# Each play script writes keystrokes on stdout; record.sh pipes that into QEMU
# and pipes QEMU's output back out to the recorder. QEMU therefore sees pipes
# on both ends, which is the configuration the kernel's interrupt-driven UART
# is happy with -- give QEMU a pty and the user program's output never arrives.
#
# Nothing here echoes: what shows up in the recording is the guest echoing the
# characters back, which is what makes it read like someone typing.

# type "text" -- send a line at human speed, then Enter.
type() {
    local s=$1 i
    sleep "${TYPE_DELAY:-0.9}"
    for ((i = 0; i < ${#s}; i++)); do
        printf '%s' "${s:i:1}"
        sleep 0.055
    done
    printf '\r'
}

# key "c" -- a single unbuffered keystroke, for games that read raw keys.
key() {
    sleep "${KEY_DELAY:-0.5}"
    printf '%s' "$1"
}

# beat [secs] -- let the screen sit still so the viewer can read it.
beat() { sleep "${1:-2}"; }
