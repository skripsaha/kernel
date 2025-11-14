#include "execution_deck.h"
#include "klib.h"

// ============================================================================
// GLOBAL STATE
// ============================================================================

ExecutionStats execution_stats;
static ResponseRingBuffer* response_ring = 0;
static RoutingTable* routing_table = 0;
static DeckQueue* execution_queue = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

void execution_deck_init(ResponseRingBuffer* resp_ring, RoutingTable* rtable) {
    response_ring = resp_ring;
    routing_table = rtable;
    execution_queue = guide_get_execution_queue();

    execution_stats.events_executed = 0;
    execution_stats.responses_sent = 0;
    execution_stats.errors = 0;

    kprintf("[EXECUTION] Initialized\n");
}

// ============================================================================
// RESULT COLLECTION
// ============================================================================

static void collect_results(RoutingEntry* entry, Response* response) {
    // Собираем результаты от всех decks, которые обработали событие
    response_init(response, entry->event_id, EVENT_STATUS_SUCCESS);
    response->timestamp = rdtsc();

    // TODO: более сложная логика сборки результатов
    // Сейчас просто берём результат от последнего deck

    int result_index = -1;
    for (int i = MAX_ROUTING_STEPS - 1; i >= 0; i--) {
        if (entry->deck_results[i] != 0) {
            result_index = i;
            break;
        }
    }

    if (result_index >= 0) {
        // Копируем результат в response
        void* deck_result = entry->deck_results[result_index];

        // TODO: правильное копирование результата
        // Сейчас просто копируем указатель (небезопасно!)
        *(void**)response->result = deck_result;
        response->result_size = sizeof(void*);

        kprintf("[EXECUTION] Collected result from deck at index %d for event %lu\n",
                result_index, entry->event_id);
    } else {
        // Нет результатов (событие прошло, но ничего не вернуло)
        response->result_size = 0;
        kprintf("[EXECUTION] No results for event %lu\n", entry->event_id);
    }
}

// ============================================================================
// EVENT PROCESSING
// ============================================================================

static void process_completed_event(RoutingEntry* entry) {
    // 1. Собираем результаты
    Response response;
    collect_results(entry, &response);

    // 2. Отправляем response в user space
    while (!response_ring_push(response_ring, &response)) {
        cpu_pause();  // Busy-wait если буфер полон
    }

    atomic_increment_u64((volatile uint64_t*)&execution_stats.responses_sent);

    kprintf("[EXECUTION] Sent response for event %lu to user space\n", entry->event_id);

    // 3. Удаляем routing entry из таблицы (освобождаем ресурсы)
    routing_table_remove(routing_table, entry->event_id);

    atomic_increment_u64((volatile uint64_t*)&execution_stats.events_executed);
}

// ============================================================================
// MAIN LOOP
// ============================================================================

// Обработать одно завершённое событие (для синхронной обработки)
int execution_deck_run_once(void) {
    // Получаем завершённое событие от Guide
    RoutingEntry* entry = deck_queue_pop(execution_queue);

    if (entry) {
        // Обрабатываем завершённое событие
        process_completed_event(entry);
        return 1;  // Обработано
    }
    return 0;  // Очередь пуста
}

void execution_deck_run(void) {
    kprintf("[EXECUTION] Starting main loop...\n");

    uint64_t iterations = 0;

    while (1) {
        if (!execution_deck_run_once()) {
            // Очередь пуста
            cpu_pause();
        }

        // Периодическая статистика
        iterations++;
        if (iterations % 10000000 == 0) {
            execution_deck_print_stats();
        }
    }
}

// ============================================================================
// STATISTICS
// ============================================================================

void execution_deck_print_stats(void) {
    kprintf("[EXECUTION] Stats: executed=%lu responses_sent=%lu errors=%lu\n",
            execution_stats.events_executed,
            execution_stats.responses_sent,
            execution_stats.errors);
}
