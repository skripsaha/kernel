#include "process.h"
#include "workflow_rings.h"
#include "klib.h"
#include "pmm.h"
#include "vmm.h"
#include "gdt.h"
#include "atomics.h"

// ============================================================================
// GLOBAL STATE
// ============================================================================

static process_t process_table[PROCESS_MAX_COUNT];
static uint64_t next_pid = 1;
static process_t* current_process = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

void process_init(void) {
    kprintf("[PROCESS] Initializing process management...\n");

    memset(process_table, 0, sizeof(process_table));
    next_pid = 1;
    current_process = 0;

    kprintf("[PROCESS] Process table initialized (max %d processes)\n", PROCESS_MAX_COUNT);
}

// ============================================================================
// PROCESS CREATION
// ============================================================================

process_t* process_create(void* code, uint64_t code_size, uint64_t entry_offset) {
    // Find free slot
    process_t* proc = 0;
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state == 0) {
            proc = &process_table[i];
            break;
        }
    }

    if (!proc) {
        kprintf("[PROCESS] ERROR: Process table full!\n");
        return 0;
    }

    // Initialize process structure
    proc->pid = next_pid++;
    proc->state = PROCESS_STATE_READY;

    // Allocate user stack (16KB)
    uint64_t stack_pages = USER_STACK_SIZE / PMM_PAGE_SIZE;
    uint64_t stack_phys = (uint64_t)pmm_alloc(stack_pages);

    if (!stack_phys) {
        kprintf("[PROCESS] ERROR: Failed to allocate user stack!\n");
        return 0;
    }

    // Allocate user code pages
    uint64_t code_pages = (code_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t code_phys = (uint64_t)pmm_alloc(code_pages);

    if (!code_phys) {
        kprintf("[PROCESS] ERROR: Failed to allocate user code!\n");
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    // Copy code to physical memory (identity-mapped for now)
    memcpy((void*)code_phys, code, code_size);

    // ========================================================================
    // CREATE VIRTUAL MEMORY MAPPINGS FOR USER SPACE
    // ========================================================================

    // Create a NEW VMM context for this process (isolated page tables!)
    // This gives each process its own address space while sharing kernel mappings
    vmm_context_t* ctx = vmm_create_context();

    if (!ctx) {
        kprintf("[PROCESS] ERROR: Failed to create VMM context!\n");
        pmm_free((void*)code_phys, code_pages);
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    kprintf("[PROCESS] Created isolated VMM context (PML4 phys=0x%lx)\n", ctx->pml4_phys);

    // Define virtual addresses in user space
    // IMPORTANT: User space starts at 512MB (0x20000000) to avoid conflict with
    // identity-mapped region (VMM identity maps 0-256MB for kernel/device access).
    // This ensures user mappings never overlap with kernel's identity mapping,
    // preventing protection faults when transitioning to Ring 3.
    uint64_t user_code_virt = 0x20000000ULL;                    // 512MB
    uint64_t user_stack_virt = 0x20100000ULL;                   // 513MB
    uint64_t user_rings_virt = 0x20200000ULL;                   // 514MB

    // Map user code (Present + User, executable)
    vmm_map_result_t code_result = vmm_map_pages(ctx, user_code_virt, code_phys,
                                                  code_pages, VMM_FLAGS_USER_CODE);

    if (!code_result.success) {
        kprintf("[PROCESS] ERROR: Failed to map user code: %s\n",
                code_result.error_msg ? code_result.error_msg : "unknown");
        pmm_free((void*)code_phys, code_pages);
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    // Map user stack (Present + User + Writable)
    vmm_map_result_t stack_result = vmm_map_pages(ctx, user_stack_virt, stack_phys,
                                                   stack_pages, VMM_FLAGS_USER_RW);

    if (!stack_result.success) {
        kprintf("[PROCESS] ERROR: Failed to map user stack: %s\n",
                stack_result.error_msg ? stack_result.error_msg : "unknown");
        vmm_unmap_pages(ctx, user_code_virt, code_pages);
        pmm_free((void*)code_phys, code_pages);
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    // ========================================================================
    // ALLOCATE SHARED RING BUFFERS (EventRing + ResultRing)
    // ========================================================================

    // Calculate pages needed dynamically based on ring buffer sizes
    size_t total_rings_size = sizeof(EventRing) + sizeof(ResultRing);
    uint64_t rings_pages = (total_rings_size + 4095) / 4096;  // Round up to pages

    kprintf("[PROCESS] Ring buffers: %zu bytes (%lu pages)\n",
            total_rings_size, rings_pages);
    uint64_t rings_phys = (uint64_t)pmm_alloc(rings_pages);

    if (!rings_phys) {
        kprintf("[PROCESS] ERROR: Failed to allocate ring buffers!\n");
        vmm_unmap_pages(ctx, user_stack_virt, stack_pages);
        vmm_unmap_pages(ctx, user_code_virt, code_pages);
        pmm_free((void*)code_phys, code_pages);
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    // Kernel can access rings directly via identity mapping (physical == virtual in low memory)
    EventRing* event_ring = (EventRing*)rings_phys;
    ResultRing* result_ring = (ResultRing*)(rings_phys + sizeof(EventRing));

    // Initialize rings (zero-initialize)
    memset(event_ring, 0, sizeof(EventRing));
    memset(result_ring, 0, sizeof(ResultRing));

    kprintf("[PROCESS] Initialized ring buffers (phys=0x%lx, %lu pages)\n",
            rings_phys, rings_pages);

    // Map rings to user space (Present + User + Writable for shared access)
    vmm_map_result_t rings_result = vmm_map_pages(ctx, user_rings_virt, rings_phys,
                                                   rings_pages, VMM_FLAGS_USER_RW);

    if (!rings_result.success) {
        kprintf("[PROCESS] ERROR: Failed to map ring buffers to user space: %s\n",
                rings_result.error_msg ? rings_result.error_msg : "unknown");
        pmm_free((void*)rings_phys, rings_pages);
        vmm_unmap_pages(ctx, user_stack_virt, stack_pages);
        vmm_unmap_pages(ctx, user_code_virt, code_pages);
        pmm_free((void*)code_phys, code_pages);
        pmm_free((void*)stack_phys, stack_pages);
        return 0;
    }

    kprintf("[PROCESS] Mapped ring buffers to user space (vaddr=0x%lx)\n",
            user_rings_virt);

    // ========================================================================
    // Store both VIRTUAL and PHYSICAL addresses in process structure
    proc->code_base = user_code_virt;
    proc->code_phys = code_phys;
    proc->code_size = code_size;
    proc->stack_base = user_stack_virt;
    proc->stack_phys = stack_phys;
    proc->rsp = user_stack_virt + USER_STACK_SIZE - 16;  // Top of stack
    proc->rbp = proc->rsp;

    // Ring buffers
    proc->event_ring = event_ring;
    proc->result_ring = result_ring;
    proc->rings_phys = rings_phys;
    proc->rings_user_vaddr = user_rings_virt;
    proc->rings_pages = rings_pages;

    // Set entry point to VIRTUAL address
    proc->rip = user_code_virt + entry_offset;

    // User mode segments (DPL=3)
    proc->cs = GDT_USER_CODE;
    proc->ss = GDT_USER_DATA;
    proc->ds = GDT_USER_DATA;

    // RFLAGS: IF=1 (interrupts enabled), IOPL=0
    proc->rflags = 0x202;

    // Use per-process page directory (ISOLATED ADDRESS SPACE!)
    proc->vmm_context = ctx;
    proc->cr3 = ctx->pml4_phys;

    // Statistics
    proc->syscall_count = 0;
    proc->current_workflow_id = 0;
    proc->creation_time = rdtsc();

    kprintf("[PROCESS] Created process PID=%lu\n", proc->pid);
    kprintf("[PROCESS]   Code: 0x%p -> 0x%p (phys: 0x%p, %lu bytes)\n",
            (void*)proc->code_base, (void*)(proc->code_base + proc->code_size),
            (void*)code_phys, proc->code_size);
    kprintf("[PROCESS]   Entry: 0x%p\n", (void*)proc->rip);
    kprintf("[PROCESS]   Stack: 0x%p -> 0x%p (phys: 0x%p)\n",
            (void*)proc->stack_base, (void*)(proc->stack_base + USER_STACK_SIZE),
            (void*)stack_phys);
    kprintf("[PROCESS]   Rings: user=0x%p, phys=0x%p (%lu pages)\n",
            (void*)proc->rings_user_vaddr, (void*)proc->rings_phys, rings_pages);
    kprintf("[PROCESS]     EventRing: kernel=0x%p\n", proc->event_ring);
    kprintf("[PROCESS]     ResultRing: kernel=0x%p\n", proc->result_ring);
    kprintf("[PROCESS]   CS: 0x%04x, SS: 0x%04x\n", proc->cs, proc->ss);

    return proc;
}

// ============================================================================
// USER MODE TRANSITION
// ============================================================================

// Enter user mode using IRETQ
// This function does NOT return - it jumps to user code
void process_enter_usermode(process_t* proc) {
    if (!proc) {
        panic("process_enter_usermode: NULL process!");
    }

    current_process = proc;
    proc->state = PROCESS_STATE_RUNNING;

    kprintf("[PROCESS] Entering user mode for PID=%lu...\n", proc->pid);
    kprintf("[PROCESS]   RIP: 0x%p\n", (void*)proc->rip);
    kprintf("[PROCESS]   RSP: 0x%p\n", (void*)proc->rsp);
    kprintf("[PROCESS]   CS: 0x%04x (DPL=%d)\n", proc->cs, (proc->cs & 3));
    kprintf("[PROCESS]   SS: 0x%04x (DPL=%d)\n", proc->ss, (proc->ss & 3));
    kprintf("[PROCESS]   RFLAGS: 0x%llx\n", proc->rflags);
    kprintf("[PROCESS]   CR3: 0x%p (process page directory)\n", (void*)proc->cr3);

    // CRITICAL: Switch to process page directory BEFORE entering user mode!
    // User code/stack are mapped in process's page tables, not kernel's
    kprintf("[PROCESS] Switching to process CR3...\n");
    asm volatile("mov %0, %%cr3" : : "r"(proc->cr3) : "memory");

    // Update TSS RSP0 for this process (kernel stack for syscalls)
    extern void tss_set_rsp0(uint64_t rsp0);
    tss_set_rsp0(0x900000);  // Use kernel stack

    // Transition to Ring 3 using IRETQ
    // IRETQ pops: RIP, CS, RFLAGS, RSP, SS
    asm volatile(
        // Push SS (stack segment)
        "pushq %0\n\t"
        // Push RSP (user stack pointer)
        "pushq %1\n\t"
        // Push RFLAGS
        "pushq %2\n\t"
        // Push CS (code segment)
        "pushq %3\n\t"
        // Push RIP (instruction pointer)
        "pushq %4\n\t"

        // Set data segments to user data
        "movw %5, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"

        // Jump to user mode!
        "iretq"
        :
        : "r"((uint64_t)proc->ss),      // %0 - SS
          "r"(proc->rsp),                // %1 - RSP
          "r"(proc->rflags),             // %2 - RFLAGS
          "r"((uint64_t)proc->cs),       // %3 - CS
          "r"(proc->rip),                // %4 - RIP
          "r"((uint16_t)proc->ds)        // %5 - DS
        : "memory", "rax"
    );

    // Should never reach here
    panic("process_enter_usermode: IRETQ failed!");
}

// ============================================================================
// PROCESS CLEANUP
// ============================================================================

// Free all resources allocated to a terminated process
void process_destroy(process_t* proc) {
    if (!proc) {
        kprintf("[PROCESS] ERROR: process_destroy called with NULL process\n");
        return;
    }

    if (proc->state != PROCESS_STATE_ZOMBIE) {
        kprintf("[PROCESS] ERROR: Cannot destroy process PID=%lu (state=%d, not ZOMBIE)\n",
                proc->pid, proc->state);
        return;
    }

    uint64_t pid = proc->pid;
    uint64_t cr3 = proc->cr3;
    uint64_t code_phys = proc->code_phys;
    uint64_t code_pages = (proc->code_size + 4095) / 4096;
    uint64_t stack_phys = proc->stack_phys;
    uint64_t stack_pages = USER_STACK_SIZE / 4096;
    uint64_t rings_phys = proc->rings_phys;
    uint64_t rings_pages = proc->rings_pages;

    kprintf("[PROCESS] Destroying process PID=%lu...\n", pid);
    kprintf("[PROCESS]   Code: 0x%lx (%lu pages)\n", code_phys, code_pages);
    kprintf("[PROCESS]   Stack: 0x%lx (%lu pages)\n", stack_phys, stack_pages);
    kprintf("[PROCESS]   Rings: 0x%lx (%lu pages)\n", rings_phys, rings_pages);
    kprintf("[PROCESS]   CR3: 0x%lx\n", cr3);

    // CRITICAL: Switch to kernel CR3 before cleanup!
    extern vmm_context_t* vmm_get_kernel_context(void);
    vmm_context_t* kernel_ctx = vmm_get_kernel_context();
    asm volatile("mov %0, %%cr3" : : "r"(kernel_ctx->pml4_phys) : "memory");

    // Destroy VMM context (frees page tables and all mapped user pages)
    if (proc->vmm_context) {
        kprintf("[PROCESS]   Destroying VMM context...\n");
        extern void vmm_destroy_context(vmm_context_t* ctx);
        vmm_destroy_context((vmm_context_t*)proc->vmm_context);
        proc->vmm_context = NULL;
        kprintf("[PROCESS]   VMM context destroyed\n");
    }

    // Clear process table entry (free the process slot)
    kprintf("[PROCESS]   Clearing process table entry\n");
    memset(proc, 0, sizeof(process_t));

    kprintf("[PROCESS] Process PID=%lu destroyed successfully\n", pid);
}

// ============================================================================
// SYSCALL CONTEXT MANAGEMENT
// ============================================================================

void process_save_context(process_t* proc, void* frame_ptr) {
    if (!proc) return;

    // Cast to interrupt frame
    typedef struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdi, rsi, rbp, rsp_unused, rbx, rdx, rcx, rax;
        uint64_t int_no, err_code;
        uint64_t rip, cs, rflags, rsp, ss;
    } interrupt_frame_t;

    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    // Save user context
    proc->rip = frame->rip;
    proc->rsp = frame->rsp;
    proc->rbp = frame->rbp;
    proc->rflags = frame->rflags;
    proc->cs = (uint16_t)frame->cs;
    proc->ss = (uint16_t)frame->ss;

    proc->syscall_count++;
}

void process_restore_context(process_t* proc, void* frame_ptr) {
    if (!proc) return;

    typedef struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rdi, rsi, rbp, rsp_unused, rbx, rdx, rcx, rax;
        uint64_t int_no, err_code;
        uint64_t rip, cs, rflags, rsp, ss;
    } interrupt_frame_t;

    interrupt_frame_t* frame = (interrupt_frame_t*)frame_ptr;

    // Restore user context (for IRETQ)
    frame->rip = proc->rip;
    frame->rsp = proc->rsp;
    frame->cs = proc->cs;
    frame->ss = proc->ss;
    frame->rflags = proc->rflags;
}

// ============================================================================
// PROCESS QUERIES
// ============================================================================

process_t* process_get_current(void) {
    return current_process;
}

void process_set_current(process_t* proc) {
    current_process = proc;
}

// ============================================================================
// PROCESS LIFECYCLE
// ============================================================================

// Terminate current process and switch to next
void process_exit(int exit_code) {
    process_t* proc = process_get_current();

    if (!proc) {
        kprintf("[PROCESS] ERROR: process_exit called with no current process!\n");
        panic("process_exit: no current process");
    }

    kprintf("[PROCESS] Process PID=%lu exiting with code %d\n", proc->pid, exit_code);

    // Mark as zombie (waiting for cleanup)
    proc->state = PROCESS_STATE_ZOMBIE;

    // TODO: Store exit code for parent process to read
    // proc->exit_code = exit_code;

    // Remove from scheduler ready queue
    extern void scheduler_remove_process(process_t* proc);
    scheduler_remove_process(proc);

    // Clean up resources (will be done by scheduler or reaper)
    // For now, destroy immediately
    process_destroy(proc);

    // Clear current process
    current_process = NULL;

    // Switch to next process (scheduler will pick one)
    extern process_t* scheduler_pick_next(void);
    process_t* next = scheduler_pick_next();

    if (next) {
        kprintf("[PROCESS] Switching to next process PID=%lu\n", next->pid);
        process_enter_usermode(next);  // Does not return
    } else {
        kprintf("[PROCESS] No more processes to run - halting CPU\n");
        // No more processes - halt forever
        while (1) {
            asm volatile("hlt");
        }
    }

    // Should never reach here
    __builtin_unreachable();
}

// ============================================================================
// STATISTICS
// ============================================================================

void process_print_stats(process_t* proc) {
    if (!proc || proc->state == 0) return;

    kprintf("[PROCESS] PID=%lu State=%d\n", proc->pid, proc->state);
    kprintf("  RIP: 0x%p, RSP: 0x%p\n", (void*)proc->rip, (void*)proc->rsp);
    kprintf("  Syscalls: %lu, Workflow: %lu\n",
            proc->syscall_count, proc->current_workflow_id);
}

void process_print_all(void) {
    kprintf("[PROCESS] Process Table:\n");

    int count = 0;
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        if (process_table[i].state != 0) {
            process_print_stats(&process_table[i]);
            count++;
        }
    }

    kprintf("[PROCESS] Total processes: %d\n", count);
}

// Get process by index (for iteration)
process_t* process_get_by_index(int index) {
    if (index < 0 || index >= PROCESS_MAX_COUNT) {
        return NULL;
    }
    return &process_table[index];
}
