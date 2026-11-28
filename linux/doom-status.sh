#!/bin/sh
set -eu

read -r boot </proc/sys/kernel/random/boot_id
read -r kit </doom/kit-id
printf 'boot=%s\nkit=%s\n' "$boot" "$kit"
/doom/doomctl status --machine
/doom/doomctl mix
if /bin/busybox reboot --help >/dev/null 2>&1; then
    echo reset=yes
else
    echo reset=no
fi
