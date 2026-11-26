#!/bin/sh
# Build the static ARMv5 backend and the guest Doom files for the H3970.
set -eu

if [ "$#" -ne 4 ]; then
    echo 'usage: build_doom_demo.sh ZIG DOOMGENERIC_CHECKOUT IWAD OUTPUT_DIR' >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
zig=$1
source_engine=$2/doomgeneric
iwad=$3
out=$4
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
export ZIG_LOCAL_CACHE_DIR="$out/zig-cache"
export ZIG_GLOBAL_CACHE_DIR="$out/zig-global-cache"

engine=$out/engine-portrait
opl=$script_dir/doom-opl
fmopl=$script_dir/fmopl
mkdir "$engine"
cp -R "$source_engine/." "$engine/"
patch --quiet -p1 -d "$engine" < "$script_dir/doom_portrait.diff"
patch --quiet -p1 -d "$engine" < "$script_dir/doom_audio.diff"
patch --quiet -p1 -d "$engine" < "$script_dir/doom_indexed.diff"
patch --quiet -p1 -d "$engine" < "$script_dir/doom_timing.diff"

# Use the pinned engine's own core object list and replace its X11 backend.
set --
for obj in $(sed -n 's/^SRC_DOOM = //p' "$engine/Makefile"); do
    [ "$obj" = doomgeneric_xlib.o ] || set -- "$@" "$engine/${obj%.o}.c"
done
[ "$#" -gt 50 ]
"$zig" cc -target arm-linux-musleabi -mcpu=xscale -marm -static -Wl,--strip-all -O2 -std=gnu11 -pthread \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DFEATURE_SOUND -DH3970_PCM -DCMAP256 \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=356 \
    -Wno-deprecated-non-prototype -Wno-pointer-sign \
    -I "$engine" -I "$script_dir" -I "$opl" -I "$fmopl" -o "$out/doom-fb" \
    "$@" "$script_dir/doom_fb.c" "$script_dir/doom_audio.c" "$script_dir/doom_opl.c" \
    "$engine/mus2mid.c" "$opl/i_oplmusic.c" "$opl/midifile.c" "$opl/opl_queue.c" \
    "$fmopl/fmopl.c" -lm

install -m 0755 "$script_dir/doomctl" "$out/doomctl"
install -m 0644 "$iwad" "$out/doom1.wad"
