#ifndef CENTER_H
#define CENTER_H

#include "../core/events.h"
#include "../core/ringbuffer.h"
#include "../routing/routing_table.h"
#include "klib.h"

// ============================================================================
// CENTER - Определяет маршрут события через систему
// ============================================================================
//
// Функции (НОВАЯ АРХИТЕКТУРА v1):
// 1. Получает событие от Receiver
// 2. Проверяет Security ПЕРЕД маршрутизацией
// 3. Анализирует тип события
// 4. Определяет маршрут через 4 deck (массив префиксов)
// 5. Создаёт RoutingEntry в routing table
// 6. Уведомляет Guide о новом событии
//
// ============================================================================

// Статистика
typedef struct {
    volatile uint64_t events_processed;
    volatile uint64_t routes_created;
    volatile uint64_t routing_errors;
    volatile uint64_t security_denied;  // События отклоненные Security
} CenterStats;

extern CenterStats center_stats;

// ============================================================================
// INITIALIZATION
// ============================================================================

void center_init(void);

// ============================================================================
// SECURITY CHECK - Проверка безопасности ПЕРЕД маршрутизацией
// ============================================================================

// Возвращает 1 если разрешено, 0 если отказано
static inline int security_check_event(Event* event) {
    switch (event->type) {
        // ===== MEMORY OPERATIONS =====
        case EVENT_MEMORY_ALLOC:
        case EVENT_MEMORY_MAP: {
            uint64_t size = *(uint64_t*)event->data;
            // Не разрешаем аллокации > 1GB
            if (size > (1ULL << 30)) {
                kprintf("[CENTER:SECURITY] Denied: memory allocation too large (%lu bytes) for user %lu\n",
                        size, event->user_id);
                return 0;
            }
            return 1;
        }

        // ===== FILE OPERATIONS =====
        case EVENT_FILE_OPEN:
        case EVENT_FILE_READ:
        case EVENT_FILE_WRITE: {
            const char* path = (const char*)event->data;
            // Запрещаем доступ к /etc/shadow
            if (strcmp(path, "/etc/shadow") == 0) {
                kprintf("[CENTER:SECURITY] Denied: access to %s for user %lu\n",
                        path, event->user_id);
                return 0;
            }
            return 1;
        }

        // ===== NETWORK OPERATIONS =====
        case EVENT_NET_SOCKET:
        case EVENT_NET_CONNECT:
        case EVENT_NET_SEND:
        case EVENT_NET_RECV:
            // TODO: реальная проверка прав на сеть
            return 1;

        // ===== PROCESS OPERATIONS =====
        case EVENT_PROC_CREATE:
        case EVENT_PROC_KILL:
        case EVENT_PROC_SIGNAL:
            // TODO: реальная проверка прав на процессы
            return 1;

        // Все остальные операции разрешены по умолчанию
        default:
            return 1;
    }
}

// ============================================================================
// ROUTE DETERMINATION - Определение маршрута на основе типа события
// ============================================================================

