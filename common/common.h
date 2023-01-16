#pragma once
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#if defined(__amd64__) || defined(_M_AMD64)
#define CUIK__IS_X64 1
#elif defined(__aarch64__)
#define CUIK__IS_AARCH64 1
#endif

#if defined(__CUIK__) || !defined(__amd64__) || !defined(_M_AMD64)
#define USE_INTRIN 0
#else
#define USE_INTRIN 1
#endif

#define STR2(x) #x
#define STR(x) STR2(x)

#ifndef COUNTOF
#define COUNTOF(...) (sizeof(__VA_ARGS__) / sizeof(__VA_ARGS__[0]))
#endif

#ifdef NDEBUG
#define TODO() __builtin_unreachable()
#else
#define TODO() (assert(0 && "TODO"), __builtin_unreachable())
#endif

// just because we use a threads fallback layer which can include windows
// and such which is annoying... eventually need to modify that out or something
#ifndef thread_local
#define thread_local _Thread_local
#endif

#define SWAP(T, a, b) \
do {                  \
    T temp = a;       \
    a = b;            \
    b = temp;         \
} while (0)

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

void tls_init(void);
void tls_reset(void);
void* tls_push(size_t size);
void* tls_pop(size_t size);
void* tls_save();
void tls_restore(void* p);

void* cuik__valloc(size_t sz);
void cuik__vfree(void* p, size_t sz);

static bool memeq(const void* a, size_t al, const void* b, size_t bl) {
    return al == bl && memcmp(a, b, al) == 0;
}

static bool cstr_equals(const char* str1, const char* str2) {
    return strcmp(str1, str2) == 0;
}

// returns the number of bytes written
static size_t cstr_copy(size_t len, char* dst, const char* src) {
    size_t i = 0;
    while (src[i]) {
        assert(i < len);

        dst[i] = src[i];
        i += 1;
    }
    return i;
}
