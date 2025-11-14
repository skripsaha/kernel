[BITS 16]
[ORG 0x8000]

; ===================================================================
; STAGE2 - Advanced Bootloader (4096 bytes, 9 sectors)
; ===================================================================
; Responsibilities:
;   - Enable A20 line
;   - Detect memory (E820)
;   - Load kernel from disk
;   - Setup paging for long mode
;   - Enter 64-bit long mode
;   - Jump to kernel
; ===================================================================

; === MEMORY MAP ===
; 0x0500      - E820 memory map (safe low memory, up to 2KB)
; 0x7C00      - Stage1 (512 bytes)
; 0x8000      - Stage2 (4096 bytes) - THIS CODE
; 0x9000      - Boot info for kernel (256 bytes)
; 0x10000     - Kernel (148480 bytes = 290 sectors = 145KB)
; 0x34400     - End of kernel
; 0x33000     - BSS section (3.9MB uninitialized data)
; 0x3F0000    - End of BSS (~4MB mark)
; 0x500000    - Page tables (16KB: PML4, PDPT, PD, PT) - MOVED ABOVE BSS!
; 0x510000    - Stack for 32/64-bit modes (grows downward) - MOVED ABOVE BSS!

; === CONSTANTS ===
KERNEL_LOAD_ADDR      equ 0x10000
KERNEL_SECTOR_START   equ 10
KERNEL_SECTOR_COUNT   equ 290
KERNEL_SIZE_BYTES     equ 148480        ; 290 * 512
KERNEL_END_ADDR       equ 0x34400       ; 0x10000 + 0x24400 (148480 bytes)

PAGE_TABLE_BASE       equ 0x500000      ; MOVED: Above kernel BSS (was 0x70000)
E820_MAP_ADDR         equ 0x500         ; Low memory (safe after BIOS data area)
E820_COUNT_ADDR       equ 0x4FE         ; Just before E820 map
E820_SIZE_ADDR        equ 0x4FC         ; Just before count
STACK_BASE            equ 0x510000      ; MOVED: Stack above page tables (was 0x90000)
BOOT_INFO_ADDR        equ 0x9000        ; After Stage2

BOOT_DISK             equ 0x80
STAGE2_SIGNATURE      equ 0x2907

; Signature for Stage1 verification
dw STAGE2_SIGNATURE

start_stage2:
    ; Disable interrupts during initialization
    cli

    ; Clear direction flag
    cld

    ; Setup segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Setup stack (16-byte aligned for future BIOS calls)
    mov ss, ax
    mov sp, 0x7C00          ; Stack below Stage1, grows downward

    ; Re-enable interrupts for BIOS calls
    sti

    ; Print startup message
    mov si, msg_stage2_start
    call print_string_16

    ; Enable A20 line
    call enable_a20_enhanced

    ; Detect memory with E820
    call detect_memory_e820

    ; Load kernel from disk
    call load_kernel_simple

    ; Check CPU compatibility (long mode support)
    call check_long_mode_support

    ; Entering protected mode
    mov si, msg_entering_protected
    call print_string_16

    ; Disable interrupts before mode switch
    cli

    ; Load GDT
    lgdt [gdt_descriptor]

    ; Enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump to flush pipeline and load CS
    jmp 0x08:protected_mode_start

[BITS 32]
protected_mode_start:
    ; Setup segments in 32-bit mode
    mov ax, 0x10        ; Data segment selector (GDT entry 2)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack (16-byte aligned)
    mov esp, STACK_BASE
    
    ; Очистка EFLAGS
    push dword 0
    popf
    
    ; Сообщение о входе в защищенный режим
    mov edi, 0xB8000
    mov al, 'P'
    mov ah, 0x0F
    mov [edi], ax
    mov al, 'M'
    mov [edi+2], ax
    
    ; Инициализация страничной адресации
    call setup_paging_fixed
    
    ; Переход в long mode
    call enable_long_mode_fixed
    
    ; Far jump в 64-bit режим
    jmp 0x18:long_mode_start

