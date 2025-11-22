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
#include "scheduler.h"  // Scheduler stats (for watchdog)
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

    // PRODUCTION: Check if exception came from user-space (Ring 3)
    // CS & 3 gives us the CPL (Current Privilege Level)
    // If CPL == 3, it's user-space; if CPL == 0, it's kernel-space
    int from_user_space = (frame->cs & 3) == 3;

    // Special handling for page faults - try to handle silently
    if (frame->vector == EXCEPTION_PAGE_FAULT) {
        uint64_t cr2;
        asm volatile("mov %%cr2, %0" : "=r" (cr2));

        // Try to handle the page fault
        if (vmm_handle_page_fault(cr2, frame->error_code) == 0) {
            // Successfully handled - no need to print anything
            return;
        }

        // PRODUCTION: User-space page fault - kill process, don't panic kernel!
        if (from_user_space) {
            extern process_t* process_get_current(void);
            process_t* current = process_get_current();

            if (current) {
                kprintf("\n%[E]=== USER PROCESS CRASH (Page Fault) ===%[D]\n");
                kprintf("%[E]PID: %lu%[D]\n", current->pid);
                kprintf("%[E]Faulting Address (CR2): 0x%llx%[D]\n", cr2);
                kprintf("%[E]Error Code: 0x%llx%[D] ", frame->error_code);
                kprintf("(P=%d W=%d U=%d R=%d I=%d)\n",
                        (frame->error_code & 1),      // Present
                        (frame->error_code >> 1) & 1, // Write
                        (frame->error_code >> 2) & 1, // User
                        (frame->error_code >> 3) & 1, // Reserved
                        (frame->error_code >> 4) & 1  // Instruction fetch
                );
                kprintf("%[E]RIP: 0x%llx%[D]\n", frame->rip);
                kprintf("%[E]RSP: 0x%llx%[D]\n", frame->rsp);
                kprintf("%[E]Killing process PID=%lu...%[D]\n\n", current->pid);

                // Mark process as zombie and yield to next process
                current->state = PROCESS_STATE_ZOMBIE;
                extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
                scheduler_yield_cooperative(frame);

                // scheduler_yield_cooperative modifies frame to switch to next process
                // When we return, IRETQ will jump to the new process
                return;
            }
        }

        // Kernel-space page fault (or no current process) - this is CRITICAL!
        if (cr2 != last_page_fault_addr || !page_fault_printed) {
            kprintf("\n%[E]=== KERNEL PAGE FAULT (CRITICAL!) ===%[D]\n");
            kprintf("%[E]Address (CR2): 0x%llx%[D]\n", cr2);
            kprintf("%[E]Error Code: 0x%llx%[D]\n", frame->error_code);
            kprintf("%[E]RIP: 0x%llx%[D]\n", frame->rip);
            last_page_fault_addr = cr2;
            page_fault_printed = 1;
            panic("Unhandled kernel page fault");
        }
        return;
    }

    // PRODUCTION: Handle user-space exceptions - kill process, don't panic kernel!
    if (from_user_space) {
        extern process_t* process_get_current(void);
        process_t* current = process_get_current();

        if (current) {
            kprintf("\n%[E]=== USER PROCESS CRASH (Exception) ===%[D]\n");
            kprintf("%[E]PID: %lu%[D]\n", current->pid);
            kprintf("%[E]Exception Vector: %llu ", frame->vector);

            // Print exception name
            switch(frame->vector) {
                case EXCEPTION_DIVIDE_ERROR:
                    kprintf("(Divide by Zero)%[D]\n");
                    break;
                case EXCEPTION_GENERAL_PROTECTION:
                    kprintf("(General Protection Fault)%[D]\n");
                    break;
                case 6: // Invalid Opcode
                    kprintf("(Invalid Opcode)%[D]\n");
                    break;
                case 11: // Segment Not Present
                    kprintf("(Segment Not Present)%[D]\n");
                    break;
                case 12: // Stack Segment Fault
                    kprintf("(Stack Segment Fault)%[D]\n");
                    break;
                default:
                    kprintf("(Unknown)%[D]\n");
                    break;
            }

            kprintf("%[E]Error Code: 0x%llx%[D]\n", frame->error_code);
            kprintf("%[E]RIP: 0x%llx%[D]\n", frame->rip);
            kprintf("%[E]RSP: 0x%llx%[D]\n", frame->rsp);
            kprintf("%[E]RFLAGS: 0x%llx%[D]\n", frame->rflags);
            kprintf("%[E]Killing process PID=%lu...%[D]\n\n", current->pid);

            // Mark process as zombie and yield to next process
            current->state = PROCESS_STATE_ZOMBIE;
            extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
            scheduler_yield_cooperative(frame);

            // scheduler_yield_cooperative modifies frame to switch to next process
            return;
        }
    }

    // Kernel-space exception - this is CRITICAL! Print full details and panic
    kprintf("\n%[E]=== KERNEL EXCEPTION (CRITICAL!) ===%[D]\n");
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
            kprintf("%[E]Divide by zero error in kernel!%[D]\n");
            panic("Kernel divide by zero");
            break;
        case EXCEPTION_GENERAL_PROTECTION:
            kprintf("%[E]General Protection Fault in kernel!%[D]\n");
            panic("Kernel General Protection Fault");
            break;
        case EXCEPTION_DOUBLE_FAULT:
            kprintf("%[E]Double Fault! System unstable!%[D]\n");
            panic("Double Fault");
            break;
        default:
            panic("Kernel exception");
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
            // Run guide EVERY tick (10ms at 100Hz) for responsive async processing
            extern void guide_process_all(void);
            guide_process_all();

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

    // PRODUCTION: Update watchdog timestamp (shows process is alive)
    proc->last_syscall_tick = scheduler_stats.total_ticks;
    proc->syscall_count++;

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

        kprintf("[SYSCALL] kernel_notify(SUBMIT) from RIP=0x%lx, RSP=0x%lx\n",
                frame->rip, frame->rsp);
        kprintf("[SYSCALL] EventRing: head=%u tail=%u empty=%d\n",
                event_ring->head, event_ring->tail,
                wf_event_ring_is_empty(event_ring));

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
        // ASYNC: They will be processed by guide_process_all() in timer IRQ (every tick)
        // Process can continue execution or call WAIT to block until completion

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
        // Yield to another process - scheduler will destroy this process and switch context
        extern void scheduler_yield_cooperative(interrupt_frame_t* frame);
        scheduler_yield_cooperative(frame);

        // CRITICAL: scheduler_restore_context() modified frame for new process.
        // Just return normally - IRETQ will use the modified frame and jump to new process!
        // We're still executing old process code, but that's OK - IRETQ switches context.

        // NOTE: This point should never be reached (new process takes over via IRETQ)
        // But if we somehow return here, just return success
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