#include "guide.h"
#include "klib.h"

// ============================================================================
// GLOBAL STATE
// ============================================================================

GuideStats guide_stats;
GuideContext guide_context;

// ============================================================================
// INITIALIZATION
// ============================================================================

void guide_init(RoutingTable* routing_table) {
    kprintf("[GUIDE] Initializing...\n");

    guide_context.routing_table = routing_table;
    guide_context.scan_position = 0;

    // Инициализируем все deck queues (НОВАЯ АРХИТЕКТУРА: 5 queues вместо 11)
    for (int i = 0; i < 5; i++) {
        deck_queue_init(&guide_context.deck_queues[i]);
    }

    deck_queue_init(&guide_context.execution_queue);

    guide_stats.events_routed = 0;
    guide_stats.events_completed = 0;
    guide_stats.routing_iterations = 0;

    kprintf("[GUIDE] Initialized (4 decks: OPERATIONS, STORAGE, HARDWARE, NETWORK)\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================

// Обёртка для синхронной обработки
void guide_scan_and_dispatch(RoutingTable* routing_table) {
    guide_scan_and_route(&guide_context);
    atomic_increment_u64((volatile uint64_t*)&guide_stats.routing_iterations);
}

void guide_run(void) {
    kprintf("[GUIDE] Starting main loop...\n");

    uint64_t iterations = 0;

    while (1) {
        // Сканируем routing table и маршрутизируем события
        guide_scan_and_route(&guide_context);

        atomic_increment_u64((volatile uint64_t*)&guide_stats.routing_iterations);

        // Пауза для снижения нагрузки на CPU
        cpu_pause();

        // Периодическая статистика
        iterations++;
        if (iterations % 10000000 == 0) {
            guide_print_stats();
        }
    }
}

// ============================================================================
// GETTERS
// ============================================================================

DeckQueue* guide_get_deck_queue(uint8_t deck_prefix) {
    // НОВАЯ АРХИТЕКТУРА: префиксы 1-4
    if (deck_prefix >= 1 && deck_prefix <= 4) {
        return &guide_context.deck_queues[deck_prefix];
    }
    return 0;
}

DeckQueue* guide_get_execution_queue(void) {
    return &guide_context.execution_queue;
}

// ============================================================================
// STATISTICS
// ============================================================================

void guide_print_stats(void) {
    kprintf("[GUIDE] Stats: routed=%lu completed=%lu iterations=%lu\n",
            guide_stats.events_routed,
            guide_stats.events_completed,
            guide_stats.routing_iterations);
}
