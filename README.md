# Compaq iPAQ H3970 tools

Back up stock WinCE over the native USB dock, transfer a Linux Doom kit,
and boot it in RAM on the H3970. The Doom demo starts in portrait mode
with audio muted. Linux keeps independent USB and UART consoles available.

Download the prebuilt Linux and Doom kit from [GitHub Releases](https://github.com/mblsha/ipaq-h3970/releases).
The ZIP includes the Doom 1.9 shareware data. Use the matching EXE and BIN together.

## Browser

Open the [hosted iPAQ tools](https://mblsha.github.io/ipaq-h3970/) in
Chromium on the computer with the iPAQ attached. Close any other
ActiveSync session using the dock before connecting.

To run the page locally, build the static site with Rust's `wasm32-unknown-unknown` target, Python
3.11+, and ARM GNU Embedded GCC and binutils.

```sh
make web
python3 -m http.server --bind 127.0.0.1 --directory .build/web 8000
```

For the local build, open `http://127.0.0.1:8000` in Chromium.
Select **Connect iPAQ** and choose WinCE USB Sync
(`049f:0003`). The page confirms stock WinCE build 11171 before enabling
**Dump ROM** and **Upload and verify**. The 48 MiB ROM backup and the two
Doom kit files stay on your computer and the handheld. The browser and
the iPAQ both show progress. The browser's uploader leaves the kit in
**My Documents** and leaves WinCE running.

## Native tools

Build the CLI helpers with a C compiler, `pkg-config`, libusb, libslirp
4.9 or later, Python 3.11+, and ARM GNU Embedded GCC and binutils. Give
one program exclusive use of the USB dock during a transfer.

```sh
make host
host/cli-activesync info
host/cli-activesync --allow-mutations dump-rom H3970ROM.BIN
host/boot-doom .build/kit
host/h3970-console
```

On Linux pass `--tty /dev/ttyACM0` to the console if that is the iPAQ's
CDC ACM device. The boot command expects the paired files from the cold
kit. It uploads or verifies them, starts the WinCE launcher, and releases
the WinCE USB configuration during the launcher's 12-second delay.
Exit the Linux console with Ctrl-]. The Doom demo keeps playing.

## Build the Doom kit

```sh
make kit IWAD=/path/to/doom1.wad
```

Run that command on Linux with the dependencies in
[the build guide](docs/build.md), or build the kernel in a Linux
container and package it from your host. The output is in `.build/kit/`.
See [source provenance](docs/sources.md) for kernel, Doom, and audio pins.
