/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DoomGeneric backend for the H3970's rotated PXA RGB565 scanout. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"

enum {
    LCD_WIDTH = 320,
    LCD_HEIGHT = 240,
    VIEW_WIDTH = LCD_HEIGHT,
    VIEW_HEIGHT = LCD_WIDTH,
    RECORD_HOLD_MS = 900,
};

static unsigned char *framebuffer;
static unsigned int stride;
static int tty;
static int buttons;
static struct timespec started;
static volatile sig_atomic_t stopping;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static void restore_console(void)
{
    ioctl(tty, KDSETMODE, KD_TEXT);
    close(tty);
    close(buttons);
}

static int open_buttons(void)
{
    DIR *devices = opendir("/dev/input");
    struct dirent *entry;
    int fd = -1;

    if (!devices) {
        return -1;
    }
    while ((entry = readdir(devices))) {
        char name[80] = "";

        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        fd = openat(dirfd(devices), entry->d_name, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
                strcmp(name, "H3970 buttons") == 0) {
                break;
            }
            close(fd);
            fd = -1;
        }
    }
    closedir(devices);
    return fd;
}

void DG_Init(void)
{
    struct fb_fix_screeninfo fixed;
    struct fb_var_screeninfo mode;
    int fd = open("/dev/fb0", O_RDWR);

    if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &mode) < 0) {
        fail("/dev/fb0");
    }
    if (fixed.type != FB_TYPE_PACKED_PIXELS || fixed.visual != FB_VISUAL_TRUECOLOR ||
        mode.xres != LCD_WIDTH || mode.yres != LCD_HEIGHT ||
        mode.xoffset || mode.yoffset || mode.bits_per_pixel != 16 ||
        mode.red.offset != 11 || mode.red.length != 5 ||
        mode.green.offset != 5 || mode.green.length != 6 ||
        mode.blue.offset != 0 || mode.blue.length != 5 ||
        fixed.line_length < LCD_WIDTH * sizeof(uint16_t) ||
        fixed.line_length % sizeof(uint16_t) ||
        fixed.smem_len / fixed.line_length < LCD_HEIGHT) {
        fprintf(stderr, "H3970 Doom needs a 320x240 RGB565 framebuffer\n");
        exit(EXIT_FAILURE);
    }
    stride = fixed.line_length;
    framebuffer = mmap(NULL, stride * LCD_HEIGHT, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if (framebuffer == MAP_FAILED) {
        fail("framebuffer mmap");
    }
    close(fd);
    buttons = open_buttons();
    if (buttons < 0) {
        fprintf(stderr, "H3970 buttons input device is missing\n");
        exit(EXIT_FAILURE);
    }
    tty = open("/dev/tty0", O_RDWR);
    if (tty < 0 || ioctl(tty, KDSETMODE, KD_GRAPHICS) < 0) {
        fail("KD_GRAPHICS");
    }
    atexit(restore_console);
    memset(framebuffer, 0, stride * LCD_HEIGHT);
    clock_gettime(CLOCK_MONOTONIC, &started);
    fprintf(stderr, "H3970 DOOM: 240x320 full-screen portrait, native buttons ready\n");
}

#ifndef CMAP256
static uint16_t rgb565(pixel_t color)
{
    return ((color >> 8) & 0xf800) | ((color >> 5) & 0x07e0) |
           ((color >> 3) & 0x001f);
}
#endif

void DG_DrawFrame(void)
{
    static unsigned int source_row[LCD_WIDTH];
    static int initialized;

    if (!initialized) {
        for (unsigned int col = 0; col < LCD_WIDTH; ++col) {
            unsigned int y = VIEW_HEIGHT - 1 - col;
            source_row[col] = y * DOOMGENERIC_RESY / VIEW_HEIGHT * DOOMGENERIC_RESX;
        }
        initialized = 1;
    }
#ifdef CMAP256
    const uint16_t *palette = H3970_FramePalette();
#endif
    /* The H3970 maps portrait (x, y) to native [row x][column 319 - y]. */
    for (unsigned int row = 0; row < LCD_HEIGHT; ++row) {
        uint16_t *dest = (uint16_t *)(framebuffer + row * stride);
        unsigned int x = row * DOOMGENERIC_RESX / VIEW_WIDTH;
        for (unsigned int col = 0; col < LCD_WIDTH; ++col) {
            pixel_t pixel = DG_ScreenBuffer[source_row[col] + x];
#ifdef CMAP256
            dest[col] = palette[pixel];
#else
            dest[col] = rgb565(pixel);
#endif
        }
    }
}

void DG_SleepMs(uint32_t milliseconds)
{
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000};

    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {
    }
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return ((int64_t)now.tv_sec - started.tv_sec) * 1000 +
           (now.tv_nsec - started.tv_nsec) / 1000000;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    /* DoomGeneric key values from doomkeys.h. The Linux and Doom headers
       use overlapping KEY_* names for different values. */
    static const unsigned char mapping[KEY_MAX + 1] = {
        [KEY_UP] = 0xad, [KEY_DOWN] = 0xaf,
        [KEY_LEFT] = 0xac, [KEY_RIGHT] = 0xae,
        [KEY_ENTER] = '\r', [KEY_HOMEPAGE] = 27,
        [KEY_CALENDAR] = 0xa3, [KEY_ADDRESSBOOK] = 0xa2,
        [KEY_MAIL] = 0xb6,
    };
    static int record_down, release_tab;
    static uint32_t record_since;
    struct input_event event;

    if (release_tab) {
        release_tab = 0;
        *pressed = 0;
        *key = '\t';
        return 1;
    }
    while (read(buttons, &event, sizeof(event)) == sizeof(event)) {
        if (event.type != EV_KEY || event.value > 1 || event.value < 0)
            continue;
        if (event.code == KEY_RECORD) {
            if (event.value) {
                record_since = DG_GetTicksMs();
                record_down = 1;
            } else if (record_down) {
                record_down = 0;
                if (DG_GetTicksMs() - record_since >= RECORD_HOLD_MS) {
                    stopping = 1;
                    return 0;
                }
                /* A short press keeps the original automap action. */
                release_tab = 1;
                *pressed = 1;
                *key = '\t';
                return 1;
            }
            continue;
        }
        if (event.code <= KEY_MAX && mapping[event.code]) {
            *pressed = event.value;
            *key = mapping[event.code];
            return 1;
        }
    }
    if (record_down && DG_GetTicksMs() - record_since >= RECORD_HOLD_MS)
        stopping = 1;
    return 0;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    struct sigaction stop = { .sa_handler = request_stop };
    sigemptyset(&stop.sa_mask);
    if (sigaction(SIGTERM, &stop, NULL) < 0) {
        fail("sigaction");
    }
    while (!stopping) {
        doomgeneric_Tick();
    }
    return EXIT_SUCCESS;
}
