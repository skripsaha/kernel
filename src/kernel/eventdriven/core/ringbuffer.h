#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include "events.h"
#include "atomics.h"
#include "ktypes.h"

// ============================================================================
// RING BUFFER - Lock-free SPSC (Single Producer Single Consumer)
// ============================================================================

// Размер буфера - ДОЛЖЕН быть степенью 2 для быстрого % через &
// Уменьшено до 256 для экономии памяти (от 18MB до ~1.2MB)
#define RING_BUFFER_SIZE 256
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)

// Compile-time проверка
_Static_assert((RING_BUFFER_SIZE & (RING_BUFFER_SIZE - 1)) == 0,
               "RING_BUFFER_SIZE must be power of 2");

// ============================================================================
// EVENT RING BUFFER - Для передачи Event структур
// ============================================================================

typedef struct {
    // Cache-line aligned head/tail для избежания false sharing
    volatile uint64_t head __attribute__((aligned(64)));  // Consumer index
    volatile uint64_t tail __attribute__((aligned(64)));  // Producer index

    // Буфер событий
    Event events[RING_BUFFER_SIZE] __attribute__((aligned(64)));
} EventRingBuffer;

// ============================================================================
// RESPONSE RING BUFFER - Для передачи Response структур
// ============================================================================

typedef struct {
    volatile uint64_t head __attribute__((aligned(64)));
    volatile uint64_t tail __attribute__((aligned(64)));

    Response responses[RING_BUFFER_SIZE] __attribute__((aligned(64)));
} ResponseRingBuffer;

// ============================================================================
// EVENT RING BUFFER OPERATIONS
// ============================================================================

// Инициализация
static inline void event_ring_init(EventRingBuffer* ring) {
    atomic_store_u64(&ring->head, 0);
    atomic_store_u64(&ring->tail, 0);
}

// Получить количество доступных событий для чтения
static inline uint64_t event_ring_count(EventRingBuffer* ring) {
    uint64_t tail = atomic_load_u64(&ring->tail);
    uint64_t head = atomic_load_u64(&ring->head);
    return tail - head;
}

// Проверить, пуст ли буфер
static inline int event_ring_is_empty(EventRingBuffer* ring) {
    uint64_t tail = atomic_load_u64(&ring->tail);
    uint64_t head = atomic_load_u64(&ring->head);
    return head == tail;
}

// Проверить, полон ли буфер
static inline int event_ring_is_full(EventRingBuffer* ring) {
    uint64_t tail = atomic_load_u64(&ring->tail);
    uint64_t head = atomic_load_u64(&ring->head);
    return (tail - head) >= RING_BUFFER_SIZE;
}

// ============================================================================
// PRODUCER OPERATIONS (USER SPACE)
// ============================================================================

// Push event в ring buffer (возвращает 1 если успех, 0 если буфер полон)
static inline int event_ring_push(EventRingBuffer* ring, Event* event) {
    uint64_t current_tail = atomic_load_u64(&ring->tail);
    uint64_t current_head = atomic_load_u64(&ring->head);

    // Проверяем, есть ли место
    if ((current_tail - current_head) >= RING_BUFFER_SIZE) {
        return 0;  // Буфер полон
    }

    // Вычисляем индекс через битовую маску (быстрее чем %)
    uint64_t index = current_tail & RING_BUFFER_MASK;

    // Копируем событие в буфер
    Event* slot = &ring->events[index];

    // Копируем по 8 байт (оптимизация)
    uint64_t* src = (uint64_t*)event;
    uint64_t* dst = (uint64_t*)slot;
    for (int i = 0; i < sizeof(Event) / 8; i++) {
        dst[i] = src[i];
    }

    // Memory barrier для гарантии видимости записи
    COMPILER_BARRIER();

    // Атомарно увеличиваем tail (делаем событие видимым для consumer)
    atomic_store_u64(&ring->tail, current_tail + 1);

    return 1;  // Успех
}

// ============================================================================
// CONSUMER OPERATIONS (KERNEL SPACE)
// ============================================================================

