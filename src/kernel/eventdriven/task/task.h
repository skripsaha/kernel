#ifndef TASK_H
#define TASK_H

#include "ktypes.h"
#include "../core/atomics.h"
#include "vmm.h"

// ============================================================================
// TASK SYSTEM - Lightweight task management (replacement for heavy processes)
// ============================================================================
//
// Philosophy:
// - Tasks are FAST to create (microseconds, not milliseconds)
// - Energy-based priority system (0-100)
// - Self-regulating resource allocation
// - Adaptive learning: system learns which tasks use resources efficiently
// - Direct memory-queue communication (no syscalls needed)
// - Task groups for related workloads
// - Health monitoring and auto-recovery
//
// ============================================================================

// ============================================================================
// TASK STATES
// ============================================================================

typedef enum {
    TASK_STATE_RUNNING      = 0,  // Actively executing
    TASK_STATE_PROCESSING   = 1,  // Processing an event
    TASK_STATE_WAITING_IO   = 2,  // Waiting for disk/network
    TASK_STATE_WAITING_EVENT= 3,  // Waiting for kernel response
    TASK_STATE_DROWSY       = 4,  // Starting to sleep from inactivity
    TASK_STATE_SLEEPING     = 5,  // Sleeping
    TASK_STATE_HIBERNATING  = 6,  // Deep sleep, almost unloaded
    TASK_STATE_THROTTLED    = 7,  // System is limiting (too much resource usage)
    TASK_STATE_STALLED      = 8,  // Stuck, not making progress
    TASK_STATE_DEAD         = 9   // Terminated
} TaskState;

// ============================================================================
// TASK HEALTH METRICS
// ============================================================================

// Health metrics (0-100 each)
typedef struct {
    uint8_t responsiveness;  // How fast it responds to requests
    uint8_t efficiency;      // How efficiently it uses resources
    uint8_t stability;       // How often it crashes/errors
    uint8_t progress;        // Is it making progress or stuck?

    uint8_t overall_health;  // Computed from above (0-100)
} TaskHealth;

// ============================================================================
// TASK CONTEXT (CPU state)
// ============================================================================

// CPU context for task switching
typedef struct {
    // General purpose registers
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11;
    uint64_t r12, r13, r14, r15;

    // Instruction pointer and flags
    uint64_t rip;
    uint64_t rflags;

    // Segment registers
    uint16_t cs, ds, es, fs, gs, ss;

    // FPU/SSE state pointer (allocated separately for efficiency)
    void* fpu_state;
} TaskContext;

// ============================================================================
// TASK CONTROL BLOCK (TCB)
// ============================================================================

#define TASK_NAME_MAX 32
#define TASK_STACK_SIZE (16 * 1024)  // 16KB stack per task
#define TASK_MESSAGE_QUEUE_SIZE 16   // Max messages per task

// === MESSAGE QUEUE ===
typedef struct {
    uint64_t sender_id;            // ID of sender task
    uint64_t message_type;         // Type of message
    uint64_t data[4];              // Message payload (32 bytes)
} TaskMessage;

typedef struct {
    TaskMessage messages[TASK_MESSAGE_QUEUE_SIZE];
    uint32_t head;                 // Read position
    uint32_t tail;                 // Write position
    uint32_t count;                // Number of messages
    uint32_t lock;                 // Spinlock
} TaskMessageQueue;

