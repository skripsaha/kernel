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

_start:
    ; ========================================================================
    ; STEP 1: Register test workflow
    ; ========================================================================

    ; For now, just submit a simple event and wait
    ; In real test we would register workflow first

    ; Submit event via kernel_notify(SUBMIT)
    mov rdi, 2              ; workflow_id = 2 (test workflow registered at boot)
    mov rsi, 0x01           ; NOTIFY_SUBMIT flag
    int 0x80                ; syscall

    ; ========================================================================
    ; STEP 2: Wait for completion (this will YIELD CPU!)
    ; ========================================================================

    mov rdi, 2              ; workflow_id = 2
    mov rsi, 0x02           ; NOTIFY_WAIT flag
    int 0x80                ; syscall - process will YIELD here!

    ; When we reach here, event has completed and we were woken up

    ; ========================================================================
    ; STEP 3: Exit
    ; ========================================================================

    mov rdi, 2              ; workflow_id
    mov rsi, 0x10           ; NOTIFY_EXIT flag
    int 0x80                ; syscall

    ; Should never reach here
    jmp $
