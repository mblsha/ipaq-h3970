#!/bin/sh
set -eu

read -r boot </proc/sys/kernel/random/boot_id
read -r kit </doom/kit-id
printf 'boot=%s\nkit=%s\n' "$boot" "$kit"
/doom/doomctl status --machine
/doom/doomctl mix
if [ -p /tmp/h3970-terminal/input ] && [ -s /tmp/h3970-terminal/output ]; then
    echo terminal=yes
fi
if /bin/busybox reboot --help >/dev/null 2>&1; then
    echo reset=yes
else
    echo reset=no
fi
/doom/return-wince pending