[BITS 64]
long_mode_start:
    ; Setup segments in 64-bit mode
    mov ax, 0x20        ; Data segment selector (GDT entry 4)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Setup stack for 64-bit mode (16-byte aligned)
    mov rsp, STACK_BASE
    
    ; Очистка RFLAGS
    push qword 0
    popf
    
    ; Сообщение о входе в long mode
    mov rdi, 0xB8000
    mov al, 'L'
    mov ah, 0x0F
    mov [rdi+4], ax
    mov al, 'M'
    mov [rdi+6], ax
    
    ; Prepare parameters for kernel
    mov rdi, E820_MAP_ADDR              ; E820 memory map address
    movzx rsi, word [E820_COUNT_ADDR]   ; E820 entry count

    ; Verify kernel was loaded (check first 8 bytes)
    mov rax, [KERNEL_LOAD_ADDR]
    test rax, rax
    jz .kernel_not_loaded

    ; Save boot info for kernel (at BOOT_INFO_ADDR)
    mov [BOOT_INFO_ADDR], dword E820_MAP_ADDR   ; E820 map address
    mov ax, [E820_COUNT_ADDR]
    mov [BOOT_INFO_ADDR+4], ax                  ; E820 entry count
    mov [BOOT_INFO_ADDR+8], dword KERNEL_LOAD_ADDR ; Kernel load address
    mov [BOOT_INFO_ADDR+12], dword KERNEL_END_ADDR ; Kernel end address

    ; Jump to kernel entry point
    jmp KERNEL_LOAD_ADDR
    
.kernel_not_loaded:
    ; Сообщение об ошибке
    mov rdi, 0xB8000
    mov al, 'N'
    mov ah, 0x04        ; Красный цвет
    mov [rdi+8], ax
    mov al, 'K'
    mov [rdi+10], ax
    jmp .halt
    
.halt:
    cli
    hlt
    jmp $

[BITS 16]
; ===== 16-BIT MODE FUNCTIONS =====

; Check if CPU supports long mode (64-bit)
check_long_mode_support:
    push eax
    push ebx
    push ecx
    push edx

    ; Check if CPUID is supported
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 0x00200000         ; Flip ID bit
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_cpuid                ; ID bit didn't change, no CPUID

    ; Check if extended CPUID functions are available
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode            ; Extended functions not available

    ; Check for long mode support
    mov eax, 0x80000001
    cpuid
    test edx, (1 << 29)         ; LM bit (bit 29)
    jz .no_long_mode

    ; Long mode is supported
    mov si, msg_long_mode_ok
    call print_string_16

    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

.no_cpuid:
    mov si, msg_no_cpuid
    call print_string_16
    jmp .halt

.no_long_mode:
    mov si, msg_no_long_mode
    call print_string_16
    jmp .halt

.halt:
    cli
    hlt
    jmp $

print_string_16:
    push ax
    push bx
    push si
    
    mov ah, 0x0E
    mov bh, 0
    
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
    
.done:
    pop si
    pop bx
    pop ax
    ret

wait_key:
    push ax
    mov ah, 0x00
    int 0x16
    pop ax
    ret

; Улучшенная функция включения A20
enable_a20_enhanced:
    push ax
    push cx
    
    ; Сначала проверим, не включена ли A20 уже
    call test_a20
    jnc .a20_done
    
    ; Метод 1: BIOS function
    mov ax, 0x2401
    int 0x15
    call test_a20
    jnc .a20_done
    
    ; Метод 2: Keyboard controller
    call a20_wait
    mov al, 0xAD        ; Disable keyboard
    out 0x64, al
    
    call a20_wait
    mov al, 0xD0        ; Read output port
    out 0x64, al
    
    call a20_wait2
    in al, 0x60
    push ax
    
    call a20_wait
    mov al, 0xD1        ; Write output port
    out 0x64, al
    
    call a20_wait
    pop ax
    or al, 2            ; Set A20 bit
    out 0x60, al
    
    call a20_wait
    mov al, 0xAE        ; Enable keyboard
    out 0x64, al
    
    call a20_wait
    call test_a20
    jnc .a20_done
    
    ; Метод 3: Fast A20 (port 0x92)
    in al, 0x92
    test al, 2
    jnz .a20_done
    or al, 2
    and al, 0xFE
    out 0x92, al
    
.a20_done:
    pop cx
    pop ax
    
    mov si, msg_a20_enabled
    call print_string_16
    ret

; Тест A20 линии
test_a20:
    push ax
    push bx
    push es
    push ds
    
    ; Устанавливаем сегменты для теста
    xor ax, ax
    mov es, ax
    mov ds, ax
    
    mov bx, 0x7DFE      ; Адрес в первом мегабайте
    mov al, [es:bx]     ; Сохраняем оригинальное значение
    push ax
    
    mov ax, 0xFFFF
    mov es, ax
    mov bx, 0x7E0E      ; Соответствующий адрес во втором мегабайте
    mov ah, [es:bx]     ; Сохраняем оригинальное значение
    push ax
    
    ; Записываем тестовые значения
    mov byte [es:bx], 0x00
    xor ax, ax
    mov es, ax
    mov byte [es:0x7DFE], 0xFF
    
    ; Проверяем
    mov ax, 0xFFFF
    mov es, ax
    cmp byte [es:bx], 0xFF
    
    ; Восстанавливаем значения
    pop ax
    mov [es:bx], ah
    xor ax, ax
    mov es, ax
    pop ax
    mov [es:0x7DFE], al
    
    pop ds
    pop es
    pop bx
    pop ax
    
    ; CF=0 если A20 включена, CF=1 если выключена
    je .a20_disabled
    clc
    ret
