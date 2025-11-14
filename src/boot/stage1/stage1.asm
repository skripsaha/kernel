[BITS 16]
[ORG 0x7C00]

; ===================================================================
; STAGE1 - MBR Bootloader (512 bytes)
; ===================================================================
; Loads Stage2 from disk and transfers control
; Memory layout:
;   0x7C00 - Stage1 (this code)
;   0x8000 - Stage2 (9 sectors, 4608 bytes)
; ===================================================================

; === CONSTANTS ===
STAGE2_LOAD_SEGMENT  equ 0x0000
STAGE2_LOAD_OFFSET   equ 0x8000
STAGE2_START_SECTOR  equ 2
STAGE2_SECTOR_COUNT  equ 9
STAGE2_SIGNATURE     equ 0x2907
BOOT_DISK            equ 0x80

start_stage1:
    ; Setup segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; Stack grows down from bootloader
    
    ; Clear screen
    mov ax, 0x0003
    int 0x10

    ; Print loading message
    mov si, msg_loading
    call print_string

    ; Reset disk controller
    mov ah, 0x00
    mov dl, BOOT_DISK
    int 0x13
    jc disk_error

    ; Load Stage2 from disk (sectors 2-10, 9 sectors = 4608 bytes)
    mov ah, 0x02                    ; BIOS read function
    mov al, STAGE2_SECTOR_COUNT     ; Number of sectors to read
    mov ch, 0                       ; Cylinder 0
    mov cl, STAGE2_START_SECTOR     ; Starting sector (2)
    mov dh, 0                       ; Head 0
    mov dl, BOOT_DISK               ; Boot disk
    mov bx, STAGE2_LOAD_OFFSET      ; Load address ES:BX = 0x0000:0x8000
    int 0x13
    jc disk_error

    ; CRITICAL: Check if all sectors were read
    cmp al, STAGE2_SECTOR_COUNT
    jne sector_count_error

    ; Verify Stage2 signature
    mov ax, [STAGE2_LOAD_OFFSET]
    cmp ax, STAGE2_SIGNATURE
    jne stage2_error

    ; Transfer control to Stage2
    mov si, msg_stage2
    call print_string
    jmp STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET


disk_error:
    mov si, msg_disk_error
    call print_string
    jmp hang

sector_count_error:
    mov si, msg_sector_error
    call print_string
    jmp hang

stage2_error:
    mov si, msg_stage2_error
    call print_string
    jmp hang

hang:
    cli
    hlt
    jmp hang

; === PRINT STRING FUNCTION ===
print_string:
    push ax
    push bx
    mov ah, 0x0E            ; BIOS teletype output
    mov bh, 0               ; Page 0
.loop:
    lodsb                   ; Load byte from SI into AL
    test al, al             ; Check for null terminator
    jz .done
    int 0x10                ; Print character
    jmp .loop
.done:
    pop bx
    pop ax
    ret

; === MESSAGES ===
msg_loading       db "BoxKernel Stage1 Loading...", 13, 10, 0
msg_stage2        db "Stage2 OK, jumping...", 13, 10, 0
msg_disk_error    db "DISK ERROR!", 13, 10, 0
msg_sector_error  db "SECTOR COUNT ERROR!", 13, 10, 0
msg_stage2_error  db "STAGE2 SIGNATURE FAIL!", 13, 10, 0

; === PADDING AND SIGNATURE ===
times 510-($-$$) db 0
dw 0xAA55                   ; Boot signature