/* Route one PPP guest through libslirp, with only the two ActiveSync ports. */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libslirp.h>

static const uint8_t guest_mac[] = {0x02, 0, 0, 0, 0, 1};
static const uint8_t host_mac[] = {0x52, 0x55, 192, 168, 131, 102};
static const uint8_t guest_ip[] = {192, 168, 131, 201};
static const uint8_t host_ip[] = {192, 168, 131, 102};

enum { ETH_HEADER = 14, MAX_IP = 1500, DCCM_PORT = 5679, RAPI_PORT = 990 };

struct bridge {
    int link;
    bool answer_arp;
    struct pollfd pollfds[64];
    unsigned used;
};

static int64_t clock_ns(void *opaque)
{
    (void)opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static void slirp_notify(void *opaque)
{
    (void)opaque;
}

static void register_socket(slirp_os_socket socket, void *opaque)
{
    (void)socket;
    (void)opaque;
}

static void guest_error(const char *message, void *opaque)
{
    (void)opaque;
    fprintf(stderr, "ActiveSync network: %s\n", message);
}

static unsigned be16(const uint8_t *value)
{
    return (value[0] << 8) | value[1];
}

static bool allows_packet(const uint8_t *ip, size_t length)
{
    if (length < 40 || length > MAX_IP || ip[0] >> 4 != 4 ||
        ip[9] != 6 || be16(ip + 2) != length || (be16(ip + 6) & 0x3fff) ||
        memcmp(ip + 12, guest_ip, sizeof(guest_ip)) ||
        memcmp(ip + 16, host_ip, sizeof(host_ip))) {
        return false;
    }
    unsigned header = (ip[0] & 0xf) * 4;
    if (header < 20 || header + 20 > length) {
        return false;
    }
    const uint8_t *tcp = ip + header;
    bool opens_connection = (tcp[13] & 0x12) == 0x02; /* SYN without ACK */
    return be16(tcp + 2) == DCCM_PORT ||
           (!opens_connection && be16(tcp) == RAPI_PORT);
}

static slirp_ssize_t from_slirp(const void *bytes, size_t length, void *opaque)
{
    struct bridge *bridge = opaque;
    const uint8_t *frame = bytes;
    if (length >= ETH_HEADER + 20 && be16(frame + 12) == 0x0800 &&
        !memcmp(frame, guest_mac, sizeof(guest_mac))) {
        const uint8_t *ip = frame + ETH_HEADER;
        unsigned size = be16(ip + 2);
        unsigned header = (ip[0] & 15) * 4;

        /* libslirp supplies Ethernet frames padded to the minimum wire size.
         * PPP carries the IPv4 datagram alone, without Ethernet padding.
         */
        if (ip[0] >> 4 == 4 && header >= 20 && size >= header &&
            size <= MAX_IP && size <= length - ETH_HEADER) {
            ssize_t sent = send(bridge->link, ip, size, MSG_DONTWAIT);
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
        }
    } else if (length >= 42 && be16(frame + 12) == 0x0806 &&
               be16(frame + 20) == 1 && !memcmp(frame + 38, guest_ip, sizeof(guest_ip))) {
        bridge->answer_arp = true;
    }
    return length;
}

static void send_arp(Slirp *slirp, bool reply)
{
    uint8_t packet[42] = {
        [12] = 0x08, [13] = 0x06,       /* EtherType */
        [15] = 0x01, [16] = 0x08,       /* Ethernet, IPv4 */
        [18] = 6, [19] = 4, [21] = reply ? 2 : 1,
    };
    if (reply) {
        memcpy(packet, host_mac, sizeof(host_mac));
        memcpy(packet + 32, host_mac, sizeof(host_mac));
    } else {
        memset(packet, 0xff, sizeof(host_mac));
    }
    memcpy(packet + 6, guest_mac, sizeof(guest_mac));
    memcpy(packet + 22, guest_mac, sizeof(guest_mac));
    memcpy(packet + 28, guest_ip, sizeof(guest_ip));
    memcpy(packet + 38, host_ip, sizeof(host_ip));
    slirp_input(slirp, packet, sizeof(packet));
}

static int add_poll(slirp_os_socket fd, int events, void *opaque)
{
    struct bridge *bridge = opaque;
    if (bridge->used == sizeof(bridge->pollfds) / sizeof(bridge->pollfds[0])) {
        return -1;
    }
    struct pollfd *entry = &bridge->pollfds[bridge->used];
    *entry = (struct pollfd){.fd = fd};
    if (events & SLIRP_POLL_IN) entry->events |= POLLIN;
    if (events & SLIRP_POLL_OUT) entry->events |= POLLOUT;
    if (events & SLIRP_POLL_PRI) entry->events |= POLLPRI;
    return bridge->used++;
}

static int read_poll(int index, void *opaque)
{
    struct bridge *bridge = opaque;
    if (index < 0 || (unsigned)index >= bridge->used) {
        return 0;
    }
    unsigned events = bridge->pollfds[index].revents;
    int ready = 0;
    if (events & POLLIN) ready |= SLIRP_POLL_IN;
    if (events & POLLOUT) ready |= SLIRP_POLL_OUT;
    if (events & POLLPRI) ready |= SLIRP_POLL_PRI;
    if (events & (POLLERR | POLLNVAL)) ready |= SLIRP_POLL_ERR;
    if (events & POLLHUP) ready |= SLIRP_POLL_HUP;
    return ready;
}

static int proxy(Slirp *slirp, struct bridge *bridge)
{
    uint8_t frame[ETH_HEADER + MAX_IP + 1] = {[12] = 0x08};
    memcpy(frame, host_mac, sizeof(host_mac));
    memcpy(frame + 6, guest_mac, sizeof(guest_mac));
    send_arp(slirp, false);
    for (;;) {
        uint32_t timeout = 100;
        bridge->used = 1;
        bridge->pollfds[0] = (struct pollfd){.fd = bridge->link, .events = POLLIN};
        slirp_pollfds_fill_socket(slirp, &timeout, add_poll, bridge);
        int ready = poll(bridge->pollfds, bridge->used, timeout);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            perror("ActiveSync network poll");
            return 1;
        }
        slirp_pollfds_poll(slirp, 0, read_poll, bridge);
        if (bridge->pollfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            return 0;
        }
        if (bridge->pollfds[0].revents & POLLIN) {
            ssize_t length = recv(bridge->link, frame + ETH_HEADER, sizeof(frame) - ETH_HEADER, 0);
            if (length <= 0) {
                return 0;
            }
            if (allows_packet(frame + ETH_HEADER, length)) {
                slirp_input(slirp, frame, ETH_HEADER + length);
            }
        }
        if (bridge->answer_arp) {
            bridge->answer_arp = false;
            send_arp(slirp, true);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        return 2;
    }
    struct bridge bridge = {.link = atoi(argv[1])};
    int port = atoi(argv[2]);
    if (bridge.link < 3 || port < 1024 || port > 65535) {
        return 2;
    }
    SlirpConfig config = {
        .version = 6,
        .in_enabled = true,
        .disable_dhcp = true,
        .disable_dns = true,
        .if_mtu = MAX_IP,
        .if_mru = MAX_IP,
    };
    inet_pton(AF_INET, "192.168.131.0", &config.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
    memcpy(&config.vhost, host_ip, sizeof(host_ip));
    struct in_addr target;
    memcpy(&target, guest_ip, sizeof(guest_ip));
    struct in_addr local = {.s_addr = htonl(INADDR_LOOPBACK)};
    static const SlirpCb callbacks = {
        .send_packet = from_slirp,
        .guest_error = guest_error,
        .clock_get_ns = clock_ns,
        .notify = slirp_notify,
        .register_poll_socket = register_socket,
        .unregister_poll_socket = register_socket,
    };
    Slirp *slirp = slirp_new(&config, &callbacks, &bridge);
    if (!slirp) {
        fprintf(stderr, "Cannot initialize the ActiveSync network\n");
        return 1;
    }
    int status = 1;
    if (slirp_add_hostfwd(slirp, 0, local, port, target, RAPI_PORT) != 0) {
        fprintf(stderr, "Cannot bind the loopback RAPI port %d\n", port);
    } else {
        puts("ready");
        fflush(stdout);
        status = proxy(slirp, &bridge);
    }
    slirp_cleanup(slirp);
    return status;
}
