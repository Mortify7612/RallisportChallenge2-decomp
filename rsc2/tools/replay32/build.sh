#!/usr/bin/env bash
# Builds the i386 replay harness. Static and non-PIE, so it loads at
# 0x08048000 and leaves the guest's low 64 MB free to map at fixed addresses.
set -euo pipefail
cd "$(dirname "$0")"
gcc -m32 -O1 -g -static -no-pie -Wall -o replay32 replay32.c tramp.S
