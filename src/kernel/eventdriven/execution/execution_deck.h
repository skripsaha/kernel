#ifndef EXECUTION_DECK_H
#define EXECUTION_DECK_H

#include "../core/events.h"
#include "../core/ringbuffer.h"
#include "../guide/guide.h"

// ============================================================================
// EXECUTION DECK - Финальная обработка и отправка результатов
// ============================================================================
//
// Функции:
// 1. Получает завершённые routing entries от Guide
// 2. Собирает результаты от всех decks
// 3. Формирует Response
// 4. Отправляет Response в kernel→user ring buffer
// 5. Очищает routing entry из таблицы
//
// ============================================================================

typedef struct {
    volatile uint64_t events_executed;
    volatile uint64_t responses_sent;
    volatile uint64_t errors;
} ExecutionStats;

extern ExecutionStats execution_stats;

// ============================================================================
// INITIALIZATION
// ============================================================================

void execution_deck_init(ResponseRingBuffer* response_ring, RoutingTable* routing_table);

// ============================================================================
// MAIN LOOP
// ============================================================================

// Обработать одно завершённое событие (для синхронной обработки)
int execution_deck_run_once(void);

void execution_deck_run(void);

// ============================================================================
// STATS
// ============================================================================

void execution_deck_print_stats(void);

#endif // EXECUTION_DECK_H
