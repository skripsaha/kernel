#ifndef EVENTDRIVEN_SYSTEM_H
#define EVENTDRIVEN_SYSTEM_H

#include "routing/routing_table.h"

// ============================================================================
// EVENT-DRIVEN WORKFLOW ENGINE - Production System
// ============================================================================
//
// Simplified architecture:
//   User → EventRing → kernel_notify() → Guide → Decks → ResultRing → User
//
// Ring buffers managed per-process, not globally.
// Direct processing via kernel_notify() syscall.
//
// ============================================================================

// ============================================================================
// GLOBAL SYSTEM STATE
// ============================================================================

typedef struct {
    // Routing table (shared by all workflows)
    RoutingTable* routing_table;

    // System status
    volatile int initialized;
    volatile int running;

} EventDrivenSystem;

extern EventDrivenSystem global_event_system;

// ============================================================================
// SYSTEM LIFECYCLE
// ============================================================================

// Initialize workflow engine
void eventdriven_system_init(void);

// Start workflow engine (marks as ready)
void eventdriven_system_start(void);

// Stop workflow engine (graceful shutdown)
void eventdriven_system_stop(void);

// ============================================================================
// STATISTICS & MONITORING
// ============================================================================

void eventdriven_print_full_stats(void);

#endif // EVENTDRIVEN_SYSTEM_H
