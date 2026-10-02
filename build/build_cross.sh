#!/bin/sh
# FATNT cross build: produce a native NT 3.1 fatnt.sys with GCC/clang + the
# mingw-w64 DDK headers and import libs, without Visual C++. Also builds the
# tools. The real, shipping build uses Visual C++ (see src\NT\build.bat); this
# is for producing and boot-testing an NT 3.1 binary on a Linux host.
#
# Needs: gcc-mingw-w64-i686 (ships ddk headers + libntoskrnl.a), binutils.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SRC="$ROOT/src"
OUT="${1:-/tmp/fatnt-cross}"
mkdir -p "$OUT"

GCC=i686-w64-mingw32-gcc
DLLTOOL=i686-w64-mingw32-dlltool
NM=i686-w64-mingw32-nm
INC="-I$SRC -I/usr/i686-w64-mingw32/include/ddk -I/usr/i686-w64-mingw32/include"
# -march=i486: NT 3.1 runs on 80486 (and 386) CPUs, so the code generator must
# not emit P6+ instructions (CMOV and friends) - those fault with an illegal
# instruction on a 486. The modern default (i686) would emit them.
FLAGS="-c -ffreestanding -fno-stack-protector -fno-builtin -march=i486 -mtune=i486 \
 -D_X86_ -DWIN32 -D_WIN32_WINNT=0x0600 -DNTDDI_VERSION=0x06000000 -DFAT_CROSS -w $EXTRACFLAGS"

echo "Compiling driver sources..."
rm -f "$OUT"/*.o
for f in "$SRC"/fat*.c; do
    $GCC $FLAGS $INC "$f" -o "$OUT/$(basename "${f%.c}").o"
done
# mem* the code generator emits (NT 3.1 ntoskrnl does not export them)
$GCC $FLAGS $INC "$HERE/fatcrt.c" -o "$OUT/fatcrt.o"

echo "Building the NT 3.1 ntoskrnl import library from the symbols used..."
$NM "$OUT"/*.o 2>/dev/null | awk '/ U /{print $2}' | grep '^__imp_' \
    | sed 's/^__imp_//; s/^_//' | sort -u > "$OUT/need.txt"
{ echo "LIBRARY ntoskrnl.exe"; echo "EXPORTS"; cat "$OUT/need.txt"; } > "$OUT/ntoskrnl.def"
$DLLTOOL --kill-at -d "$OUT/ntoskrnl.def" -l "$OUT/libntoskrnl_nt31.a"

echo "Linking fatnt.sys (native, subsystem 3.10, base 0x10000)..."
$GCC -shared -nostdlib -nostartfiles \
    -Wl,--subsystem,native -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
    -Wl,--major-os-version,1 -Wl,--minor-os-version,0 \
    -Wl,--major-image-version,0 -Wl,--minor-image-version,0 \
    -Wl,--image-base,0x10000 -Wl,--entry,_DriverEntry@8 -Wl,--file-alignment,0x200 \
    -o "$OUT/fatnt.sys" "$OUT"/*.o "$OUT/libntoskrnl_nt31.a" -lgcc

# Strip the COFF symbol table (ld leaves ~30 KB of it appended past the last
# section). NT 3.1's loader tolerated the slack, but the NT 3.51+/2000 loader
# wants the file to match the PE image, so strip, then write a valid checksum.
i686-w64-mingw32-strip --strip-all "$OUT/fatnt.sys"
python3 "$HERE/pechecksum.py" "$OUT/fatnt.sys"

echo "Built: $OUT/fatnt.sys"

# ----------------------------------------------------------------------
# The user-mode tools: fatinst, fatfmt, fatchk. Freestanding console
# programs (subsystem 3.10) so they run on NT 3.1 with no C-runtime DLL;
# build/nocrt.c and build/toolcrt.c provide the tiny built-in runtime.
# ----------------------------------------------------------------------
EXEBASE="-O2 -w -ffreestanding -nostdlib -nostartfiles -march=i486 -mtune=i486 -e _mainCRTStartup \
 -Wl,--subsystem,console -Wl,--major-subsystem-version,3 -Wl,--minor-subsystem-version,10 \
 -Wl,--major-os-version,1 -Wl,--minor-os-version,0 -D_WIN32 -D_CONSOLE"

echo "Linking fatinst.exe..."
$GCC $EXEBASE -D_STDIO_H_=1 -D_STRING_H_=1 -D_INC_STDIO -D_INC_STRING \
    -Dmain=appmain -Dprintf=fat_printf -Dsprintf=fat_sprintf -D_stricmp=fat_stricmp -Dstrlen=fat_strlen \
    -o "$OUT/fatinst.exe" "$ROOT/inst/fatinst.c" "$HERE/nocrt.c" -ladvapi32 -lkernel32

TOOLCRT="-include $HERE/toolcrt.h \
 -D_INC_STDIO -D_INC_STDLIB -D_INC_STRING -D_INC_CTYPE -D_INC_TIME \
 -D_STDIO_H_=1 -D_STDLIB_H=1 -D_STRING_H_=1 -D_CTYPE_H_=1 -D_TIME_H_=1 \
 -Dmain=appmain -Dprintf=fat_printf -Dfprintf=fat_fprintf -Dsprintf=fat_sprintf -Dsnprintf=fat_snprintf \
 -Dmalloc=fat_malloc -Dfree=fat_free -Dcalloc=fat_calloc -Drealloc=fat_realloc \
 -Dstrtoul=fat_strtoul -Dtime=fat_time -Dlocaltime=fat_localtime -Dtoupper=fat_toupper -D_stricmp=fat_stricmp"

echo "Linking fatfmt.exe..."
$GCC $EXEBASE $TOOLCRT -I"$ROOT" -o "$OUT/fatfmt.exe" "$ROOT/fmt/fatfmt.c" "$HERE/toolcrt.c" -lkernel32 -lgcc

echo "Linking fatchk.exe..."
$GCC $EXEBASE $TOOLCRT -DFAT_USERMODE -I"$SRC" -o "$OUT/fatchk.exe" \
    "$ROOT/chk/fatchk.c" "$SRC/fatsup.c" "$HERE/toolcrt.c" -lkernel32 -lgcc

echo "Built: $OUT/fatinst.exe $OUT/fatfmt.exe $OUT/fatchk.exe"
echo "Done."
