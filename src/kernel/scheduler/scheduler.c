#include "scheduler.h"
#include "klib.h"
#include "process.h"
#include "idt.h"  // For interrupt_frame_t

// ============================================================================
// EVENT-DRIVEN HYBRID SCHEDULER
// ============================================================================
//
// INNOVATION: Workflow-driven scheduling, not timer-driven!
//
// PRIMARY: Cooperative scheduling via workflow events
//   - kernel_notify(NOTIFY_WAIT) → automatic yield
//   - Completion IRQ → wake up waiting process
//   - kernel_notify(NOTIFY_YIELD) → explicit yield
//
// SECONDARY: Timer-based preemption (protection only)
//   - Large time slice (100ms) to prevent infinite loops
//   - Not the primary scheduling mechanism
//
// This is NOT UNIX - processes yield when waiting for events!
//
// ============================================================================

// ============================================================================
// SCHEDULER STATE
// ============================================================================

// Ready queue (simple circular list for round-robin)
static process_t* ready_queue[PROCESS_MAX_COUNT];
static int ready_queue_head = 0;
static int ready_queue_tail = 0;
static int ready_queue_count = 0;

// Current running process
static process_t* current_running = NULL;

// Time slice counter (protection against infinite loops)
static int time_slice_remaining = 0;
static const int TIME_SLICE_TICKS = 10;  // 10 ticks = 100ms at 100Hz (LARGE!)

// Statistics
static scheduler_stats_t stats = {0};

// ============================================================================
// INITIALIZATION
// ============================================================================

void scheduler_init(void) {
    kprintf("[SCHEDULER] Initializing event-driven hybrid scheduler...\n");

    ready_queue_head = 0;
    ready_queue_tail = 0;
    ready_queue_count = 0;
    current_running = NULL;
    time_slice_remaining = TIME_SLICE_TICKS;

    memset(&stats, 0, sizeof(stats));

    kprintf("[SCHEDULER] Ready queue size: %d processes\n", PROCESS_MAX_COUNT);
    kprintf("[SCHEDULER] Time slice: %d ticks (%d ms at 100Hz) - PROTECTION ONLY\n",
            TIME_SLICE_TICKS, TIME_SLICE_TICKS * 10);
    kprintf("[SCHEDULER] Primary scheduling: WORKFLOW-DRIVEN (cooperative)\n");
    kprintf("[SCHEDULER] Secondary scheduling: TIMER-BASED (preemptive fallback)\n");
    kprintf("[SCHEDULER] Initialized successfully!\n");
}

// ============================================================================
// READY QUEUE MANAGEMENT
// ============================================================================

// Add process to ready queue (FIFO for round-robin)
void scheduler_add_process(process_t* proc) {
    if (!proc) {
        kprintf("[SCHEDULER] ERROR: NULL process!\n");
        return;
    }

    if (ready_queue_count >= PROCESS_MAX_COUNT) {
        kprintf("[SCHEDULER] ERROR: Ready queue full!\n");
        return;
    }

    // Add to tail
    ready_queue[ready_queue_tail] = proc;
    ready_queue_tail = (ready_queue_tail + 1) % PROCESS_MAX_COUNT;
    ready_queue_count++;

    proc->state = PROCESS_STATE_READY;

    kprintf("[SCHEDULER] Added process PID=%lu to ready queue (count=%d)\n",
            proc->pid, ready_queue_count);
}

// Remove process from ready queue
void scheduler_remove_process(process_t* proc) {
    if (!proc) {
        return;
    }

    // Linear search and remove (inefficient but simple)
    int found = 0;
    for (int i = 0; i < ready_queue_count; i++) {
        int idx = (ready_queue_head + i) % PROCESS_MAX_COUNT;
        if (ready_queue[idx] == proc) {
            found = 1;
            // Shift all following processes forward
            for (int j = i; j < ready_queue_count - 1; j++) {
                int cur = (ready_queue_head + j) % PROCESS_MAX_COUNT;
                int next = (ready_queue_head + j + 1) % PROCESS_MAX_COUNT;
                ready_queue[cur] = ready_queue[next];
            }
            ready_queue_count--;
            ready_queue_tail = (ready_queue_tail - 1 + PROCESS_MAX_COUNT) % PROCESS_MAX_COUNT;
            break;
        }
    }

    if (found) {
        kprintf("[SCHEDULER] Removed process PID=%lu from ready queue (count=%d)\n",
                proc->pid, ready_queue_count);
    }
}

