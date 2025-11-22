#include "execution_deck.h"
#include "process.h"
#include "klib.h"
#include "workflow.h"  // For workflow_on_event_completed() callback

// ============================================================================
// GLOBAL STATE
// ============================================================================

ExecutionStats execution_stats;
static RoutingTable* routing_table = 0;
static DeckQueue* execution_queue = 0;

// ============================================================================
// INITIALIZATION
// ============================================================================

void execution_deck_init(RoutingTable* rtable) {
    routing_table = rtable;
    execution_queue = guide_get_execution_queue();

    execution_stats.events_executed = 0;
    execution_stats.responses_sent = 0;
    execution_stats.errors = 0;

    kprintf("[EXECUTION] Initialized\n");
}

// ============================================================================
// RESULT COLLECTION
// ============================================================================

static void collect_results(RoutingEntry* entry, RingResult* result) {
    // Initialize RingResult
    result->event_id = entry->event_id;
    result->workflow_id = entry->event_copy.user_id;  // user_id contains workflow_id
    result->completion_time = rdtsc();
    result->status = entry->abort_flag ? entry->error_code : 0;
    result->error_code = entry->error_code;
    result->result_size = 0;

    // Collect results from decks
    int result_index = -1;
    for (int i = MAX_ROUTING_STEPS - 1; i >= 0; i--) {
        if (entry->deck_results[i] != 0) {
            result_index = i;
            break;
        }
    }

    if (result_index >= 0) {
        // Copy deck result to RingResult
        void* deck_result = entry->deck_results[result_index];

        // Copy pointer value to result (simplified for now)
        *(void**)result->result = deck_result;
        result->result_size = sizeof(void*);

        kprintf("[EXECUTION] Collected result from deck at index %d for event %lu\n",
                result_index, entry->event_id);
    } else {
        kprintf("[EXECUTION] No results for event %lu\n", entry->event_id);
    }
}

// ============================================================================
// EVENT PROCESSING
// ============================================================================

