#include "deck_interface.h"
#include "klib.h"
#include "../task/task.h"  // NEW: Task system integration

// ============================================================================
// OPERATIONS DECK - Task & IPC Operations
// ============================================================================
//
// Now using the revolutionary Task system instead of heavy processes!
// Tasks are lightweight, energy-driven, and self-regulating.
// ============================================================================

// ============================================================================
// PROCESSING FUNCTION - Task Operations
// ============================================================================

int operations_deck_process(RoutingEntry* entry) {
    Event* event = &entry->event_copy;

    switch (event->type) {
        // === TASK OPERATIONS (Real implementation) ===
        case EVENT_PROC_CREATE: {
            // Payload: [name_len:4][name:...][entry_point:8][energy:1]
            uint32_t name_len = *(uint32_t*)event->data;
            const char* name = (const char*)(event->data + 4);
            void* entry_point = *(void**)(event->data + 4 + name_len);
            uint8_t energy = (event->data[4 + name_len + 8] != 0) ?
                              event->data[4 + name_len + 8] : 50;  // Default energy = 50

            // Create task using new Task system
            Task* task = task_spawn(name, entry_point, energy);

            if (task) {
                kprintf("[OPERATIONS] Event %lu: spawned task '%s' (ID=%lu, energy=%u)\n",
                        event->id, name, task->task_id, energy);

                // Return task ID as result
                uint64_t* result = (uint64_t*)kmalloc(sizeof(uint64_t));
                *result = task->task_id;
                deck_complete(entry, DECK_PREFIX_OPERATIONS, result);
                return 1;
            } else {
                kprintf("[OPERATIONS] ERROR: Event %lu: failed to spawn task '%s'\n",
                        event->id, name);
                deck_error(entry, DECK_PREFIX_OPERATIONS, 1);
                return 0;
            }
        }

        case EVENT_PROC_EXIT: {
            // Current task exits
            uint64_t exit_code = *(uint64_t*)event->data;
            uint64_t task_id = task_get_current_id();

            kprintf("[OPERATIONS] Event %lu: task %lu exiting with code %lu\n",
                    event->id, task_id, exit_code);

            if (task_id > 0) {
                task_kill(task_id);
            }

            deck_complete(entry, DECK_PREFIX_OPERATIONS, 0);
            return 1;
        }

        case EVENT_PROC_KILL: {
            // Kill specific task
            uint64_t task_id = *(uint64_t*)event->data;

            int ret = task_kill(task_id);
            if (ret == 0) {
                kprintf("[OPERATIONS] Event %lu: killed task %lu\n", event->id, task_id);
                deck_complete(entry, DECK_PREFIX_OPERATIONS, 0);
                return 1;
            } else {
                kprintf("[OPERATIONS] ERROR: Event %lu: failed to kill task %lu\n",
                        event->id, task_id);
                deck_error(entry, DECK_PREFIX_OPERATIONS, 2);
                return 0;
            }
        }

        case EVENT_PROC_WAIT: {
            // Sleep task for specified time
            // Payload: [task_id:8][milliseconds:8]
            uint64_t task_id = *(uint64_t*)event->data;
            uint64_t milliseconds = *(uint64_t*)(event->data + 8);

            int ret = task_sleep(task_id, milliseconds);
            if (ret == 0) {
                kprintf("[OPERATIONS] Event %lu: task %lu sleeping for %lu ms\n",
                        event->id, task_id, milliseconds);
                deck_complete(entry, DECK_PREFIX_OPERATIONS, 0);
                return 1;
            } else {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 3);
                return 0;
            }
        }

        case EVENT_PROC_GETPID: {
            // Get current task ID
            uint64_t task_id = task_get_current_id();

            uint64_t* result = (uint64_t*)kmalloc(sizeof(uint64_t));
            *result = task_id;

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result);
            kprintf("[OPERATIONS] Event %lu: get_task_id() = %lu\n", event->id, task_id);
            return 1;
        }

        case EVENT_PROC_SIGNAL: {
            // Task control: pause/resume/boost/throttle
            // Payload: [task_id:8][operation:4][value:4]
            uint64_t task_id = *(uint64_t*)event->data;
            uint32_t operation = *(uint32_t*)(event->data + 8);
            uint32_t value = *(uint32_t*)(event->data + 12);

            int ret = -1;
            switch (operation) {
                case 0:  // Pause
                    ret = task_pause(task_id);
                    kprintf("[OPERATIONS] Task %lu paused\n", task_id);
                    break;
                case 1:  // Resume
                    ret = task_resume(task_id);
                    kprintf("[OPERATIONS] Task %lu resumed\n", task_id);
                    break;
                case 2:  // Boost energy
                    ret = task_boost(task_id, (uint8_t)value);
                    kprintf("[OPERATIONS] Task %lu boosted by %u\n", task_id, value);
                    break;
                case 3:  // Throttle
                    ret = task_throttle(task_id, (uint8_t)value);
                    kprintf("[OPERATIONS] Task %lu throttled by %u\n", task_id, value);
                    break;
                case 4:  // Wake up
                    ret = task_wake(task_id);
                    kprintf("[OPERATIONS] Task %lu woken up\n", task_id);
                    break;
                default:
                    kprintf("[OPERATIONS] ERROR: Unknown task operation %u\n", operation);
                    break;
            }

            if (ret == 0) {
                deck_complete(entry, DECK_PREFIX_OPERATIONS, 0);
                return 1;
            } else {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 4);
                return 0;
            }
        }

        // === IPC OPERATIONS (TODO - будет в следующей версии) ===
        case EVENT_IPC_SEND:
        case EVENT_IPC_RECV:
        case EVENT_IPC_SHM_CREATE:
        case EVENT_IPC_SHM_ATTACH:
        case EVENT_IPC_PIPE_CREATE: {
            kprintf("[OPERATIONS] Event %lu: IPC operation (type=%d) - TODO\n",
                    event->id, event->type);
            deck_complete(entry, DECK_PREFIX_OPERATIONS, 0);
            return 1;
        }

        default:
            kprintf("[OPERATIONS] Unknown event type %d\n", event->type);
            deck_error(entry, DECK_PREFIX_OPERATIONS, 5);
            return 0;
    }
}

// ============================================================================
// INITIALIZATION & RUN
// ============================================================================

DeckContext operations_deck_context;

void operations_deck_init(void) {
    deck_init(&operations_deck_context, "Operations", DECK_PREFIX_OPERATIONS, operations_deck_process);
}

int operations_deck_run_once(void) {
    return deck_run_once(&operations_deck_context);
}

void operations_deck_run(void) {
    deck_run(&operations_deck_context);
}
