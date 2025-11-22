; ============================================================================
; USER TEST PROGRAM - EventRing/ResultRing Async Workflow Demo
; ============================================================================
;
; This program demonstrates the async workflow pipeline:
;   1. Create RingEvent with test data (CRC32 hash operation)
;   2. Push event to EventRing (shared memory with kernel)
;   3. Call kernel_notify(workflow_id, NOTIFY_SUBMIT) to process
;   4. Call kernel_notify(workflow_id, NOTIFY_WAIT) to block until completion
;   5. Read result from ResultRing
;   6. Loop forever
;
; Memory layout:
;   0x20200000: EventRing  (147,584 bytes)
;   0x202400A0: ResultRing (147,584 bytes)
;
; Syscall interface:
;   kernel_notify(workflow_id, flags)
;   RAX = 0 (syscall number ignored, only INT 0x80 used)
;   RDI = workflow_id
;   RSI = flags (NOTIFY_SUBMIT=0x01, NOTIFY_WAIT=0x02, NOTIFY_POLL=0x04)
;   INT 0x80
;
; ============================================================================

BITS 64

; Syscall flags
%define NOTIFY_SUBMIT  0x01
%define NOTIFY_WAIT    0x02
%define NOTIFY_POLL    0x04

; Ring buffer addresses (mapped by kernel in process_create)
%define EVENT_RING_ADDR   0x20200000
%define RESULT_RING_ADDR  0x202400A0

; Ring buffer sizes
%define EVENT_RING_SIZE   256
%define RESULT_RING_SIZE  256

; Event types (from operations_deck.c)
%define EVENT_OP_HASH_CRC32  100

; Workflow ID (registered in main.c)
%define WORKFLOW_ID  2

section .text
global _start

_start:
    ; ========================================================================
    ; STEP 1: Create RingEvent
    ; ========================================================================

    ; RingEvent structure (576 bytes) - WITH COMPILER ALIGNMENT:
    ;   +0:   id (8 bytes) - kernel fills this
    ;   +8:   workflow_id (8 bytes)
    ;   +16:  type (4 bytes)
    ;   +20:  [PADDING] (4 bytes) - compiler adds for alignment!
    ;   +24:  timestamp (8 bytes) - kernel fills this
    ;   +32:  route[8] (8 bytes)
    ;   +40:  payload[512] (512 bytes)
    ;   +552: payload_size (4 bytes)
    ;   +556: padding[20] (20 bytes)

    ; Build event on stack
    sub rsp, 576                ; Allocate 576 bytes for RingEvent
    mov rdi, rsp                ; RDI = event pointer

    ; Zero the event structure
    mov rcx, 72                 ; 576 / 8 = 72 qwords
    xor rax, rax
.zero_loop:
    mov [rdi + rcx*8 - 8], rax
    loop .zero_loop

    ; Fill event fields
    mov qword [rdi + 0], 0      ; id = 0 (kernel assigns)
    mov qword [rdi + 8], WORKFLOW_ID  ; workflow_id = 2
    mov dword [rdi + 16], EVENT_OP_HASH_CRC32  ; type = 100
    mov qword [rdi + 24], 0     ; timestamp = 0 (kernel assigns) - FIXED: +24 not +20

    ; Route: [1,0,0,0,0,0,0,0] = Operations Deck → Execution
    mov byte [rdi + 32], 1      ; route[0] = 1 (Operations Deck) - FIXED: +32 not +28
    mov byte [rdi + 33], 0      ; route[1-7] = 0

    ; Payload for CRC32: [size:8][data:...]
    ; Let's hash the string "Hello from Ring 3!"
    lea rsi, [rel test_string]
    mov qword [rdi + 40], 18    ; size = 18 bytes - FIXED: +40 not +36

    ; Copy test string to payload
    push rdi                    ; Save RDI (pointer to event)
    lea rdi, [rdi + 48]         ; RDI = dest = event.payload + 8 - FIXED: +48 not +44
    mov rcx, 18
