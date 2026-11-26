#!/bin/sh
# Keep the USB console independent of Doom and the UART.
udc=/sys/class/udc/40600000.usb
reconnect() {
    while [ "$(/bin/busybox cat "$udc/state")" != configured ]; do
        /bin/busybox sleep 1
    done
    if printf 'disconnect\n' > "$udc/soft_connect"; then
        /bin/busybox sleep "$1"
        printf 'connect\n' > "$udc/soft_connect"
    fi
}

if [ -w "$udc/soft_connect" ]; then
    # The first enumeration can pass output only. Wait for the host to
    # configure the gadget before reconnecting.
    reconnect 1
    read -r bootargs </proc/cmdline
    case " $bootargs " in
        *" h3970_usb_cold=1 "*)
            # After a WinCE USB reset the host can retain stale CDC endpoint
            # state through the first reconnect. Let it settle and detach again.
            /bin/busybox sleep 2
            reconnect 3
            ;;
    esac
fi

while :; do
    if [ -c /dev/ttyGS0 ]; then
        /bin/busybox setsid /bin/sh -c '
            exec </dev/ttyGS0 >/dev/ttyGS0 2>&1
            /bin/busybox stty sane
            printf "\nH3970 USB: BusyBox console ready\n"
            exec /bin/sh -i
        ' </dev/null
    fi
    /bin/busybox sleep 1
done
