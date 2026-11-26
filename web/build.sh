#!/bin/sh
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
out=${1:-"$root/.build/h3970-webusb"}
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)

build="$root/.build/h3970-webusb-build"
CARGO_TARGET_DIR="$build" cargo build --locked --release --target wasm32-unknown-unknown \
    --manifest-path "$here/tcp/Cargo.toml"
python3 "$root/wince/build_transfers.py" "$out"
cp "$build/wasm32-unknown-unknown/release/h3970_webusb_tcp.wasm" "$out/tcp.wasm"
cp "$here/index.html" "$here/style.css" "$here/"*.mjs "$out/"