.copy_string:
    lodsb
    stosb
    loop .copy_string
    pop rdi                     ; Restore RDI (pointer to event)

    ; payload_size
    mov dword [rdi + 552], 26   ; 8 (size field) + 18 (string) - FIXED: +552 not +548

    ; ========================================================================
    ; STEP 2: Push event to EventRing
    ; ========================================================================

    ; EventRing structure:
    ;   +0:   head (8 bytes, aligned 64) - kernel reads
    ;   +64:  tail (8 bytes, aligned 64) - user writes
    ;   +128: events[256] (256 * 576 = 147,456 bytes)

    mov r12, EVENT_RING_ADDR    ; R12 = EventRing address
    mov rax, [r12 + 64]         ; tail
    mov rbx, [r12 + 0]          ; head

    ; Check if ring is full
    mov rcx, rax
    sub rcx, rbx
    cmp rcx, EVENT_RING_SIZE
    jge .ring_full

    ; Calculate index: idx = tail % 256
    mov rdx, rax
    and rdx, 0xFF               ; idx = tail & 0xFF

    ; Calculate event address: events[idx] = base + 128 + idx * 576
    imul rdx, 576
    lea r13, [r12 + 128 + rdx]  ; R13 = destination event address

    ; Copy event from stack to ring
    mov rsi, rsp                ; source = stack
    mov rdi, r13                ; dest = ring
    mov rcx, 72                 ; 576 / 8 = 72 qwords
    rep movsq

    ; Update tail (with memory barrier)
    inc rax
    mfence
    mov [r12 + 64], rax

    ; Event successfully pushed!

    ; Clean up stack
    add rsp, 576

    ; ========================================================================
    ; STEP 3: Call kernel_notify(SUBMIT) to process event
    ; ========================================================================

    mov rdi, WORKFLOW_ID        ; workflow_id = 2
    mov rsi, NOTIFY_SUBMIT      ; flags = SUBMIT
    int 0x80                    ; kernel_notify()

    ; RAX now contains number of events processed

    ; ========================================================================
    ; STEP 4: Call kernel_notify(WAIT) to block until completion
    ; ========================================================================

    mov rdi, WORKFLOW_ID        ; workflow_id = 2
    mov rsi, NOTIFY_WAIT        ; flags = WAIT
    int 0x80                    ; kernel_notify()

    ; Workflow completed!

    ; ========================================================================
    ; STEP 5: Read result from ResultRing
    ; ========================================================================

    ; ResultRing structure:
    ;   +0:   head (8 bytes, aligned 64) - user reads
    ;   +64:  tail (8 bytes, aligned 64) - kernel writes
    ;   +128: results[256] (256 * 576 = 147,456 bytes)

    mov r12, RESULT_RING_ADDR   ; R12 = ResultRing address
    mov rax, [r12 + 0]          ; head
    mov rbx, [r12 + 64]         ; tail

    ; Check if ring has results
    cmp rax, rbx
    je .no_results

    ; Calculate index: idx = head % 256
    mov rdx, rax
    and rdx, 0xFF

    ; Calculate result address: results[idx] = base + 128 + idx * 576
    imul rdx, 576
    lea r13, [r12 + 128 + rdx]  ; R13 = result address

    ; Read result fields
    mov r14, [r13 + 0]          ; event_id
    mov r15, [r13 + 24]         ; status

    ; Update head
    inc rax
    mfence
    mov [r12 + 0], rax

    ; ========================================================================
    ; STEP 6: Loop forever (workflow completed successfully!)
    ; ========================================================================

.success_loop:
    pause
    jmp .success_loop

.ring_full:
    ; EventRing is full - shouldn't happen in this simple test
    jmp .error_loop

.no_results:
    ; No results yet - shouldn't happen after WAIT
    jmp .error_loop

.error_loop:
    pause
    jmp .error_loop

section .rodata
test_string: db "Hello from Ring 3!"