.a20_disabled:
    stc
    ret

a20_wait:
    in al, 0x64
    test al, 2
    jnz a20_wait
    ret

a20_wait2:
    in al, 0x64
    test al, 1
    jz a20_wait2
    ret

load_kernel_simple:
    mov si, msg_loading_kernel
    call print_string_16

    ; Проверка поддержки INT 13h Extensions
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, 0x80
    int 0x13
    jc .use_chs          ; Если не поддерживается, используем CHS

    ; Используем INT 13h Extensions (LBA)
    ; Загружаем 266 секторов (133KB) начиная с LBA 10

    ; Часть 1: 127 секторов
    mov si, dap1
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 2: 127 секторов
    mov si, dap2
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error

    ; Часть 3: 26 секторов (remaining: 280 - 127 - 127 = 26)
    mov si, dap3
    mov ah, 0x42
    mov dl, 0x80
    int 0x13
    jc .disk_error
    jmp .check_kernel

.use_chs:
    ; Загружаем меньшими порциями, не переходя границу дорожки
    ; Total: 250 sectors (53+63+63+63+8)
    ; Часть 1: 53 сектора (сектора 11-63 на головке 0) → 0x10000
    mov ah, 0x02
    mov al, 53
    mov ch, 0
    mov cl, 11
    mov dh, 0
    mov dl, 0x80
    mov bx, 0x1000
    mov es, bx
    mov bx, 0x0000
    int 0x13
    jc .disk_error

    ; Часть 2: 63 сектора (вся головка 1) → 0x16A00
    mov ah, 0x02
    mov al, 63
    mov ch, 0
    mov cl, 1
    mov dh, 1
    mov dl, 0x80
    mov bx, 0x16A0
    mov es, bx
    mov bx, 0x0000
    int 0x13
    jc .disk_error

    ; Часть 3: 63 сектора (вся головка 2) → 0x1E800
    mov ah, 0x02
    mov al, 63
    mov ch, 0
    mov cl, 1
    mov dh, 2
    mov dl, 0x80
    mov bx, 0x1E80
    mov es, bx
    mov bx, 0x0000
    int 0x13
    jc .disk_error

    ; Часть 4: 63 сектора (вся головка 3) → 0x26600
    mov ah, 0x02
    mov al, 63
    mov ch, 0
    mov cl, 1
    mov dh, 3
    mov dl, 0x80
    mov bx, 0x2660
    mov es, bx
    mov bx, 0x0000
    int 0x13
    jc .disk_error

    ; Часть 5: 8 секторов (головка 4) → 0x2E400
    mov ah, 0x02
    mov al, 8
    mov ch, 0
    mov cl, 1
    mov dh, 4
    mov dl, 0x80
    mov bx, 0x2E40
    mov es, bx
    mov bx, 0x0000
    int 0x13
    jc .disk_error

.check_kernel:
    
    ; Проверка загрузки (проверяем первые 4 байта)
    mov ax, 0x1000
    mov es, ax
    mov bx, 0x0000
    mov eax, [es:bx]
    test eax, eax
    jz .empty_kernel
    
    ; Восстановка ES
    xor ax, ax
    mov es, ax
    
    mov si, msg_kernel_loaded
    call print_string_16
    ret
    
.empty_kernel:
    xor ax, ax
    mov es, ax
    mov si, msg_kernel_empty
    call print_string_16
    ret
    
.disk_error:
    mov si, msg_disk_error
    call print_string_16
    call wait_key
    cli
    hlt

detect_memory_e820:
    mov si, msg_detecting_memory
    call print_string_16

    ; Очистим e820 буфер (0x500 = segment 0x50, offset 0)
    mov ax, 0x50
    mov es, ax
    xor di, di
    mov cx, 1024        ; Увеличиваем буфер
    xor ax, ax
    rep stosw

    ; E820 detection
    xor ebx, ebx
    mov edx, 0x534D4150    ; 'SMAP'
    mov ax, 0x50
    mov es, ax
    xor di, di
    xor bp, bp             ; Счетчик записей

.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150
    int 0x15
    jc .e820_fail
    
    ; Проверяем подпись
    cmp eax, 0x534D4150
    jne .e820_fail
    
    ; Проверяем размер записи
    cmp ecx, 20
    jl .skip_entry
    
    ; Увеличиваем счетчик и указатель
    inc bp
    add di, 24
    
