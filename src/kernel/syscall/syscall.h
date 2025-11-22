#ifndef SYSCALL_H
#define SYSCALL_H

#include "ktypes.h"

// ============================================================================
// KERNEL_NOTIFY SYSCALL - Single System Call Interface
// ============================================================================
//
// The ONLY syscall in the system for workflow activation and control.
//
// Architecture: User → EventRing → kernel_notify(SUBMIT) → Guide → Decks
//
// ============================================================================

// Syscall vector (INT 0x80)
#define SYSCALL_VECTOR 0x80

// ============================================================================
// KERNEL_NOTIFY FLAGS
// ============================================================================

#define NOTIFY_SUBMIT  0x01  // Process events from EventRing
#define NOTIFY_WAIT    0x02  // Block until workflow completes (yields CPU)
#define NOTIFY_POLL    0x04  // Check workflow status (non-blocking)
#define NOTIFY_YIELD   0x08  // Cooperative yield (give up CPU voluntarily)
#define NOTIFY_EXIT    0x10  // Terminate current process (cleanup and exit)

// ============================================================================
// KERNEL_NOTIFY SYSCALL
// ============================================================================
//
// int kernel_notify(uint64_t workflow_id, uint64_t flags)
//
// Parameters:
//   RDI = workflow_id : ID of workflow to operate on
//   RSI = flags       : NOTIFY_SUBMIT | NOTIFY_WAIT | NOTIFY_POLL
//
// Returns (RAX):
//   NOTIFY_SUBMIT: number of events processed (0 if none)
//   NOTIFY_WAIT:   0 on success, -1 on error
//   NOTIFY_POLL:   0 if completed, 1 if in progress, -1 on error
//
// ============================================================================

#endif // SYSCALL_H