// Pop event из ring buffer (возвращает 1 если успех, 0 если буфер пуст)
static inline int event_ring_pop(EventRingBuffer* ring, Event* out_event) {
    uint64_t current_head = atomic_load_u64(&ring->head);
    uint64_t current_tail = atomic_load_u64(&ring->tail);

    // Проверяем, есть ли данные
    if (current_head == current_tail) {
        return 0;  // Буфер пуст
    }

    // Вычисляем индекс
    uint64_t index = current_head & RING_BUFFER_MASK;

    // Копируем событие из буфера
    Event* slot = &ring->events[index];

    // Копируем по 8 байт
    uint64_t* src = (uint64_t*)slot;
    uint64_t* dst = (uint64_t*)out_event;
    for (int i = 0; i < sizeof(Event) / 8; i++) {
        dst[i] = src[i];
    }

    // Memory barrier
    COMPILER_BARRIER();

    // Атомарно увеличиваем head (освобождаем слот)
    atomic_store_u64(&ring->head, current_head + 1);

    return 1;  // Успех
}

// Peek event без удаления (для проверки)
static inline int event_ring_peek(EventRingBuffer* ring, Event* out_event) {
    uint64_t current_head = atomic_load_u64(&ring->head);
    uint64_t current_tail = atomic_load_u64(&ring->tail);

    if (current_head == current_tail) {
        return 0;
    }

    uint64_t index = current_head & RING_BUFFER_MASK;
    Event* slot = &ring->events[index];

    uint64_t* src = (uint64_t*)slot;
    uint64_t* dst = (uint64_t*)out_event;
    for (int i = 0; i < sizeof(Event) / 8; i++) {
        dst[i] = src[i];
    }

    return 1;
}

// ============================================================================
// RESPONSE RING BUFFER OPERATIONS (идентично, но для Response)
// ============================================================================

static inline void response_ring_init(ResponseRingBuffer* ring) {
    atomic_store_u64(&ring->head, 0);
    atomic_store_u64(&ring->tail, 0);
}

static inline int response_ring_is_empty(ResponseRingBuffer* ring) {
    uint64_t tail = atomic_load_u64(&ring->tail);
    uint64_t head = atomic_load_u64(&ring->head);
    return head == tail;
}

static inline int response_ring_is_full(ResponseRingBuffer* ring) {
    uint64_t tail = atomic_load_u64(&ring->tail);
    uint64_t head = atomic_load_u64(&ring->head);
    return (tail - head) >= RING_BUFFER_SIZE;
}

// KERNEL pushes responses
static inline int response_ring_push(ResponseRingBuffer* ring, Response* response) {
    uint64_t current_tail = atomic_load_u64(&ring->tail);
    uint64_t current_head = atomic_load_u64(&ring->head);

    if ((current_tail - current_head) >= RING_BUFFER_SIZE) {
        return 0;
    }

    uint64_t index = current_tail & RING_BUFFER_MASK;
    Response* slot = &ring->responses[index];

    // Копируем по 8 байт
    uint64_t* src = (uint64_t*)response;
    uint64_t* dst = (uint64_t*)slot;
    for (int i = 0; i < sizeof(Response) / 8; i++) {
        dst[i] = src[i];
    }

    COMPILER_BARRIER();
    atomic_store_u64(&ring->tail, current_tail + 1);

    return 1;
}

// USER pops responses
static inline int response_ring_pop(ResponseRingBuffer* ring, Response* out_response) {
    uint64_t current_head = atomic_load_u64(&ring->head);
    uint64_t current_tail = atomic_load_u64(&ring->tail);

    if (current_head == current_tail) {
        return 0;
    }

    uint64_t index = current_head & RING_BUFFER_MASK;
    Response* slot = &ring->responses[index];

    uint64_t* src = (uint64_t*)slot;
    uint64_t* dst = (uint64_t*)out_response;
    for (int i = 0; i < sizeof(Response) / 8; i++) {
        dst[i] = src[i];
    }

    COMPILER_BARRIER();
    atomic_store_u64(&ring->head, current_head + 1);

    return 1;
}

// ============================================================================
// BATCH OPERATIONS - Для высокой пропускной способности
// ============================================================================

// Batch push (возвращает количество успешно добавленных событий)
static inline int event_ring_push_batch(EventRingBuffer* ring, Event* events, int count) {
    int pushed = 0;
    for (int i = 0; i < count; i++) {
        if (!event_ring_push(ring, &events[i])) {
            break;
        }
        pushed++;
    }
    return pushed;
}

// Batch pop (возвращает количество успешно извлечённых событий)
static inline int event_ring_pop_batch(EventRingBuffer* ring, Event* events, int max_count) {
    int popped = 0;
    for (int i = 0; i < max_count; i++) {
        if (!event_ring_pop(ring, &events[i])) {
            break;
        }
        popped++;
    }
    return popped;
}

#endif // RINGBUFFER_H