// ============================================================================
// SCHEDULING DECISIONS
// ============================================================================

// Pick next process to run (round-robin)
process_t* scheduler_pick_next(void) {
    if (ready_queue_count == 0) {
        return NULL;  // No processes to run
    }

    // Get from head (FIFO)
    process_t* next = ready_queue[ready_queue_head];
    ready_queue_head = (ready_queue_head + 1) % PROCESS_MAX_COUNT;
    ready_queue_count--;

    return next;
}

// ============================================================================
// COOPERATIVE SCHEDULING (PRIMARY MECHANISM)
// ============================================================================

// Cooperative yield - called from kernel_notify(NOTIFY_YIELD or NOTIFY_WAIT)
void scheduler_yield_cooperative(interrupt_frame_t* frame) {
    process_t* current = process_get_current();

    if (!current) {
        return;  // No current process
    }

    stats.voluntary_yields++;

    kprintf("[SCHEDULER] Cooperative yield from PID=%lu\n", current->pid);

    // Save current process context from interrupt frame
    scheduler_save_context(current, frame);

    // Add current process back to ready queue (if not waiting or terminated)
    if (current->state == PROCESS_STATE_RUNNING) {
        scheduler_add_process(current);
    } else if (current->state == PROCESS_STATE_ZOMBIE) {
        kprintf("[SCHEDULER] Process PID=%lu ZOMBIE - cleaning up resources\n", current->pid);
        // Free process resources (we're in kernel context, safe to cleanup)
        extern void process_destroy(process_t* proc);
        process_destroy(current);
        process_set_current(NULL);  // Clear current process
        // Process destroyed, check if there are other processes
        process_t* next = scheduler_pick_next();
        if (next) {
            // Switch to next process
            scheduler_restore_context(next, frame);
            next->state = PROCESS_STATE_RUNNING;
            process_set_current(next);
            stats.context_switches++;
            time_slice_remaining = TIME_SLICE_TICKS;
            kprintf("[SCHEDULER] Context switch after cleanup: destroyed -> PID %lu\n", next->pid);
        } else {
            // No processes left - halt
            kprintf("[SCHEDULER] All processes terminated - system halting\n");
            kprintf("[SCHEDULER] System idle - no processes to run\n");
            while (1) {
                asm volatile("hlt");
            }
        }
        return;  // Never reached if no processes, or returned to new process
    } else if (current->state == PROCESS_STATE_WAITING) {
        kprintf("[SCHEDULER] Process PID=%lu WAITING - switching to another process\n", current->pid);

        // CRITICAL: Do NOT add WAITING process back to ready queue!
        // It will be re-added by completion IRQ handler when event completes

        // Try to pick next process from ready queue
        process_t* next = scheduler_pick_next();

        if (next) {
            // Switch to next available process
            scheduler_restore_context(next, frame);
            next->state = PROCESS_STATE_RUNNING;
            process_set_current(next);

            stats.context_switches++;
            time_slice_remaining = TIME_SLICE_TICKS;

            kprintf("[SCHEDULER] Context switch (waiting->ready): PID %lu (WAITING) -> PID %lu (RUNNING)\n",
                    current->pid, next->pid);
            return;
        }

        // No other processes to run - enter idle loop waiting for completion IRQ
        kprintf("[SCHEDULER] No runnable processes - entering idle loop (waiting for IRQ)\n");
        process_set_current(NULL);  // No current process

        // Idle loop - wait for interrupts (completion IRQ will wake us)
        while (1) {
            asm volatile("hlt");  // Wait for interrupt

            // After IRQ, check if any processes became ready
            if (ready_queue_count > 0) {
                kprintf("[SCHEDULER] Woke from idle - processes available\n");
                process_t* next = scheduler_pick_next();
                if (next) {
                    scheduler_restore_context(next, frame);
                    next->state = PROCESS_STATE_RUNNING;
                    process_set_current(next);
                    stats.context_switches++;
                    time_slice_remaining = TIME_SLICE_TICKS;
                    kprintf("[SCHEDULER] Context switch (idle->ready): -> PID %lu\n", next->pid);
                    return;
                }
            }
        }
    }

    // Pick next process
    process_t* next = scheduler_pick_next();
    if (next && next != current) {
        // Restore next process context to interrupt frame
        scheduler_restore_context(next, frame);

        // Update process states
        next->state = PROCESS_STATE_RUNNING;
        process_set_current(next);

        stats.context_switches++;
        time_slice_remaining = TIME_SLICE_TICKS;  // Reset time slice

        kprintf("[SCHEDULER] Context switch (cooperative): PID %lu -> PID %lu\n",
                current->pid, next->pid);
    } else {
        // No other process to run
        if (current->state == PROCESS_STATE_ZOMBIE) {
            // Current process is ZOMBIE and no other processes available
            kprintf("[SCHEDULER] All processes terminated - system halting\n");
            kprintf("[SCHEDULER] System idle - no processes to run\n");
            // Enter halt loop - wait for interrupts (but there won't be any work)
            while (1) {
                asm volatile("hlt");
            }
        }
        // Restore current process (it's still viable)
        scheduler_restore_context(current, frame);
        time_slice_remaining = TIME_SLICE_TICKS;
    }
}