typedef struct Task {
    // === IDENTITY ===
    uint64_t task_id;              // Unique task ID
    char name[TASK_NAME_MAX];      // Task name (for debugging)
    uint64_t parent_id;            // Parent task ID (0 = kernel)
    uint64_t group_id;             // Task group ID (0 = none)

    // === ENERGY & PRIORITY ===
    uint8_t energy_requested;      // Energy requested by task (0-100)
    uint8_t energy_allocated;      // Energy actually allocated (0-100)
    uint8_t energy_efficiency;     // How well task used energy last time (0-100)

    // === STATE ===
    TaskState state;               // Current state
    TaskHealth health;             // Health metrics

    // === TIMING ===
    uint64_t creation_time;        // RDTSC at creation
    uint64_t last_run_time;        // RDTSC of last execution
    uint64_t total_runtime;        // Total CPU time used
    uint64_t sleep_until;          // RDTSC to wake up (if sleeping)

    // === MEMORY ===
    uint64_t page_table;           // CR3 value (virtual memory context)
    void* stack_base;              // Stack base address
    uint64_t stack_size;           // Stack size
    void* entry_point;             // Task entry point function
    void* args;                    // Arguments passed to entry point

    // === CPU CONTEXT ===
    TaskContext context;           // Saved CPU state

    // === STATISTICS ===
    uint64_t events_processed;     // Total events processed
    uint64_t errors_count;         // Number of errors/crashes
    uint64_t io_operations;        // Number of I/O ops
    uint64_t last_progress_time;   // RDTSC of last progress

    // === COMMUNICATION ===
    TaskMessageQueue* message_queue;  // Pointer to task's message queue
    uint64_t pending_messages;        // Number of pending messages

    // === LINKED LIST ===
    struct Task* next;             // Next task in scheduler queue
    struct Task* prev;             // Previous task in scheduler queue
} Task;

// ============================================================================
// TASK GROUP
// ============================================================================

#define TASK_GROUP_NAME_MAX 32
#define TASK_GROUP_MAX_TASKS 256

typedef struct {
    uint64_t group_id;                  // Unique group ID
    char name[TASK_GROUP_NAME_MAX];     // Group name

    uint64_t task_count;                // Number of tasks in group
    uint64_t task_ids[TASK_GROUP_MAX_TASKS];  // Array of task IDs

    // Shared limits
    uint64_t memory_limit;              // Total memory limit for group
    uint64_t memory_used;               // Current memory usage
    uint8_t  energy_limit;              // Total energy limit (0-100)

    // Group state
    uint8_t  throttled;                 // Is group throttled?
    uint64_t creation_time;             // RDTSC at creation
} TaskGroup;

// ============================================================================
// TASK SYSTEM API
// ============================================================================

// === INITIALIZATION ===
void task_system_init(void);

// === TASK CREATION & DESTRUCTION ===
Task* task_spawn(const char* name, void* entry_point, uint8_t energy);
Task* task_spawn_with_args(const char* name, void* entry_point, void* args, uint8_t energy);
int task_kill(uint64_t task_id);

// === TASK CONTROL ===
int task_sleep(uint64_t task_id, uint64_t milliseconds);
int task_wake(uint64_t task_id);
int task_pause(uint64_t task_id);
int task_resume(uint64_t task_id);

// === ENERGY MANAGEMENT ===
int task_boost(uint64_t task_id, uint8_t extra_energy);  // Temporarily increase energy
int task_throttle(uint64_t task_id, uint8_t reduction);  // Limit energy
int task_set_energy(uint64_t task_id, uint8_t energy);   // Set energy request

// === TASK QUERY ===
Task* task_get(uint64_t task_id);
Task* task_get_current(void);
uint64_t task_get_current_id(void);
TaskState task_get_state(uint64_t task_id);
int task_enumerate(Task** tasks_out, uint32_t max_tasks);  // Get list of all tasks

// === TASK GROUPS ===
uint64_t task_group_create(const char* name);
int task_group_add(uint64_t group_id, uint64_t task_id);
int task_group_remove(uint64_t group_id, uint64_t task_id);
int task_group_set_memory_limit(uint64_t group_id, uint64_t bytes);
int task_group_broadcast(uint64_t group_id, void* message);

// === HEALTH & MONITORING ===
void task_update_health(Task* task);
int task_auto_recover(Task* task);
void task_print_stats(uint64_t task_id);

// === SCHEDULER INTERFACE ===
Task* task_scheduler_next(void);  // Get next task to run
void task_scheduler_yield(void);  // Current task yields CPU
void task_scheduler_tick(void);   // Called by timer interrupt

// === MIGRATION ===
int task_migrate(uint64_t task_id, uint8_t target_core);

// === CONTEXT SWITCHING (Assembly functions) ===
// These are implemented in arch/x86-64/context/context_switch.asm
void task_save_context(TaskContext* ctx);
void task_restore_context(TaskContext* ctx);
void task_switch_to(TaskContext* old_ctx, TaskContext* new_ctx);
void task_init_context(TaskContext* ctx, void* entry, void* stack, void* arg);

#endif  // TASK_H
