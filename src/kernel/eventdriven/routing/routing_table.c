#include "routing_table.h"
#include "klib.h"

// ============================================================================
// GLOBAL ROUTING TABLE
// ============================================================================

RoutingTable global_routing_table;

// ============================================================================
// INITIALIZATION
// ============================================================================

void routing_table_init(RoutingTable* table) {
    kprintf("[ROUTING_TABLE] Initializing...\n");

    // Быстрая инициализация через memset
    memset(table, 0, sizeof(RoutingTable));

    kprintf("[ROUTING_TABLE] Initialized (size=%d buckets, capacity=%d entries)\n",
            ROUTING_TABLE_SIZE, ROUTING_TABLE_SIZE * BUCKET_CAPACITY);
}

// ============================================================================
// INSERT - Вставка routing entry
// ============================================================================

int routing_table_insert(RoutingTable* table, RoutingEntry* entry) {
    uint64_t index = routing_table_index(entry->event_id);
    RoutingBucket* bucket = &table->buckets[index];

    bucket_lock(bucket);

    // Проверяем, есть ли место в bucket
    if (bucket->count >= BUCKET_CAPACITY) {
        bucket_unlock(bucket);
        atomic_increment_u64(&table->collisions);
        return 0;  // Bucket полон (коллизия!)
    }

    // Ищем свободный slot
    for (int i = 0; i < BUCKET_CAPACITY; i++) {
        if (bucket->entries[i].event_id == 0) {
            // Копируем entry
            bucket->entries[i] = *entry;
            bucket->count++;
            atomic_increment_u64(&table->total_entries);
            bucket_unlock(bucket);
            return 1;  // Успех
        }
    }

    bucket_unlock(bucket);
    return 0;  // Не должно произойти
}

// ============================================================================
// LOOKUP - Поиск routing entry
// ============================================================================

RoutingEntry* routing_table_lookup(RoutingTable* table, uint64_t event_id) {
    uint64_t index = routing_table_index(event_id);
    RoutingBucket* bucket = &table->buckets[index];

    bucket_lock(bucket);

    // Ищем entry с нужным event_id
    for (int i = 0; i < BUCKET_CAPACITY; i++) {
        if (bucket->entries[i].event_id == event_id) {
            RoutingEntry* entry = &bucket->entries[i];
            bucket_unlock(bucket);
            return entry;  // Найдено
        }
    }

    bucket_unlock(bucket);
    return 0;  // Не найдено
}

// ============================================================================
// REMOVE - Удаление routing entry
// ============================================================================

int routing_table_remove(RoutingTable* table, uint64_t event_id) {
    uint64_t index = routing_table_index(event_id);
    RoutingBucket* bucket = &table->buckets[index];

    bucket_lock(bucket);

    // Ищем и удаляем entry
    for (int i = 0; i < BUCKET_CAPACITY; i++) {
        if (bucket->entries[i].event_id == event_id) {
            bucket->entries[i].event_id = 0;
            bucket->entries[i].state = 0;
            bucket->count--;
            atomic_decrement_u64(&table->total_entries);
            bucket_unlock(bucket);
            return 1;  // Успех
        }
    }

    bucket_unlock(bucket);
    return 0;  // Не найдено
}

// ============================================================================
// STATISTICS
// ============================================================================

void routing_table_print_stats(RoutingTable* table) {
    uint64_t total = atomic_load_u64(&table->total_entries);
    uint64_t collisions = atomic_load_u64(&table->collisions);
    uint64_t utilization = (total * 100) / (ROUTING_TABLE_SIZE * BUCKET_CAPACITY);

    kprintf("[ROUTING_TABLE] entries=%lu collisions=%lu utilization=%lu%%\n",
            total, collisions, utilization);
}