.skip_entry:
    ; Check if more entries exist
    test ebx, ebx
    jnz .e820_loop

    ; Восстанавливаем ES перед сохранением
    xor ax, ax
    mov es, ax

    ; Save entry count
    mov [E820_COUNT_ADDR], bp

    ; Calculate total size: bp * 24 bytes per entry
    ; 24 = 16 + 8, so: (bp << 4) + (bp << 3)
    mov ax, bp
    shl ax, 4               ; AX = bp * 16
    mov cx, bp
    shl cx, 3               ; CX = bp * 8
    add ax, cx              ; AX = bp * 24
    mov [E820_SIZE_ADDR], ax

    mov si, msg_e820_success
    call print_string_16
    ret

.e820_fail:
    mov si, msg_e820_fail
    call print_string_16

    ; Fallback: create minimal memory map at 0x500
    mov ax, 0x50
    mov es, ax
    xor di, di

    ; Entry 1: 0-640KB (usable RAM)
    mov dword [es:di], 0x00000000      ; Base address low
    mov dword [es:di+4], 0x00000000    ; Base address high
    mov dword [es:di+8], 0x0009FC00    ; Length: 640KB
    mov dword [es:di+12], 0x00000000   ; Length high
    mov dword [es:di+16], 1            ; Type: usable
    mov dword [es:di+20], 0            ; Extended attributes
    add di, 24

    ; Entry 2: 1MB+ (detect size via INT 15h AH=88h)
    mov ah, 0x88
    int 0x15
    jc .memory_fail

    mov dword [es:di], 0x00100000      ; Base: 1MB
    mov dword [es:di+4], 0x00000000
    movzx eax, ax
    shl eax, 10                        ; Convert KB to bytes
    mov [es:di+8], eax
    mov dword [es:di+12], 0x00000000
    mov dword [es:di+16], 1            ; Type: usable
    mov dword [es:di+20], 0

    ; Восстанавливаем ES
    xor ax, ax
    mov es, ax

    ; Save entry count and size (2 entries * 24 bytes = 48)
    mov word [E820_COUNT_ADDR], 2
    mov word [E820_SIZE_ADDR], 48

    mov si, msg_memory_fallback
    call print_string_16
    ret

.memory_fail:
    xor ax, ax
    mov es, ax
    mov si, msg_memory_error
    call print_string_16
    ret

[BITS 32]
; ===== ФУНКЦИИ 32-BIT РЕЖИМА =====

setup_paging_fixed:
    ; Очистка области для таблиц страниц (16KB) - now at 0x500000
    mov edi, PAGE_TABLE_BASE
    mov ecx, 4096          ; 16KB / 4 = 4096 dwords
    xor eax, eax
    rep stosd

    ; PML4 Table (0x500000) - только первая запись (64-bit entry)
    mov dword [PAGE_TABLE_BASE], PAGE_TABLE_BASE + 0x1000 + 3  ; PDPT at +4KB
    mov dword [PAGE_TABLE_BASE + 4], 0x00000000

    ; PDPT (0x501000) - только первая запись (64-bit entry)
    mov dword [PAGE_TABLE_BASE + 0x1000], PAGE_TABLE_BASE + 0x2000 + 3  ; PD at +8KB
    mov dword [PAGE_TABLE_BASE + 0x1004], 0x00000000

    ; PD (0x502000) - маппинг первых 32MB как 2MB страницы (для безопасности)
    mov edi, PAGE_TABLE_BASE
    add edi, 0x2000       ; PD offset
    mov eax, 0x000083     ; Present, Writable, Page Size (2MB)
    mov ecx, 16           ; 16 записей по 2MB = 32MB (was 8 entries = 16MB)

.fill_pd:
    mov [edi], eax        ; Lower 32 bits
    mov dword [edi+4], 0  ; Upper 32 bits (explicit zero)
    add eax, 0x200000     ; Следующие 2MB
    add edi, 8
    loop .fill_pd

    ret

enable_long_mode_fixed:
    ; Включение PAE в CR4
    mov eax, cr4
    or eax, (1 << 5)      ; PAE bit
    mov cr4, eax

    ; Загрузка PML4 в CR3 (now at 0x500000)
    mov eax, PAGE_TABLE_BASE
    mov cr3, eax
    
    ; Включение Long Mode в EFER
    mov ecx, 0xC0000080   ; EFER MSR
    rdmsr
    or eax, (1 << 8)      ; LME bit
    wrmsr
    
    ; Включение paging в CR0
    mov eax, cr0
    or eax, (1 << 31)     ; PG bit
    mov cr0, eax
    
    ret

