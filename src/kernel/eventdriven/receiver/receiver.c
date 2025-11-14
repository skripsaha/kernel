#include "receiver.h"
#include "klib.h"  // Для kprintf

// ============================================================================
// GLOBAL STATE
// ============================================================================

volatile uint64_t global_event_id_counter = 1;  // Начинаем с 1 (0 = invalid)
ReceiverStats receiver_stats;

// ============================================================================
// INITIALIZATION
// ============================================================================

void receiver_init(void) {
    global_event_id_counter = 1;

    receiver_stats.events_received = 0;
    receiver_stats.events_validated = 0;
    receiver_stats.events_rejected = 0;
    receiver_stats.events_forwarded = 0;

    kprintf("[RECEIVER] Initialized (ID counter = %lu)\n", global_event_id_counter);
}

// ============================================================================
// MAIN LOOP - Polling events from user space
// ============================================================================

void receiver_run(EventRingBuffer* from_user_ring, EventRingBuffer* to_center_ring) {
    kprintf("[RECEIVER] Starting main loop...\n");

    Event event;
    uint64_t iterations = 0;

    while (1) {
        // Пытаемся получить событие из user→kernel ring buffer
        if (event_ring_pop(from_user_ring, &event)) {
            // Событие получено - обрабатываем
            receiver_process_event(&event, to_center_ring);
        } else {
            // Буфер пуст - делаем паузу для снижения нагрузки на CPU
            cpu_pause();
        }

        // Периодически выводим статистику (каждые 10 млн итераций)
        iterations++;
        if (iterations % 10000000 == 0) {
            receiver_print_stats();
        }
    }
}

// ============================================================================
// STATISTICS
// ============================================================================

void receiver_print_stats(void) {
    kprintf("[RECEIVER] Stats: received=%lu validated=%lu rejected=%lu forwarded=%lu\n",
            receiver_stats.events_received,
            receiver_stats.events_validated,
            receiver_stats.events_rejected,
            receiver_stats.events_forwarded);
}
