[BITS 64]

section .text
global _start

; RingEvent structure (576 bytes) - from workflow_rings.h
; typedef struct {
;     uint64_t id;              // +0 (8)
;     uint64_t workflow_id;     // +8 (8)
;     uint32_t type;            // +16 (4)
;     [PADDING 4 bytes]         // +20 (4) - compiler alignment
;     uint64_t timestamp;       // +24 (8)
;     uint8_t route[8];         // +32 (8)
;     uint8_t payload[512];     // +40 (512)
;     uint32_t payload_size;    // +552 (4)
;     uint8_t _padding[20];     // +556 (20)
; } RingEvent __attribute__((aligned(64)));

; Ring buffer addresses (from kernel)
%define EVENT_RING_ADDR   0x20200000
; EventRing size: 64 (head) + 64 (tail) + 256*576 (events) = 147584 (0x24080)
%define RESULT_RING_ADDR  0x20224080

; Ring buffer sizes
%define EVENT_RING_SIZE   256
%define RESULT_RING_SIZE  256

; Event types
%define EVENT_FILE_CREATE_TAGGED 15

; Workflow ID
%define WORKFLOW_ID 2

; kernel_notify flags
%define NOTIFY_SUBMIT 0x01
%define NOTIFY_WAIT   0x02
%define NOTIFY_EXIT   0x10

_start:
    ; Build RingEvent on stack (576 bytes)
    sub rsp, 576
    mov rdi, rsp

    ; Zero the event structure
    mov rcx, 72
    xor rax, rax
.zero_loop:
    mov [rdi + rcx*8 - 8], rax
    loop .zero_loop

    ; Fill RingEvent
    mov qword [rdi + 0], 0
    mov qword [rdi + 8], WORKFLOW_ID
    mov dword [rdi + 16], EVENT_FILE_CREATE_TAGGED
    mov qword [rdi + 24], 0

    ; Route
    mov byte [rdi + 32], 2
    mov byte [rdi + 33], 0

    ; Payload: tag_count = 2
    mov dword [rdi + 40], 2

    ; Tag 1: name:test.txt
    lea rsi, [rel tag1_key]
    lea rdx, [rdi + 44]
    mov rcx, 32
    call copy_bytes

    lea rsi, [rel tag1_value]
    lea rdx, [rdi + 76]
    mov rcx, 64
    call copy_bytes

    ; Tag 2: type:document
    lea rsi, [rel tag2_key]
    lea rdx, [rdi + 140]
    mov rcx, 32
    call copy_bytes

    lea rsi, [rel tag2_value]
    lea rdx, [rdi + 172]
    mov rcx, 64
    call copy_bytes

    ; payload_size
    mov dword [rdi + 552], 196

    ; Push to EventRing
    mov r12, EVENT_RING_ADDR
    mov rax, [r12 + 64]
    mov rbx, [r12 + 0]

    mov rcx, rax
    sub rcx, rbx
    cmp rcx, EVENT_RING_SIZE
    jge .error

    mov rdx, rax
    and rdx, 0xFF

    imul rdx, 576
    lea r13, [r12 + 128 + rdx]

    mov rsi, rsp
    mov rdi, r13
    mov rcx, 72
    rep movsq

    inc rax
    mfence
    mov [r12 + 64], rax

    add rsp, 576

    ; kernel_notify(SUBMIT)
    mov rdi, WORKFLOW_ID
    mov rsi, NOTIFY_SUBMIT
    int 0x80

    ; kernel_notify(WAIT)
    mov rdi, WORKFLOW_ID
    mov rsi, NOTIFY_WAIT
    int 0x80

    ; Read from ResultRing
    mov r12, RESULT_RING_ADDR
    mov rax, [r12 + 0]
    mov rbx, [r12 + 64]

    cmp rax, rbx
    je .done

    mov rdx, rax
    and rdx, 0xFF

    imul rdx, 576
    lea r13, [r12 + 128 + rdx]

    mov r14, [r13 + 0]
    mov r15, [r13 + 24]

    inc rax
    mfence
    mov [r12 + 0], rax

.done:
    ; Workflow completed successfully - exit gracefully
    ; kernel_notify(NOTIFY_EXIT)
    mov rdi, 0              ; workflow_id (ignored for EXIT)
    mov rsi, NOTIFY_EXIT    ; flags = NOTIFY_EXIT
    int 0x80

    ; Should never reach here (kernel terminates process)
    ud2                     ; Invalid opcode - triple fault if we somehow return

.error:
    ; Error occurred - exit with error indication
    ; kernel_notify(NOTIFY_EXIT)
    mov rdi, 0              ; workflow_id (ignored for EXIT)
    mov rsi, NOTIFY_EXIT    ; flags = NOTIFY_EXIT
    int 0x80

    ; Should never reach here
    ud2

copy_bytes:
    push rdi
    push rcx
    mov rdi, rdx
.loop:
    test rcx, rcx
    jz .done_copy
    lodsb
    stosb
    dec rcx
    jmp .loop
.done_copy:
    pop rcx
    pop rdi
    ret

section .rodata
tag1_key:    db 'name', 0
tag1_value:  db 'test.txt', 0
tag2_key:    db 'type', 0
tag2_value:  db 'document', 0
