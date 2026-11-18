#!/bin/sh
# Rebuilds the Windows build environment in a fresh Linux workspace, using only the system clang/lld
# and the free mingw-w64 headers (fetched with git). Result:
#   /home/claude/deps/sysroot32   headers + import libraries for 32-bit Windows
#   /home/claude/deps/wcc         compiler wrapper (clang --target=i686-w64-windows-gnu ...)
#   /home/claude/deps/rt/*.o      tiny compiler runtime (rt.c) and C++ support (cxx.cpp)
set -e
D=/home/claude/deps; M=$D/mingw-w64; R=$D/sysroot32; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $D
if [ ! -d $M ]; then
  git clone --depth 1 --filter=blob:none --sparse https://github.com/mingw-w64/mingw-w64 $M
  git -C $M sparse-checkout set mingw-w64-headers/include mingw-w64-headers/crt mingw-w64-crt/lib32 mingw-w64-crt/lib-common mingw-w64-crt/def-include
fi
rm -rf $R; mkdir -p $R/include $R/lib
cp -r $M/mingw-w64-headers/include/. $R/include/; cp -r $M/mingw-w64-headers/crt/. $R/include/
for f in $R/include/*.h.in; do sed 's/@[A-Z_]*@//g' "$f" > "${f%.in}"; done
sed -e 's/@MINGW_HAS_SECURE_API@/#define MINGW_HAS_SECURE_API 1/' -e 's/@DEFAULT_WIN32_WINNT@/0x601/' \
    -e 's/@DEFAULT_MSVCRT_VERSION@/0x700/' -e 's/@[A-Z_]*@//g' $M/mingw-w64-headers/crt/_mingw.h.in > $R/include/_mingw.h
cd $R/lib
for n in kernel32 user32 gdi32 d3d11 dxgi d3dcompiler_47 comctl32 shell32 winmm ole32 version dinput8 psapi dbghelp msimg32 advapi32 msvcrt; do
  for src in $M/mingw-w64-crt/lib32/$n.def $M/mingw-w64-crt/lib-common/$n.def $M/mingw-w64-crt/lib32/$n.def.in $M/mingw-w64-crt/lib-common/$n.def.in; do
    [ -f $src ] || continue
    case $src in *.in) clang -E -P -x c -m32 -D__i386__ -DDEF_I386 -Wno-extra-tokens -I$M/mingw-w64-crt/def-include $src -o $n.def; src=$n.def;; esac
    llvm-dlltool -m i386 -k -d $src -l lib$n.a; break
  done
done
cat > $D/wcc <<'W'
#!/bin/sh
R=/home/claude/deps/sysroot32
exec clang --target=i686-w64-windows-gnu -isystem $R/include -D_WIN32_WINNT=0x0601 -DWINVER=0x0601 \
  -msse2 -mfpmath=sse -fno-math-errno -fno-stack-protector -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -fms-extensions -Wno-ignored-attributes -Wno-pragma-pack -nostdlib -nostdinc++ "$@"
W
chmod +x $D/wcc
mkdir -p $D/rt
$D/wcc -O2 -c $HERE/rt.c -o $D/rt/rt.o
$D/wcc -O2 -std=c++17 -c $HERE/cxx.cpp -o $D/rt/cxx.o
echo "toolchain ready"
