#!/bin/sh
# Build the Linux zImage and native USB DTB from the public base and local patches.
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 SOURCE_CACHE OUTPUT_DIR" >&2
    exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cache=$(CDPATH= cd -- "$1" && pwd)
mkdir -p "$2"
out=$(CDPATH= cd -- "$2" && pwd)
work=$(mktemp -d /tmp/ipaq-linux.XXXXXX)
trap 'rm -rf -- "$work"' EXIT
export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1794396363
export KBUILD_BUILD_TIMESTAMP="$(date -u -d "@$SOURCE_DATE_EPOCH" '+%a %b %d %T UTC %Y')"
export KBUILD_BUILD_USER=ipaq KBUILD_BUILD_HOST=build KBUILD_BUILD_VERSION=1
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN)}
cross=${CROSS_COMPILE:-arm-linux-gnueabi-}
{ "${cross}gcc" --version; printf "LLVM=%s\n" "${LLVM:-1}"; clang --version; } > "$out/toolchain.txt"

mkdir "$work/linux" "$work/busybox" "$work/kbuild"
tar -xf "$cache/linux-base.tar.gz" -C "$work/linux"
for patch in h3970-tested-base asic3-write usb-cdc; do
    patch --fuzz=0 --batch -p1 -d "$work/linux" < "$here/patches/$patch.patch"
done
tar -xf "$cache/busybox-1.37.0.tar.bz2" -C "$work/busybox" --strip-components=1

bbmake() {
    make -C "$work/busybox" CROSS_COMPILE="$cross" CFLAGS="-march=armv5te -marm" "$@"
}
bbmake allnoconfig
python3 - "$work/busybox/.config" "$here/busybox.config" <<'PY'
from pathlib import Path
from sys import argv
import re

path, overrides = map(Path, argv[1:])
settings = overrides.read_text().splitlines()
key = re.compile(r"(?:# )?(CONFIG_[A-Z0-9_]+)(?:=| is not set)")
replace = {key.match(line)[1] for line in settings if key.match(line)}
remaining = (line for line in path.read_text().splitlines() if not key.match(line) or key.match(line)[1] not in replace)
path.write_text("\n".join([*remaining, *settings]) + "\n")
PY
yes "" | bbmake oldconfig > "$out/busybox-config.log"
for setting in STATIC ASH ASH_TEST SH_IS_ASH CPIO GUNZIP CTTYHACK STTY SETSID REBOOT SYNC; do
    grep -qx "CONFIG_$setting=y" "$work/busybox/.config"
done
bbmake -j"$jobs"
cp "$work/busybox/.config" "$out/busybox.config"
cp "$work/busybox/busybox" "$out/busybox"
"${cross}strip" "$out/busybox"
if "${cross}readelf" -l "$out/busybox" | grep -q INTERP; then
    echo "Expected statically linked BusyBox" >&2
    exit 1
fi
cp "$out/busybox" "$work/busybox-static"
cp "$here/rescue-init" "$work/rescue-init"
cat > "$work/init.list" <<INIT
dir /bin 0755 0 0
file /bin/busybox $work/busybox-static 0755 0 0
slink /bin/sh busybox 0777 0 0
file /init $work/rescue-init 0755 0 0
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
dir /proc 0555 0 0
dir /sys 0555 0 0
dir /tmp 1777 0 0
INIT
for applet in awk base64 cat chmod cp cpio dd dmesg grep head hexdump ls mkdir mktemp mount mv printf ps rm sed sha256sum sleep stat stty tail tee umount uname wc; do
    printf 'slink /bin/%s busybox 0777 0 0\n' "$applet" >> "$work/init.list"
done
{
    cat "$here/kernel.config"
    printf 'CONFIG_INITRAMFS_SOURCE="%s"\n' "$work/init.list"
} > "$work/kernel.config"
kmake() {
    make -C "$work/linux" O="$work/kbuild" ARCH=arm LLVM=${LLVM:-1} "$@"
}
kmake KCONFIG_ALLCONFIG="$work/kernel.config" allnoconfig
for setting in FB_PXA USB_PXA25X USB_G_SERIAL SND_SOC_UDA1380 SND_SIMPLE_CARD KEYBOARD_ASIC2 KEXEC BINFMT_SCRIPT PROC_SYSCTL; do
    grep -qx "CONFIG_$setting=y" "$work/kbuild/.config"
done
kmake -j"$jobs" zImage intel/pxa/hp-ipaq-h3970.dtb
cp "$work/kbuild/arch/arm/boot/zImage" "$out/ZIMAGE.BIN"
sed 's/^CONFIG_INITRAMFS_SOURCE=.*/CONFIG_INITRAMFS_SOURCE="init.list"/' "$work/kbuild/.config" > "$out/kernel.config"
tools="$work/kbuild/scripts/dtc"
"$tools/dtc" -I dts -O dtb -o "$work/usb.dtbo" "$here/enable-udc.dtso"
"$tools/fdtoverlay" -i "$work/kbuild/arch/arm/boot/dts/intel/pxa/hp-ipaq-h3970.dtb" \
    -o "$out/H3970.DTB" "$work/usb.dtbo"
