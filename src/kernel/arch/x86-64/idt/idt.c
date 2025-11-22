#include "idt.h"
#include "gdt.h"
#include "klib.h"
#include "io.h"
#include "pic.h"
#include "pit.h"  // PIT timer driver
#include "keyboard.h" // Keyboard driver
#include "vmm.h"  // VMM for page fault handling
#include "workflow_rings.h"  // EventRing, ResultRing structures
#include "syscall.h"  // NOTIFY_* flags
#include "workflow.h"  // Workflow API
#include "routing_table.h"  // Routing table
#include "process.h"  // Process management
#include "atomics.h"  // Atomic operations

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

// Счетчики для статистики
static uint64_t exception_count = 0;
static uint64_t irq_count[16] = {0};

// Глобальный счетчик событий (атомарный)
volatile uint64_t global_event_id_counter = 0;

// Inline ASM для загрузки IDT
static void idt_load_asm(uint64_t idt_desc_addr) {
    asm volatile("lidt (%0)" : : "r" (idt_desc_addr) : "memory");
}

void idt_set_entry(int index, uint64_t handler, uint16_t selector, uint8_t type_attr, uint8_t ist) {
    idt[index].offset_low = handler & 0xFFFF;
    idt[index].selector = selector;
    idt[index].ist = ist & 0x07;
    idt[index].type_attr = type_attr;
    idt[index].offset_middle = (handler >> 16) & 0xFFFF;
    idt[index].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[index].reserved = 0;
}

