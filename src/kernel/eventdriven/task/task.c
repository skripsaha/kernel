#include "task.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "cpu.h"

// ============================================================================
// GLOBAL STATE
// ============================================================================

#define MAX_TASKS 1024
#define MAX_TASK_GROUPS 64

// Task table
static Task* task_table[MAX_TASKS];
static spinlock_t task_table_lock;

// Task groups
static TaskGroup task_groups[MAX_TASK_GROUPS];
static spinlock_t task_groups_lock;

// Current task (per-CPU, for now we only support single CPU)
static Task* current_task = NULL;

// ID counters
static volatile uint64_t next_task_id = 1;
static volatile uint64_t next_group_id = 1;

// Scheduler queue (simple round-robin for now)
static Task* scheduler_head = NULL;
static Task* scheduler_tail = NULL;
static spinlock_t scheduler_lock;

// Statistics
static uint64_t tasks_created = 0;
static uint64_t tasks_destroyed = 0;
static uint64_t context_switches = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

void task_system_init(void) {
    // Clear task table
    memset(task_table, 0, sizeof(task_table));
    memset(task_groups, 0, sizeof(task_groups));

    // Initialize locks
    spinlock_init(&task_table_lock);
    spinlock_init(&task_groups_lock);
    spinlock_init(&scheduler_lock);

    // Reset counters
    next_task_id = 1;
    next_group_id = 1;
    tasks_created = 0;
    tasks_destroyed = 0;
    context_switches = 0;

    kprintf("[TASK] Task system initialized (max %d tasks)\n", MAX_TASKS);
}

// ============================================================================
// TASK TABLE MANAGEMENT
// ============================================================================

static int task_table_insert(Task* task) {
    spin_lock(&task_table_lock);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i] == NULL) {
            task_table[i] = task;
            spin_unlock(&task_table_lock);
            return i;
        }
    }

    spin_unlock(&task_table_lock);
    return -1;  // Table full
}

static void task_table_remove(uint64_t task_id) {
    spin_lock(&task_table_lock);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i] && task_table[i]->task_id == task_id) {
            task_table[i] = NULL;
            break;
        }
    }

    spin_unlock(&task_table_lock);
}

Task* task_get(uint64_t task_id) {
    spin_lock(&task_table_lock);

    Task* result = NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i] && task_table[i]->task_id == task_id) {
            result = task_table[i];
            break;
        }
    }

    spin_unlock(&task_table_lock);
    return result;
}

// ============================================================================
// SCHEDULER QUEUE MANAGEMENT
// ============================================================================

static void scheduler_enqueue(Task* task) {
    spin_lock(&scheduler_lock);

    task->next = NULL;
    task->prev = scheduler_tail;

    if (scheduler_tail) {
        scheduler_tail->next = task;
    } else {
        scheduler_head = task;
    }
    scheduler_tail = task;

    spin_unlock(&scheduler_lock);
}

static void scheduler_dequeue(Task* task) {
    spin_lock(&scheduler_lock);

    if (task->prev) {
        task->prev->next = task->next;
    } else {
        scheduler_head = task->next;
    }

    if (task->next) {
        task->next->prev = task->prev;
    } else {
        scheduler_tail = task->prev;
    }

    task->next = NULL;
    task->prev = NULL;

    spin_unlock(&scheduler_lock);
}

// ============================================================================
// TASK CREATION
// ============================================================================

Task* task_spawn(const char* name, void* entry_point, uint8_t energy) {
    return task_spawn_with_args(name, entry_point, NULL, energy);
}

