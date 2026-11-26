/* DoomGeneric compatibility for the Chocolate Doom sequencer. */
#ifndef H3970_DOOM_OPL_COMPAT_H
#define H3970_DOOM_OPL_COMPAT_H

#include <stdint.h>

#define PACKED_STRUCT(...) struct __attribute__((packed)) __VA_ARGS__

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SDL_SwapBE16(value) __builtin_bswap16(value)
#define SDL_SwapBE32(value) __builtin_bswap32(value)
#else
#define SDL_SwapBE16(value) (value)
#define SDL_SwapBE32(value) (value)
#endif

typedef enum {
    opl_doom1_1_666,
    opl_doom2_1_666,
    opl_doom_1_9,
} opl_driver_ver_t;

#endif
