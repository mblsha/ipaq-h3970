#!/bin/sh
# Build a self-contained WinCE RAM-boot kit with the pinned shareware IWAD.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$1"
out=$(CDPATH= cd -- "$1" && pwd)
if [ -e "$out/release" ]; then
    echo 'Choose a BUILD_DIR without existing release assets.' >&2
    exit 2
fi
work=$(mktemp -d "$out/.release.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
cd "$work"

package=doom-wad-shareware_1.9.fixed-4_all.deb
curl --fail --silent --show-error --location --retry 2 --connect-timeout 15 --max-time 120 \
    "https://deb.debian.org/debian/pool/non-free/d/doom-wad-shareware/$package" --output "$package"
printf '%s  %s\n' 29c673d3ed41fb9ab64b3f3907ab9d17e2ca861b1cd6f4aca53aced79d257d0e "$package" | sha256sum --check
dpkg-deb --extract "$package" shareware
iwad="$work/shareware/usr/share/games/doom/doom1.wad"
printf '%s  %s\n' 1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771 "$iwad" | sha256sum --check

make -C "$root" kit BUILD_DIR="$work/build" IWAD="$iwad"
mkdir assets
tar -czf assets/ipaq-h3970-source-inputs.tar.gz -C build/sources \
    linux-base.tar.gz busybox-1.37.0.tar.bz2 doomgeneric.tar.gz

cd build/kit
sha256sum --check SHA256SUMS
cp -R "$root/LICENSES" LICENSES
cp "$work/shareware/usr/share/doc/doom-wad-shareware/copyright" LICENSES/doom-shareware.txt
cp "$root/docs/release.txt" README.txt
python3 -m zipfile -c "$work/assets/ipaq-h3970-doom.zip" \
    DOOMUSB.EXE DOOMUSB.BIN H3970.DTB build-manifest.json SHA256SUMS LICENSES README.txt

cd "$work/assets"
sha256sum ipaq-h3970-doom.zip ipaq-h3970-source-inputs.tar.gz > SHA256SUMS
mv "$work/assets" "$out/release"
