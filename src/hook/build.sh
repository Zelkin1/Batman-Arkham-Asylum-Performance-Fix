#!/bin/bash
# builds faultrep.dll (drop-in) and BmFixHook.dll (same binary, for injection by a test harness) - 32-bit, no CRT,
# with the msys2 ucrt64 toolchain (gcc -m32 + ld i386pe)
set -e
export PATH=/c/msys64/ucrt64/bin:$PATH
cd "$(dirname "$0")"
for l in kernel32 user32 shell32; do dlltool -m i386 -f --32 -k -d $l.def -l lib$l.a; done
gcc -m32 -O2 -Wall -Wno-unused-function -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fno-stack-protector -mno-stack-arg-probe -fno-asynchronous-unwind-tables -c BmFixHook.c -o BmFixHook.o
ld -m i386pe --dll -e _DllMain@12 --enable-stdcall-fixup --kill-at --dynamicbase --nxcompat --image-base 0x6A800000 -o faultrep.dll exports.def BmFixHook.o libkernel32.a libuser32.a libshell32.a
strip -s faultrep.dll 2>/dev/null || true
cp faultrep.dll BmFixHook.dll
echo built faultrep.dll + BmFixHook.dll