Task* task_spawn_with_args(const char* name, void* entry_point, void* args, uint8_t energy) {
    // Allocate task structure
    Task* task = (Task*)kmalloc(sizeof(Task));
    if (!task) {
        kprintf("[TASK] ERROR: Failed to allocate task structure\n");
        return NULL;
    }

    // Clear structure
    memset(task, 0, sizeof(Task));

    // === IDENTITY ===
    task->task_id = atomic_increment_u64(&next_task_id);
    strncpy(task->name, name, TASK_NAME_MAX - 1);
    task->name[TASK_NAME_MAX - 1] = '\0';
    task->parent_id = current_task ? current_task->task_id : 0;
    task->group_id = 0;

    // === ENERGY ===
    task->energy_requested = energy;
    task->energy_allocated = energy;  // Initially grant full request
    task->energy_efficiency = 50;     // Start with neutral efficiency

    // === STATE ===
    task->state = TASK_STATE_RUNNING;
    task->health.responsiveness = 100;
    task->health.efficiency = 50;
    task->health.stability = 100;
    task->health.progress = 100;
    task->health.overall_health = 87;  // (100+50+100+100)/4 ≈ 87

    // === TIMING ===
    task->creation_time = rdtsc();
    task->last_run_time = task->creation_time;
    task->total_runtime = 0;
    task->sleep_until = 0;

    // === MEMORY ===
    // Allocate stack
    task->stack_base = vmalloc(TASK_STACK_SIZE);
    if (!task->stack_base) {
        kprintf("[TASK] ERROR: Failed to allocate stack for task '%s'\n", name);
        kfree(task);
        return NULL;
    }
    task->stack_size = TASK_STACK_SIZE;
    task->entry_point = entry_point;
    task->args = args;  // Save arguments

    // Use kernel page table for all tasks (no isolation in v1)
    // Future: Create separate page table per task for memory isolation
    vmm_context_t* kernel_ctx = vmm_get_kernel_context();
    task->page_table = kernel_ctx->pml4_phys;

    // === CPU CONTEXT ===
    // Initialize context for first run using assembly helper
    void* stack_top = (void*)((uint64_t)task->stack_base + TASK_STACK_SIZE - 16);
    task_init_context(&task->context, entry_point, stack_top, args);

    // === STATISTICS ===
    task->events_processed = 0;
    task->errors_count = 0;
    task->io_operations = 0;
    task->last_progress_time = task->creation_time;

    // === COMMUNICATION ===
    // Allocate message queue
    task->message_queue = (TaskMessageQueue*)kmalloc(sizeof(TaskMessageQueue));
    if (!task->message_queue) {
        kprintf("[TASK] WARNING: Failed to allocate message queue for task '%s'\n", name);
        vfree(task->stack_base);
        kfree(task);
        return NULL;
    }
    memset(task->message_queue, 0, sizeof(TaskMessageQueue));
    task->pending_messages = 0;

    // Add to task table
    int slot = task_table_insert(task);
    if (slot < 0) {
        kprintf("[TASK] ERROR: Task table full, cannot spawn '%s'\n", name);
        vfree(task->stack_base);
        kfree(task);
        return NULL;
    }

    // Add to scheduler queue
    scheduler_enqueue(task);

    // Update statistics
    atomic_increment_u64(&tasks_created);

    kprintf("[TASK] Spawned task '%s' (ID=%lu, energy=%u, stack=%p)\n",
            name, task->task_id, energy, task->stack_base);

    return task;
}

// ============================================================================
// TASK DESTRUCTION
// ============================================================================

int task_kill(uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) {
        kprintf("[TASK] ERROR: Cannot kill task %lu: not found\n", task_id);
        return -1;
    }

    // Remove from scheduler
    scheduler_dequeue(task);

    // Mark as dead
    task->state = TASK_STATE_DEAD;

    // Free resources
    if (task->stack_base) {
        vfree(task->stack_base);
    }

    if (task->context.fpu_state) {
        kfree(task->context.fpu_state);
    }

    if (task->message_queue) {
        kfree(task->message_queue);
    }

    // Remove from task table
    task_table_remove(task_id);

    // Free task structure
    kfree(task);

    atomic_increment_u64(&tasks_destroyed);

    kprintf("[TASK] Killed task %lu\n", task_id);
    return 0;
}

// ============================================================================
// TASK CONTROL
// ============================================================================

int task_sleep(uint64_t task_id, uint64_t milliseconds) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    // Calculate wake-up time (approximate: 2.4GHz CPU = 2.4M cycles/ms)
    uint64_t cycles = milliseconds * 2400000;
    task->sleep_until = rdtsc() + cycles;
    task->state = TASK_STATE_SLEEPING;

    // Remove from scheduler (will be re-added when woken)
    scheduler_dequeue(task);

    kprintf("[TASK] Task %lu '%s' sleeping for %lu ms\n",
            task_id, task->name, milliseconds);
    return 0;
}

