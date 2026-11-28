#!/bin/sh
# Cross-compile the guest Doom backend and package a compiled H3970 kernel.
set -eu
if [ "$#" -ne 4 ]; then
    echo "Usage: $0 SOURCE_CACHE KERNEL_DIR IWAD OUTPUT_DIR" >&2
    exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
cache=$(CDPATH= cd -- "$1" && pwd)
kernel=$(CDPATH= cd -- "$2" && pwd)
iwad=$3
out=$4
test -s "$iwad"
test -s "$kernel/ZIMAGE.BIN"
test -s "$kernel/H3970.DTB"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
case "$(uname -m)" in aarch64|arm64) arch=aarch64 ;; x86_64) arch=x86_64 ;; *) exit 2 ;; esac
case "$(uname -s)" in Darwin) os=macos ;; Linux) os=linux ;; *) exit 2 ;; esac

mkdir "$work/zig" "$work/doomgeneric"
tar -xf "$cache/zig-$arch-$os-0.15.2.tar.xz" -C "$work/zig" --strip-components=1
tar -xf "$cache/doomgeneric.tar.gz" -C "$work/doomgeneric" --strip-components=1
export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1794396363
"$root/doom/build_doom_demo.sh" "$work/zig/zig" "$work/doomgeneric" "$iwad" "$work/doom"
ZIG_LOCAL_CACHE_DIR="$work/doom/zig-cache" ZIG_GLOBAL_CACHE_DIR="$work/doom/zig-global-cache" \
    "$work/zig/zig" cc -target arm-linux-musleabi -mcpu=xscale -marm -static -Wl,--strip-all \
    -Os -Wall -Wextra -o "$work/doom/terminal" "$root/linux/shared-terminal.c"
python3 "$here/initramfs.py" "$work/doom" "$work/doom-outer.cpio" --kit-id "${H3970_KIT_ID:-local}"
python3 "$root/wince/pack_doom.py" "$kernel/ZIMAGE.BIN" "$kernel/H3970.DTB" "$work/doom-outer.cpio" "$out"
python3 "$here/manifest.py" "$kernel" "$iwad" "$out" "$work/doom/doom-fb" "$work/doom-outer.cpio"
