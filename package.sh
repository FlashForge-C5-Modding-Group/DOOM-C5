#!/bin/bash
# Builds for the printer and assembles dist/c5doom/, ready to copy to /usr/data/doom/
set -e
cd "$(dirname "$0")"
FREEDOOM_VERSION=0.13.0
FREEDOOM_SHA256=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
D=dist/c5doom

bash build.sh
export PATH="$HOME/toolchains/mips32el--musl--stable-2024.02-1/bin:/usr/bin:/bin"
mkdir -p "$D"
mipsel-buildroot-linux-musl-strip -o "$D/c5doom" build/mips/c5doom
cp doom.sh README.md "$D/"
cp third_party/doomgeneric/LICENSE "$D/DOOMGENERIC-LICENSE"
chmod +x "$D/c5doom" "$D/doom.sh"

if [ ! -f "$D/freedoom1.wad" ]; then
    T=$(mktemp -d)
    curl -fsSL -o "$T/freedoom.zip" "https://github.com/freedoom/freedoom/releases/download/v$FREEDOOM_VERSION/freedoom-$FREEDOOM_VERSION.zip"
    echo "$FREEDOOM_SHA256  $T/freedoom.zip" | sha256sum -c -
    python3 -m zipfile -e "$T/freedoom.zip" "$T"
    cp "$T/freedoom-$FREEDOOM_VERSION/freedoom1.wad" "$D/"
    cp "$T/freedoom-$FREEDOOM_VERSION/COPYING.txt" "$D/FREEDOOM-COPYING.txt"
    rm -rf "$T"
fi
ls -l "$D"
