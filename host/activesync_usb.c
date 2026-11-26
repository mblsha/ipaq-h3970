/* Bridge Pocket PC 2002 USB ActiveSync bulk pipes to a byte stream. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <libusb.h>

enum { IPAQ_VENDOR = 0x049f, IPAQ_USB_SYNC = 0x0003 };

static int connect_ipaq(libusb_context *context, libusb_device_handle **handle)
{
    libusb_device **devices;
    libusb_device *ipaq = NULL;
    ssize_t count = libusb_get_device_list(context, &devices);
    int status = LIBUSB_ERROR_NO_DEVICE;

    if (count < 0) {
        return (int)count;
    }
    for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor descriptor;

        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0 ||
            descriptor.idVendor != IPAQ_VENDOR ||
            descriptor.idProduct != IPAQ_USB_SYNC) {
            continue;
        }
        if (ipaq) {
            status = LIBUSB_ERROR_BUSY;
            goto done;
        }
        ipaq = devices[i];
    }
    if (ipaq) {
        status = libusb_open(ipaq, handle);
    }
done:
    libusb_free_device_list(devices, 1);
    return status;
}

static int find_interface(const struct libusb_config_descriptor *config,
                          uint8_t *input, uint8_t *output, int *input_size)
{
    for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const struct libusb_interface *candidate = &config->interface[i];
        uint8_t in = 0, out = 0;
        int packet_size = 0;

        if (!candidate->num_altsetting) {
            continue;
        }
        const struct libusb_interface_descriptor *interface =
            &candidate->altsetting[0];
        if (interface->bAlternateSetting != 0 ||
            interface->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) {
            continue;
        }
        for (uint8_t j = 0; j < interface->bNumEndpoints; ++j) {
            const struct libusb_endpoint_descriptor *endpoint =
                &interface->endpoint[j];

            if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                LIBUSB_TRANSFER_TYPE_BULK) {
                continue;
            }
            if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                in = endpoint->bEndpointAddress;
                packet_size = endpoint->wMaxPacketSize;
            } else {
                out = endpoint->bEndpointAddress;
            }
        }
        if (in && out && packet_size > 0 && packet_size <= 64) {
            *input = in;
            *output = out;
            *input_size = packet_size;
            return interface->bInterfaceNumber;
        }
    }
    return LIBUSB_ERROR_NOT_FOUND;
}

struct relay {
    libusb_device_handle *handle;
    uint8_t input;
    int input_size;
    atomic_bool stopped;
    int receive_status;
};

static int wait_for_pipe(struct relay *relay, int fd, short events)
{
    struct pollfd pipe = { .fd = fd, .events = events };

    while (!atomic_load(&relay->stopped)) {
        int ready = poll(&pipe, 1, 100);
        if (ready > 0) {
            return 1;
        }
        if (ready < 0 && errno != EINTR) {
            return LIBUSB_ERROR_IO;
        }
    }
    return 0;
}

static int send_to_usb(struct relay *relay, uint8_t output)
{
    /* Stock WinCE can acknowledge USB writes and still lose PPP data.
     * The H3970 sustained 1 KiB batches with a 500 us gap. */
    unsigned char buffer[1024];
    const struct timespec gap = {.tv_nsec = 500 * 1000};

    for (;;) {
        int ready = wait_for_pipe(relay, STDIN_FILENO, POLLIN);
        if (ready <= 0) {
            return ready;
        }
        ssize_t length = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length <= 0) {
            return length == 0 ? 0 : LIBUSB_ERROR_IO;
        }
        int amount = 0;
        int status = libusb_bulk_transfer(relay->handle, output, buffer,
                                          (int)length, &amount, 5000);
        if (status < 0) {
            return status;
        }
        if (amount != length) {
            return LIBUSB_ERROR_IO;
        }
        struct timespec remaining = gap;
        while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
            /* Preserve the gap if a signal interrupted it. */
        }
    }
}

