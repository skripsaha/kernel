#include "deck_interface.h"
#include "klib.h"
#include "../task/task.h"  // NEW: Task system integration

// ============================================================================
// HARDWARE DECK - Timer & Device Operations
// ============================================================================

// Timer descriptor
typedef struct {
    uint64_t id;
    uint64_t owner_task_id;      // Changed from owner_pid to owner_task_id
    uint64_t expiration;         // TSC timestamp
    uint64_t interval;           // 0 = one-shot, >0 = periodic
    uint64_t event_id;           // Для отправки уведомления
    int active;
} Timer;

#define MAX_TIMERS 64
static Timer timers[MAX_TIMERS];
static volatile uint64_t next_timer_id = 1;

// ============================================================================
// TIMER OPERATIONS (Integrated with Task system)
// ============================================================================

static Timer* timer_create(uint64_t delay_ms, uint64_t interval_ms) {
    // Находим свободный slot
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].id = atomic_increment_u64(&next_timer_id);

            // Get current task ID
            timers[i].owner_task_id = task_get_current_id();

            timers[i].expiration = rdtsc() + (delay_ms * 2400000);  // Примерная конверсия ms->TSC
            timers[i].interval = interval_ms * 2400000;
            timers[i].active = 1;

            kprintf("[HARDWARE] Created timer %lu for task %lu: delay=%lu ms, interval=%lu ms\n",
                    timers[i].id, timers[i].owner_task_id, delay_ms, interval_ms);

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
    // Real implementation using Task system!
    uint64_t task_id = task_get_current_id();

    if (task_id > 0) {
        task_sleep(task_id, ms);
        kprintf("[HARDWARE] Task %lu sleeping for %lu ms\n", task_id, ms);
    } else {
        kprintf("[HARDWARE] WARNING: No current task to sleep\n");
    }
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

            // Wake up the owner task!
            if (timers[i].owner_task_id > 0) {
                task_wake(timers[i].owner_task_id);
                kprintf("[HARDWARE] Woke up task %lu\n", timers[i].owner_task_id);
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

            Timer* timer = timer_create(delay_ms, interval_ms);

            if (timer) {
                deck_complete(entry, DECK_PREFIX_HARDWARE, timer);
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
                deck_complete(entry, DECK_PREFIX_HARDWARE, 0);
            } else {
                deck_error(entry, DECK_PREFIX_HARDWARE, 2);
            }
            kprintf("[HARDWARE] Event %lu: cancelled timer %lu (status=%d)\n",
                    event->id, timer_id, success);
            return success;
        }

        case EVENT_TIMER_SLEEP: {
            uint64_t ms = *(uint64_t*)event->data;
            timer_sleep(ms);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0);
            kprintf("[HARDWARE] Event %lu: sleep %lu ms\n", event->id, ms);
            return 1;
        }

        case EVENT_TIMER_GETTICKS: {
            uint64_t ticks = timer_get_ticks();
            deck_complete(entry, DECK_PREFIX_HARDWARE, (void*)ticks);
            kprintf("[HARDWARE] Event %lu: getticks = %lu\n", event->id, ticks);
            return 1;
        }

        // === DEVICE OPERATIONS (STUBS) ===
        case EVENT_DEV_OPEN: {
            const char* name = (const char*)event->data;
            int device_id = device_open(name);
            deck_complete(entry, DECK_PREFIX_HARDWARE, (void*)(uint64_t)device_id);
            kprintf("[HARDWARE] Event %lu: device open '%s'\n", event->id, name);
            return 1;
        }

        case EVENT_DEV_IOCTL: {
            // Payload: [device_id:4][command:8][arg:...]
            int device_id = *(int*)event->data;
            uint64_t command = *(uint64_t*)(event->data + 4);
            void* arg = event->data + 12;
            device_ioctl(device_id, command, arg);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0);
            kprintf("[HARDWARE] Event %lu: device ioctl\n", event->id);
            return 1;
        }

        case EVENT_DEV_READ: {
            // Payload: [device_id:4][size:8]
            int device_id = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            device_read(device_id, 0, size);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0);
            kprintf("[HARDWARE] Event %lu: device read\n", event->id);
            return 1;
        }

        case EVENT_DEV_WRITE: {
            // Payload: [device_id:4][size:8][data:...]
            int device_id = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            void* data = event->data + 12;
            device_write(device_id, data, size);
            deck_complete(entry, DECK_PREFIX_HARDWARE, 0);
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
