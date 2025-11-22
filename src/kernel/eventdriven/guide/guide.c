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

// Process all events in routing table (called from timer IRQ in background)
void guide_process_all(void) {
    // ASYNC MODE: Called from timer IRQ every 100ms

    // Check if there's work to do (avoid spam if idle)
    static int debug_once = 1;
    if (debug_once && guide_stats.events_routed == 0) {
        kprintf("[GUIDE] Background processing started (called from timer IRQ)\n");
        debug_once = 0;
    }

    // SCAN #1: Find new events and route to deck queues
    // ROUTING_TABLE_SIZE = 64, so we need 4 calls to scan all buckets (16 per call)
    for (int i = 0; i < 4; i++) {
        guide_scan_and_route(&guide_context);
    }

    // Process events in each deck queue
    extern int operations_deck_run_once(void);
    extern int storage_deck_run_once(void);
    extern int hardware_deck_run_once(void);
    extern int network_deck_run_once(void);
    extern int execution_deck_run_once(void);

    // Process all events in deck queues
    while (operations_deck_run_once());
    while (storage_deck_run_once());
    while (hardware_deck_run_once());
    while (network_deck_run_once());

    // SCAN #2: Find completed events (after decks cleared prefixes)
    // CRITICAL: This moves completed events to execution_queue!
    for (int i = 0; i < 4; i++) {
        guide_scan_and_route(&guide_context);
    }

    // Process completed events (write results to ResultRing and send INT 0x81)
    while (execution_deck_run_once());

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