int task_wake(uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    if (task->state == TASK_STATE_SLEEPING ||
        task->state == TASK_STATE_HIBERNATING ||
        task->state == TASK_STATE_DROWSY) {
        task->state = TASK_STATE_RUNNING;
        task->sleep_until = 0;

        // Re-add to scheduler
        scheduler_enqueue(task);

        kprintf("[TASK] Task %lu '%s' woken up\n", task_id, task->name);
        return 0;
    }

    return -1;  // Not in a sleepable state
}

int task_pause(uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    task->state = TASK_STATE_THROTTLED;
    scheduler_dequeue(task);

    kprintf("[TASK] Task %lu '%s' paused\n", task_id, task->name);
    return 0;
}

int task_resume(uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    if (task->state == TASK_STATE_THROTTLED) {
        task->state = TASK_STATE_RUNNING;
        scheduler_enqueue(task);

        kprintf("[TASK] Task %lu '%s' resumed\n", task_id, task->name);
        return 0;
    }

    return -1;
}

// ============================================================================
// ENERGY MANAGEMENT
// ============================================================================

int task_boost(uint64_t task_id, uint8_t extra_energy) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    task->energy_allocated = (task->energy_allocated + extra_energy > 100) ?
                              100 : task->energy_allocated + extra_energy;

    kprintf("[TASK] Task %lu boosted to energy=%u\n", task_id, task->energy_allocated);
    return 0;
}

int task_throttle(uint64_t task_id, uint8_t reduction) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    task->energy_allocated = (task->energy_allocated < reduction) ?
                              0 : task->energy_allocated - reduction;
    task->state = TASK_STATE_THROTTLED;

    kprintf("[TASK] Task %lu throttled to energy=%u\n", task_id, task->energy_allocated);
    return 0;
}

int task_set_energy(uint64_t task_id, uint8_t energy) {
    Task* task = task_get(task_id);
    if (!task) {
        return -1;
    }

    task->energy_requested = energy;
    // System will adjust energy_allocated based on efficiency later
    task->energy_allocated = energy;

    return 0;
}

// ============================================================================
// TASK QUERY
// ============================================================================

Task* task_get_current(void) {
    return current_task;
}

uint64_t task_get_current_id(void) {
    return current_task ? current_task->task_id : 0;
}

TaskState task_get_state(uint64_t task_id) {
    Task* task = task_get(task_id);
    return task ? task->state : TASK_STATE_DEAD;
}

int task_enumerate(Task** tasks_out, uint32_t max_tasks) {
    if (!tasks_out || max_tasks == 0) {
        return 0;
    }

    spin_lock(&task_table_lock);

    uint32_t count = 0;
    for (int i = 0; i < MAX_TASKS && count < max_tasks; i++) {
        if (task_table[i] != NULL) {
            tasks_out[count++] = task_table[i];
        }
    }

    spin_unlock(&task_table_lock);
    return count;
}

// ============================================================================
// HEALTH & MONITORING
// ============================================================================

void task_update_health(Task* task) {
    if (!task) return;

    uint64_t now = rdtsc();

    // Update responsiveness (based on how recently it ran)
    uint64_t time_since_run = now - task->last_run_time;
    if (time_since_run < 10000000) {  // Less than ~4ms
        task->health.responsiveness = 100;
    } else if (time_since_run < 100000000) {  // Less than ~40ms
        task->health.responsiveness = 70;
    } else {
        task->health.responsiveness = 30;
    }

    // Update efficiency (based on energy usage vs progress)
    if (task->events_processed > 0) {
        task->health.efficiency = (task->energy_efficiency + task->health.efficiency) / 2;
    }

    // Update stability (based on error rate)
    if (task->events_processed > 0) {
        uint64_t error_rate = (task->errors_count * 100) / task->events_processed;
        task->health.stability = (error_rate < 100) ? 100 - error_rate : 0;
    }

    // Update progress (based on time since last progress)
    uint64_t time_since_progress = now - task->last_progress_time;
    if (time_since_progress < 50000000) {  // Less than ~20ms
        task->health.progress = 100;
    } else if (time_since_progress < 500000000) {  // Less than ~200ms
        task->health.progress = 50;
    } else {
        task->health.progress = 10;
        task->state = TASK_STATE_STALLED;
    }

    // Compute overall health
    task->health.overall_health = (
        task->health.responsiveness +
        task->health.efficiency +
        task->health.stability +
        task->health.progress
    ) / 4;
}