void idt_init(void) {
    kprintf("[IDT] Initializing Interrupt Descriptor Table...\n");
    
    // Очищаем IDT
    memset(idt, 0, sizeof(idt));
    
    // Настройка дескриптора IDT
    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint64_t)idt;
    
    kprintf("[IDT] Setting up exception handlers (0-31)...\n");
    
    // Устанавливаем обработчики исключений (0-31)
    for (int i = 0; i < 32; i++) {
        uint8_t ist = 0;  // По умолчанию без IST
        
        // Критические исключения используют IST
        switch(i) {
            case EXCEPTION_DOUBLE_FAULT:
                ist = IST_DOUBLE_FAULT;
                kprintf("[IDT] Double Fault (vector %d) using IST%d\n", i, ist);
                break;
            case EXCEPTION_NMI:
                ist = IST_NMI;
                kprintf("[IDT] NMI (vector %d) using IST%d\n", i, ist);
                break;
            case EXCEPTION_MACHINE_CHECK:
                ist = IST_MACHINE_CHECK;
                kprintf("[IDT] Machine Check (vector %d) using IST%d\n", i, ist);
                break;
            case EXCEPTION_DEBUG:
                ist = IST_DEBUG;
                kprintf("[IDT] Debug (vector %d) using IST%d\n", i, ist);
                break;
        }
        
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, ist);
    }
    
    kprintf("[IDT] Setting up IRQ handlers (32-47)...\n");
    
    // Устанавливаем обработчики IRQ (32-47)
    for (int i = 32; i < 48; i++) {
        idt_set_entry(i, (uint64_t)isr_table[i], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }
    
    // System call gate (INT 0x80) - USER-CALLABLE (DPL=3)!
    kprintf("[IDT] Setting up system call gate (vector 0x80, DPL=3)...\n");
    idt_set_entry(SYSCALL_VECTOR, (uint64_t)isr_table[SYSCALL_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_USER_INTERRUPT, 0);

    // Completion IRQ (INT 0x81) - KERNEL ONLY (DPL=0)
    kprintf("[IDT] Setting up completion IRQ (vector 0x81, DPL=0)...\n");
    idt_set_entry(COMPLETION_IRQ_VECTOR, (uint64_t)isr_table[COMPLETION_IRQ_VECTOR],
                  GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);

    // Оставшиеся записи (48-127, 130-255, except 0x80-0x81) пустые - будут вызывать General Protection Fault
    for (int i = 48; i < SYSCALL_VECTOR; i++) {
        idt_set_entry(i, (uint64_t)isr_table[13], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }
    for (int i = COMPLETION_IRQ_VECTOR + 1; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, (uint64_t)isr_table[13], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0);
    }

    kprintf("[IDT] IDT configured with %d entries\n", IDT_ENTRIES);
    kprintf("[IDT] IDT base: 0x%p, limit: %d\n", (void*)idt_desc.base, idt_desc.limit);
    kprintf("[IDT] Syscall gate: INT 0x80 (DPL=3, user-callable)\n");

    idt_load();

    kprintf("[IDT] %[S]IDT loaded successfully!%[D]\n");
}

void idt_load(void) {
    kprintf("[IDT] Loading IDT...\n");
    idt_load_asm((uint64_t)&idt_desc);
}

void idt_test(void) {
    kprintf("[IDT] %[H]Testing IDT...%[D]\n");
    
    // Тест 1: Проверяем что IDT загружен
    idt_descriptor_t current_idt;
    asm volatile("sidt %0" : "=m" (current_idt));
    
    kprintf("[IDT] Current IDT base: 0x%p (expected: 0x%p)\n", 
           (void*)current_idt.base, (void*)idt_desc.base);
    kprintf("[IDT] Current IDT limit: %d (expected: %d)\n", 
           current_idt.limit, idt_desc.limit);
    
    if (current_idt.base == idt_desc.base && current_idt.limit == idt_desc.limit) {
        kprintf("[IDT] %[S]IDT load verification: PASSED%[D]\n");
    } else {
        kprintf("[IDT] %[E]IDT load verification: FAILED%[D]\n");
        return;
    }
    
    // Тест 2: Проверяем несколько записей IDT
    kprintf("[IDT] Checking IDT entries...\n");
    kprintf("[IDT] Entry 0 (Divide Error): handler=0x%p\n", 
           (void*)((uint64_t)idt[0].offset_low | 
                  ((uint64_t)idt[0].offset_middle << 16) | 
                  ((uint64_t)idt[0].offset_high << 32)));
    kprintf("[IDT] Entry 13 (General Protection): handler=0x%p\n", 
           (void*)((uint64_t)idt[13].offset_low | 
                  ((uint64_t)idt[13].offset_middle << 16) | 
                  ((uint64_t)idt[13].offset_high << 32)));
    kprintf("[IDT] Entry 32 (Timer IRQ): handler=0x%p\n", 
           (void*)((uint64_t)idt[32].offset_low | 
                  ((uint64_t)idt[32].offset_middle << 16) | 
                  ((uint64_t)idt[32].offset_high << 32)));
    
    kprintf("[IDT] %[S]IDT test PASSED!%[D]\n");
    kprintf("[IDT] %[W]Note: Actual interrupt testing will happen when PIC is configured%[D]\n");
}

// Static flag to track if we've already printed exception info for page faults
static uint64_t last_page_fault_addr = 0;
static int page_fault_printed = 0;

// Обработчик исключений
void exception_handler(interrupt_frame_t* frame) {
    exception_count++;

    // Special handling for page faults - try to handle silently
    if (frame->vector == EXCEPTION_PAGE_FAULT) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r" (cr2));

        // Try to handle the page fault
        if (vmm_handle_page_fault(cr2, frame->error_code) == 0) {
            // Successfully handled - no need to print anything
            return;
        }

        // Failed to handle - print error only once per address
        if (cr2 != last_page_fault_addr || !page_fault_printed) {
            kprintf("\n%[E]=== PAGE FAULT (unhandled) ===%[D]\n");
            kprintf("%[E]Address: 0x%llx%[D]\n", cr2);
            kprintf("%[E]Error Code: 0x%llx%[D]\n", frame->error_code);
            kprintf("%[E]RIP: 0x%llx%[D]\n", frame->rip);
            last_page_fault_addr = cr2;
            page_fault_printed = 1;
            panic("Unhandled page fault");
        }
        return;
    }

    // For other exceptions, print full details
    kprintf("\n%[E]=== EXCEPTION OCCURRED ===%[D]\n");
    kprintf("%[E]Exception Vector: %llu%[D]\n", frame->vector);
    kprintf("%[E]Error Code: 0x%llx%[D]\n", frame->error_code);
    kprintf("%[E]RIP: 0x%llx%[D]\n", frame->rip);
    kprintf("%[E]CS: 0x%llx%[D]\n", frame->cs);
    kprintf("%[E]RFLAGS: 0x%llx%[D]\n", frame->rflags);
    kprintf("%[E]RSP: 0x%llx%[D]\n", frame->rsp);
    kprintf("%[E]SS: 0x%llx%[D]\n", frame->ss);
    kprintf("%[E]RAX: 0x%llx, RBX: 0x%llx%[D]\n", frame->rax, frame->rbx);

    // Дополнительная информация для некоторых исключений
    switch(frame->vector) {
        case EXCEPTION_DIVIDE_ERROR:
            kprintf("%[E]Divide by zero error!%[D]\n");
            panic("Divide by zero");
            break;
        case EXCEPTION_GENERAL_PROTECTION:
            kprintf("%[E]General Protection Fault!%[D]\n");
            panic("General Protection Fault");
            break;
        case EXCEPTION_DOUBLE_FAULT:
            kprintf("%[E]Double Fault! System unstable!%[D]\n");
            panic("Double Fault");
            break;
    }

    kprintf("%[E]Total exceptions so far: %llu%[D]\n", exception_count);
    panic("Unhandled exception");
}

