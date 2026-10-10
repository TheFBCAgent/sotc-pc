# gdb setup for the runtime on Linux (session 29). The GPU page tracker and guest faults raise
# SIGSEGV all the time and the runtime handles them, so gdb passes them on silently and stops
# only once the runtime gives up on a fault (fault_reported in main.c, after its crash report).
#   gdb -x scripts/sotc.gdb --args build-vk/sotc-runtime build/game/boot.img
# Needs SOTC_GAME_DIR (and SOTC_GPU=vulkan, SOTC_USER_DIR=build/user, ... as smoke.py sets).
# At the stop: `bt` (the frames above "<signal handler called>" are the faulting code; guest
# code shows as ??, look it up with the crash report's module+offset), `info threads`.
set debuginfod enabled off
set pagination off
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGILL nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
break fault_reported
