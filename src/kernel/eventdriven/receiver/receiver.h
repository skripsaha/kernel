#ifndef RECEIVER_H
#define RECEIVER_H

#include "../core/events.h"
#include "../core/ringbuffer.h"
#include "../core/atomics.h"
#include "klib.h"

// ============================================================================
// EVENT RECEIVER - Первый компонент pipeline
// ============================================================================
//
// Функции:
// 1. Получает события из user→kernel ring buffer
// 2. Генерирует уникальные ID (SECURITY: только kernel может это делать!)
// 3. Валидирует события (проверка корректности полей)
// 4. Добавляет timestamp
// 5. Отправляет в Center для определения маршрута
//
// ============================================================================

// Глобальный счётчик событий (atomic)
extern volatile uint64_t global_event_id_counter;

// Статистика receiver
typedef struct {
    volatile uint64_t events_received;     // Всего получено событий
    volatile uint64_t events_validated;    // Успешно валидировано
    volatile uint64_t events_rejected;     // Отклонено (invalid)
    volatile uint64_t events_forwarded;    // Отправлено в Center
} ReceiverStats;

// Глобальная статистика
extern ReceiverStats receiver_stats;

// ============================================================================
// RECEIVER INITIALIZATION
// ============================================================================

void receiver_init(void);

// ============================================================================
// ID GENERATION - Генерация уникальных ID
// ============================================================================

static inline uint64_t receiver_generate_event_id(void) {
    // Атомарно инкрементируем глобальный счётчик
    uint64_t id = atomic_increment_u64(&global_event_id_counter);
    return id;
}

// ============================================================================
// EVENT VALIDATION - Валидация события
// ============================================================================

static inline int receiver_validate_event(Event* event) {
    // 1. Проверяем тип события
    if (event->type == EVENT_NONE || event->type >= EVENT_MAX) {
        return 0;  // Invalid type
    }

    // 2. Проверяем user_id (не должен быть 0 для user events)
    if (event->user_id == 0) {
        return 0;  // Invalid user_id
    }

    // 3. Проверяем, что ID == 0 (user НЕ ДОЛЖЕН устанавливать ID!)
    // Это критично для безопасности!
    if (event->id != 0) {
        // User пытался подделать ID - REJECT!
        return 0;
    }

    // 4. Дополнительные проверки по типу события
    switch (event->type) {
        case EVENT_MEMORY_ALLOC:
            // Проверяем размер аллокации
            {
                uint64_t size = *(uint64_t*)event->data;
                if (size == 0 || size > (1ULL << 32)) {
                    return 0;  // Некорректный размер
                }
            }
            break;

        case EVENT_FILE_OPEN:
            // Проверяем, что путь null-terminated
            {
                int found_null = 0;
                for (int i = 0; i < EVENT_DATA_SIZE; i++) {
                    if (event->data[i] == 0) {
                        found_null = 1;
                        break;
                    }
                }
                if (!found_null) {
                    return 0;  // Путь не null-terminated
                }
            }
            break;

        default:
            // Для остальных типов базовая валидация достаточна
            break;
    }

    return 1;  // Событие валидно
}

// ============================================================================
// EVENT PROCESSING - Обработка события
// ============================================================================

static inline void receiver_process_event(Event* event, EventRingBuffer* to_center_ring) {
    // 1. Инкрементируем счётчик полученных событий
    atomic_increment_u64((volatile uint64_t*)&receiver_stats.events_received);

    // 2. Валидация
    if (!receiver_validate_event(event)) {
        atomic_increment_u64((volatile uint64_t*)&receiver_stats.events_rejected);
        return;  // Отклоняем невалидное событие
    }

    atomic_increment_u64((volatile uint64_t*)&receiver_stats.events_validated);

    // 3. Генерируем уникальный ID (ПЕРЕПИСЫВАЕМ поле id!)
    event->id = receiver_generate_event_id();

    // 4. Добавляем timestamp
    event->timestamp = rdtsc();

    // 5. Отправляем в Center для определения маршрута
    // FIXED: Добавлен timeout чтобы избежать бесконечного зависания
    uint64_t timeout = 1000000;  // ~1млн итераций
    while (!event_ring_push(to_center_ring, event)) {
        cpu_pause();
        if (--timeout == 0) {
            // Timeout - буфер переполнен слишком долго!
            kprintf("[RECEIVER] ERROR: Center ring buffer timeout for event %lu\n", event->id);
            atomic_increment_u64((volatile uint64_t*)&receiver_stats.events_rejected);
            return;  // Отбрасываем событие
        }
    }

    atomic_increment_u64((volatile uint64_t*)&receiver_stats.events_forwarded);
}

// ============================================================================
// RECEIVER MAIN LOOP - Главный цикл (запускается на отдельном core)
// ============================================================================

void receiver_run(EventRingBuffer* from_user_ring, EventRingBuffer* to_center_ring);

// ============================================================================
// STATS & MONITORING
// ============================================================================

void receiver_print_stats(void);

#endif // RECEIVER_H
