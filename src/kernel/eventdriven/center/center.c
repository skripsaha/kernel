#include "center.h"
#include "klib.h"

// ============================================================================
// GLOBAL STATE
// ============================================================================

CenterStats center_stats;

// ============================================================================
// INITIALIZATION
// ============================================================================

void center_init(void) {
    center_stats.events_processed = 0;
    center_stats.routes_created = 0;
    center_stats.routing_errors = 0;
    center_stats.security_denied = 0;

    kprintf("[CENTER] Initialized (with Security checks)\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void center_run(EventRingBuffer* from_receiver_ring, RoutingTable* routing_table, ResponseRingBuffer* kernel_to_user_ring) {
    kprintf("[CENTER] Starting main loop...\n");

    Event event;
    uint64_t iterations = 0;

    while (1) {
        // Получаем событие от Receiver
        if (event_ring_pop(from_receiver_ring, &event)) {
            // Обрабатываем: определяем маршрут и создаём routing entry
            // center_process_event() увеличит счетчик внутри
            center_process_event(&event, routing_table, kernel_to_user_ring);

            // NOTE: Guide будет polling routing table и обнаружит новый entry
        } else {
            // Буфер пуст
            cpu_pause();
        }

        // Периодическая статистика
        iterations++;
        if (iterations % 10000000 == 0) {
            center_print_stats();
        }
    }
}

// ============================================================================
// STATISTICS
// ============================================================================

void center_print_stats(void) {
    kprintf("[CENTER] Stats: processed=%lu routes_created=%lu errors=%lu security_denied=%lu\n",
            center_stats.events_processed,
            center_stats.routes_created,
            center_stats.routing_errors,
            center_stats.security_denied);
}
