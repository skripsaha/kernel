#include "eventdriven_system.h"
#include "receiver/receiver.h"
#include "center/center.h"
#include "guide/guide.h"
#include "execution/execution_deck.h"
#include "decks/deck_interface.h"
#include "klib.h"

// Forward declarations для deck init/run функций (НОВАЯ АРХИТЕКТУРА v1)
extern void operations_deck_init(void);
extern void storage_deck_init(void);
extern void hardware_deck_init(void);
extern void network_deck_init(void);

extern int operations_deck_run_once(void);
extern int storage_deck_run_once(void);
extern int hardware_deck_run_once(void);
extern int network_deck_run_once(void);

// ============================================================================
// GLOBAL SYSTEM
// ============================================================================

EventDrivenSystem global_event_system;

// ============================================================================
// MEMORY ALLOCATION для Ring Buffers (shared memory)
// ============================================================================

// TODO: использовать vmm_alloc для выделения shared memory
// Сейчас используем статические буферы

static EventRingBuffer user_to_kernel_buffer;
static EventRingBuffer receiver_to_center_buffer;
static ResponseRingBuffer kernel_to_user_buffer;

// ============================================================================
// INITIALIZATION
// ============================================================================

void eventdriven_system_init(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  EVENT-DRIVEN SYSTEM INITIALIZATION (v1)\n");
    kprintf("============================================================\n");
    kprintf("\n");

    // 1. Инициализируем ring buffers
    kprintf("[SYSTEM] Initializing ring buffers...\n");
    event_ring_init(&user_to_kernel_buffer);
    event_ring_init(&receiver_to_center_buffer);
    response_ring_init(&kernel_to_user_buffer);

    global_event_system.user_to_kernel_ring = &user_to_kernel_buffer;
    global_event_system.receiver_to_center_ring = &receiver_to_center_buffer;
    global_event_system.kernel_to_user_ring = &kernel_to_user_buffer;

    kprintf("[SYSTEM] Ring buffers initialized\n");

    // 2. Инициализируем routing table
    kprintf("[SYSTEM] Initializing routing table...\n");
    routing_table_init(&global_routing_table);
    global_event_system.routing_table = &global_routing_table;

    // 3. Инициализируем компоненты pipeline
    kprintf("[SYSTEM] Initializing pipeline components...\n");

    receiver_init();
    center_init();  // Center теперь включает Security проверки!
    guide_init(&global_routing_table);

    // 4. Инициализируем 4 processing decks (НОВАЯ АРХИТЕКТУРА)
    kprintf("[SYSTEM] Initializing processing decks...\n");

    operations_deck_init();  // Process + IPC
    storage_deck_init();     // Memory + Filesystem
    hardware_deck_init();    // Timer + Devices
    network_deck_init();     // Network (stub в v1)

    // 5. Инициализируем execution deck
    kprintf("[SYSTEM] Initializing execution deck...\n");
    execution_deck_init(&kernel_to_user_buffer, &global_routing_table);

    global_event_system.initialized = 1;
    global_event_system.running = 0;

    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  EVENT-DRIVEN SYSTEM INITIALIZED SUCCESSFULLY\n");
    kprintf("  Architecture: 4 decks (OPERATIONS, STORAGE, HARDWARE, NETWORK)\n");
    kprintf("  Security: Integrated in Center (pre-routing check)\n");
    kprintf("============================================================\n");
    kprintf("\n");
}

// ============================================================================
// START SYSTEM (для демонстрации - запускаем только один компонент)
// ============================================================================

// NOTE: В реальной системе каждый компонент должен работать на отдельном CPU core
// Сейчас мы создадим демонстрационную версию, которая запускает только Receiver

void eventdriven_system_start(void) {
    if (!global_event_system.initialized) {
        kprintf("[SYSTEM] ERROR: System not initialized!\n");
        return;
    }

    kprintf("[SYSTEM] Starting event-driven system...\n");
    global_event_system.running = 1;

    // TODO: В реальной системе здесь должны быть:
    // 1. CPU core pinning для каждого компонента
    // 2. Создание kernel threads для каждого компонента
    // 3. Запуск всех компонентов параллельно

    kprintf("[SYSTEM] System is ready to process events!\n");
    kprintf("[SYSTEM] NOTE: Using synchronous processing for demo\n");
}

// ============================================================================
// SYNCHRONOUS PROCESSING (для демонстрации)
// ============================================================================

// Обработка одной итерации всего pipeline
void eventdriven_process_one_iteration(void) {
    Event event;

    // 1. Receiver: забираем событие из user→kernel
    if (event_ring_pop(global_event_system.user_to_kernel_ring, &event)) {
        receiver_process_event(&event, global_event_system.receiver_to_center_ring);
    }

    // 2. Center: забираем из receiver→center, проверяем Security и определяем маршрут
    if (event_ring_pop(global_event_system.receiver_to_center_ring, &event)) {
        center_process_event(&event, global_event_system.routing_table, global_event_system.kernel_to_user_ring);
    }

    // 3. Guide: сканируем routing table и раздаём по deck'ам
    guide_scan_and_dispatch(global_event_system.routing_table);

    // 4. Decks: обрабатываем события в каждом deck (НОВАЯ АРХИТЕКТУРА: 4 decks)
    operations_deck_run_once();
    storage_deck_run_once();
    hardware_deck_run_once();
    network_deck_run_once();

    // 5. Execution: собираем завершённые события и отправляем ответы
    execution_deck_run_once();
}

// Обработка N итераций pipeline для полной обработки событий
void eventdriven_process_events(int iterations) {
    for (int i = 0; i < iterations; i++) {
        eventdriven_process_one_iteration();
    }
}

// ============================================================================
// STOP SYSTEM
// ============================================================================

void eventdriven_system_stop(void) {
    kprintf("[SYSTEM] Stopping event-driven system...\n");
    global_event_system.running = 0;
    kprintf("[SYSTEM] System stopped\n");
}

// ============================================================================
// ACCESSORS
// ============================================================================

EventRingBuffer* eventdriven_get_user_to_kernel_ring(void) {
    return global_event_system.user_to_kernel_ring;
}

ResponseRingBuffer* eventdriven_get_kernel_to_user_ring(void) {
    return global_event_system.kernel_to_user_ring;
}

// ============================================================================
// STATISTICS
// ============================================================================

void eventdriven_print_full_stats(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  EVENT-DRIVEN SYSTEM STATISTICS\n");
    kprintf("============================================================\n");

    receiver_print_stats();
    center_print_stats();
    guide_print_stats();
    routing_table_print_stats(&global_routing_table);

    // Статистика decks (НОВАЯ АРХИТЕКТУРА)
    extern DeckContext operations_deck_context;
    extern DeckContext storage_deck_context;
    extern DeckContext hardware_deck_context;
    extern DeckContext network_deck_context;

    kprintf("[DECK:Operations] processed=%lu errors=%lu\n",
            operations_deck_context.stats.events_processed,
            operations_deck_context.stats.errors);
    kprintf("[DECK:Storage] processed=%lu errors=%lu\n",
            storage_deck_context.stats.events_processed,
            storage_deck_context.stats.errors);
    kprintf("[DECK:Hardware] processed=%lu errors=%lu\n",
            hardware_deck_context.stats.events_processed,
            hardware_deck_context.stats.errors);
    kprintf("[DECK:Network] processed=%lu errors=%lu\n",
            network_deck_context.stats.events_processed,
            network_deck_context.stats.errors);

    execution_deck_print_stats();

    kprintf("============================================================\n");
    kprintf("\n");
}