// Обработчик аппаратных прерываний (ИСПРАВЛЕНО)
void irq_handler(interrupt_frame_t* frame) {
    uint8_t irq = frame->vector - 32;
    
    if (irq < 16) {
        irq_count[irq]++;
    }
    
    // Обработчики для основных IRQ
    switch(frame->vector) {
        case IRQ_TIMER:
            // Increment PIT tick counter
            pit_tick();

            // ASYNC WORKFLOW PROCESSING: Process events in background
            // Run guide every 10 ticks (100ms at 100Hz)
            if (irq_count[0] % 10 == 0) {
                extern void guide_process_all(void);
                guide_process_all();
            }

            // PREEMPTIVE SCHEDULING: Call scheduler on timer tick
            // This enables hybrid scheduling (cooperative + preemptive)
            extern void scheduler_tick(interrupt_frame_t* frame);
            scheduler_tick(frame);

            // Таймер - уменьшили частоту логирования
            if (irq_count[0] % 1000 == 0) {  // Каждые ~55 секунд вместо 5.5
                uint64_t minutes = irq_count[0] > 1100 ? irq_count[0] / 1100 : 0;
                // kprintf("%[H]Timer: %llu ticks (~%llu minutes)%[D]\n",
                //        irq_count[0], minutes);
            }
            break;
            
        case IRQ_KEYBOARD:
            // Клавиатура - читаем scancode и обрабатываем
            uint8_t scancode = inb(0x60);
            keyboard_handle_scancode(scancode);
            break;
            
        default:
            // Остальные IRQ - логируем только первые несколько раз
            if (irq_count[irq] <= 3) {  // Только первые 3 раза
                kprintf("%[H]IRQ %d triggered (vector %d, count=%llu)%[D]\n", 
                       irq, frame->vector, irq_count[irq]);
            }
            break;
    }
    
    // Отправляем EOI через PIC (ИСПРАВЛЕНО)
    pic_send_eoi(irq);
}

// ============================================================================
// KERNEL_NOTIFY SYSCALL HANDLER
// ============================================================================
//
// Single syscall interface for async workflow processing:
//   User → EventRing → kernel_notify(SUBMIT) → Guide → Decks → ResultRing
//
// Flags:
//   NOTIFY_SUBMIT - process events from EventRing
//   NOTIFY_WAIT   - block until workflow completes
//   NOTIFY_POLL   - check workflow status (non-blocking)
//
// ============================================================================

