#include "deck_interface.h"
#include "klib.h"

// ============================================================================
// HARDWARE DECK - Timer & Device Operations
// ============================================================================

// Timer descriptor
typedef struct {
    uint64_t id;
    uint64_t owner_workflow_id;  // Workflow that owns this timer
    uint64_t expiration;         // TSC timestamp
    uint64_t interval;           // 0 = one-shot, >0 = periodic
    uint64_t event_id;           // Event to trigger on expiration
    RoutingEntry* suspended_entry; // Suspended RoutingEntry waiting for this timer
    int active;
} Timer;

#define MAX_TIMERS 64
static Timer timers[MAX_TIMERS];
static volatile uint64_t next_timer_id = 1;

// ============================================================================
// TIMER OPERATIONS (Integrated with Task system)
// ============================================================================

static Timer* timer_create(uint64_t delay_ms, uint64_t interval_ms, RoutingEntry* entry) {
    // Находим свободный slot
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].id = atomic_increment_u64(&next_timer_id);

            // Get workflow ID from entry (user_id field stores workflow_id)
            timers[i].owner_workflow_id = entry ? entry->event_copy.user_id : 0;

            timers[i].expiration = rdtsc() + (delay_ms * 2400000);  // Примерная конверсия ms->TSC
            timers[i].interval = interval_ms * 2400000;
            timers[i].suspended_entry = entry;  // Link to suspended RoutingEntry
            timers[i].active = 1;

            kprintf("[HARDWARE] Created timer %lu: delay=%lu ms, interval=%lu ms (entry=%p)\n",
                    timers[i].id, delay_ms, interval_ms, (void*)entry);

            return &timers[i];
        }
    }

    kprintf("[HARDWARE] ERROR: No free timer slots!\n");
    return 0;
}

static int timer_cancel(uint64_t timer_id) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && timers[i].id == timer_id) {
            timers[i].active = 0;
            kprintf("[HARDWARE] Cancelled timer %lu\n", timer_id);
            return 1;
        }
    }
    return 0;  // Not found
}

static void timer_sleep(uint64_t ms) {
    // TODO: Workflow sleep (будет реализовано с Workflow Engine)
    // For now, just busy wait (not production-ready)
    uint64_t start = rdtsc();
    uint64_t cycles = ms * 2400000;  // Approximate
    while (rdtsc() - start < cycles) {
        asm volatile("pause");
    }
    kprintf("[HARDWARE] Slept for %lu ms (busy wait)\n", ms);
}

static uint64_t timer_get_ticks(void) {
    // Возвращает текущий TSC
    return rdtsc();
}

// Проверка истёкших таймеров (вызывается периодически)
static void timer_check_expired(void) {
    uint64_t now = rdtsc();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active && now >= timers[i].expiration) {
            kprintf("[HARDWARE] Timer %lu expired!\n", timers[i].id);

            // Wake up suspended workflow entry
            if (timers[i].suspended_entry) {
                RoutingEntry* entry = timers[i].suspended_entry;

                // Complete the suspended event (no result for sleep)
                deck_complete(entry, DECK_PREFIX_HARDWARE, 0, RESULT_TYPE_NONE);

                // Change state from SUSPENDED back to PROCESSING
                entry->state = EVENT_STATUS_PROCESSING;

                kprintf("[HARDWARE] Woke up suspended entry (event_id=%lu)\n", entry->event_id);

                // Clear the suspended_entry link
                timers[i].suspended_entry = 0;
            }

            // Если periodic - перезапускаем
            if (timers[i].interval > 0) {
                timers[i].expiration = now + timers[i].interval;
            } else {
                timers[i].active = 0;  // One-shot
            }
        }
    }
}

// ============================================================================
// DEVICE OPERATIONS (STUBS - для v1)
// ============================================================================

static int device_open(const char* name) {
    kprintf("[HARDWARE] Device open '%s' - STUB\n", name);
    return 100;  // Fake device handle
}

static int device_ioctl(int device_id, uint64_t command, void* arg) {
    kprintf("[HARDWARE] Device ioctl on device %d, cmd=%lu - STUB\n", device_id, command);
    return 0;
}

static int device_read(int device_id, void* buffer, uint64_t size) {
    kprintf("[HARDWARE] Device read from device %d, size=%lu - STUB\n", device_id, size);
    return size;
}

static int device_write(int device_id, const void* buffer, uint64_t size) {
    kprintf("[HARDWARE] Device write to device %d, size=%lu - STUB\n", device_id, size);
    return size;
}

// ============================================================================
// PROCESSING FUNCTION
// ============================================================================

