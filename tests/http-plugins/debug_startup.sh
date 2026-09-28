#!/bin/sh
# Capture a crash backtrace without downloading anything on the target server.
set -eu
export DEBUGINFOD_URLS=

printf 'Kernel: '
uname -r
cat /proc/self/limits
dpkg-query -W libc6 libsofia-sip-ua0 libcurl4 libjson-c5 libssl3 gdb

# SIGSEGV stops GDB immediately. If startup succeeds, interrupt after 30 seconds
# and print the live threads instead; timeout then returns 124.
exec timeout --signal=INT --kill-after=5s 30s \
  gdb -q -nx -batch \
    -ex 'set debuginfod enabled off' \
    -ex 'set disable-randomization off' \
    -ex 'set print frame-arguments none' \
    -ex run \
    -ex 'thread apply all bt 30' \
    -ex 'info sharedlibrary' \
    --args /opt/unimrcp/bin/unimrcpserver -r /opt/unimrcp -w -o 1 -l 7