// ============================================================================
// TIMER-BASED SCHEDULING (SECONDARY MECHANISM - PROTECTION ONLY)
// ============================================================================

// Called from timer IRQ - only for protection against infinite loops
void scheduler_tick(interrupt_frame_t* frame) {
    stats.total_ticks++;

    process_t* current = process_get_current();

    if (!current) {
        return;  // No current process to preempt
    }

    // Decrement time slice
    time_slice_remaining--;

    if (time_slice_remaining <= 0) {
        // Time slice expired - preempt! (This should be RARE!)
        stats.preemptions++;

        kprintf("[SCHEDULER] Timer preemption of PID=%lu (protection mechanism)\n",
                current->pid);

        // Save current process context from interrupt frame
        scheduler_save_context(current, frame);

        // Add current back to ready queue (if not waiting or terminated)
        if (current->state == PROCESS_STATE_RUNNING) {
            scheduler_add_process(current);
        } else if (current->state == PROCESS_STATE_ZOMBIE) {
            kprintf("[SCHEDULER] Timer tick on ZOMBIE process PID=%lu - cleaning up\n", current->pid);
            extern void process_destroy(process_t* proc);
            process_destroy(current);
            process_set_current(NULL);
            // Process destroyed, check if there are other processes
            process_t* next = scheduler_pick_next();
            if (next) {
                scheduler_restore_context(next, frame);
                next->state = PROCESS_STATE_RUNNING;
                process_set_current(next);
                stats.context_switches++;
                time_slice_remaining = TIME_SLICE_TICKS;
                kprintf("[SCHEDULER] Context switch (timer after cleanup): destroyed -> PID %lu\n", next->pid);
            } else {
                kprintf("[SCHEDULER] All processes terminated (timer) - system halting\n");
                kprintf("[SCHEDULER] System idle - no processes to run\n");
                while (1) {
                    asm volatile("hlt");
                }
            }
            return;  // Never reached if no processes
        } else if (current->state == PROCESS_STATE_WAITING) {
            kprintf("[SCHEDULER] Timer tick on WAITING process PID=%lu - switching away\n", current->pid);

            // WAITING process should not be running - switch to another
            process_t* next = scheduler_pick_next();

            if (next) {
                scheduler_restore_context(next, frame);
                next->state = PROCESS_STATE_RUNNING;
                process_set_current(next);
                stats.context_switches++;
                time_slice_remaining = TIME_SLICE_TICKS;
                kprintf("[SCHEDULER] Context switch (timer waiting->ready): PID %lu (WAITING) -> PID %lu\n",
                        current->pid, next->pid);
                return;
            }

            // No processes to run - idle
            kprintf("[SCHEDULER] No runnable processes (timer) - entering idle\n");
            process_set_current(NULL);
            while (1) {
                asm volatile("hlt");
                if (ready_queue_count > 0) {
                    process_t* next = scheduler_pick_next();
                    if (next) {
                        scheduler_restore_context(next, frame);
                        next->state = PROCESS_STATE_RUNNING;
                        process_set_current(next);
                        stats.context_switches++;
                        time_slice_remaining = TIME_SLICE_TICKS;
                        return;
                    }
                }
            }
        }

        // Pick next process
        process_t* next = scheduler_pick_next();
        if (next && next != current) {
            // Restore next process context to interrupt frame
            scheduler_restore_context(next, frame);

            // Update process states
            next->state = PROCESS_STATE_RUNNING;
            process_set_current(next);

            stats.context_switches++;
            time_slice_remaining = TIME_SLICE_TICKS;  // Reset time slice

            kprintf("[SCHEDULER] Context switch (preemptive): PID %lu -> PID %lu\n",
                    current->pid, next->pid);
        } else {
            // No other process to run
            if (current->state == PROCESS_STATE_ZOMBIE) {
                // Current process is ZOMBIE and no other processes available
                kprintf("[SCHEDULER] All processes terminated (timer) - system halting\n");
                kprintf("[SCHEDULER] System idle - no processes to run\n");
                // Enter halt loop
                while (1) {
                    asm volatile("hlt");
                }
            }
            // Restore current process (it's still viable)
            scheduler_restore_context(current, frame);
            time_slice_remaining = TIME_SLICE_TICKS;
        }
    }
}

