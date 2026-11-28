# Compaq iPAQ H3970 tools

Back up stock WinCE over the native USB dock, transfer a Linux Doom kit,
and boot it in RAM on the H3970. The Doom demo starts in portrait mode
with audio muted. Linux keeps independent USB and UART consoles available.

Download the prebuilt Linux and Doom kit from [GitHub Releases](https://github.com/mblsha/ipaq-h3970/releases).
The ZIP includes the Doom 1.9 shareware data. Use the matching EXE and BIN together.

## Browser

Open the [hosted iPAQ tools](https://mblsha.github.io/ipaq-h3970/) in
Chromium with the native dock attached. Close any other ActiveSync or
serial session using the dock.

- Choose **Connect iPAQ** and select the handheld in Chromium's device
  picker. The page detects WinCE or Linux. If Chromium asks for serial
  permission on Linux, choose **Allow Linux serial access**.
- From WinCE, choose **Install and start Doom**. The page verifies the
  published kit, starts the RAM launcher, and follows the switch to Linux.
  It also reconnects on a browser reload or USB replug. If Chromium needs
  permission during a switch, the connection button offers the matching
  USB or serial permission and the page resumes from the device's reply.
- While Doom is running, **Audio** toggles sound and restarts the demo.
  **Restart demo** keeps the audio mode. **Music** and **Sound effects**
  adjust the live mix.
- Hold **Record** for about a second or choose **Exit Doom** to open the
  portrait BusyBox terminal. The browser sends keyboard input over the
  same USB cable and displays the shell output alongside the iPAQ LCD.
  Choose **Start demo** to return to Doom in the same Linux boot.
- Boot the kit from calibrated WinCE to save the handheld's touch settings
  in Linux RAM. A new tab or browser can read the backup while Linux is
  running. Choose **Reset to WinCE** or run `return-wince` at the BusyBox
  prompt. Leave the page connected over USB so it can take the backup before
  rebooting, restore it in WinCE, and open Today. The shell request expires
  after a minute if a connected page has not picked it up.
- Open **WinCE tools** to save the ROM to your computer or stage a
  custom kit.

To run the page locally, build the static site with Node.js and npm, Rust's
`wasm32-unknown-unknown` target, Python 3.11+, and ARM GNU Embedded GCC and
binutils.

```sh
make web
python3 -m http.server --bind 127.0.0.1 --directory .build/web 8000
```

For the local build, open `http://127.0.0.1:8000` in Chromium.
Use **Use your own kit** to transfer local files. Run
`GH_REPO=mblsha/ipaq-h3970 python3 web/stage-kit.py .build/web`
to stage the published kit locally using an authenticated `gh` CLI.
WinCE operations require stock build 11171. Both the browser and the
iPAQ show transfer progress. Manual upload stages files in
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
