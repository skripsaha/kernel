#ifndef WORKFLOW_RINGS_H
#define WORKFLOW_RINGS_H

#include "ktypes.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

// ============================================================================
// WORKFLOW RING BUFFERS - Shared Memory Communication
// ============================================================================
//
// Zero-copy asynchronous communication between User Space and Kernel:
//   User → EventRing → kernel_notify(SUBMIT) → Guide → Decks → ResultRing → User
//
// Similar to io_uring but with workflow semantics:
//   - Events have routes through Decks
//   - Kernel understands dependencies (DAG)
//   - Automatic parallelization and optimization
//
// ============================================================================

// Ring buffer sizes (power of 2 for efficient modulo)
#define EVENT_RING_SIZE    256
#define RESULT_RING_SIZE   256
#define MAX_ROUTING_STEPS  8
#define EVENT_PAYLOAD_SIZE 512

// ============================================================================
// RING_EVENT - User submits to Kernel via EventRing
// ============================================================================
// Size: 576 bytes (9 cache lines)
typedef struct {
    // Identity (kernel fills id and timestamp)
    uint64_t id;              // 0 при submit, kernel assigns
    uint64_t workflow_id;     // Which workflow this event belongs to
    uint32_t type;            // EVENT_TIMER_CREATE, EVENT_FILE_READ, etc.
    uint64_t timestamp;       // rdtsc() когда kernel принял event

    // Routing through Decks
    // Example: [1, 0, 0, 0] = Operations Deck → Execution Deck
    // Example: [3, 1, 2, 0] = Storage → Operations → Hardware → Execution
    uint8_t route[MAX_ROUTING_STEPS];

    // Payload (user data)
    uint8_t payload[EVENT_PAYLOAD_SIZE];
    uint32_t payload_size;

    // Padding to 576 bytes (9 * 64)
    uint8_t _padding[20];
} RingEvent __attribute__((aligned(64)));

// ============================================================================
// RING_RESULT - Kernel returns to User via ResultRing
// ============================================================================
// Size: 576 bytes (9 cache lines)
typedef struct {
    // Identity
    uint64_t event_id;        // ID of completed event
    uint64_t workflow_id;     // Workflow ID
    uint64_t completion_time; // rdtsc() when completed
    uint32_t status;          // 0=success, -errno=error
    uint32_t error_code;      // Detailed error code if status != 0
    uint32_t result_size;     // Size of result data

    // Result data
    uint8_t result[EVENT_PAYLOAD_SIZE];

    // Padding to 576 bytes (9 * 64)
    uint8_t _padding[28];
} RingResult __attribute__((aligned(64)));

// ============================================================================
// EVENT_RING - User → Kernel (submission queue)
// ============================================================================
typedef struct {
    // Synchronization (cache-aligned to avoid false sharing)
    volatile uint64_t head __attribute__((aligned(64)));  // Kernel reads
    volatile uint64_t tail __attribute__((aligned(64)));  // User writes

    // Events array (RingEvent itself is already aligned(64))
    RingEvent events[EVENT_RING_SIZE];
} EventRing;

// ============================================================================
// RESULT_RING - Kernel → User (completion queue)
// ============================================================================
typedef struct {
    // Synchronization (cache-aligned)
    volatile uint64_t head __attribute__((aligned(64)));  // User reads
    volatile uint64_t tail __attribute__((aligned(64)));  // Kernel writes

    // Results array (RingResult itself is already aligned(64))
    RingResult results[RESULT_RING_SIZE];
} ResultRing;

// ============================================================================
// RING BUFFER OPERATIONS - Lock-free SPSC queues
// ============================================================================

// Event Ring: User pushes, Kernel pops

#ifndef WORKFLOW_RING_FUNCTIONS_DEFINED
#define WORKFLOW_RING_FUNCTIONS_DEFINED

static inline int wf_event_ring_push(EventRing* ring, RingEvent* event) {
    uint64_t tail = ring->tail;
    uint64_t head = ring->head;

    if (tail - head >= EVENT_RING_SIZE) {
        return 0;  // Full
    }

    uint64_t idx = tail % EVENT_RING_SIZE;
    ring->events[idx] = *event;

    __sync_synchronize();  // Memory barrier
    ring->tail = tail + 1;

    return 1;  // Success
}

static inline RingEvent* wf_event_ring_pop(EventRing* ring) {
    uint64_t head = ring->head;
    uint64_t tail = ring->tail;

    if (head == tail) {
        return NULL;  // Empty
    }

    uint64_t idx = head % EVENT_RING_SIZE;
    RingEvent* event = &ring->events[idx];

    __sync_synchronize();  // Memory barrier
    ring->head = head + 1;

    return event;
}

static inline int wf_event_ring_is_empty(EventRing* ring) {
    return ring->head == ring->tail;
}

static inline int wf_event_ring_is_full(EventRing* ring) {
    return (ring->tail - ring->head) >= EVENT_RING_SIZE;
}

// Result Ring: Kernel pushes, User pops

static inline int wf_result_ring_push(ResultRing* ring, RingResult* result) {
    uint64_t tail = ring->tail;
    uint64_t head = ring->head;

    if (tail - head >= RESULT_RING_SIZE) {
        return 0;  // Full
    }

    uint64_t idx = tail % RESULT_RING_SIZE;
    ring->results[idx] = *result;

    __sync_synchronize();  // Memory barrier
    ring->tail = tail + 1;

    return 1;  // Success
}

static inline RingResult* wf_result_ring_pop(ResultRing* ring) {
    uint64_t head = ring->head;
    uint64_t tail = ring->tail;

    if (head == tail) {
        return NULL;  // Empty
    }

    uint64_t idx = head % RESULT_RING_SIZE;
    RingResult* result = &ring->results[idx];

    __sync_synchronize();  // Memory barrier
    ring->head = head + 1;

    return result;
}

static inline int wf_result_ring_is_empty(ResultRing* ring) {
    return ring->head == ring->tail;
}

static inline int wf_result_ring_is_full(ResultRing* ring) {
    return (ring->tail - ring->head) >= RESULT_RING_SIZE;
}

#endif // WORKFLOW_RING_FUNCTIONS_DEFINED

#endif // WORKFLOW_RINGS_H
