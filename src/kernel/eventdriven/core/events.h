#ifndef EVENTS_H
#define EVENTS_H

#include "ktypes.h"

// ============================================================================
// EVENT TYPES - Типы событий в системе
// ============================================================================

typedef enum {
    EVENT_NONE = 0,

    // Memory operations
    EVENT_MEMORY_ALLOC = 1,
    EVENT_MEMORY_FREE = 2,
    EVENT_MEMORY_MAP = 3,

    // File operations
    EVENT_FILE_OPEN = 10,
    EVENT_FILE_CLOSE = 11,
    EVENT_FILE_READ = 12,
    EVENT_FILE_WRITE = 13,
    EVENT_FILE_STAT = 14,

    // TagFS operations - Tag-based filesystem
    EVENT_FILE_CREATE_TAGGED = 15,  // Create file with tags
    EVENT_FILE_QUERY = 16,           // Query files by tags
    EVENT_FILE_TAG_ADD = 17,         // Add tag to file
    EVENT_FILE_TAG_REMOVE = 18,      // Remove tag from file
    EVENT_FILE_TAG_GET = 19,         // Get all tags of a file

    // Network operations
    EVENT_NET_SOCKET = 20,
    EVENT_NET_CONNECT = 21,
    EVENT_NET_SEND = 22,
    EVENT_NET_RECV = 23,

    // Process operations
    EVENT_PROC_CREATE = 30,
    EVENT_PROC_EXIT = 31,
    EVENT_PROC_SIGNAL = 32,
    EVENT_PROC_KILL = 33,
    EVENT_PROC_WAIT = 34,
    EVENT_PROC_GETPID = 35,

    // Device operations
    EVENT_DEV_OPEN = 40,
    EVENT_DEV_IOCTL = 41,
    EVENT_DEV_READ = 42,
    EVENT_DEV_WRITE = 43,

    // Timer operations
    EVENT_TIMER_CREATE = 50,
    EVENT_TIMER_CANCEL = 51,
    EVENT_TIMER_SLEEP = 52,
    EVENT_TIMER_GETTICKS = 53,

    // IPC operations
    EVENT_IPC_SEND = 60,
    EVENT_IPC_RECV = 61,
    EVENT_IPC_SHM_CREATE = 62,
    EVENT_IPC_SHM_ATTACH = 63,
    EVENT_IPC_PIPE_CREATE = 64,

    EVENT_MAX = 255
} EventType;

// ============================================================================
// EVENT STATUS - Статусы обработки событий
// ============================================================================

typedef enum {
    EVENT_STATUS_SUCCESS = 0,
    EVENT_STATUS_PENDING = 1,
    EVENT_STATUS_PROCESSING = 2,
    EVENT_STATUS_ERROR = 3,
    EVENT_STATUS_INVALID = 4,
    EVENT_STATUS_DENIED = 5,
    EVENT_STATUS_TIMEOUT = 6
} EventStatus;

// ============================================================================
// DECK PREFIXES - Уникальные префиксы для каждого Processing Deck
// ============================================================================
// Новая архитектура v1: 4 логических deck вместо 10
// Security проверяется в Center ДО маршрутизации

#define DECK_PREFIX_NONE        0
#define DECK_PREFIX_OPERATIONS  1  // Process + IPC operations
#define DECK_PREFIX_STORAGE     2  // Memory + Filesystem operations
#define DECK_PREFIX_HARDWARE    3  // Timer + Devices operations
#define DECK_PREFIX_NETWORK     4  // Network operations (stub в v1)

// ============================================================================
// EVENT STRUCTURE - Основная структура события (256 байт)
// ============================================================================

#define EVENT_DATA_SIZE 224

typedef struct __attribute__((packed)) {
    // === METADATA (32 bytes) ===
    uint64_t id;              // Уникальный ID (устанавливает ТОЛЬКО kernel!)
    uint64_t user_id;         // PID процесса-отправителя
    uint64_t timestamp;       // Timestamp создания (TSC или RDTSC)
    uint32_t type;            // Тип события (EventType)
    uint32_t flags;           // Дополнительные флаги

    // === PAYLOAD (224 bytes) ===
    uint8_t data[EVENT_DATA_SIZE];  // Данные события
} Event;

// Compile-time проверка размера
_Static_assert(sizeof(Event) == 256, "Event must be exactly 256 bytes");

