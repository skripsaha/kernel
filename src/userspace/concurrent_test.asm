; ============================================================================
; CONCURRENT TEST - Multi-process workflow test
; ============================================================================
;
; Simple program to test concurrent execution of multiple processes
; Each process executes a workflow and yields CPU while waiting
;
; ============================================================================

[BITS 64]

section .text
global _start

%define EVENT_RING_ADDR   0x20200000
%define WORKFLOW_ID       2
%define NOTIFY_SUBMIT     0x01
%define NOTIFY_WAIT       0x02
%define NOTIFY_EXIT       0x10
%define EVENT_OP_HASH_CRC32 101  ; Simple operations deck event

_start:
    ; ========================================================================
    ; STEP 1: Create and submit a simple event (CRC32 hash)
    ; ========================================================================

    ; Build RingEvent on stack (576 bytes)
    sub rsp, 576
    mov rdi, rsp

    ; Zero the event structure
    mov rcx, 72             ; 576/8 = 72 qwords
    xor rax, rax
.zero_loop:
    mov [rdi + rcx*8 - 8], rax
    loop .zero_loop

    ; Fill RingEvent structure
    mov qword [rdi + 0], 0                  ; id (will be assigned by kernel)
    mov qword [rdi + 8], WORKFLOW_ID        ; workflow_id = 2
    mov dword [rdi + 16], EVENT_OP_HASH_CRC32 ; type = CRC32
    mov qword [rdi + 24], 0                 ; timestamp (will be set by kernel)

    ; Route: [1,0,0,0] = Operations deck only
    mov byte [rdi + 32], 1                  ; route[0] = Operations
    mov byte [rdi + 33], 0

    ; Payload: [size:8][data:...] for CRC32
    mov qword [rdi + 40], 8                 ; size = 8 bytes
    mov qword [rdi + 48], 0x0123456789ABCDEF ; test data

    ; payload_size
    mov dword [rdi + 552], 16               ; 8 (size) + 8 (data)

    ; ========================================================================
    ; STEP 2: Push event to EventRing
    ; ========================================================================

    mov r12, EVENT_RING_ADDR
    mov rax, [r12 + 64]     ; tail
    mov rbx, [r12 + 0]      ; head

    ; Check if ring is full
    mov rcx, rax
    sub rcx, rbx
    cmp rcx, 256            ; EVENT_RING_SIZE
    jge .error              ; Ring full

    ; Calculate slot position: (tail % 256) * 576
    mov rdx, rax
    and rdx, 0xFF
    imul rdx, 576
    lea r13, [r12 + 128 + rdx]  ; slot address

    ; Copy event to ring (72 qwords = 576 bytes)
    mov rsi, rsp
    mov rdi, r13
    mov rcx, 72
    rep movsq

    ; Increment tail
    inc rax
    mfence
    mov [r12 + 64], rax

    add rsp, 576

    ; ========================================================================
    ; STEP 3: Submit event via kernel_notify(SUBMIT)
    ; ========================================================================

    mov rdi, WORKFLOW_ID    ; workflow_id = 2
    mov rsi, NOTIFY_SUBMIT  ; NOTIFY_SUBMIT flag
    int 0x80                ; syscall

    ; ========================================================================
    ; STEP 4: Wait for completion (this will YIELD CPU!)
    ; ========================================================================

    mov rdi, WORKFLOW_ID    ; workflow_id = 2
    mov rsi, NOTIFY_WAIT    ; NOTIFY_WAIT flag
    int 0x80                ; syscall - process will YIELD here!

    ; When we reach here, event has completed and we were woken up

    ; ========================================================================
    ; STEP 5: Exit
    ; ========================================================================

    mov rdi, WORKFLOW_ID    ; workflow_id
    mov rsi, NOTIFY_EXIT    ; NOTIFY_EXIT flag
    int 0x80                ; syscall

    ; Should never reach here
    jmp $

.error:
    ; Error handler - just hang
    jmp $