int task_auto_recover(Task* task) {
    if (!task) return -1;

    task_update_health(task);

    // If health is low, try to recover
    if (task->health.overall_health < 30) {
        kprintf("[TASK] WARNING: Task %lu '%s' health=%u, attempting recovery\n",
                task->task_id, task->name, task->health.overall_health);

        // Strategy 1: If stalled, boost energy
        if (task->state == TASK_STATE_STALLED) {
            task_boost(task->task_id, 20);
            task->state = TASK_STATE_RUNNING;
            return 0;
        }

        // Strategy 2: If low stability, log and consider restart
        if (task->health.stability < 30) {
            kprintf("[TASK] WARNING: Task %lu has low stability (many errors)\n", task->task_id);
            // TODO: Could restart task here
            return -1;
        }
    }

    return 0;
}

void task_print_stats(uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) {
        kprintf("[TASK] Task %lu not found\n", task_id);
        return;
    }

    task_update_health(task);

    kprintf("\n========== TASK STATS ==========\n");
    kprintf("ID:         %lu\n", task->task_id);
    kprintf("Name:       %s\n", task->name);
    kprintf("State:      %u\n", task->state);
    kprintf("Energy:     %u (requested %u)\n", task->energy_allocated, task->energy_requested);
    kprintf("Health:     %u (R:%u E:%u S:%u P:%u)\n",
            task->health.overall_health,
            task->health.responsiveness,
            task->health.efficiency,
            task->health.stability,
            task->health.progress);
    kprintf("Events:     %lu processed, %lu errors\n",
            task->events_processed, task->errors_count);
    kprintf("Runtime:    %lu cycles\n", task->total_runtime);
    kprintf("================================\n\n");
}

// ============================================================================
// SCHEDULER
// ============================================================================

Task* task_scheduler_next(void) {
    spin_lock(&scheduler_lock);

    // Simple round-robin: take head, move to tail
    Task* task = scheduler_head;

    if (task) {
        // Check if task should wake up from sleep
        if (task->state == TASK_STATE_SLEEPING) {
            uint64_t now = rdtsc();
            if (now >= task->sleep_until) {
                task->state = TASK_STATE_RUNNING;
                task->sleep_until = 0;
            } else {
                // Still sleeping, skip to next
                task = task->next;
            }
        }

        // Move head to tail for round-robin
        if (task && task->state == TASK_STATE_RUNNING) {
            scheduler_head = task->next;
            if (scheduler_head) {
                scheduler_head->prev = NULL;
            }

            task->next = NULL;
            task->prev = scheduler_tail;
            if (scheduler_tail) {
                scheduler_tail->next = task;
            }
            scheduler_tail = task;

            current_task = task;
            context_switches++;
        }
    }

    spin_unlock(&scheduler_lock);
    return task;
}

void task_scheduler_yield(void) {
    // Current task voluntarily yields CPU
    if (!current_task) {
        return;  // No current task to yield from
    }

    // Save current task state
    current_task->state = TASK_STATE_WAITING_EVENT;

    // Get next task to run
    Task* next_task = task_scheduler_next();
    if (!next_task || next_task == current_task) {
        return;  // No other task available
    }

    // Perform context switch
    Task* old_task = current_task;
    current_task = next_task;
    next_task->state = TASK_STATE_RUNNING;
    next_task->last_run_time = rdtsc();

    kprintf("[SCHEDULER] Switching from task %lu to %lu\n",
            old_task->task_id, next_task->task_id);

    // Switch contexts (this will save old and restore new)
    task_switch_to(&old_task->context, &next_task->context);

    // When we return here, we've been switched back
}

void task_scheduler_tick(void) {
    // Called by timer interrupt
    // Update health of all tasks
    spin_lock(&task_table_lock);

    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i]) {
            task_update_health(task_table[i]);
            task_auto_recover(task_table[i]);
        }
    }

    spin_unlock(&task_table_lock);

    // NOTE: For v1, we use COOPERATIVE multitasking
    // Tasks explicitly call task_scheduler_yield() to switch contexts
    // This provides real context switching without IRQ complexity

    // Future (v2): Implement PREEMPTIVE multitasking:
    // 1. Save context in IRQ handler before calling this
    // 2. Select next task here
    // 3. Return next task's context to IRQ handler
    // 4. IRQ handler restores new context and IRET
}

