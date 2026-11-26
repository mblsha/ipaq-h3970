#!/bin/sh
# Keep the USB console independent of Doom and the UART.
udc=/sys/class/udc/40600000.usb
if [ -w "$udc/soft_connect" ]; then
    # On the physical warm boot, the first enumeration passed output only.
    # Reconnect once after the host has configured the gadget.
    while [ "$(/bin/busybox cat "$udc/state")" != configured ]; do
        /bin/busybox sleep 1
    done
    if printf 'disconnect\n' > "$udc/soft_connect"; then
        # Give the host time to detach its CDC driver and discard the old endpoint state.
        /bin/busybox sleep 3
        printf 'connect\n' > "$udc/soft_connect"
    fi
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
