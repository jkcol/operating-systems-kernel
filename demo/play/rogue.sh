#!/usr/bin/env bash
# Rogue: full-screen curses, so this one leans hardest on the terminal path --
# raw single-key input and a screen redrawn with ANSI escapes.
source /src/demo/play/lib.sh

beat 3
key " "         # "Press any key ..."
beat 3

for k in l l l l j l l l k l l h h j j; do key "$k"; done

beat 2
key "i"         # inventory
beat 3
key " "
beat 2

key "Q"         # quit
beat 1
key "y"
beat 2
key " "         # clear the -more- on the score line
beat 3