void syscall_handler(interrupt_frame_t* frame) {
    // Parameters:
    // RDI = workflow_id
    // RSI = flags (NOTIFY_SUBMIT | NOTIFY_WAIT | NOTIFY_POLL)

    uint64_t workflow_id = frame->rdi;
    uint64_t flags = frame->rsi;

    // ========================================================================
    // INPUT VALIDATION - SECURITY CRITICAL!
    // ========================================================================

    // 1. Validate current process exists
    process_t* proc = process_get_current();
    if (!proc) {
        kprintf("[SYSCALL] ERROR: No current process!\n");
        frame->rax = (uint64_t)-1;
        return;
    }

    // 2. Validate workflow_id bounds (max 16 workflows)
    #define MAX_WORKFLOWS 16
    if (workflow_id >= MAX_WORKFLOWS) {
        kprintf("[SYSCALL] ERROR: Invalid workflow_id %lu (max %d)\n",
                workflow_id, MAX_WORKFLOWS);
        frame->rax = (uint64_t)-2;
        return;
    }

    // 3. Validate flags (NOTIFY_SUBMIT, NOTIFY_WAIT, NOTIFY_POLL, NOTIFY_YIELD, NOTIFY_EXIT allowed)
    #define NOTIFY_SUBMIT 0x01
    #define NOTIFY_WAIT   0x02
    #define NOTIFY_POLL   0x04
    #define NOTIFY_YIELD  0x08
    #define NOTIFY_EXIT   0x10
    #define VALID_FLAGS_MASK (NOTIFY_SUBMIT | NOTIFY_WAIT | NOTIFY_POLL | NOTIFY_YIELD | NOTIFY_EXIT)

    if (flags & ~VALID_FLAGS_MASK) {
        kprintf("[SYSCALL] ERROR: Invalid flags 0x%lx (valid mask: 0x%x)\n",
                flags, VALID_FLAGS_MASK);
        frame->rax = (uint64_t)-3;
        return;
    }

    // 4. Validate ring buffer pointers
    if (!proc->event_ring || !proc->result_ring) {
        kprintf("[SYSCALL] ERROR: Process ring buffers not initialized!\n");
        frame->rax = (uint64_t)-4;
        return;
    }

    // 5. At least one flag must be set
    if (flags == 0) {
        kprintf("[SYSCALL] ERROR: No operation specified (flags=0)\n");
        frame->rax = (uint64_t)-5;
        return;
    }

    // External references
    extern Workflow* workflow_get(uint64_t workflow_id);
    extern RoutingTable global_routing_table;

    // ========================================================================
    // MODE 1: SUBMIT - Process events from EventRing
    // ========================================================================

    if (flags & NOTIFY_SUBMIT) {
        // Ring buffers already validated in input validation section
        EventRing* event_ring = (EventRing*)proc->event_ring;

        kprintf("[SYSCALL] kernel_notify(SUBMIT) - processing EventRing\n");

        uint64_t processed = 0;

        // Process ALL events from EventRing (batch processing)
        while (!wf_event_ring_is_empty(event_ring)) {
            RingEvent* user_event = wf_event_ring_pop(event_ring);

            if (!user_event) {
                break;  // Empty
            }

            // Validate event data - SECURITY CRITICAL!
            // 1. Check workflow_id matches
            if (user_event->workflow_id != workflow_id) {
                kprintf("[SYSCALL] WARNING: Event workflow_id=%lu != %lu\n",
                        user_event->workflow_id, workflow_id);
                continue;
            }

            // 2. Validate payload size (max 512 bytes as per RingEvent definition)
            #define MAX_EVENT_PAYLOAD_SIZE 512
            if (user_event->payload_size > MAX_EVENT_PAYLOAD_SIZE) {
                kprintf("[SYSCALL] ERROR: Invalid payload size %u (max %d), skipping event\n",
                        user_event->payload_size, MAX_EVENT_PAYLOAD_SIZE);
                continue;
            }

            // 3. Validate event type (basic sanity check)
            #define MAX_EVENT_TYPE 255
            if (user_event->type > MAX_EVENT_TYPE) {
                kprintf("[SYSCALL] WARNING: Suspicious event type %u, continuing anyway\n",
                        user_event->type);
            }

            // Assign unique ID and timestamp
            user_event->id = atomic_increment_u64(&global_event_id_counter);
            user_event->timestamp = rdtsc();

            kprintf("[SYSCALL] Event ID=%lu, type=%u, route=[%u,%u,%u,%u]\n",
                    user_event->id, user_event->type,
                    user_event->route[0], user_event->route[1],
                    user_event->route[2], user_event->route[3]);

            // Add to Routing Table
            routing_table_add_event(&global_routing_table, user_event);

            processed++;
        }

        kprintf("[SYSCALL] Processed %lu events from EventRing\n", processed);

        // Events are now in routing table.
        // ASYNC: They will be processed by timer IRQ in background!
        // User can call kernel_notify(WAIT) to block until completion.

        frame->rax = processed;  // Return number of events processed
        return;
    }

    // ========================================================================
    // MODE 2: WAIT - Block until workflow completes (COOPERATIVE YIELD!)
    // ========================================================================

    if (flags & NOTIFY_WAIT) {
        Workflow* workflow = workflow_get(workflow_id);

        if (!workflow) {
            kprintf("[SYSCALL] ERROR: Workflow ID=%lu not found\n", workflow_id);
            frame->rax = (uint64_t)-1;
            return;
        }

        kprintf("[SYSCALL] kernel_notify(WAIT) - checking completion\n");

        // Check if already completed (completion IRQ came during SUBMIT)
        if (atomic_load_u32(&proc->completion_ready)) {
            kprintf("[SYSCALL] Already completed (completion IRQ arrived during SUBMIT)\n");
            atomic_store_u32(&proc->completion_ready, 0);  // Clear for next time
            frame->rax = 0;
            return;
        }

        // *** EVENT-DRIVEN INNOVATION ***
        // Instead of busy-waiting, YIELD CPU to other processes!
        // This is the PRIMARY scheduling mechanism - not timer preemption!

        kprintf("[SYSCALL] Workflow not ready - COOPERATIVE YIELD (event-driven scheduling)\n");

        // Mark process as WAITING (so it won't be re-scheduled yet)
        proc->state = PROCESS_STATE_WAITING;

        // Save workflow ID so completion IRQ knows which process to wake
        proc->current_workflow_id = workflow_id;

        // YIELD CPU - let other processes run while we wait for event
        extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
        scheduler_yield_cooperative(frame);

        // When we return here, completion IRQ has woken us up!
        kprintf("[SYSCALL] Woke up from WAIT - workflow %lu completed\n", workflow_id);
        atomic_store_u32(&proc->completion_ready, 0);  // Clear for next time
        frame->rax = 0;  // Success
        return;
    }

    // ========================================================================
    // MODE 3: POLL - Check workflow status (non-blocking)
    // ========================================================================

    if (flags & NOTIFY_POLL) {
        Workflow* workflow = workflow_get(workflow_id);

        if (!workflow) {
            kprintf("[SYSCALL] ERROR: Workflow ID=%lu not found\n", workflow_id);
            frame->rax = (uint64_t)-1;
            return;
        }

        if (workflow->state == WORKFLOW_STATE_COMPLETED) {
            frame->rax = 0;  // Completed
        } else {
            frame->rax = 1;  // In progress
        }

        return;
    }

    // ========================================================================
    // MODE 4: YIELD - Cooperative yield (explicit CPU release)
    // ========================================================================

    if (flags & NOTIFY_YIELD) {
        kprintf("[SYSCALL] kernel_notify(YIELD) - explicit cooperative yield\n");

        // Explicitly give up CPU to other processes
        // This allows well-behaved processes to voluntarily yield
        // instead of consuming their entire time slice

        extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
        scheduler_yield_cooperative(frame);

        // When we return here, scheduler has run other processes
        kprintf("[SYSCALL] Resumed after YIELD\n");
        frame->rax = 0;  // Success
        return;
    }

    // ========================================================================
    // MODE 5: EXIT - Terminate current process
    // ========================================================================

    if (flags & NOTIFY_EXIT) {
        kprintf("[SYSCALL] kernel_notify(EXIT) - terminating process PID=%lu\n", proc->pid);

        // Mark process as TERMINATED
        proc->state = PROCESS_STATE_ZOMBIE;

        // Cleanup will be handled by scheduler
        // For now, just yield and don't come back
        extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
        scheduler_yield_cooperative(frame);

        // Should never reach here - scheduler won't schedule TERMINATED processes
        kprintf("[SYSCALL] ERROR: Returned from EXIT (should not happen!)\n");
        frame->rax = 0;
        return;
    }

    // Unknown flags
    kprintf("[SYSCALL] ERROR: Unknown flags 0x%lx\n", flags);
    frame->rax = (uint64_t)-1;
}

