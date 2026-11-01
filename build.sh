#!/bin/bash
# Builds c5doom for the printer, or for this machine with: build.sh host
set -e
cd "$(dirname "$0")"
DG=third_party/doomgeneric/doomgeneric
SRC=$(sed -n 's/^SRC_DOOM *= *//p' $DG/Makefile | tr ' ' '\n' | grep '\.o$' | grep -v -E '^doomgeneric_|^i_sdl|^i_allegro' | sed 's/\.o$/.c/')
if [ "$1" = host ]; then
    CC=gcc; OUT=build/host; LINK=""
else
    export PATH="$HOME/toolchains/mips32el--musl--stable-2024.02-1/bin:/usr/bin:/bin"
    CC=mipsel-buildroot-linux-musl-gcc; OUT=build/mips; LINK=-static
fi
mkdir -p "$OUT"
FLAGS="-O2 -w -std=gnu99 -D_DEFAULT_SOURCE -DNORMALUNIX -DLINUX -DFEATURE_SOUND -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 -I$DG -Isrc -ffunction-sections -fdata-sections"
pids=(); objs=()
for f in $SRC; do
    o="$OUT/${f%.c}.o"; objs+=("$o")
    if [ "$DG/$f" -nt "$o" ]; then
        $CC $FLAGS -c "$DG/$f" -o "$o" & pids+=($!)
        if [ ${#pids[@]} -ge 8 ]; then for p in "${pids[@]}"; do wait $p; done; pids=(); fi
    fi
done
for p in "${pids[@]}"; do wait $p; done
for f in doomgeneric_c5 i_c5sound c5_keyboard; do
    $CC $FLAGS -c src/$f.c -o "$OUT/$f.o"; objs+=("$OUT/$f.o")
done
$CC $LINK -Wl,--gc-sections "${objs[@]}" -o "$OUT/c5doom" -lm -lpthread
ls -l "$OUT/c5doom"
