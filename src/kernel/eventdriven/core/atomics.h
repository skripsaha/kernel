#ifndef ATOMICS_H
#define ATOMICS_H

#include "ktypes.h"

// ============================================================================
// ATOMIC OPERATIONS - Lock-free примитивы
// ============================================================================

// Memory ordering для x86-64
#define MEMORY_BARRIER() __asm__ volatile("mfence" ::: "memory")
#define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

// ============================================================================
// ATOMIC LOAD/STORE
// ============================================================================

static inline uint64_t atomic_load_u64(volatile uint64_t* ptr) {
    uint64_t value;
    __asm__ volatile(
        "movq %1, %0"
        : "=r" (value)
        : "m" (*ptr)
        : "memory"
    );
    return value;
}

static inline void atomic_store_u64(volatile uint64_t* ptr, uint64_t value) {
    __asm__ volatile(
        "movq %1, %0"
        : "=m" (*ptr)
        : "r" (value)
        : "memory"
    );
}

static inline uint32_t atomic_load_u32(volatile uint32_t* ptr) {
    uint32_t value;
    __asm__ volatile(
        "movl %1, %0"
        : "=r" (value)
        : "m" (*ptr)
        : "memory"
    );
    return value;
}

static inline void atomic_store_u32(volatile uint32_t* ptr, uint32_t value) {
    __asm__ volatile(
        "movl %1, %0"
        : "=m" (*ptr)
        : "r" (value)
        : "memory"
    );
}

// ============================================================================
// ATOMIC INCREMENT/DECREMENT
// ============================================================================

static inline uint64_t atomic_increment_u64(volatile uint64_t* ptr) {
    uint64_t result;
    __asm__ volatile(
        "lock; xaddq %0, %1"
        : "=r" (result), "+m" (*ptr)
        : "0" (1)
        : "memory", "cc"
    );
    return result + 1;  // xadd возвращает старое значение
}

static inline uint64_t atomic_decrement_u64(volatile uint64_t* ptr) {
    uint64_t result;
    __asm__ volatile(
        "lock; xaddq %0, %1"
        : "=r" (result), "+m" (*ptr)
        : "0" (-1)
        : "memory", "cc"
    );
    return result - 1;
}

static inline uint32_t atomic_increment_u32(volatile uint32_t* ptr) {
    uint32_t result;
    __asm__ volatile(
        "lock; xaddl %0, %1"
        : "=r" (result), "+m" (*ptr)
        : "0" (1)
        : "memory", "cc"
    );
    return result + 1;
}

// ============================================================================
// COMPARE-AND-SWAP (CAS) - Основа lock-free алгоритмов
// ============================================================================

// Возвращает 1 если успешно, 0 если failed
static inline int atomic_cas_u64(volatile uint64_t* ptr, uint64_t expected, uint64_t desired) {
    uint64_t prev;
    __asm__ volatile(
        "lock; cmpxchgq %2, %1"
        : "=a" (prev), "+m" (*ptr)
        : "r" (desired), "0" (expected)
        : "memory", "cc"
    );
    return (prev == expected);
}

static inline int atomic_cas_u32(volatile uint32_t* ptr, uint32_t expected, uint32_t desired) {
    uint32_t prev;
    __asm__ volatile(
        "lock; cmpxchgl %2, %1"
        : "=a" (prev), "+m" (*ptr)
        : "r" (desired), "0" (expected)
        : "memory", "cc"
    );
    return (prev == expected);
}

// CAS с возвратом предыдущего значения
static inline uint64_t atomic_cas_u64_val(volatile uint64_t* ptr, uint64_t expected, uint64_t desired) {
    uint64_t prev;
    __asm__ volatile(
        "lock; cmpxchgq %2, %1"
        : "=a" (prev), "+m" (*ptr)
        : "r" (desired), "0" (expected)
        : "memory", "cc"
    );
    return prev;
}

// ============================================================================
// ATOMIC EXCHANGE
// ============================================================================

static inline uint64_t atomic_exchange_u64(volatile uint64_t* ptr, uint64_t value) {
    uint64_t result;
    __asm__ volatile(
        "xchgq %0, %1"
        : "=r" (result), "+m" (*ptr)
        : "0" (value)
        : "memory"
    );
    return result;
}

// ============================================================================
// SPIN-WAIT OPTIMIZATION - Pause instruction для гиперпоточности
// ============================================================================

static inline void cpu_pause(void) {
    __asm__ volatile("pause" ::: "memory");
}

static inline void cpu_relax(void) {
    cpu_pause();
}

// ============================================================================
// RDTSC - High-precision timestamp
// ============================================================================

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile(
        "rdtsc"
        : "=a" (lo), "=d" (hi)
        :: "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// Serializing RDTSC (более медленный, но точный)
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    __asm__ volatile(
        "rdtscp"
        : "=a" (lo), "=d" (hi)
        :: "rcx", "memory"
    );
    return ((uint64_t)hi << 32) | lo;
}

// ============================================================================
// PREFETCH - Cache optimization hints
// ============================================================================

static inline void prefetch_read(const void* ptr) {
    __asm__ volatile(
        "prefetcht0 %0"
        :
        : "m" (*(const char*)ptr)
    );
}

static inline void prefetch_write(const void* ptr) {
    __asm__ volatile(
        "prefetchw %0"
        :
        : "m" (*(const char*)ptr)
    );
}

#endif // ATOMICS_H
