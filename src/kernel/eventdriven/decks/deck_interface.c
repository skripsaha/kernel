#include "deck_interface.h"
#include "klib.h"

// ============================================================================
// INITIALIZATION
// ============================================================================

void deck_init(DeckContext* ctx, const char* name, uint8_t prefix, DeckProcessFunc func) {
    ctx->stats.name = name;
    ctx->stats.prefix = prefix;
    ctx->stats.events_processed = 0;
    ctx->stats.errors = 0;

    ctx->process_func = func;
    ctx->deck_prefix = prefix;

    // Получаем input queue от Guide
    ctx->input_queue = guide_get_deck_queue(prefix);

    kprintf("[DECK:%s] Initialized (prefix=%d)\n", name, prefix);
}

// ============================================================================
// GENERIC MAIN LOOP
// ============================================================================

// Обработать одно событие (для синхронной обработки в демо)
int deck_run_once(DeckContext* ctx) {
    // Получаем событие из очереди
    RoutingEntry* entry = deck_queue_pop(ctx->input_queue);

    if (entry) {
        // Обрабатываем событие - deck сам вызовет deck_complete() или deck_error()
        int success = ctx->process_func(entry);

        if (success) {
            atomic_increment_u64((volatile uint64_t*)&ctx->stats.events_processed);
        } else {
            atomic_increment_u64((volatile uint64_t*)&ctx->stats.errors);
        }
        return 1;  // Обработано событие
    }
    return 0;  // Очередь пуста
}

void deck_run(DeckContext* ctx) {
    kprintf("[DECK:%s] Starting main loop...\n", ctx->stats.name);

    uint64_t iterations = 0;

    while (1) {
        if (!deck_run_once(ctx)) {
            // Очередь пуста - делаем паузу
            cpu_pause();
        }

        // Периодическая статистика
        iterations++;
        if (iterations % 10000000 == 0) {
            kprintf("[DECK:%s] processed=%lu errors=%lu\n",
                    ctx->stats.name,
                    ctx->stats.events_processed,
                    ctx->stats.errors);
        }
    }
}
