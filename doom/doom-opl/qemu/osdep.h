/* The C library subset required by QEMU's YM3812 core in the guest build. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define g_new(type, count) ((type *)malloc(sizeof(type) * (count)))
#define g_free free
