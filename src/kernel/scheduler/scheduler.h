#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "ktypes.h"
#include "process.h"
#include "idt.h"  // For interrupt_frame_t

// ============================================================================
// EVENT-DRIVEN HYBRID SCHEDULER
// ============================================================================
//
// INNOVATION: Workflow-driven scheduling, not timer-driven!
//
// PRIMARY: Cooperative scheduling via workflow events
//   - kernel_notify(NOTIFY_WAIT) → automatic yield (when waiting for events)
//   - Completion IRQ → wake up waiting process
//   - kernel_notify(NOTIFY_YIELD) → explicit yield
//
// SECONDARY: Timer-based preemption (protection only)
//   - Large time slice (100ms) to prevent infinite loops
//   - Not the primary scheduling mechanism
//
// ============================================================================

// Scheduler statistics
typedef struct {
    uint64_t context_switches;      // Total context switches
    uint64_t preemptions;            // Timer-based preemptions (should be rare!)
    uint64_t voluntary_yields;       // Workflow-driven yields (primary mechanism)
    uint64_t total_ticks;            // Total scheduler ticks
} scheduler_stats_t;

// ============================================================================
// SCHEDULER API
// ============================================================================

// === INITIALIZATION ===
void scheduler_init(void);

// === PROCESS MANAGEMENT ===
// Add process to ready queue (called after process_create)
void scheduler_add_process(process_t* proc);

// Remove process from scheduling (called on exit)
void scheduler_remove_process(process_t* proc);

// === SCHEDULING DECISIONS ===
// Pick next process to run (round-robin)
process_t* scheduler_pick_next(void);

// === COOPERATIVE SCHEDULING (PRIMARY) ===
// Cooperative yield - called from kernel_notify(NOTIFY_YIELD or NOTIFY_WAIT)
// Saves context from interrupt_frame, switches to next process
void scheduler_yield_cooperative(interrupt_frame_t* frame);

// === TIMER-BASED SCHEDULING (SECONDARY - PROTECTION ONLY) ===
// Called from timer IRQ - only for protection against infinite loops
// Large time slice (100ms) ensures this is rarely triggered
void scheduler_tick(interrupt_frame_t* frame);

// === CONTEXT SWITCHING ===
// Save current process context (registers, etc) from interrupt frame
void scheduler_save_context(process_t* proc, void* interrupt_frame);

// Restore process context (registers, etc) to interrupt frame
void scheduler_restore_context(process_t* proc, void* interrupt_frame);

// === STATISTICS ===
scheduler_stats_t scheduler_get_stats(void);
void scheduler_print_stats(void);

// === DEBUG ===
void scheduler_print_queue(void);

#endif // SCHEDULER_H
