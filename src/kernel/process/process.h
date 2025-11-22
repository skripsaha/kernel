#ifndef PROCESS_H
#define PROCESS_H

#include "ktypes.h"

// ============================================================================
// PROCESS MANAGEMENT - User Mode Support
// ============================================================================
//
// Minimal process structure for user mode execution.
// Each process runs in Ring 3 and can call kernel_notify() to activate
// workflows.
//
// ============================================================================

#define PROCESS_MAX_COUNT   64
#define USER_STACK_SIZE     (16 * 1024)  // 16KB user stack

// Process state
typedef enum {
    PROCESS_STATE_READY = 0,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_WAITING,
    PROCESS_STATE_ZOMBIE
} process_state_t;

// Process structure
typedef struct {
    uint64_t pid;                   // Process ID
    process_state_t state;          // Current state

    // CPU context (saved during syscall/interrupt)
    uint64_t rip;                   // Instruction pointer
    uint64_t rsp;                   // Stack pointer
    uint64_t rbp;                   // Base pointer
    uint64_t rflags;                // Flags register

    // Segment selectors
    uint16_t cs;                    // Code segment
    uint16_t ss;                    // Stack segment
    uint16_t ds;                    // Data segment

    // Memory management
    uint64_t cr3;                   // Page directory base (physical address)
    void* vmm_context;              // VMM context (vmm_context_t*) - per-process page tables
    uint64_t stack_base;            // User stack base (virtual)
    uint64_t stack_phys;            // User stack base (physical)
    uint64_t code_base;             // User code base (virtual)
    uint64_t code_phys;             // User code base (physical)
    uint64_t code_size;             // User code size

    // Shared ring buffers (async workflow communication)
    void* event_ring;               // EventRing* (kernel virtual address)
    void* result_ring;              // ResultRing* (kernel virtual address)
    uint64_t rings_phys;            // Physical address of ring buffers
    uint64_t rings_user_vaddr;      // User virtual address of ring buffers
    uint64_t rings_pages;           // Number of pages allocated for ring buffers

    // Workflow integration
    uint64_t current_workflow_id;   // Currently executing workflow
    volatile uint32_t completion_ready;  // Flag: workflow completed (for WAIT)

    // Statistics
    uint64_t syscall_count;         // Number of syscalls made
    uint64_t creation_time;         // RDTSC at creation

} process_t;

// ============================================================================
// PROCESS API
// ============================================================================

// === INITIALIZATION ===
void process_init(void);

// === PROCESS CREATION ===
// Create a new user process from code buffer
process_t* process_create(void* code, uint64_t code_size, uint64_t entry_offset);

// === EXECUTION ===
// Enter user mode and start executing process
void process_enter_usermode(process_t* proc) __attribute__((noreturn));

// === SYSCALL SUPPORT ===
// Called by syscall handler to save process context
void process_save_context(process_t* proc, void* interrupt_frame);

// Called by syscall handler to restore process context
void process_restore_context(process_t* proc, void* interrupt_frame);

// === SCHEDULING ===
// Get current running process
process_t* process_get_current(void);

// Set current running process (internal - used by scheduler)
void process_set_current(process_t* proc);

// Switch to another process
void process_switch(process_t* next);

// === LIFECYCLE ===
// Terminate current process (called by process itself or kernel)
void process_exit(int exit_code) __attribute__((noreturn));

// Free all resources of a ZOMBIE process (called by scheduler/cleanup)
void process_destroy(process_t* proc);

// === STATISTICS ===
void process_print_stats(process_t* proc);
void process_print_all(void);

// === ITERATION ===
// Get process by index (for iteration through process table)
process_t* process_get_by_index(int index);

#endif  // PROCESS_H