static int receive_from_usb(struct relay *relay)
{
    unsigned char buffer[64];

    while (!atomic_load(&relay->stopped)) {
        int amount = 0;
        /* One packet lets short replies reach PPP immediately. The timeout
         * bounds shutdown when WinCE has nothing to send. */
        int status = libusb_bulk_transfer(relay->handle, relay->input, buffer,
                                          relay->input_size, &amount, 100);
        if (status < 0 && status != LIBUSB_ERROR_TIMEOUT) {
            return status;
        }
        for (int offset = 0; offset < amount;) {
            ssize_t sent = write(STDOUT_FILENO, buffer + offset,
                                 (size_t)(amount - offset));
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    int ready = wait_for_pipe(relay, STDOUT_FILENO, POLLOUT);
                    if (ready > 0) {
                        continue;
                    }
                    return ready;
                }
            }
            if (sent <= 0) {
                return LIBUSB_ERROR_IO;
            }
            offset += (int)sent;
        }
    }
    return 0;
}

static void *receive_worker(void *opaque)
{
    struct relay *relay = opaque;
    relay->receive_status = receive_from_usb(relay);
    atomic_store(&relay->stopped, true);
    return NULL;
}

static int relay(libusb_device_handle *handle, uint8_t input, uint8_t output,
                 int input_size)
{
    struct relay state = {.handle = handle, .input = input, .input_size = input_size};
    pthread_t worker;
    int stdout_flags = fcntl(STDOUT_FILENO, F_GETFL);
    if (stdout_flags < 0 ||
        fcntl(STDOUT_FILENO, F_SETFL, stdout_flags | O_NONBLOCK) < 0) {
        return LIBUSB_ERROR_IO;
    }

    int status = LIBUSB_ERROR_OTHER;
    /* Keep a receive posted without blocking host writes while USB IN is idle. */
    if (pthread_create(&worker, NULL, receive_worker, &state) == 0) {
        status = send_to_usb(&state, output);
        atomic_store(&state.stopped, true);
        pthread_join(worker, NULL);
        if (status == 0) {
            status = state.receive_status;
        }
    }
    if (fcntl(STDOUT_FILENO, F_SETFL, stdout_flags) < 0 && status == 0) {
        status = LIBUSB_ERROR_IO;
    }
    return status;
}

int main(void)
{
    libusb_context *context = NULL;
    libusb_device_handle *handle = NULL;
    struct libusb_config_descriptor *descriptor = NULL;
    const char *operation = "initialize libusb";
    uint8_t input = 0, output = 0;
    int interface = -1;
    int configuration, input_size;
    int status = libusb_init(&context);

    if (status < 0) {
        goto done;
    }
    operation = "open the iPAQ USB Sync device (049f:0003)";
    status = connect_ipaq(context, &handle);
    if (status < 0) {
        goto done;
    }
    operation = "configure the iPAQ USB Sync device";
    status = libusb_get_configuration(handle, &configuration);
    if (status == 0 && configuration == 0) {
        status = libusb_set_configuration(handle, 1);
    }
    if (status < 0) {
        goto done;
    }
    status = libusb_get_active_config_descriptor(libusb_get_device(handle),
                                                 &descriptor);
    if (status < 0) {
        goto done;
    }
    operation = "find the USB Sync bulk pipes";
    status = find_interface(descriptor, &input, &output, &input_size);
    if (status < 0) {
        goto done;
    }
    operation = "claim the USB Sync interface";
    interface = status;
    status = libusb_claim_interface(handle, interface);
    if (status < 0) {
        interface = -1;
        goto done;
    }
    operation = "begin the USB ActiveSync handshake";
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        /* DTR starts the stock WinCE CLIENT greeting. */
        status = libusb_control_transfer(handle, 0x21, 0x22, 1,
                                          (uint16_t)interface, NULL, 0, 500);
        if (status == 0 || status == LIBUSB_ERROR_NO_DEVICE) {
            break;
        }
        poll(NULL, 0, 150);
    }
    if (status == 0) {
        operation = "relay USB ActiveSync";
        signal(SIGPIPE, SIG_IGN);
        status = relay(handle, input, output, input_size);
    }
done:
    if (status < 0) {
        fprintf(stderr, "activesync-usb: %s: %s\n", operation,
                libusb_error_name(status));
    }
    if (interface >= 0) {
        libusb_release_interface(handle, interface);
    }
    if (descriptor) {
        libusb_free_config_descriptor(descriptor);
    }
    if (handle) {
        libusb_close(handle);
    }
    if (context) {
        libusb_exit(context);
    }
    return status == 0 ? 0 : 1;
}
