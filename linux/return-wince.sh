#!/bin/sh
# Ask the connected browser to take the calibration backup before rebooting.
set -eu

backup=/tmp/h3970-wince-calibration
requests=/tmp/h3970-wince-returns
read -r uptime _ </proc/uptime
now=${uptime%%.*}

active_request() {
    printf '%s\n' "$1" | /bin/busybox grep -Eq '^[1-9][0-9]{0,9}\.[A-Za-z0-9]{6}$' &&
        [ "${1%%.*}" -gt "$now" ] && [ -f "$requests/$1" ]
}

case "${1:-request}" in
    request)
        if [ ! -s "$backup" ] ||
           ! /bin/busybox head -n 1 "$backup" |
               /bin/busybox grep -Eq '^[[:blank:]]*-?[0-9]+([,[:blank:]]+-?[0-9]+){9}[[:blank:]]*$'; then
            echo "No saved WinCE calibration for this boot. The iPAQ is staying in Linux." >&2
            exit 1
        fi
        umask 077
        /bin/busybox mkdir -p "$requests"
        for file in "$requests"/*; do
            [ -f "$file" ] || continue
            active_request "${file##*/}" || /bin/busybox rm -f "$file"
        done
        /bin/busybox mktemp "$requests/$((now + 60)).XXXXXX" >/dev/null
        echo "WinCE return requested. Keep the WebUSB page connected so it can save calibration before rebooting."
        ;;
    pending)
        for file in "$requests"/*; do
            [ -f "$file" ] || continue
            token=${file##*/}
            if active_request "$token"; then
                printf 'return_request=%s\n' "$token"
                break
            fi
        done
        ;;
    claim)
        token=${2:-}
        active_request "$token" && /bin/busybox rm "$requests/$token"
        ;;
    *)
        echo "Usage: return-wince" >&2
        exit 2
        ;;
esac
