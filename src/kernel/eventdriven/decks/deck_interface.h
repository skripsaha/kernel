#ifndef DECK_INTERFACE_H
#define DECK_INTERFACE_H

#include "../core/events.h"
#include "../guide/guide.h"
#include "klib.h"

// ============================================================================
// DECK INTERFACE - Общий интерфейс для всех Processing Decks
// ============================================================================

typedef struct {
    const char* name;                  // Название deck
    uint8_t prefix;                    // Уникальный prefix
    volatile uint64_t events_processed;
    volatile uint64_t errors;
} DeckStats;

// Функция обработки события (реализуется каждым deck)
// Deck должен вызвать deck_complete() в конце обработки
// Возвращает: 1 = success, 0 = error
typedef int (*DeckProcessFunc)(RoutingEntry* entry);

// ============================================================================
// DECK CONTEXT - Контекст для каждого deck
// ============================================================================

typedef struct {
    DeckStats stats;
    DeckProcessFunc process_func;
    DeckQueue* input_queue;
    uint8_t deck_prefix;
} DeckContext;

// ============================================================================
// GENERIC DECK OPERATIONS
// ============================================================================

// Инициализация deck
void deck_init(DeckContext* ctx, const char* name, uint8_t prefix, DeckProcessFunc func);

// Обработать одно событие (для синхронной обработки)
int deck_run_once(DeckContext* ctx);

// Главный цикл deck (generic)
void deck_run(DeckContext* ctx);

// ============================================================================
// DECK HELPERS - Завершение обработки
// ============================================================================

// Deck вызывает эту функцию после УСПЕШНОЙ обработки
static inline void deck_complete(RoutingEntry* entry, uint8_t deck_prefix, void* result) {
    // 1. Сохраняем результат
    entry->deck_results[deck_prefix - 1] = result;
    entry->deck_timestamps[deck_prefix - 1] = rdtsc();

    // 2. ЗАТИРАЕМ prefix (это ключевой момент!)
    routing_entry_clear_prefix(entry, deck_prefix);

    // 3. Устанавливаем completion flag
    uint32_t flag = 1 << (deck_prefix - 1);
    atomic_store_u32(&entry->completion_flags, entry->completion_flags | flag);

    // 4. Entry теперь готов для следующего шага (Guide его подхватит)
}

// Deck вызывает эту функцию при ОШИБКЕ обработки
static inline void deck_error(RoutingEntry* entry, uint8_t deck_prefix, uint32_t error_code) {
    // Устанавливаем флаг прерывания
    atomic_store_u32(&entry->abort_flag, 1);
    entry->error_code = error_code;
    entry->deck_timestamps[deck_prefix - 1] = rdtsc();

    // Затираем prefix чтобы Guide мог продолжить
    routing_entry_clear_prefix(entry, deck_prefix);

    kprintf("[DECK_ERROR] Event %lu: deck %d error code %u\n",
            entry->event_id, deck_prefix, error_code);
}

#endif // DECK_INTERFACE_H
