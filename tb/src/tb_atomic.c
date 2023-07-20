#include <assert.h>
#include <stdbool.h>

#if defined(_MSC_VER)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static_assert(sizeof(int) == sizeof(long), "expected LLP64");

int tb_atomic_int_load(int* dst) {
    return InterlockedOr((long*)dst, 0);
}

int tb_atomic_int_add(int* dst, int src) {
    return InterlockedExchangeAdd((long*)dst, src);
}

int tb_atomic_int_store(int* dst, int src) {
    return InterlockedExchange((long*)dst, src);
}

bool tb_atomic_int_cmpxchg(int* address, int old_value, int new_value) {
    return _InterlockedCompareExchange((volatile long*) address, new_value, old_value) == new_value;
}

size_t tb_atomic_size_load(size_t* dst) {
    return InterlockedOr64((LONG64*)dst, 0);
}

size_t tb_atomic_size_add(size_t* dst, size_t src) {
    return InterlockedExchangeAdd64((LONG64*)dst, src);
}

size_t tb_atomic_size_sub(size_t* dst, size_t src) {
    return InterlockedExchangeAdd64((LONG64*)dst, -src);
}

size_t tb_atomic_size_store(size_t* dst, size_t src) {
    return InterlockedExchange64((LONG64*)dst, src);
}
#elif __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#include <stddef.h>
#include <stdbool.h>

int tb_atomic_int_load(int* dst) {
    return atomic_load((atomic_int*) dst);
}

int tb_atomic_int_add(int* dst, int src) {
    return atomic_fetch_add((atomic_int*) dst, src);
}

int tb_atomic_int_store(int* dst, int src) {
    return atomic_exchange((atomic_int*) dst, src);
}

bool tb_atomic_int_cmpxchg(int* address, int old_value, int new_value) {
    return atomic_compare_exchange_strong((_Atomic(int)*) address, &old_value, new_value);
}

size_t tb_atomic_size_load(size_t* dst) {
    return atomic_load((atomic_size_t*) dst);
}

size_t tb_atomic_size_add(size_t* dst, size_t src) {
    return atomic_fetch_add((atomic_size_t*) dst, src);
}

size_t tb_atomic_size_sub(size_t* dst, size_t src) {
    return atomic_fetch_sub((atomic_size_t*) dst, src);
}

size_t tb_atomic_size_store(size_t* dst, size_t src) {
    return atomic_exchange((atomic_size_t*) dst, src);
}

#else
#error "Implement atomics for this platform"
#endif