// ============================================================================
// RESPONSE STRUCTURE - Ответ от kernel к user (4096 байт)
// ============================================================================

#define RESPONSE_DATA_SIZE 4064

typedef struct __attribute__((packed)) {
    // === METADATA (32 bytes) ===
    uint64_t event_id;        // ID соответствующего события
    uint64_t timestamp;       // Timestamp завершения
    uint32_t status;          // EventStatus
    uint32_t error_code;      // Код ошибки (если status == ERROR)
    uint64_t result_size;     // Размер результата в байтах

    // === RESULT (4064 bytes) ===
    uint8_t result[RESPONSE_DATA_SIZE];  // Результат операции
} Response;

// Compile-time проверка размера
_Static_assert(sizeof(Response) == 4096, "Response must be exactly 4096 bytes");

// ============================================================================
// ROUTING ENTRY - Запись в таблице маршрутизации
// ============================================================================

#define MAX_ROUTING_STEPS 8

typedef struct {
    uint64_t event_id;                    // ID события
    Event event_copy;                     // КОПИЯ события (не указатель!)

    // Prefix routing system
    uint8_t prefixes[MAX_ROUTING_STEPS];  // Массив префиксов (маршрут)
    volatile uint8_t current_index;       // Текущая позиция в маршруте

    // Результаты от каждого deck
    void* deck_results[MAX_ROUTING_STEPS];
    uint64_t deck_timestamps[MAX_ROUTING_STEPS];

    // Метаданные
    uint64_t created_at;                  // Timestamp создания
    volatile uint32_t completion_flags;   // Битовые флаги завершения decks
    volatile uint32_t state;              // Состояние обработки
    volatile uint32_t abort_flag;         // Флаг прерывания (например, при отказе Security)
    uint32_t error_code;                  // Код ошибки
} RoutingEntry;

// ============================================================================
// EVENT HELPERS - Вспомогательные функции для работы с событиями
// ============================================================================

static inline void event_init(Event* e, EventType type, uint64_t user_id) {
    e->id = 0;  // Будет установлен kernel!
    e->user_id = user_id;
    e->type = type;
    e->flags = 0;
    e->timestamp = 0;

    // Очищаем payload
    for (int i = 0; i < EVENT_DATA_SIZE; i++) {
        e->data[i] = 0;
    }
}

static inline void response_init(Response* r, uint64_t event_id, EventStatus status) {
    r->event_id = event_id;
    r->status = status;
    r->error_code = 0;
    r->result_size = 0;
    r->timestamp = 0;

    // Очищаем результат
    for (int i = 0; i < RESPONSE_DATA_SIZE; i++) {
        r->result[i] = 0;
    }
}

// ============================================================================
// ROUTING HELPERS
// ============================================================================

static inline void routing_entry_init(RoutingEntry* entry, uint64_t event_id, Event* event) {
    entry->event_id = event_id;

    // КОПИРУЕМ событие (не сохраняем указатель!)
    entry->event_copy = *event;

    entry->current_index = 0;
    entry->completion_flags = 0;
    entry->state = EVENT_STATUS_PENDING;
    entry->created_at = 0;  // Будет установлен timestamp
    entry->abort_flag = 0;  // Нет ошибок
    entry->error_code = 0;

    // Очищаем префиксы и результаты
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        entry->prefixes[i] = DECK_PREFIX_NONE;
        entry->deck_results[i] = 0;
        entry->deck_timestamps[i] = 0;
    }
}

// Проверяет, завершена ли обработка события (все префиксы = 0)
static inline int routing_entry_is_complete(RoutingEntry* entry) {
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        if (entry->prefixes[i] != DECK_PREFIX_NONE) {
            return 0;  // Есть незавершённые decks
        }
    }
    return 1;  // Все decks завершены
}

// Получает следующий префикс для обработки
static inline uint8_t routing_entry_get_next_prefix(RoutingEntry* entry) {
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        if (entry->prefixes[i] != DECK_PREFIX_NONE) {
            return entry->prefixes[i];
        }
    }
    return DECK_PREFIX_NONE;
}

// Затирает префикс (вызывается deck после завершения обработки)
static inline void routing_entry_clear_prefix(RoutingEntry* entry, uint8_t prefix) {
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        if (entry->prefixes[i] == prefix) {
            entry->prefixes[i] = DECK_PREFIX_NONE;
            break;
        }
    }
}

#endif // EVENTS_H