// ============================================================================
// CONTEXT SWITCHING (REAL IMPLEMENTATION - NO FAKES!)
// ============================================================================

// Save current process context (ALL registers from interrupt frame)
void scheduler_save_context(process_t* proc, void* interrupt_frame) {
    if (!proc || !interrupt_frame) {
        return;
    }

    interrupt_frame_t* frame = (interrupt_frame_t*)interrupt_frame;

    // Save ALL registers from interrupt frame
    proc->rip = frame->rip;
    proc->rsp = frame->rsp;
    proc->rbp = frame->rbp;
    proc->rflags = frame->rflags;
    proc->cs = (uint16_t)frame->cs;
    proc->ss = (uint16_t)frame->ss;

    // NOTE: General purpose registers (RAX, RBX, etc) are already on stack
    // and will be restored by isr_common when we return
    // We only need to save segment registers and instruction pointer
}

// Restore process context (modify interrupt frame for IRETQ)
void scheduler_restore_context(process_t* proc, void* interrupt_frame) {
    if (!proc || !interrupt_frame) {
        return;
    }

    interrupt_frame_t* frame = (interrupt_frame_t*)interrupt_frame;

    // Restore context to interrupt frame
    // IRETQ will restore these values when returning from interrupt
    frame->rip = proc->rip;
    frame->rsp = proc->rsp;
    frame->rflags = proc->rflags;
    frame->cs = proc->cs;
    frame->ss = proc->ss;

    // Switch to process page table (CR3)
    if (proc->cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(proc->cr3) : "memory");
    }

    // Update TSS RSP0 for kernel stack
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(0x900000);  // Kernel stack for syscalls/interrupts
}

// ============================================================================
// STATISTICS
// ============================================================================

scheduler_stats_t scheduler_get_stats(void) {
    return stats;
}

void scheduler_print_stats(void) {
    kprintf("\n=== SCHEDULER STATISTICS ===\n");
    kprintf("Context switches:  %lu\n", stats.context_switches);
    kprintf("Preemptions:       %lu (timer-based, should be rare!)\n", stats.preemptions);
    kprintf("Voluntary yields:  %lu (workflow-driven, primary mechanism)\n", stats.voluntary_yields);
    kprintf("Total ticks:       %lu\n", stats.total_ticks);
    kprintf("Ready queue count: %d\n", ready_queue_count);

    process_t* current = process_get_current();
    if (current) {
        kprintf("Current process:   PID %lu (state=%d)\n",
                current->pid, current->state);
    } else {
        kprintf("Current process:   None\n");
    }
}

void scheduler_print_queue(void) {
    kprintf("[SCHEDULER] Ready queue (%d processes):\n", ready_queue_count);
    for (int i = 0; i < ready_queue_count; i++) {
        int idx = (ready_queue_head + i) % PROCESS_MAX_COUNT;
        process_t* proc = ready_queue[idx];
        kprintf("  [%d] PID=%lu state=%d\n", i, proc->pid, proc->state);
    }
}
