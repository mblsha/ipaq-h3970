# Source provenance

- Host tools, WinCE progress, ROM dump and Today helpers were extracted
  from the Zaurus project at `98e5aab86`. The browser sources are the
  H3970 WebUSB port with a locked smoltcp dependency. The WinCE cold
  launcher and its trampoline were extracted from the working
  delayed launcher used for the native USB cold-boot test.
- [Linux](https://github.com/mblsha/linux-zaurus/tree/7ab8afd5256bb42d92cfa74abd09f580f2b737bd)
  starts at public commit `7ab8afd5256bb42d92cfa74abd09f580f2b737bd`.
  The first patch reconstructs the tree of tested kernel base
  `7734a917581009d3b496e9677f9e56107c99ff50`. The following patches
  preserve the tested ASIC3 and PXA USB driver changes. The local DT
  overlay enables the USB gadget for the RAM boot. Kernel sources and
  these changes retain the upstream Linux license and file notices.
- [DoomGeneric](https://github.com/ozkl/doomgeneric/tree/e257a09527062bfb986b6e5e0780839b6cbd5b71)
  is pinned at `e257a09527062bfb986b6e5e0780839b6cbd5b71`. The portrait,
  indexed renderer, audio and timing changes plus H3970 runtime
  files were extracted from `slop-qemu` at
  `4c7d1a342c6787aa927ba9e3bdaade295e14faa2`.
  The OPL sequencer and MIDI files come from Chocolate Doom 3.0.1.
  The YM3812 implementation is the LGPL QEMU `fmopl` core.
  Copyright notices remain in the imported files, and their license
  texts are under `LICENSES/`.
- BusyBox 1.37.0 and Zig 0.15.2 are fetched using the checksums in
  `build/sources.json`. The Linux build enables the BusyBox applets
  listed in `linux/busybox.config`.
