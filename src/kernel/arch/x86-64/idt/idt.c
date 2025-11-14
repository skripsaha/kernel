#include "idt.h"
#include "gdt.h"
#include "klib.h"
#include "io.h"
#include "pic.h"  // ИСПРАВЛЕНО: добавлен include
#include "pit.h"  // PIT timer driver
#include "task.h" // Task scheduler
#include "keyboard.h" // Keyboard driver
#include "vmm.h"  // VMM for page fault handling

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_desc;

// Счетчики для статистики
static uint64_t exception_count = 0;
static uint64_t irq_count[16] = {0};

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
    
    // Оставшиеся записи (48-255) пока пустые - будут вызывать General Protection Fault
    for (int i = 48; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, (uint64_t)isr_table[13], GDT_KERNEL_CODE, IDT_TYPE_INTERRUPT_GATE, 0); // GPF handler
    }
    
    kprintf("[IDT] IDT configured with %d entries\n", IDT_ENTRIES);
    kprintf("[IDT] IDT base: 0x%p, limit: %d\n", (void*)idt_desc.base, idt_desc.limit);
    
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

            // Run task scheduler (switch tasks if needed)
            task_scheduler_tick();

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