; ===== ИСПРАВЛЕННЫЙ GDT =====
align 8
gdt_start:
    ; 0x00: Null Descriptor
    dq 0x0000000000000000

    ; 0x08: 32-bit Kernel Code Segment
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x9A         ; Access: Present, Ring 0, Code, Executable, Readable
    db 0xCF         ; Flags: 4KB granularity, 32-bit, Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

    ; 0x10: 32-bit Kernel Data Segment
    dw 0xFFFF       ; Limit 15:0
    dw 0x0000       ; Base 15:0
    db 0x00         ; Base 23:16
    db 0x92         ; Access: Present, Ring 0, Data, Writable
    db 0xCF         ; Flags: 4KB granularity, 32-bit, Limit 19:16 = 0xF
    db 0x00         ; Base 31:24

    ; 0x18: 64-bit Kernel Code Segment
    dw 0x0000       ; Limit (ignored in 64-bit)
    dw 0x0000       ; Base (ignored in 64-bit)
    db 0x00         ; Base (ignored in 64-bit)
    db 0x9A         ; Access: Present, Ring 0, Code, Executable, Readable
    db 0x20         ; Flags: Long mode bit (L=1), все остальные 0
    db 0x00         ; Base (ignored in 64-bit)

    ; 0x20: 64-bit Kernel Data Segment
    dw 0x0000       ; Limit (ignored in 64-bit)
    dw 0x0000       ; Base (ignored in 64-bit)
    db 0x00         ; Base (ignored in 64-bit)
    db 0x92         ; Access: Present, Ring 0, Data, Writable
    db 0x00         ; Flags: (все биты 0 для data segment)
    db 0x00         ; Base (ignored in 64-bit)

gdt_end:

align 4
gdt_descriptor:
    dw gdt_end - gdt_start - 1    ; Limit
    dd gdt_start                  ; Base address (32-bit в 16-bit режиме)

; ===== DAP STRUCTURES FOR INT 13h EXTENSIONS (LBA MODE) =====
; Total: 250 sectors = 125KB
; Part 1: 127 sectors (max single read) → 0x10000
; Part 2: 123 sectors (250-127)        → 0x1FE00
align 4
dap1:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 127              ; Sector count: 127 (maximum per INT 13h call)
    dw 0x0000           ; Offset
    dw 0x1000           ; Segment (0x1000:0x0000 = 0x10000 physical)
    dq 10               ; Starting LBA sector: 10

align 4
dap2:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 127              ; Sector count: 127
    dw 0x0000           ; Offset
    dw 0x1FE0           ; Segment (0x1FE0:0x0000 = 0x1FE00 physical)
    dq 137              ; Starting LBA sector: 137 (10 + 127)

align 4
dap3:
    db 0x10             ; DAP size (16 bytes)
    db 0                ; Reserved
    dw 36               ; Sector count: 36 (290 - 127 - 127 = 36)
    dw 0x0000           ; Offset
    dw 0x2FC0           ; Segment (0x2FC0:0x0000 = 0x2FC00 physical)
    dq 264              ; Starting LBA sector: 264 (10 + 127 + 127)

; ===== MESSAGES =====
msg_stage2_start      db 'BoxKernel Stage2 Started', 13, 10, 0
msg_a20_enabled       db '[OK] A20 line enabled', 13, 10, 0
msg_detecting_memory  db 'Detecting memory (E820)...', 13, 10, 0
msg_e820_success      db '[OK] E820 memory map created', 13, 10, 0
msg_e820_fail         db '[WARN] E820 failed, using fallback', 13, 10, 0
msg_memory_fallback   db '[OK] Fallback memory detection', 13, 10, 0
msg_memory_error      db '[ERROR] Memory detection failed!', 13, 10, 0
msg_loading_kernel    db 'Loading kernel (290 sectors)...', 13, 10, 0
msg_kernel_loaded     db '[OK] Kernel loaded (145KB)', 13, 10, 0
msg_kernel_empty      db '[WARN] Kernel appears empty', 13, 10, 0
msg_disk_error        db '[ERROR] Disk read failed!', 13, 10, 0
msg_long_mode_ok      db '[OK] CPU supports 64-bit mode', 13, 10, 0
msg_no_cpuid          db '[ERROR] CPUID not supported!', 13, 10, 0
msg_no_long_mode      db '[ERROR] 64-bit mode not supported!', 13, 10, 0
msg_entering_protected db 'Entering protected mode...', 13, 10, 0


; Заполнение до 4KB
times 4096-($-$$) db 0