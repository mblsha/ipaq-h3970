# Build the H3970 Linux and Doom kit

## Linux build environment

The kernel build uses GNU Make, Clang/LLVM, the ARM GNU EABI Linux
cross compiler and development libc, Python 3.11+, and standard Linux
kernel build tools. The packager uses ARM GNU Embedded GCC and binutils
and the device-tree tools. The Doom build downloads the pinned Zig
compiler for the build host. The Docker/OCI build environment is in
`build/Containerfile`.

```sh
docker build -f build/Containerfile -t ipaq-h3970-build .
mkdir -p .build
docker run --rm -v "$PWD:/src:ro" -v "$PWD/.build:/out" \
  -v "/absolute/path/to/doom1.wad:/input/doom1.wad:ro" \
  ipaq-h3970-build make kit BUILD_DIR=/out IWAD=/input/doom1.wad
```

The same steps can run on a suitable Linux host without a container.
`make sources` fetches the public kernel commit, SHA-256-checks the
BusyBox, DoomGeneric and Zig archives, and caches them. If you
already have the pinned commit locally, use
`python3 build/fetch.py .build/sources --kernel-checkout /path/to/linux-zaurus`
first. For a split build, run `make kernel` on Linux, copy its
`kernel/` output directory to your host's `.build/`, and run
`make kit-from-kernel IWAD=/path/to/doom1.wad` there. That packaging
step supports macOS and Linux with GNU Embedded GCC/binutils and
device-tree tools.

Use a fresh kit output directory for each candidate. The kit contains
`DOOMUSB.EXE`, `DOOMUSB.BIN`, the packed DTB, `SHA256SUMS`, and a build
manifest with input hashes, patch hashes and resolved configurations.
Keep the EXE and BIN from the same build because the EXE embeds the
payload's size, layout and CRC. Kernel build timestamps and archive
metadata are fixed. Byte-for-byte rebuilds also require the same
toolchain and input IWAD.

The build expects an original Doom-compatible IWAD supplied locally.
The tested WAD is Doom 1.9 shareware, SHA-256
`1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`.
Other WADs must fit the RAM payload's page-list limit and may have
different demos. Keep ROM dumps, IWADs and release binaries outside
the source tree.

## Bench evidence

The cold packer reproduces the hardware-tested launcher and payload
byte-for-byte when given the original tested zImage, DTB and outer
initramfs. The standalone source build regenerates BusyBox and the
archives, so its hashes differ from those historical files. A source
build with Ubuntu Clang 18.1.3 and GNU ARM EABI GCC 13.3.0 passed the
kit checksum check. In H3970 QEMU, that kernel and the kit's initramfs
initialized the portrait framebuffer and answered `/doom/doomctl status`
over the serial console. The earlier cold boot on hardware used the
historical pair.

The browser source was tested on the physical H3970. The 48 MiB
dump matched a preserved ROM byte-for-byte and the Doom kit passed
SHA-256 read-back. Hardware automation used Electron's Chromium
renderer and the standard WebUSB calls. The regular Chrome chooser
was outside that run.
