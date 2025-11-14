#ifndef GUIDE_H
#define GUIDE_H

#include "../core/events.h"
#include "../routing/routing_table.h"
#include "../core/ringbuffer.h"

// ============================================================================
// GUIDE - Динамическая маршрутизация событий к Decks
// ============================================================================
//
// Функции:
// 1. Сканирует routing table для событий, ожидающих обработки
// 2. Читает следующий prefix из routing entry
// 3. Отправляет событие в соответствующий Deck
// 4. Проверяет завершение обработки decks (префикс затёрт?)
// 5. Если все префиксы = 0, отправляет в Execution Deck
//
// ============================================================================

// Forward declarations для deck queues
typedef struct DeckQueue DeckQueue;

// Статистика
typedef struct {
    volatile uint64_t events_routed;
    volatile uint64_t events_completed;
    volatile uint64_t routing_iterations;
} GuideStats;

extern GuideStats guide_stats;

// ============================================================================
// DECK QUEUE - Очередь событий для каждого deck
// ============================================================================

// Уменьшено для экономии памяти
#define DECK_QUEUE_SIZE 128
#define DECK_QUEUE_MASK (DECK_QUEUE_SIZE - 1)

struct DeckQueue {
    volatile uint64_t head __attribute__((aligned(64)));
    volatile uint64_t tail __attribute__((aligned(64)));

    // Вместо копирования Event, храним указатели на RoutingEntry
    RoutingEntry* entries[DECK_QUEUE_SIZE] __attribute__((aligned(64)));
};

// Операции с deck queue
static inline void deck_queue_init(DeckQueue* queue) {
    atomic_store_u64(&queue->head, 0);
    atomic_store_u64(&queue->tail, 0);
}

static inline int deck_queue_push(DeckQueue* queue, RoutingEntry* entry) {
    uint64_t current_tail = atomic_load_u64(&queue->tail);
    uint64_t current_head = atomic_load_u64(&queue->head);

    if ((current_tail - current_head) >= DECK_QUEUE_SIZE) {
        return 0;  // Queue full
    }

    uint64_t index = current_tail & DECK_QUEUE_MASK;
    queue->entries[index] = entry;

    COMPILER_BARRIER();
    atomic_store_u64(&queue->tail, current_tail + 1);

    return 1;
}

static inline RoutingEntry* deck_queue_pop(DeckQueue* queue) {
    uint64_t current_head = atomic_load_u64(&queue->head);
    uint64_t current_tail = atomic_load_u64(&queue->tail);

    if (current_head == current_tail) {
        return 0;  // Queue empty
    }

    uint64_t index = current_head & DECK_QUEUE_MASK;
    RoutingEntry* entry = queue->entries[index];

    COMPILER_BARRIER();
    atomic_store_u64(&queue->head, current_head + 1);

    return entry;
}

static inline int deck_queue_is_empty(DeckQueue* queue) {
    return atomic_load_u64(&queue->head) == atomic_load_u64(&queue->tail);
}

// ============================================================================
// GUIDE CONTEXT - Глобальное состояние Guide
// ============================================================================

typedef struct {
    RoutingTable* routing_table;

    // Очереди для каждого deck (НОВАЯ АРХИТЕКТУРА v1)
    DeckQueue deck_queues[5];  // 0 = unused, 1-4 = deck prefixes (OPERATIONS, STORAGE, HARDWARE, NETWORK)

    // Очередь для Execution Deck (завершённые события)
    DeckQueue execution_queue;

    // Scan position в routing table (для round-robin)
    volatile uint64_t scan_position;
} GuideContext;

extern GuideContext guide_context;

// ============================================================================
// INITIALIZATION
// ============================================================================

void guide_init(RoutingTable* routing_table);

// ============================================================================
// ROUTING LOGIC
// ============================================================================

// Сканирует routing table и маршрутизирует события
static inline void guide_scan_and_route(GuideContext* ctx) {
    RoutingTable* table = ctx->routing_table;

    // Сканируем несколько buckets за один проход (batch processing)
    for (int batch = 0; batch < 16; batch++) {
        uint64_t pos = atomic_load_u64(&ctx->scan_position);
        uint64_t bucket_index = pos % ROUTING_TABLE_SIZE;

        RoutingBucket* bucket = &table->buckets[bucket_index];

        // Lock bucket
        bucket_lock(bucket);

        // Проверяем каждую entry в bucket
        for (int i = 0; i < BUCKET_CAPACITY; i++) {
            RoutingEntry* entry = &bucket->entries[i];

            if (entry->event_id == 0) {
                continue;  // Empty slot
            }

            if (entry->state != EVENT_STATUS_PROCESSING) {
                continue;  // Не в состоянии обработки
            }

            // Получаем следующий prefix
            uint8_t next_prefix = routing_entry_get_next_prefix(entry);

            if (next_prefix == DECK_PREFIX_NONE) {
                // Все префиксы обработаны! Отправляем в Execution Deck
                if (deck_queue_push(&ctx->execution_queue, entry)) {
                    entry->state = EVENT_STATUS_SUCCESS;
                    atomic_increment_u64((volatile uint64_t*)&guide_stats.events_completed);
                }
            } else {
                // Проверяем abort_flag - если установлен, пропускаем обработку
                if (entry->abort_flag) {
                    // Прерываем обработку - очищаем все префиксы и отправляем в Execution
                    for (int j = 0; j < MAX_ROUTING_STEPS; j++) {
                        entry->prefixes[j] = DECK_PREFIX_NONE;
                    }
                    if (deck_queue_push(&ctx->execution_queue, entry)) {
                        entry->state = EVENT_STATUS_ERROR;
                        atomic_increment_u64((volatile uint64_t*)&guide_stats.events_completed);
                    }
                } else {
                    // Отправляем в соответствующий deck (НОВАЯ АРХИТЕКТУРА: префиксы 1-4)
                    if (next_prefix >= 1 && next_prefix <= 4) {
                        if (deck_queue_push(&ctx->deck_queues[next_prefix], entry)) {
                            // ИСПРАВЛЕНО: НЕ затираем prefix! Deck сам затрет после обработки!
                            atomic_increment_u64((volatile uint64_t*)&guide_stats.events_routed);
                        }
                    }
                }
            }
        }

        bucket_unlock(bucket);

        // Двигаем scan position
        atomic_increment_u64(&ctx->scan_position);
    }
}

// ============================================================================
// MAIN LOOP
// ============================================================================

// Обработать один проход сканирования (для синхронной обработки)
void guide_scan_and_dispatch(RoutingTable* routing_table);

void guide_run(void);

// ============================================================================
// STATS
// ============================================================================

void guide_print_stats(void);

// ============================================================================
// GETTERS для deck queues (используется decks)
// ============================================================================

DeckQueue* guide_get_deck_queue(uint8_t deck_prefix);
DeckQueue* guide_get_execution_queue(void);

#endif // GUIDE_H