// ============================================================================
// COMPLETION IRQ HANDLER - Workflow completion notification
// ============================================================================

void completion_irq_handler(interrupt_frame_t* frame) {
    // Workflow completion notification from Execution Deck
    // This IRQ wakes up processes waiting in kernel_notify(WAIT)

    kprintf("[COMPLETION_IRQ] Workflow completion - waking waiting processes\n");

    // EVENT-DRIVEN INNOVATION: Wake up ALL waiting processes!
    // Each process will check its completion_ready flag
    // This is cooperative scheduling in action!

    // Get current running process
    process_t* current = process_get_current();

    // Set completion flag for current process (if any)
    if (current) {
        atomic_store_u32(&current->completion_ready, 1);
        kprintf("[COMPLETION_IRQ] Marked current PID=%lu as completed\n", current->pid);
    }

    // Wake up ALL waiting processes by adding them to ready queue
    // They will check their completion_ready flag when scheduled
    extern void scheduler_add_process(process_t* proc);
    extern process_t* process_get_by_index(int index);

    int woken = 0;
    for (int i = 0; i < PROCESS_MAX_COUNT; i++) {
        process_t* proc = process_get_by_index(i);

        if (proc && proc->state == PROCESS_STATE_WAITING) {
            // Wake up this process!
            scheduler_add_process(proc);
            woken++;
            kprintf("[COMPLETION_IRQ] Woke PID=%lu from WAITING\n", proc->pid);
        }
    }

    if (woken > 0) {
        kprintf("[COMPLETION_IRQ] Total processes woken: %d\n", woken);
    }

    // No EOI needed - this is a software interrupt
    (void)frame;  // Unused
}