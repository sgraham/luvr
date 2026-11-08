#pragma once

#include "base_config.h"

#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if COMPILER_MSVC
#  define FORCE_INLINE __forceinline
#elif COMPILER_CLANG || COMPILER_GCC
#  define FORCE_INLINE __attribute__((always_inline))
#endif

#if COMPILER_MSVC
#  define NORETURN __declspec(noreturn)
#elif COMPILER_CLANG || COMPILER_GCC
#  define NORETURN __attribute__((noreturn))
#else
#  error port
#endif

#if COMPILER_MSVC
#  define TRAP() __debugbreak()
#elif COMPILER_CLANG || COMPILER_GCC
#  define TRAP() __builtin_trap()
#else
#  error port
#endif

#define CHECK(x)                                                            \
  do {                                                                      \
    if (!(x)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
      TRAP();                                                               \
    }                                                                       \
  } while (0)

#if BUILD_DEBUG
#  define ASSERT(x) CHECK(x)
#else
#  define ASSERT(x) (void)(x)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
