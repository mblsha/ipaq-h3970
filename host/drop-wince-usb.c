#include <libusb.h>
#include <stdio.h>
#include <string.h>

static int open_dock(libusb_context *context, libusb_device_handle **handle)
{
    libusb_device **devices;
    ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0)
        return (int)count;

    libusb_device *dock = NULL;
    int result = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
            descriptor.idVendor != 0x049f || descriptor.idProduct != 0x0003)
            continue;
        if (dock) {
            result = LIBUSB_ERROR_BUSY;
            goto done;
        }
        dock = devices[i];
    }
    if (dock)
        result = libusb_open(dock, handle);
done:
    libusb_free_device_list(devices, 1);
    return result;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "unconfigure"))) {
        fputs("usage: drop-wince-usb [unconfigure]\n", stderr);
        return 2;
    }
    libusb_context *context = NULL;
    int result = libusb_init(&context);
    if (result < 0)
        return 1;
    libusb_device_handle *device = NULL;
    result = open_dock(context, &device);
    if (result < 0) {
        fprintf(stderr, "Open one WinCE USB Sync dock: %s\n", libusb_error_name(result));
        libusb_exit(context);
        return 1;
    }
    result = libusb_claim_interface(device, 0);
    if (result == 0) {
        int clear = libusb_control_transfer(device, 0x21, 0x22, 0, 0, NULL, 0, 1500);
        printf("WinCE USB DTR release: %s\n", libusb_error_name(clear));
        libusb_release_interface(device, 0);
        result = clear;
        if (argc == 2 && result == 0) {
            result = libusb_set_configuration(device, 0);
            printf("WinCE USB unconfigure: %s\n", libusb_error_name(result));
        }
    } else {
        fprintf(stderr, "WinCE USB interface claim: %s\n", libusb_error_name(result));
    }
    libusb_close(device);
    libusb_exit(context);
    return result < 0;
}
