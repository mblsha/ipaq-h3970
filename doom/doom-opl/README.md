# Doom music sources

The OPL2 sequencer, MIDI reader and callback queue come from [Chocolate Doom
3.0.1](https://github.com/chocolate-doom/chocolate-doom/tree/chocolate-doom-3.0.1).
The imported files retain their copyright and license notices. The archive
SHA-256 is `a54383beef6a52babc5b00d58fcf53a454f012ced7b1936ba359b13f1f10ac66`.

DoomGeneric supplies its own MUS-to-MIDI converter. The compatibility header
supplies the packed types and endian conversions used by the imported files.
The sequencer starts the PCM worker in music-only mode and uses the mixer's
lock around shared playback state. The MIDI reader uses DoomGeneric's fatal
error handler if it cannot allocate an event.
The build reuses QEMU's LGPL YM3812 core from `hw/audio/fmopl.c`.
