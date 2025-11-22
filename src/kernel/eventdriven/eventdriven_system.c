#include "eventdriven_system.h"
#include "guide/guide.h"
#include "execution/execution_deck.h"
#include "decks/deck_interface.h"
#include "routing/routing_table.h"
#include "klib.h"

// ============================================================================
// EVENT-DRIVEN SYSTEM - Simplified for Production
// ============================================================================
//
// Architecture:
//   User Space → EventRing → kernel_notify() → Guide → Decks → ResultRing → User
//
// NO Receiver, NO Center - direct processing from kernel_notify()
//
// ============================================================================

// Forward declarations для deck init
extern void operations_deck_init(void);
extern void storage_deck_init(void);
extern void hardware_deck_init(void);
extern void network_deck_init(void);

// ============================================================================
// GLOBAL SYSTEM
// ============================================================================

EventDrivenSystem global_event_system;

// ============================================================================
// INITIALIZATION
// ============================================================================

void eventdriven_system_init(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  EVENT-DRIVEN WORKFLOW ENGINE - Production Mode\n");
    kprintf("============================================================\n");
    kprintf("\n");

    // 1. Инициализируем routing table
    kprintf("[SYSTEM] Initializing routing table...\n");
    routing_table_init(&global_routing_table);
    global_event_system.routing_table = &global_routing_table;

    // 2. Инициализируем Guide
    kprintf("[SYSTEM] Initializing Guide...\n");
    guide_init(&global_routing_table);

    // 3. Инициализируем 4 processing decks
    kprintf("[SYSTEM] Initializing processing decks...\n");
    operations_deck_init();  // Deck 1: Operations
    storage_deck_init();     // Deck 3: Storage
    hardware_deck_init();    // Deck 2: Hardware
    network_deck_init();     // Deck 4: Network

    // 4. Инициализируем execution deck
    kprintf("[SYSTEM] Initializing execution deck...\n");
    execution_deck_init(&global_routing_table);

    global_event_system.initialized = 1;

    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  WORKFLOW ENGINE INITIALIZED\n");
    kprintf("  Decks: OPERATIONS, HARDWARE, STORAGE, NETWORK, EXECUTION\n");
    kprintf("  Mode: Direct processing via kernel_notify()\n");
    kprintf("============================================================\n");
    kprintf("\n");
}

// ============================================================================
// START SYSTEM
// ============================================================================

void eventdriven_system_start(void) {
    if (!global_event_system.initialized) {
        kprintf("[SYSTEM] ERROR: System not initialized!\n");
        return;
    }

    kprintf("[SYSTEM] Workflow Engine ready\n");
    global_event_system.running = 1;
}

// ============================================================================
// STOP SYSTEM
// ============================================================================

void eventdriven_system_stop(void) {
    kprintf("[SYSTEM] Stopping workflow engine...\n");
    global_event_system.running = 0;
    kprintf("[SYSTEM] System stopped\n");
}

// ============================================================================
// STATISTICS
// ============================================================================

void eventdriven_print_full_stats(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  WORKFLOW ENGINE STATISTICS\n");
    kprintf("============================================================\n");

    guide_print_stats();
    routing_table_print_stats(&global_routing_table);

    // Deck statistics
    extern DeckContext operations_deck_context;
    extern DeckContext storage_deck_context;
    extern DeckContext hardware_deck_context;
    extern DeckContext network_deck_context;

    kprintf("[DECK:Operations] processed=%lu errors=%lu\n",
            operations_deck_context.stats.events_processed,
            operations_deck_context.stats.errors);
    kprintf("[DECK:Storage] processed=%lu errors=%lu\n",
            storage_deck_context.stats.events_processed,
            storage_deck_context.stats.errors);
    kprintf("[DECK:Hardware] processed=%lu errors=%lu\n",
            hardware_deck_context.stats.events_processed,
            hardware_deck_context.stats.errors);
    kprintf("[DECK:Network] processed=%lu errors=%lu\n",
            network_deck_context.stats.events_processed,
            network_deck_context.stats.errors);

    execution_deck_print_stats();

    kprintf("============================================================\n");
    kprintf("\n");
}
