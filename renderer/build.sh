#!/bin/sh
# Builds ddraw.dll (32-bit) with the toolchain from tools/setup-toolchain.sh.
set -e
cd "$(dirname "$0")"
W=/home/claude/deps/wcc; L=/home/claude/deps/sysroot32/lib; RT=/home/claude/deps/rt
[ -x $W ] || sh ../tools/setup-toolchain.sh
mkdir -p build
python3 tools/gen_stubs.py /home/claude/deps/sysroot32/include src/stubs.h >/dev/null
OBJS=""
for f in src/*.cpp; do
  o=build/$(basename $f .cpp).o
  $W -O2 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Wno-unused-private-field -Isrc -c $f -o $o
  OBJS="$OBJS $o"
done
clang --target=i686-w64-windows-gnu -fuse-ld=lld -nostdlib -shared -o build/ddraw.dll $OBJS $RT/cxx.o $RT/rt.o \
  src/ddraw.def -L$L -lkernel32 -luser32 -lgdi32 -lmsvcrt -lole32 -Wl,--entry,_DllMainCRTStartup@12 -Wl,--kill-at 2>&1
echo "built build/ddraw.dll"
