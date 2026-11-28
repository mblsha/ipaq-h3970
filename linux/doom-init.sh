#!/bin/sh
# Start the demo and independent USB and UART recovery consoles.
export PATH=/doom:/bin:/sbin HOME=/tmp
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t sysfs sysfs /sys
if /bin/busybox mkdir -p /dev/pts && /bin/busybox mount -t devpts devpts /dev/pts; then
    printf '1\n' > /sys/class/graphics/fbcon/rotate_all
    /doom/terminal serve </dev/null >/dev/ttyS0 2>&1 &
fi
if [ -x /doom/autorun ]; then
    /doom/autorun </dev/null &
else
    /doom/doomctl start
fi

/doom/usb-console </dev/null &
echo 'H3970 SERIAL: BusyBox console ready on ttyS0'
while :; do
    /bin/busybox setsid /bin/busybox cttyhack /bin/sh -i </dev/ttyS0 >/dev/ttyS0 2>&1
    /bin/busybox sleep 1
done