// Заполняет массив prefixes на основе типа события
// НОВАЯ АРХИТЕКТУРА: только 4 deck вместо 10
static inline void center_determine_route(EventType type, uint8_t prefixes[MAX_ROUTING_STEPS]) {
    // Очищаем массив
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        prefixes[i] = DECK_PREFIX_NONE;
    }

    // Определяем маршрут на основе типа события
    switch (type) {
        // ===== STORAGE DECK: Memory Operations =====
        case EVENT_MEMORY_ALLOC:
        case EVENT_MEMORY_FREE:
        case EVENT_MEMORY_MAP:
            prefixes[0] = DECK_PREFIX_STORAGE;
            break;

        // ===== STORAGE DECK: File Operations =====
        case EVENT_FILE_OPEN:
        case EVENT_FILE_CLOSE:
        case EVENT_FILE_READ:
        case EVENT_FILE_WRITE:
        case EVENT_FILE_STAT:
        // TagFS operations
        case EVENT_FILE_CREATE_TAGGED:
        case EVENT_FILE_QUERY:
        case EVENT_FILE_TAG_ADD:
        case EVENT_FILE_TAG_REMOVE:
        case EVENT_FILE_TAG_GET:
            prefixes[0] = DECK_PREFIX_STORAGE;
            break;

        // ===== OPERATIONS DECK: Process Operations =====
        case EVENT_PROC_CREATE:
        case EVENT_PROC_EXIT:
        case EVENT_PROC_KILL:
        case EVENT_PROC_WAIT:
        case EVENT_PROC_GETPID:
        case EVENT_PROC_SIGNAL:
            prefixes[0] = DECK_PREFIX_OPERATIONS;
            break;

        // ===== OPERATIONS DECK: IPC Operations =====
        case EVENT_IPC_SEND:
        case EVENT_IPC_RECV:
        case EVENT_IPC_SHM_CREATE:
        case EVENT_IPC_SHM_ATTACH:
        case EVENT_IPC_PIPE_CREATE:
            prefixes[0] = DECK_PREFIX_OPERATIONS;
            break;

        // ===== HARDWARE DECK: Timer Operations =====
        case EVENT_TIMER_CREATE:
        case EVENT_TIMER_CANCEL:
        case EVENT_TIMER_SLEEP:
        case EVENT_TIMER_GETTICKS:
            prefixes[0] = DECK_PREFIX_HARDWARE;
            break;

        // ===== HARDWARE DECK: Device Operations =====
        case EVENT_DEV_OPEN:
        case EVENT_DEV_IOCTL:
        case EVENT_DEV_READ:
        case EVENT_DEV_WRITE:
            prefixes[0] = DECK_PREFIX_HARDWARE;
            break;

        // ===== NETWORK DECK: Network Operations (stub в v1) =====
        case EVENT_NET_SOCKET:
        case EVENT_NET_CONNECT:
        case EVENT_NET_SEND:
        case EVENT_NET_RECV:
            prefixes[0] = DECK_PREFIX_NETWORK;
            break;

        default:
            // Неизвестный тип - отправим в OPERATIONS
            kprintf("[CENTER] Unknown event type %d, routing to OPERATIONS\n", type);
            prefixes[0] = DECK_PREFIX_OPERATIONS;
            break;
    }
}

// ============================================================================
// EVENT PROCESSING
// ============================================================================

// Обрабатывает событие: проверяет security, создаёт routing entry и добавляет в таблицу
static inline int center_process_event(Event* event, RoutingTable* routing_table, ResponseRingBuffer* kernel_to_user_ring) {
    atomic_increment_u64((volatile uint64_t*)&center_stats.events_processed);

    // 1. SECURITY CHECK - ПЕРЕД маршрутизацией!
    if (!security_check_event(event)) {
        atomic_increment_u64((volatile uint64_t*)&center_stats.security_denied);
        kprintf("[CENTER] Event %lu DENIED by security\n", event->id);

        // FIXED: Отправляем error response обратно в user space
        Response error_response;
        response_init(&error_response, event->id, EVENT_STATUS_DENIED);
        error_response.timestamp = rdtsc();
        error_response.error_code = 1;  // Security violation

        // Отправляем в response ring
        // FIXED: Добавлен timeout
        uint64_t timeout = 1000000;
        while (!response_ring_push(kernel_to_user_ring, &error_response)) {
            cpu_pause();
            if (--timeout == 0) {
                kprintf("[CENTER] ERROR: Response ring buffer timeout for event %lu\n", event->id);
                return 0;  // Не смогли отправить ответ
            }
        }

        return 0;
    }

    // 2. Определяем маршрут
    uint8_t prefixes[MAX_ROUTING_STEPS];
    center_determine_route(event->type, prefixes);

    // 3. Создаём routing entry
    RoutingEntry entry;
    routing_entry_init(&entry, event->id, event);

    // Копируем префиксы
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        entry.prefixes[i] = prefixes[i];
    }

    entry.created_at = rdtsc();
    entry.state = EVENT_STATUS_PROCESSING;

    // 4. Добавляем в routing table
    if (!routing_table_insert(routing_table, &entry)) {
        // Не удалось добавить (таблица полна?)
        atomic_increment_u64((volatile uint64_t*)&center_stats.routing_errors);
        return 0;
    }

    atomic_increment_u64((volatile uint64_t*)&center_stats.routes_created);
    return 1;
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void center_run(EventRingBuffer* from_receiver_ring, RoutingTable* routing_table, ResponseRingBuffer* kernel_to_user_ring);

// ============================================================================
// STATS
// ============================================================================

void center_print_stats(void);

#endif // CENTER_H
