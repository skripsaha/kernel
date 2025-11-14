[BITS 64]
[EXTERN kernel_main]
[EXTERN pmm_init]
[EXTERN __bss_start]
[EXTERN __bss_end]

section .text
global _start

global write_port
global read_port

global get_gdt_base

global clear_screen_vga
global hide_cursor

global user_experience_level

_start:
    ; Отладочное сообщение через последовательный порт
    mov al, 'K'
    mov dx, 0x3f8
    out dx, al
    
    ; Очистка сегментных регистров
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
   
    ; Настройка стека (moved above BSS to 0x510000)
    mov rsp, 0x510000
    mov rbp, rsp
    
    ; Еще одно отладочное сообщение
    mov al, 'S'
    mov dx, 0x3f8
    out dx, al

    mov al, 'M'
    mov dx, 0x3f8
    out dx, al

    ; === CRITICAL: Zero out BSS section ===
    ; All uninitialized global variables must be zeroed
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi                ; RCX = size of BSS in bytes
    shr rcx, 3                  ; Divide by 8 (we're clearing 8 bytes at a time)
    xor rax, rax                ; RAX = 0
    rep stosq                   ; Zero out BSS

    ; Debug message after BSS zeroing
    mov al, 'B'
    mov dx, 0x3f8
    out dx, al

    ; Подготовка параметров для ядра (ПОСЛЕ очистки BSS!)
    mov rdi, 0x500               ; Адрес e820 карты (обновлен с 0x90000)
    movzx rsi, word [0x4FE]      ; Кол-во записей E820 (обновлен с 0x8FFE)
    mov rdx, 0x100000            ; Начало доступной памяти (1MB)

    ; call pick_user_experience

    ; ; Вызов анимации загрузки для красоты
    ; call loading_animation

    ; call check_hyperthreading
    ; cmp eax, 1
    ; ja .ht_active
    ; ; HT неактивен или не поддерживается
    ; jmp .ht_inactive

    ; .ht_active:
    ;     ; Отображаем вопрос
    ;     mov rdi, htt_good
    ;     mov ah, 0x0F
    ;     mov rcx, 1650 ; Позиция
    ;     call print_string_vga
    
    ; .ht_inactive:
    ;     ; Отображаем вопрос
    ;     mov rdi, htt_bad
    ;     mov ah, 0x0F
    ;     mov rcx, 1650 ; Позиция
    ;     call print_string_vga

        
    ; ; Hyper-Threading активен, в EAX количество логических процессоров на ядро
    ; ; Можно использовать для инициализации планировщика

    ; call delay

    ; Вызов main функции ядра
    call kernel_main
    
    ; Остановка если ядро вернулось
    cli
    hlt
    jmp $

pick_user_experience:
    call clear_screen_vga
    call hide_cursor

    ; Отображаем подсказку
    mov rdi, msg_pick_tip
    mov ah, 0x07
    mov rcx, 3562 ; Позиция
    call print_string_vga

    ; Отображаем вопрос
    mov rdi, msg_pick_level_question
    mov ah, 0x0F
    mov rcx, 1650 ; Позиция
    call print_string_vga

    ; Инициализация выбора (0 = newbie, 1 = programmer, 2 = gamer)
    mov byte [current_selection], 0
    call update_selection_display

.keyboard_loop:
    ; Читаем клавишу
    call read_keyboard
    cmp al, 0x1C ; Enter
    je .selection_made
    cmp al, 0x4B ; Стрелка влево
    je .move_left
    cmp al, 0x4D ; Стрелка вправо
    je .move_right
    jmp .keyboard_loop

.move_left:
    cmp byte [current_selection], 0
    je .keyboard_loop ; Уже на первом варианте
    dec byte [current_selection]
    call update_selection_display
    jmp .keyboard_loop

.move_right:
    cmp byte [current_selection], 2
    je .keyboard_loop ; Уже на последнем варианте
    inc byte [current_selection]
    call update_selection_display
    jmp .keyboard_loop

.selection_made:
    ; Сохраняем выбор (0 = newbie, 1 = programmer, 2 = gamer)
    mov al, [current_selection]
    mov [user_experience_level], al
    ret

update_selection_display:
    ; Отображаем опцию newbie
    mov rdi, msg_pick_newbie
    mov ah, 0x8F ; Серый на черном (не выбран)
    cmp byte [current_selection], 0
    jne .not_selected_newbie
    mov ah, 0x7F ; Зеленый на черном (выбран)
.not_selected_newbie:
    mov rcx, 2114 ; Позиция
    call print_string_vga

    ; Отображаем опцию programmer
    mov rdi, msg_pick_programmer
    mov ah, 0x8F ; Серый на черном (не выбран)
    cmp byte [current_selection], 1
    jne .not_selected_programmer
    mov ah, 0x7F ; Зеленый на черном (выбран)
.not_selected_programmer:
    mov rcx, 2142 ; Позиция
    call print_string_vga

    ; Отображаем опцию gamer
    mov rdi, msg_pick_gamer
    mov ah, 0x8F ; Серый на черном (не выбран)
    cmp byte [current_selection], 2
    jne .not_selected_gamer
    mov ah, 0x7F ; Зеленый на черном (выбран)
.not_selected_gamer:
    mov rcx, 2178 ; Позиция (смещение на 36 символов от предыдущего)
    call print_string_vga
    ret


read_keyboard:
    ; Ждем, пока буфер клавиатуры не станет доступным
    mov dx, 0x64
    in al, dx
    test al, 1
    jz read_keyboard
    
    ; Читаем скан-код
    mov dx, 0x60
    in al, dx
    ret

loading_animation:
    call clear_screen_vga ; Очищаем экран

    mov rdi, loading_anim_entry_msg ; Строка
    mov ah, 0x0F
    mov rcx, 1986 ; Позиция
    call print_string_vga

    mov edi, 0xB8000
    add edi, 1810 ; Середина для 30 клеток загрузки
    mov ecx, 0
    mov rbx, 80000000 ; Грубо - длина задержки


_loop_start:

    call delay ; Вызываем задержку при итерации
    mov al, ' '
    mov ah, 0x2F ; Зеленая ячейка
    mov [edi], ax
    add edi, 2

    inc ecx
    cmp ecx, 30
    jl _loop_start

    call delay

write_port:
    mov dx, di      ; порт (в 1-м аргументе — rdi)
    mov al, sil     ; байт данных (в 2-м аргументе — sil)
    out dx, al
    ret


read_port:
    mov dx, di      ; порт (в rdi)
    in al, dx
    movzx eax, al   ; расширение до 32 бит без знака
    ret

;CPUID check hyper-threding
; Input: none
; Output: RAX = количество логических процессоров на физическое ядро
;         (1 = HT отключен/не поддерживается, >1 = HT активен)
; Сохраняет: RBX, RCX, RDX (кроме возвращаемых значений)
check_hyperthreading:
    push rbx
    push rcx
    push rdx
    
    ; 1. Проверяем поддержку CPUID функции 1
    mov eax, 0x00000001
    cpuid
    
    ; 2. Проверяем бит 28 (HTT) в EDX
    test edx, (1 << 28)
    jz .no_hyperthreading  ; HT не поддерживается
    
    ; 3. Получаем количество логических процессоров из EBX[23:16]
    mov eax, ebx
    shr eax, 16
    and eax, 0xFF          ; Максимальное число логических процессоров
    
    ; 4. Если значение 0, значит что-то не так - считаем что HT отключен
    test eax, eax
    jz .no_hyperthreading
    
    ; 5. Проверяем расширенную топологию (функция 0xB) если доступна
    mov eax, 0x00000000    ; Максимальная стандартная функция
    cpuid
    cmp eax, 0x0000000B
    jb .done               ; Функция 0xB не поддерживается
    
    ; 6. Используем расширенную топологию для точного определения
    mov eax, 0x0000000B    ; Extended Topology Enumeration
    xor ecx, ecx           ; Level 0 (SMT уровень)
    cpuid
    
    ; 7. В EBX[15:0] - количество логических процессоров на уровне SMT
    test eax, eax          ; Если уровень невалиден
    jz .done
    
    mov eax, ebx
    and eax, 0xFFFF        ; Берем младшие 16 бит
    jmp .done

.no_hyperthreading:
    mov eax, 1             ; Возвращаем 1 = HT неактивен

.done:
    pop rdx
    pop rcx
    pop rbx
    ret

; Получить базовый адрес GDT

get_gdt_base:

    ; Временный буфер для GDTR
    sub rsp, 16
    sgdt [rsp]
    
    ; Загружаем базовый адрес (8 байт) из GDTR
    mov rax, [rsp + 2]  ; Пропускаем limit (2 байта), берем base
    
    add rsp, 16
    ret


; Загрузить новый GDT
global load_gdt

load_gdt:
    lgdt [rdi]
    
    ; Перезагружаем сегментные регистры
    mov ax, 0x20        ; Kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    ; Перезагружаем CS через far return
    push 0x18           ; Kernel code segment
    lea rax, [rel .reload_cs]
    push rax
    retfq
    
.reload_cs:
    ret

; Полезные функции
delay:
    push rcx ; Сохраняем rcx
    mov rcx, rbx ; Передаем длительность задержки (грубо кол-во итераций)
.delay_loop:
    nop ; Пустая инструкция (иллюзия ожидания)
    loop .delay_loop
    pop rcx ; Восстанавливаем rcx
    ret 

; VGA Функции
print_string_vga:
    ; Сохраняем регистры
    push rdi
    push rbx
    push rax
    push rcx

    mov rbx, 0xB8000
    add rbx, rcx
    mov ah, ah

.print_loop:
    mov al, [rdi]
    test al, al
    jz .done

    mov [rbx], ax
    add rbx, 2

    inc rdi

    jmp .print_loop

.done:
    pop rcx
    pop rax
    pop rbx
    pop rdi
    ret


clear_screen_vga:
    ; Сохраняем регистры
    push rdi
    push rcx
    push rax

    mov rdi, 0xB8000
    mov rcx, 2000

    mov al, ' '
    mov ah, 0x07

    rep stosw

    pop rax
    pop rcx
    pop rdi
    ret

;Функция чтобы скрыть аппаратый курсор за экран
hide_cursor:
    mov dx, 0x3D4
    mov al, 0x0E
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al
    
    mov dx, 0x3D4  
    mov al, 0x0F
    out dx, al
    mov dx, 0x3D5
    mov al, 0xFF
    out dx, al
    ret

; === DATA SECTION ===
loading_anim_entry_msg db "BoxOS loading..." , 0
msg_pick_newbie db "I am newbie" , 0
msg_pick_programmer db "I am programmer", 0
msg_pick_gamer db "I am gamer", 0
msg_pick_level_question db "What's your current level?", 0
htt_good db "HTT is okay bro!", 0
htt_bad db "HTT don't available!", 0
msg_pick_tip db "Use ", 0x1A, " and ", 0x1B, " to choose the right one", 0

user_experience_level db 0

; === BSS SECTION ===
section .bss
current_selection resb 1
