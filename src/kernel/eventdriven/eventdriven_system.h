#ifndef EVENTDRIVEN_SYSTEM_H
#define EVENTDRIVEN_SYSTEM_H

#include "core/events.h"
#include "core/ringbuffer.h"
#include "routing/routing_table.h"

// ============================================================================
// EVENT-DRIVEN SYSTEM - Главный интегратор
// ============================================================================
//
// Управляет всей event-driven архитектурой:
// - Ring buffers (user↔kernel коммуникация)
// - Routing table
// - Все компоненты pipeline (Receiver, Center, Guide, Decks, Execution)
//
// ============================================================================

// ============================================================================
// GLOBAL SYSTEM STATE
// ============================================================================

typedef struct {
    // === RING BUFFERS ===
    EventRingBuffer* user_to_kernel_ring;     // User → Kernel события
    EventRingBuffer* receiver_to_center_ring; // Receiver → Center
    ResponseRingBuffer* kernel_to_user_ring;  // Kernel → User ответы

    // === ROUTING TABLE ===
    RoutingTable* routing_table;

    // === SYSTEM STATUS ===
    volatile int initialized;
    volatile int running;

} EventDrivenSystem;

extern EventDrivenSystem global_event_system;

// ============================================================================
// SYSTEM LIFECYCLE
// ============================================================================

// Инициализация всей event-driven системы
void eventdriven_system_init(void);

// Запуск всех компонентов (должно вызываться после инициализации)
// NOTE: Это блокирующая функция - запускает бесконечные циклы компонентов
void eventdriven_system_start(void);

// Остановка системы (graceful shutdown)
void eventdriven_system_stop(void);

// ============================================================================
// ACCESSORS - Для получения доступа к ring buffers извне
// ============================================================================

EventRingBuffer* eventdriven_get_user_to_kernel_ring(void);
ResponseRingBuffer* eventdriven_get_kernel_to_user_ring(void);

// ============================================================================
// SYNCHRONOUS PROCESSING (для демонстрации)
// ============================================================================

// Обработать одну итерацию всего pipeline (все компоненты)
void eventdriven_process_one_iteration(void);

// Обработать N итераций для полной обработки событий
void eventdriven_process_events(int iterations);

// ============================================================================
// STATISTICS & MONITORING
// ============================================================================

void eventdriven_print_full_stats(void);

#endif // EVENTDRIVEN_SYSTEM_H
