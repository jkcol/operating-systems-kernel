#!/usr/bin/env bash
# Zork / Dungeon: streams its 133K data file off KTFS as it plays.
source /src/demo/play/lib.sh

beat 3
type "open mailbox"
beat 2
type "take leaflet"
beat 2
type "read leaflet"
beat 4
type "north"
beat 2
type "east"          # Behind House
beat 2
type "open window"
beat 2
type "enter window"  # Kitchen
beat 3
type "west"          # Living Room
beat 3
type "take lamp"
beat 2
type "inventory"
beat 3
type "quit"
beat 1
type "y"
beat 3
