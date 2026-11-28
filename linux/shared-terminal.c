/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A single BusyBox PTY with output on the LCD and in a bounded replay buffer. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static const char root[] = "/tmp/h3970-terminal";
static const char input_path[] = "/tmp/h3970-terminal/input";
static const char output_path[] = "/tmp/h3970-terminal/output";
enum { history_size = 64 * 1024, reply_size = 512, input_size = 256 };

struct history {
    uint64_t end;
    unsigned int cols;
    unsigned int rows;
};

static void fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static void seek_read(int fd, void *bytes, size_t size, off_t offset)
{
    if (pread(fd, bytes, size, offset) != (ssize_t)size)
        fail("read terminal history");
}

static void seek_write(int fd, const void *bytes, size_t size, off_t offset)
{
    if (pwrite(fd, bytes, size, offset) != (ssize_t)size)
        fail("write terminal history");
}

static void lock_history(int fd, short type)
{
    struct flock lock = { .l_type = type, .l_whence = SEEK_SET };
    while (fcntl(fd, F_SETLKW, &lock) < 0) {
        if (errno != EINTR)
            fail("lock terminal history");
    }
}

static void write_lcd(int lcd, const unsigned char *bytes, size_t size)
{
    while (size) {
        ssize_t written = write(lcd, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            fail("write LCD console");
        bytes += written;
        size -= written;
    }
}

static void history_output(int fd, struct history *history, const unsigned char *bytes, size_t size)
{
    const size_t write_at = history->end % history_size;
    const size_t trailing = history_size - write_at;
    const size_t first = size < trailing ? size : trailing;
    lock_history(fd, F_WRLCK);
    seek_write(fd, bytes, first, sizeof(*history) + write_at);
    if (size > first)
        seek_write(fd, bytes + first, size - first, sizeof(*history));
    history->end += size;
    seek_write(fd, history, sizeof(*history), 0);
    lock_history(fd, F_UNLCK);
}

static int start_shell(const struct winsize *size, pid_t *child)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
        fail("open BusyBox PTY");
    const char *path = ptsname(master);
    if (!path)
        fail("get BusyBox tty");
    int slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0 || ioctl(slave, TIOCSWINSZ, size) < 0)
        fail("size BusyBox tty");

    *child = fork();
    if (*child < 0)
        fail("fork BusyBox");
    if (*child == 0) {
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
            dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0)
            fail("attach BusyBox tty");
        setenv("TERM", "linux", 1);
        setenv("PS1", "iPAQ # ", 1);
        execl("/bin/sh", "sh", "-i", (char *)NULL);
        fail("run BusyBox");
    }
    close(slave);
    return master;
}

static void relay(int master, int input, int lcd, int output, struct history *history)
{
    unsigned char keys[input_size], bytes[4096];
    size_t head = 0, length = 0;
    for (;;) {
        struct pollfd fds[2] = {
            { .fd = master, .events = POLLIN | (length ? POLLOUT : 0) },
            { .fd = input, .events = length ? 0 : POLLIN },
        };
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            fail("poll shared terminal");
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t size = read(master, bytes, sizeof(bytes));
            if (size == 0 || (size < 0 && errno == EIO))
                return;
            if (size > 0) {
                write_lcd(lcd, bytes, size);
                history_output(output, history, bytes, size);
            } else if (errno != EAGAIN && errno != EINTR) {
                fail("read BusyBox output");
            }
        }
        if (length && (fds[0].revents & POLLOUT)) {
            ssize_t size = write(master, keys + head, length);
            if (size < 0 && errno == EIO)
                return;
            if (size > 0) {
                head += size;
                length -= size;
            } else if (size < 0 && errno != EAGAIN && errno != EINTR) {
                fail("write BusyBox input");
            }
        }
        if (fds[1].revents & POLLIN) {
            ssize_t size = read(input, keys, sizeof(keys));
            if (size > 0) {
                head = 0;
                length = size;
            } else if (size < 0 && errno != EAGAIN && errno != EINTR) {
                fail("read browser input");
            }
        }
    }
}

static void serve(void)
{
    if (mkdir(root, 0700) < 0 || mkfifo(input_path, 0600) < 0)
        fail("create shared terminal");
    const int input = open(input_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    const int output = open(output_path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    const int lcd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (input < 0 || output < 0 || lcd < 0)
        fail("open shared terminal");

    struct termios settings;
    struct winsize size;
    if (ioctl(lcd, TIOCGWINSZ, &size) < 0 || !size.ws_row || !size.ws_col ||
        tcgetattr(lcd, &settings) < 0)
        fail("read LCD console settings");
    settings.c_oflag &= ~OPOST;
    settings.c_lflag &= ~ECHO;
    if (tcsetattr(lcd, TCSANOW, &settings) < 0)
        fail("set LCD console settings");

    struct history history = { .cols = size.ws_col, .rows = size.ws_row };
    seek_write(output, &history, sizeof(history), 0);
    for (;;) {
        pid_t child;
        const int master = start_shell(&size, &child);
        relay(master, input, lcd, output, &history);
        close(master);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
        /* Give an exec failure or a deliberate shell exit a bounded restart rate. */
        sleep(1);
    }
}

static int nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void send_keys(const char *hex)
{
    const size_t length = strlen(hex);
    unsigned char keys[input_size];
    if (!length || length % 2 || length > sizeof(keys) * 2) {
        fputs("invalid terminal input length\n", stderr);
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < length / 2; i++) {
        const int high = nibble(hex[2 * i]), low = nibble(hex[2 * i + 1]);
        if (high < 0 || low < 0) {
            fputs("invalid terminal input\n", stderr);
            exit(EXIT_FAILURE);
        }
        keys[i] = high * 16 + low;
    }
    const int fd = open(input_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || write(fd, keys, length / 2) != (ssize_t)(length / 2))
        fail("send terminal input");
    close(fd);
}

static void exchange(const char *cursor, const char *keys)
{
    char *tail;
    const bool initial = strcmp(cursor, "-") == 0;
    errno = 0;
    uint64_t position = strtoull(cursor, &tail, 10);
    if (!initial && (*cursor < '0' || *cursor > '9' || *tail || errno)) {
        fputs("invalid terminal cursor\n", stderr);
        exit(EXIT_FAILURE);
    }
    const int fd = open(output_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        fail("open terminal history");
    if (keys)
        send_keys(keys);
    lock_history(fd, F_RDLCK);
    struct history history;
    seek_read(fd, &history, sizeof(history), 0);
    const uint64_t oldest = history.end > history_size ? history.end - history_size : 0;
    const bool reset = initial || position < oldest || position > history.end;
    if (reset)
        position = oldest;
    const size_t read_at = position % history_size;
    const size_t trailing = history_size - read_at;
    const size_t available = history.end - position;
    const size_t count = available < reply_size ? available : reply_size;
    const size_t first = count < trailing ? count : trailing;
    unsigned char bytes[reply_size];
    if (first)
        seek_read(fd, bytes, first, sizeof(history) + read_at);
    if (count > first)
        seek_read(fd, bytes + first, count - first, sizeof(history));
    close(fd);

    printf("terminal=%" PRIu64 ",%d,%d,%u,%u:", position + count,
           count < available, reset, history.cols, history.rows);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; i++) {
        putchar(digits[bytes[i] >> 4]);
        putchar(digits[bytes[i] & 15]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "serve") == 0)
        serve();
    else if ((argc == 3 || argc == 4) && strcmp(argv[1], "exchange") == 0)
        exchange(argv[2], argc == 4 ? argv[3] : NULL);
    else {
        fputs("usage: terminal serve | exchange CURSOR [HEX]\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
