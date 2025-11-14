#ifndef ROUTING_TABLE_H
#define ROUTING_TABLE_H

#include "../core/events.h"
#include "../core/atomics.h"
#include "ktypes.h"

// ============================================================================
// ROUTING TABLE - Hash table для хранения routing entries
// ============================================================================

// Размер hash table (должен быть степенью 2)
// Уменьшено до 64 для экономии памяти и быстрой инициализации
#define ROUTING_TABLE_SIZE 64
#define ROUTING_TABLE_MASK (ROUTING_TABLE_SIZE - 1)

_Static_assert((ROUTING_TABLE_SIZE & (ROUTING_TABLE_SIZE - 1)) == 0,
               "ROUTING_TABLE_SIZE must be power of 2");

// ============================================================================
// ROUTING BUCKET - Bucket в hash table (для разрешения коллизий)
// ============================================================================

#define BUCKET_CAPACITY 4  // Количество entries в одном bucket

typedef struct {
    volatile uint32_t lock;  // Spinlock для bucket (0 = unlocked, 1 = locked)
    uint32_t count;          // Количество занятых slots
    RoutingEntry entries[BUCKET_CAPACITY];
} RoutingBucket;

// ============================================================================
// ROUTING TABLE STRUCTURE
// ============================================================================

typedef struct {
    RoutingBucket buckets[ROUTING_TABLE_SIZE] __attribute__((aligned(64)));
    volatile uint64_t total_entries;  // Общее количество entries
    volatile uint64_t collisions;     // Количество коллизий
} RoutingTable;

// Глобальная routing table
extern RoutingTable global_routing_table;

// ============================================================================
// BUCKET LOCKING - Spinlock для bucket
// ============================================================================

static inline void bucket_lock(RoutingBucket* bucket) {
    while (!atomic_cas_u32(&bucket->lock, 0, 1)) {
        cpu_pause();
    }
}

static inline void bucket_unlock(RoutingBucket* bucket) {
    atomic_store_u32(&bucket->lock, 0);
}

// ============================================================================
// HASH FUNCTION - Простая и быстрая hash функция
// ============================================================================

static inline uint64_t hash_event_id(uint64_t event_id) {
    // MurmurHash-inspired mixing
    event_id ^= event_id >> 33;
    event_id *= 0xff51afd7ed558ccdULL;
    event_id ^= event_id >> 33;
    event_id *= 0xc4ceb9fe1a85ec53ULL;
    event_id ^= event_id >> 33;
    return event_id;
}

static inline uint64_t routing_table_index(uint64_t event_id) {
    return hash_event_id(event_id) & ROUTING_TABLE_MASK;
}

// ============================================================================
// ROUTING TABLE OPERATIONS
// ============================================================================

// Инициализация
void routing_table_init(RoutingTable* table);

// Вставка routing entry
int routing_table_insert(RoutingTable* table, RoutingEntry* entry);

// Поиск routing entry по event_id
RoutingEntry* routing_table_lookup(RoutingTable* table, uint64_t event_id);

// Удаление routing entry (после завершения обработки)
int routing_table_remove(RoutingTable* table, uint64_t event_id);

// Статистика
void routing_table_print_stats(RoutingTable* table);

// ============================================================================
// INLINE HELPERS
// ============================================================================

static inline int routing_table_is_full(RoutingTable* table) {
    return table->total_entries >= (ROUTING_TABLE_SIZE * BUCKET_CAPACITY);
}

#endif // ROUTING_TABLE_H
