#!/usr/bin/env bash
# Star Trek: sensors, damage report, shields, library computer.
source /src/demo/play/lib.sh

beat 3          # kernel boot + trek banner
type "n"        # no instructions
beat 3
type ""         # "Hit enter to accept command."
beat 2
type "srs"      # short range sensors
beat 3
type "lrs"      # long range sensors
beat 3
type "dam"      # damage report
beat 3
type "shi"      # shield control
type "1500"
beat 3
type "com"      # library computer
type "0"        # galaxy record
beat 4
type "xxx"      # resign command
beat 2
type "no"       # decline a new mission -> program exits -> kernel halts
beat 3