static void process_completed_event(RoutingEntry* entry) {
    // Get current running process
    process_t* proc = process_get_current();

    if (!proc) {
        kprintf("[EXECUTION] ERROR: No current process for event %lu\n", entry->event_id);
        atomic_increment_u64((volatile uint64_t*)&execution_stats.errors);
        return;
    }

    if (!proc->result_ring) {
        kprintf("[EXECUTION] ERROR: Process PID=%lu has no ResultRing for event %lu\n",
                proc->pid, entry->event_id);
        atomic_increment_u64((volatile uint64_t*)&execution_stats.errors);
        return;
    }

    ResultRing* result_ring = (ResultRing*)proc->result_ring;

    // 1. Collect results from decks
    RingResult result;
    collect_results(entry, &result);

    // 2. Push result to ResultRing (kernel → user)
    // CRITICAL: Add timeout to prevent infinite busy-wait deadlock!
    int push_attempts = 0;
    const int MAX_PUSH_ATTEMPTS = 10000;  // ~10ms worst case with cpu_pause

    while (!wf_result_ring_push(result_ring, &result)) {
        if (++push_attempts >= MAX_PUSH_ATTEMPTS) {
            kprintf("[EXECUTION] ERROR: ResultRing full after %d attempts for event %lu! (PID=%lu)\n",
                    MAX_PUSH_ATTEMPTS, entry->event_id, proc->pid);
            kprintf("[EXECUTION]   This means user is not reading results fast enough!\n");
            atomic_increment_u64((volatile uint64_t*)&execution_stats.errors);
            return;  // Drop result to prevent deadlock
        }
        cpu_pause();  // Yield CPU while waiting
    }

    atomic_increment_u64((volatile uint64_t*)&execution_stats.responses_sent);

    kprintf("[EXECUTION] Sent result for event %lu to user space\n", entry->event_id);

    // 3. Send completion IRQ to wake up waiting process
    // INT 0x81 is REQUIRED to wake process from hlt instruction!
    // The interrupt will:
    //   1. Call completion_irq_handler
    //   2. Set completion_ready flag
    //   3. Wake process from hlt (if waiting)
    kprintf("[EXECUTION] Sending completion IRQ (INT 0x81)\n");
    asm volatile("int $0x81");

    // 4. NOTIFY WORKFLOW SYSTEM - CRITICAL INTEGRATION POINT!
    // This callback enables automatic DAG dependency resolution
    uint64_t workflow_id = entry->event_copy.user_id;  // user_id contains workflow_id
    uint64_t event_id = entry->event_id;

    // Find the deck result (last non-null result)
    void* deck_result = 0;
    uint64_t deck_result_size = 0;
    for (int i = MAX_ROUTING_STEPS - 1; i >= 0; i--) {
        if (entry->deck_results[i] != 0) {
            deck_result = entry->deck_results[i];
            // Size is not tracked currently, TODO: add result_size tracking
            deck_result_size = sizeof(void*);  // Placeholder
            break;
        }
    }

    // Copy result for workflow (workflow takes ownership)
    void* result_copy = 0;
    if (deck_result != 0) {
        // CRITICAL: We make a SHALLOW copy (just the pointer)
        // The actual data ownership transfers to workflow
        // Workflow will be responsible for cleanup
        result_copy = deck_result;
        kprintf("[EXECUTION] Transferring result %p to workflow %lu\n",
                result_copy, workflow_id);
    }

    // Call workflow callback (this may trigger new events!)
    int32_t error_code = entry->abort_flag ? entry->error_code : 0;
    workflow_on_event_completed(workflow_id, event_id,
                                 result_copy, deck_result_size, error_code);

    // 5. DECK RESULT CLEANUP - Now properly implemented!
    // Use result_types array to determine how to cleanup each result
    // IMPORTANT: Skip the result that was transferred to workflow (ownership transferred)
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        void* deck_result = entry->deck_results[i];
        ResultType result_type = entry->result_types[i];

        if (deck_result == 0 || result_type == RESULT_TYPE_NONE) {
            continue;  // No result or already handled
        }

        // Skip the result that was transferred to workflow (ownership transferred)
        if (deck_result == result_copy) {
            kprintf("[EXECUTION] Skipping cleanup for result %p (transferred to workflow)\n",
                    deck_result);
            continue;
        }

        switch (result_type) {
            case RESULT_TYPE_KMALLOC:
                // Allocated via kmalloc - free it
                kfree(deck_result);
                kprintf("[EXECUTION] Freed kmalloc result at %p (deck %d)\n", deck_result, i);
                break;

            case RESULT_TYPE_VALUE:
                // Value cast to pointer - no cleanup needed
                break;

            case RESULT_TYPE_STATIC:
                // Static data or stack pointer - no cleanup needed
                break;

            case RESULT_TYPE_MEMORY_MAPPED:
                // Memory-mapped region - TODO: implement unmap
                // For now, leave mapped (will be freed with process cleanup)
                kprintf("[EXECUTION] Warning: memory-mapped result at %p not unmapped (deck %d)\n",
                        deck_result, i);
                break;

            default:
                kprintf("[EXECUTION] Warning: unknown result type %d for deck %d\n",
                        result_type, i);
                break;
        }
    }

    // 6. Remove routing entry from table (cleanup)
    routing_table_remove(routing_table, entry->event_id);

    atomic_increment_u64((volatile uint64_t*)&execution_stats.events_executed);
}

// ============================================================================
// MAIN LOOP
// ============================================================================

// Обработать одно завершённое событие (для синхронной обработки)
int execution_deck_run_once(void) {
    // Получаем завершённое событие от Guide
    RoutingEntry* entry = deck_queue_pop(execution_queue);

    if (entry) {
        // Обрабатываем завершённое событие
        process_completed_event(entry);
        return 1;  // Обработано
    }
    return 0;  // Очередь пуста
}

void execution_deck_run(void) {
    kprintf("[EXECUTION] Starting main loop...\n");

    uint64_t iterations = 0;

    while (1) {
        if (!execution_deck_run_once()) {
            // Очередь пуста
            cpu_pause();
        }

        // Периодическая статистика
        iterations++;
        if (iterations % 10000000 == 0) {
            execution_deck_print_stats();
        }
    }
}

// ============================================================================
// STATISTICS
// ============================================================================

void execution_deck_print_stats(void) {
    kprintf("[EXECUTION] Stats: executed=%lu responses_sent=%lu errors=%lu\n",
            execution_stats.events_executed,
            execution_stats.responses_sent,
            execution_stats.errors);
}
