#pragma once
#include "basic_types.h"
#include <math.h> //cel,log2,pow
#include <stdint.h>
#include <stdlib.h>

#undef max
#undef min
static inline ssize_t min(ssize_t x, ssize_t y)
{
    return (((x) < (y)) ? (x) : (y));
}
static inline ssize_t max(ssize_t x, ssize_t y)
{
    return (((x) < (y)) ? (y) : (x));
}

#define ISSIGNED(t) (((t) (-1)) < ((t) 0))

#define UMAXOF(t) (((0x1ULL << ((sizeof(t) * 8ULL) - 1ULL)) - 1ULL) | (0xFULL << ((sizeof(t) * 8ULL) - 4ULL)))

#define SMAXOF(t) (((0x1ULL << ((sizeof(t) * 8ULL) - 1ULL)) - 1ULL) | (0x7ULL << ((sizeof(t) * 8ULL) - 4ULL)))

#define MAXOF(t) ((unsigned long long) (ISSIGNED(t) ? SMAXOF(t) : UMAXOF(t)))
