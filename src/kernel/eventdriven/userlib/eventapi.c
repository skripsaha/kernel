#include "eventapi.h"
#include "klib.h"

// ============================================================================
// GLOBAL STATE (user space)
// ============================================================================

static EventRingBuffer* to_kernel_ring = 0;
static ResponseRingBuffer* from_kernel_ring = 0;

// Локальный кэш ответов (простая реализация)
// Уменьшено для экономии памяти (Response = 4KB, 32 * 4KB = 128KB)
#define RESPONSE_CACHE_SIZE 32
static Response response_cache[RESPONSE_CACHE_SIZE];
static int response_cache_valid[RESPONSE_CACHE_SIZE];

// PID текущего процесса (для заполнения событий)
static uint64_t current_user_id = 1;  // TODO: получать реальный PID

// ============================================================================
// INITIALIZATION
// ============================================================================

void eventapi_init(EventRingBuffer* to_kernel, ResponseRingBuffer* from_kernel) {
    to_kernel_ring = to_kernel;
    from_kernel_ring = from_kernel;

    // Очищаем кэш ответов
    for (int i = 0; i < RESPONSE_CACHE_SIZE; i++) {
        response_cache_valid[i] = 0;
    }

    kprintf("[EVENTAPI] Initialized (user_id=%lu)\n", current_user_id);
}

// ============================================================================
// EVENT SUBMISSION
// ============================================================================

uint64_t eventapi_submit_event(Event* event) {
    if (!to_kernel_ring) {
        kprintf("[EVENTAPI] ERROR: Not initialized!\n");
        return 0;
    }

    // Заполняем метаданные
    event->id = 0;  // ВАЖНО! User НЕ устанавливает ID
    event->user_id = current_user_id;
    event->timestamp = 0;  // Kernel установит timestamp

    // Отправляем в ring buffer
    while (!event_ring_push(to_kernel_ring, event)) {
        // Busy-wait если буфер полон
        cpu_pause();
    }

    // NOTE: Мы НЕ ЗНАЕМ ID события! Kernel его установит.
    // В реальной системе нужен механизм получения ID обратно.
    // Сейчас для простоты возвращаем фиктивный ID.

    // TODO: правильная синхронизация для получения реального ID
    return 0;  // Placeholder
}

// ============================================================================
// MEMORY OPERATIONS
// ============================================================================

uint64_t eventapi_memory_alloc(uint64_t size) {
    Event event;
    event_init(&event, EVENT_MEMORY_ALLOC, current_user_id);

    // Payload: размер
    *(uint64_t*)event.data = size;

    return eventapi_submit_event(&event);
}

uint64_t eventapi_memory_free(void* addr) {
    Event event;
    event_init(&event, EVENT_MEMORY_FREE, current_user_id);

    // Payload: адрес
    *(void**)event.data = addr;

    return eventapi_submit_event(&event);
}

// ============================================================================
// FILE OPERATIONS
// ============================================================================

uint64_t eventapi_file_open(const char* path) {
    Event event;
    event_init(&event, EVENT_FILE_OPEN, current_user_id);

    // Payload: путь (копируем строку)
    int i = 0;
    while (path[i] && i < EVENT_DATA_SIZE - 1) {
        event.data[i] = path[i];
        i++;
    }
    event.data[i] = 0;  // null terminator

    return eventapi_submit_event(&event);
}

uint64_t eventapi_file_close(int fd) {
    Event event;
    event_init(&event, EVENT_FILE_CLOSE, current_user_id);

    // Payload: fd
    *(int*)event.data = fd;

    return eventapi_submit_event(&event);
}

uint64_t eventapi_file_read(int fd, uint64_t size) {
    Event event;
    event_init(&event, EVENT_FILE_READ, current_user_id);

    // Payload: [fd:4 bytes][size:8 bytes]
    *(int*)event.data = fd;
    *(uint64_t*)(event.data + 4) = size;

    return eventapi_submit_event(&event);
}

uint64_t eventapi_file_write(int fd, const void* data, uint64_t size) {
    Event event;
    event_init(&event, EVENT_FILE_WRITE, current_user_id);

    // Payload: [fd:4 bytes][size:8 bytes][data:...]
    *(int*)event.data = fd;
    *(uint64_t*)(event.data + 4) = size;

    // Копируем данные (ограничено размером payload)
    uint64_t copy_size = size;
    if (copy_size > EVENT_DATA_SIZE - 12) {
        copy_size = EVENT_DATA_SIZE - 12;
    }

    for (uint64_t i = 0; i < copy_size; i++) {
        event.data[12 + i] = ((uint8_t*)data)[i];
    }

    return eventapi_submit_event(&event);
}

// ============================================================================
// RESPONSE POLLING
// ============================================================================

Response* eventapi_poll_response(uint64_t event_id) {
    if (!from_kernel_ring) {
        return 0;
    }

    // Проверяем кэш
    int cache_index = event_id % RESPONSE_CACHE_SIZE;
    if (response_cache_valid[cache_index] &&
        response_cache[cache_index].event_id == event_id) {
        return &response_cache[cache_index];
    }

    // Проверяем ring buffer
    Response temp;
    while (response_ring_pop(from_kernel_ring, &temp)) {
        // Сохраняем в кэш
        int idx = temp.event_id % RESPONSE_CACHE_SIZE;
        response_cache[idx] = temp;
        response_cache_valid[idx] = 1;

        // Если это тот ответ, что мы ищем
        if (temp.event_id == event_id) {
            return &response_cache[idx];
        }
    }

    return 0;  // Ответ ещё не готов
}

Response* eventapi_wait_response(uint64_t event_id) {
    // Busy-wait (НЕ РЕКОМЕНДУЕТСЯ!)
    Response* resp;
    while (!(resp = eventapi_poll_response(event_id))) {
        cpu_pause();
    }
    return resp;
}

// ============================================================================
// HELPERS
// ============================================================================

int eventapi_pending_count(void) {
    // Подсчитываем valid entries в кэше
    int count = 0;
    for (int i = 0; i < RESPONSE_CACHE_SIZE; i++) {
        if (response_cache_valid[i]) {
            count++;
        }
    }
    return count;
}
