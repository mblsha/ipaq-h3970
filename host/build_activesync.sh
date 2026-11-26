#!/bin/sh
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
out=${1:-"$root/.build/h3970-activesync"}
mkdir -p "$out"

pkg-config --atleast-version=4.9 slirp
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
    $(pkg-config --cflags slirp) "$here/activesync_slirp.c" \
    $(pkg-config --libs slirp) -o "$out/activesync-slirp"

if pkg-config --exists libusb-1.0; then
    ${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -pthread \
        $(pkg-config --cflags libusb-1.0) "$here/activesync_usb.c" \
        $(pkg-config --libs libusb-1.0) -o "$out/activesync-usb"
    ${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror \
        $(pkg-config --cflags libusb-1.0) "$here/drop-wince-usb.c" \
        $(pkg-config --libs libusb-1.0) -o "$out/drop-wince-usb"
else
    printf '%s\n' 'Install libusb development files to build the default USB helper.' >&2
fi