// ============================================================================
// TASK GROUPS (basic implementation)
// ============================================================================

uint64_t task_group_create(const char* name) {
    spin_lock(&task_groups_lock);

    for (int i = 0; i < MAX_TASK_GROUPS; i++) {
        if (task_groups[i].group_id == 0) {
            task_groups[i].group_id = atomic_increment_u64(&next_group_id);
            strncpy(task_groups[i].name, name, TASK_GROUP_NAME_MAX - 1);
            task_groups[i].task_count = 0;
            task_groups[i].memory_limit = 0;
            task_groups[i].memory_used = 0;
            task_groups[i].energy_limit = 100;
            task_groups[i].throttled = 0;
            task_groups[i].creation_time = rdtsc();

            uint64_t group_id = task_groups[i].group_id;
            spin_unlock(&task_groups_lock);

            kprintf("[TASK] Created task group '%s' (ID=%lu)\n", name, group_id);
            return group_id;
        }
    }

    spin_unlock(&task_groups_lock);
    return 0;  // No free slots
}

int task_group_add(uint64_t group_id, uint64_t task_id) {
    Task* task = task_get(task_id);
    if (!task) return -1;

    spin_lock(&task_groups_lock);

    for (int i = 0; i < MAX_TASK_GROUPS; i++) {
        if (task_groups[i].group_id == group_id) {
            if (task_groups[i].task_count < TASK_GROUP_MAX_TASKS) {
                task_groups[i].task_ids[task_groups[i].task_count] = task_id;
                task_groups[i].task_count++;
                task->group_id = group_id;

                spin_unlock(&task_groups_lock);
                return 0;
            }
        }
    }

    spin_unlock(&task_groups_lock);
    return -1;
}

int task_group_remove(uint64_t group_id, uint64_t task_id) {
    spin_lock(&task_groups_lock);

    for (int i = 0; i < MAX_TASK_GROUPS; i++) {
        if (task_groups[i].group_id == group_id) {
            // Find and remove task from array
            for (uint32_t j = 0; j < task_groups[i].task_count; j++) {
                if (task_groups[i].task_ids[j] == task_id) {
                    // Shift remaining tasks
                    for (uint32_t k = j; k < task_groups[i].task_count - 1; k++) {
                        task_groups[i].task_ids[k] = task_groups[i].task_ids[k + 1];
                    }
                    task_groups[i].task_count--;
                    spin_unlock(&task_groups_lock);
                    return 0;
                }
            }
        }
    }

    spin_unlock(&task_groups_lock);
    return -1;  // Group or task not found
}

int task_group_set_memory_limit(uint64_t group_id, uint64_t bytes) {
    spin_lock(&task_groups_lock);

    for (int i = 0; i < MAX_TASK_GROUPS; i++) {
        if (task_groups[i].group_id == group_id) {
            task_groups[i].memory_limit = bytes;
            spin_unlock(&task_groups_lock);
            return 0;
        }
    }

    spin_unlock(&task_groups_lock);
    return -1;
}

int task_group_broadcast(uint64_t group_id, void* message) {
    (void)message;  // Message API not yet implemented

    spin_lock(&task_groups_lock);

    for (int i = 0; i < MAX_TASK_GROUPS; i++) {
        if (task_groups[i].group_id == group_id) {
            // NOTE: Message queue API for tasks not yet implemented
            // When implemented, will send message to all tasks in group
            kprintf("[TASK] Group broadcast to %u tasks (message API pending)\n",
                    task_groups[i].task_count);
            spin_unlock(&task_groups_lock);
            return 0;
        }
    }

    spin_unlock(&task_groups_lock);
    return -1;  // Group not found
}

// ============================================================================
// MIGRATION (stub for now)
// ============================================================================

int task_migrate(uint64_t task_id, uint8_t target_core) {
    // TODO: Implement task migration between CPU cores
    kprintf("[TASK] Task migration not yet implemented\n");
    return -1;
}