int hardware_deck_process(RoutingEntry* entry) {
    Event* event = &entry->event_copy;

    switch (event->type) {
        // === TIMER OPERATIONS ===
        case EVENT_TIMER_CREATE: {
            // Payload: [delay_ms:8][interval_ms:8]
            uint64_t delay_ms = *(uint64_t*)event->data;
            uint64_t interval_ms = *(uint64_t*)(event->data + 8);

            Timer* timer = timer_create(delay_ms, interval_ms, NULL);  // NULL = not suspended

            if (timer) {
                deck_complete(entry, DECK_PREFIX_HARDWARE, timer, RESULT_TYPE_STATIC);
                kprintf("[HARDWARE] Event %lu: created timer %lu\n",
                        event->id, timer->id);
                return 1;
            }
            deck_error(entry, DECK_PREFIX_HARDWARE, 1);
            return 0;
        }

        case EVENT_TIMER_CANCEL: {
            uint64_t timer_id = *(uint64_t*)event->data;
            int success = timer_cancel(timer_id);
            if (success) {
                deck_complete(entry, DECK_PREFIX_HARDWARE, 0, RESULT_TYPE_NONE);
            } else {
                deck_error(entry, DECK_PREFIX_HARDWARE, 2);
            }
            kprintf("[HARDWARE] Event %lu: cancelled timer %lu (status=%d)\n",
                    event->id, timer_id, success);
            return success;
        }

        case EVENT_TIMER_SLEEP: {
            uint64_t ms = *(uint64_t*)event->data;

            // Create one-shot timer linked to this entry (workflow suspension)
            Timer* timer = timer_create(ms, 0, entry);  // 0 interval = one-shot

            if (timer) {
                // Mark entry as SUSPENDED - it will be completed when timer expires
                entry->state = EVENT_STATUS_SUSPENDED;

                kprintf("[HARDWARE] Event %lu: suspended for %lu ms (timer %lu)\n",
                        event->id, ms, timer->id);

                // Do NOT call deck_complete() - timer_check_expired() will do it
                return 1;
            }

            // Failed to create timer - fall back to error
            deck_error(entry, DECK_PREFIX_HARDWARE, 1);
            return 0;
        }

        case EVENT_TIMER_GETTICKS: {
            uint64_t ticks = timer_get_ticks();
            deck_complete(entry, DECK_PREFIX_HARDWARE, (void*)ticks, RESULT_TYPE_VALUE);
            kprintf("[HARDWARE] Event %lu: getticks = %lu\n", event->id, ticks);
            return 1;
        }

        // === DEVICE OPERATIONS (STUBS) ===
        case EVENT_DEV_OPEN: {
            const char* name = (const char*)event->data;
            int device_id = device_open(name);
            deck_complete(entry, DECK_PREFIX_HARDWARE, (void*)(uint64_t)device_id, RESULT_TYPE_VALUE);
            kprintf("[HARDWARE] Event %lu: device open '%s'\n", event->id, name);
            return 1;
        }

        case EVENT_DEV_IOCTL: {
            // Payload: [device_id:4][command:8][arg:...]
            int device_id = *(int*)event->data;
            uint64_t command = *(uint64_t*)(event->data + 4);
            void* arg = event->data + 12;
            device_ioctl(device_id, command, arg);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0, RESULT_TYPE_NONE);
            kprintf("[HARDWARE] Event %lu: device ioctl\n", event->id);
            return 1;
        }

        case EVENT_DEV_READ: {
            // Payload: [device_id:4][size:8]
            int device_id = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            device_read(device_id, 0, size);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0, RESULT_TYPE_NONE);
            kprintf("[HARDWARE] Event %lu: device read\n", event->id);
            return 1;
        }

        case EVENT_DEV_WRITE: {
            // Payload: [device_id:4][size:8][data:...]
            int device_id = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            void* data = event->data + 12;
            device_write(device_id, data, size);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0, RESULT_TYPE_NONE);
            kprintf("[HARDWARE] Event %lu: device write\n", event->id);
            return 1;
        }

        default:
            kprintf("[HARDWARE] Unknown event type %d\n", event->type);
            deck_error(entry, DECK_PREFIX_HARDWARE, 3);
            return 0;
    }
}

// ============================================================================
// INITIALIZATION & RUN
// ============================================================================

DeckContext hardware_deck_context;

void hardware_deck_init(void) {
    // Очищаем все таймеры
    for (int i = 0; i < MAX_TIMERS; i++) {
        timers[i].active = 0;
    }

    deck_init(&hardware_deck_context, "Hardware", DECK_PREFIX_HARDWARE, hardware_deck_process);
}

int hardware_deck_run_once(void) {
    // Проверяем истёкшие таймеры
    timer_check_expired();

    // Обрабатываем события
    return deck_run_once(&hardware_deck_context);
}

void hardware_deck_run(void) {
    deck_run(&hardware_deck_context);
}
