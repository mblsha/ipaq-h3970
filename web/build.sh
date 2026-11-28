#!/bin/sh
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
out=${1:-"$root/.build/h3970-webusb"}
mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)

build="$root/.build/h3970-webusb-build"
mkdir -p "$build/terminal"
cp "$here/package.json" "$here/package-lock.json" "$build/terminal/"
npm ci --prefix "$build/terminal" --ignore-scripts --no-audit --no-fund
xterm="$build/terminal/node_modules/@xterm/xterm"
cp "$xterm/lib/xterm.mjs" "$out/xterm.mjs"
cp "$xterm/css/xterm.css" "$out/xterm.css"
cp "$xterm/LICENSE" "$out/xterm.LICENSE"
CARGO_TARGET_DIR="$build" cargo build --locked --release --target wasm32-unknown-unknown \
    --manifest-path "$here/tcp/Cargo.toml"
python3 "$root/wince/build_transfers.py" "$out"
python3 "$root/wince/build_today.py" "$out/today"
cp "$build/wasm32-unknown-unknown/release/h3970_webusb_tcp.wasm" "$out/tcp.wasm"
cp "$here/index.html" "$here/style.css" "$here/"*.mjs "$out/"
