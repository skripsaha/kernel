#include "workflow.h"
#include "klib.h"
#include "routing_table.h"
#include "workflow_rings.h"  // For RingEvent structure

// ============================================================================
// GLOBAL STATE
// ============================================================================

static WorkflowRegistry registry;

// External reference to global routing table
extern RoutingTable global_routing_table;
extern uint64_t global_event_id_counter;  // For assigning unique event IDs

// ============================================================================
// INITIALIZATION
// ============================================================================

void workflow_engine_init(void) {
    kprintf("[WORKFLOW] Starting initialization...\n");
    kprintf("[WORKFLOW] Registry size: %lu bytes\n", sizeof(WorkflowRegistry));

    kprintf("[WORKFLOW] Zeroing registry...\n");
    memset(&registry, 0, sizeof(WorkflowRegistry));

    kprintf("[WORKFLOW] Setting next_workflow_id...\n");
    registry.next_workflow_id = 1;  // Start from 1 (0 = invalid)

    kprintf("[WORKFLOW] Initializing spinlock...\n");
    spinlock_init(&registry.lock);

    kprintf("[WORKFLOW] Engine initialized\n");
    kprintf("[WORKFLOW] Workflows: UNLIMITED (linked list, allocated on-demand)\n");
    kprintf("[WORKFLOW] Max events per workflow: %d\n", WORKFLOW_MAX_EVENTS);
}

// ============================================================================
// WORKFLOW REGISTRATION
// ============================================================================

uint64_t workflow_register(const char* name,
                           const uint8_t* route,
                           uint32_t event_count,
                           const WorkflowNode* events,
                           uint64_t owner_pid) {
    if (!name || !route || !events || event_count == 0 || event_count > WORKFLOW_MAX_EVENTS) {
        kprintf("[WORKFLOW] ERROR: Invalid parameters\n");
        return 0;
    }

    // DYNAMIC WORKFLOW REGISTRY: Allocate workflow on heap
    Workflow* workflow = (Workflow*)kmalloc(sizeof(Workflow));
    if (!workflow) {
        kprintf("[WORKFLOW] ERROR: Out of memory for workflow\n");
        return 0;
    }
    memset(workflow, 0, sizeof(Workflow));

    spin_lock(&registry.lock);

    // Set identity
    workflow->workflow_id = atomic_increment_u64(&registry.next_workflow_id);

    int i = 0;
    while (name[i] && i < WORKFLOW_NAME_MAX - 1) {
        workflow->name[i] = name[i];
        i++;
    }
    workflow->name[i] = 0;

    workflow->owner_pid = owner_pid;

    // Copy routing path
    for (int j = 0; j < MAX_ROUTING_STEPS; j++) {
        workflow->route[j] = route[j];
    }

    // Copy DAG
    workflow->event_count = event_count;
    for (uint32_t j = 0; j < event_count; j++) {
        workflow->events[j] = events[j];
        workflow->events[j].ready = 0;
        workflow->events[j].completed = 0;
        workflow->events[j].error = 0;
        workflow->events[j].event_id = 0;
        workflow->events[j].result = 0;
    }

    // Set state
    workflow->state = WORKFLOW_STATE_REGISTERED;
    workflow->registration_time = rdtsc();
    workflow->activation_count = 0;
    workflow->total_execution_time = 0;

    // Analyze DAG for optimization
    workflow_analyze_dag(workflow);

    // Set default error handling configuration
    workflow->error_policy = ERROR_POLICY_ABORT;  // Default: abort on error
    workflow->retry_config.enabled = 1;            // Enable retry by default
    workflow->retry_config.max_retries = 3;        // Retry up to 3 times
    workflow->retry_config.base_delay_ms = 100;    // 100ms base delay
    workflow->retry_config.exponential_backoff = 1; // Use exponential backoff

    // DYNAMIC WORKFLOW REGISTRY: Add to head of linked list (O(1) insertion)
    workflow->next = registry.head;
    registry.head = workflow;
    registry.workflow_count++;

    spin_unlock(&registry.lock);

    kprintf("[WORKFLOW] Registered workflow '%s' (ID=%lu, events=%u, route=[%u,%u,%u,%u])\n",
            name, workflow->workflow_id, event_count,
            workflow->route[0], workflow->route[1], workflow->route[2], workflow->route[3]);

    return workflow->workflow_id;
}

int workflow_unregister(uint64_t workflow_id) {
    spin_lock(&registry.lock);

    // DYNAMIC WORKFLOW REGISTRY: Walk linked list to find and remove
    Workflow* current = registry.head;
    Workflow* prev = NULL;

    while (current) {
        if (current->workflow_id == workflow_id) {
            // Found - remove from list
            if (prev) {
                prev->next = current->next;
            } else {
                registry.head = current->next;
            }

            registry.workflow_count--;
            spin_unlock(&registry.lock);

            // Clean up execution context if exists
            if (current->context) {
                if (current->context->final_result) {
                    kfree(current->context->final_result);
                }
                kfree(current->context);
            }

            // Clean up event results
            for (uint32_t j = 0; j < current->event_count; j++) {
                if (current->events[j].result) {
                    kfree(current->events[j].result);
                }
            }

            // CRITICAL: Free the workflow itself
            kfree(current);

            kprintf("[WORKFLOW] Unregistered workflow ID=%lu\n", workflow_id);
            return 0;
        }

        prev = current;
        current = current->next;
    }

    spin_unlock(&registry.lock);
    kprintf("[WORKFLOW] ERROR: Workflow ID=%lu not found\n", workflow_id);
    return -1;
}

Workflow* workflow_get(uint64_t workflow_id) {
    // DYNAMIC WORKFLOW REGISTRY: Walk linked list to find workflow
    Workflow* current = registry.head;
    while (current) {
        if (current->workflow_id == workflow_id) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Submit a single WorkflowNode as RingEvent to the event-driven system
// Returns: event_id on success, 0 on failure
static uint64_t workflow_submit_event(Workflow* workflow, uint32_t event_index) {
    if (!workflow || event_index >= workflow->event_count) {
        return 0;
    }

    WorkflowNode* node = &workflow->events[event_index];

    // Create RingEvent from WorkflowNode
    RingEvent ring_event;
    memset(&ring_event, 0, sizeof(RingEvent));

    // Identity (kernel will assign id and timestamp in routing_table_add_event)
    ring_event.id = 0;  // Will be assigned by kernel
    ring_event.workflow_id = workflow->workflow_id;
    ring_event.type = node->type;
    ring_event.timestamp = 0;  // Will be assigned by kernel

    // Copy routing path from workflow
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        ring_event.route[i] = workflow->route[i];
    }

    // Copy payload from WorkflowNode
    uint64_t copy_size = node->data_size;
    if (copy_size > EVENT_PAYLOAD_SIZE) {
        kprintf("[WORKFLOW] WARNING: Event %u data size %lu exceeds payload limit %d, truncating\n",
                event_index, copy_size, EVENT_PAYLOAD_SIZE);
        copy_size = EVENT_PAYLOAD_SIZE;
    }

    if (copy_size > 0) {
        memcpy(ring_event.payload, node->data, copy_size);
    }
    ring_event.payload_size = copy_size;

    // Submit to routing table (this creates RoutingEntry and starts processing!)
    // routing_table_add_event() will assign unique event_id
    int result = routing_table_add_event(&global_routing_table, &ring_event);

    if (result != 0) {
        kprintf("[WORKFLOW] ERROR: Failed to submit event %u (type=%d) to routing table\n",
                event_index, node->type);
        return 0;
    }

    // routing_table_add_event() assigns the ID, retrieve it
    // The event is now in the routing table and being processed asynchronously!
    uint64_t assigned_event_id = ring_event.id;

    kprintf("[WORKFLOW] Submitted event %u (type=%d, event_id=%lu) to event-driven system\n",
            event_index, node->type, assigned_event_id);

    return assigned_event_id;
}

// ============================================================================
// WORKFLOW ACTIVATION & EXECUTION
// ============================================================================

int workflow_activate(uint64_t workflow_id, void* params, uint64_t param_size) {
    Workflow* workflow = workflow_get(workflow_id);
    if (!workflow) {
        kprintf("[WORKFLOW] ERROR: Workflow ID=%lu not found\n", workflow_id);
        return -1;
    }

    if (workflow->state == WORKFLOW_STATE_RUNNING) {
        kprintf("[WORKFLOW] ERROR: Workflow '%s' already running\n", workflow->name);
        return -2;
    }

    // Create execution context
    workflow->context = (ExecutionContext*)kmalloc(sizeof(ExecutionContext));
    memset(workflow->context, 0, sizeof(ExecutionContext));

    workflow->context->workflow_id = workflow_id;
    workflow->context->activation_time = rdtsc();
    workflow->context->total_events = workflow->event_count;
    workflow->context->completed_events = 0;
    workflow->context->running_events = 0;

    // Reset event states
    for (uint32_t i = 0; i < workflow->event_count; i++) {
        workflow->events[i].ready = 0;
        workflow->events[i].completed = 0;
        workflow->events[i].error = 0;
        workflow->events[i].event_id = 0;

        // Clean up old results
        if (workflow->events[i].result) {
            kfree(workflow->events[i].result);
            workflow->events[i].result = 0;
        }
    }

    // Apply parameters to first event if provided
    if (params && param_size > 0 && workflow->event_count > 0) {
        uint64_t copy_size = param_size;
        if (copy_size > EVENT_DATA_SIZE) {
            copy_size = EVENT_DATA_SIZE;
        }
        memcpy(workflow->events[0].data, params, copy_size);
        workflow->events[0].data_size = copy_size;
    }

    // Set state
    workflow->state = WORKFLOW_STATE_READY;
    workflow->activation_count++;

    kprintf("[WORKFLOW] Activated workflow '%s' (ID=%lu, activation #%lu)\n",
            workflow->name, workflow_id, workflow->activation_count);

    // CRITICAL: Process workflow to submit initial events (those with no dependencies)
    // This starts the workflow execution!
    int result = workflow_process(workflow);
    if (result < 0) {
        kprintf("[WORKFLOW] ERROR: Failed to process workflow %lu\n", workflow_id);
        return -3;
    }

    kprintf("[WORKFLOW] Workflow processing started, initial events submitted\n");

    return 0;
}

int workflow_process(Workflow* workflow) {
    if (!workflow || !workflow->context) {
        return -1;
    }

    if (workflow->state != WORKFLOW_STATE_READY &&
        workflow->state != WORKFLOW_STATE_RUNNING) {
        return 0;  // Nothing to do
    }

    workflow->state = WORKFLOW_STATE_RUNNING;

    // Find events ready to execute (dependencies met)
    for (uint32_t i = 0; i < workflow->event_count; i++) {
        if (workflow->events[i].completed || workflow->events[i].error) {
            continue;  // Skip completed/errored events
        }

        // Check if dependencies are met
        if (workflow_dependencies_met(workflow, i)) {
            workflow->events[i].ready = 1;

            // REAL IMPLEMENTATION: Submit event to event-driven system!
            uint64_t event_id = workflow_submit_event(workflow, i);

            if (event_id == 0) {
                // Submission failed - mark as error
                kprintf("[WORKFLOW] ERROR: Failed to submit event %u\n", i);
                workflow->events[i].error = 1;
                workflow->context->error_count++;
                continue;
            }

            // Store event_id for tracking
            workflow->events[i].event_id = event_id;
            workflow->context->running_events++;

            // NOTE: Event is now being processed ASYNCHRONOUSLY by Guide → Decks!
            // Completion will be signaled by workflow_on_event_completed() callback
            // DO NOT mark as completed here - that's done by the callback!
        }
    }

    // Check if workflow is complete
    if (workflow_is_complete(workflow)) {
        workflow->state = WORKFLOW_STATE_COMPLETED;

        uint64_t exec_time = rdtsc() - workflow->context->activation_time;
        workflow->total_execution_time += exec_time;

        kprintf("[WORKFLOW] Workflow '%s' completed (time=%lu cycles)\n",
                workflow->name, exec_time);

        return 1;  // Workflow complete
    }

    return 0;  // Still running
}

int workflow_is_complete(Workflow* workflow) {
    if (!workflow || !workflow->context) {
        return 0;
    }

    return workflow->context->completed_events >= workflow->context->total_events;
}

void* workflow_get_result(Workflow* workflow, uint64_t* result_size) {
    if (!workflow || !workflow->context) {
        if (result_size) *result_size = 0;
        return 0;
    }

    if (!workflow_is_complete(workflow)) {
        if (result_size) *result_size = 0;
        return 0;  // Not complete yet
    }

    // Return result from last event in DAG
    if (workflow->event_count > 0) {
        uint32_t last_idx = workflow->event_count - 1;
        if (result_size) {
            *result_size = workflow->events[last_idx].result_size;
        }
        return workflow->events[last_idx].result;
    }

    if (result_size) *result_size = 0;
    return 0;
}

// ============================================================================
// EVENT COMPLETION CALLBACK
// ============================================================================

// Called by Execution Deck when an event completes
// This is the CRITICAL integration point between event system and workflow system!
void workflow_on_event_completed(uint64_t workflow_id, uint64_t event_id,
                                  void* result, uint64_t result_size,
                                  int32_t error_code) {
    Workflow* workflow = workflow_get(workflow_id);
    if (!workflow) {
        kprintf("[WORKFLOW] WARNING: Event %lu completed but workflow %lu not found\n",
                event_id, workflow_id);
        // Cleanup result to prevent leak
        if (result) {
            kfree(result);
        }
        return;
    }

    if (!workflow->context) {
        kprintf("[WORKFLOW] WARNING: Event %lu completed but workflow %lu has no context\n",
                event_id, workflow_id);
        if (result) {
            kfree(result);
        }
        return;
    }

    // Find which WorkflowNode this event_id belongs to
    uint32_t event_index = (uint32_t)-1;
    for (uint32_t i = 0; i < workflow->event_count; i++) {
        if (workflow->events[i].event_id == event_id) {
            event_index = i;
            break;
        }
    }

    if (event_index == (uint32_t)-1) {
        kprintf("[WORKFLOW] WARNING: Event %lu completed but not found in workflow %lu\n",
                event_id, workflow_id);
        if (result) {
            kfree(result);
        }
        return;
    }

    WorkflowNode* node = &workflow->events[event_index];

    // Update node state
    if (error_code != 0) {
        // Event failed - check if we should retry
        node->last_error_code = error_code;

        kprintf("[WORKFLOW] Event %u (id=%lu) FAILED with error 0x%04x (%s)\n",
                event_index, event_id, error_code, error_to_string(error_code));

        // Clean up result if provided
        if (result) {
            kfree(result);
        }

        // Determine if we should retry based on error policy and error type
        bool should_retry = false;

        if (workflow->retry_config.enabled && error_is_transient(error_code)) {
            if (node->retry_count < workflow->retry_config.max_retries) {
                should_retry = true;
            }
        }

        if (should_retry) {
            // RETRY LOGIC
            node->retry_count++;

            // Calculate delay (exponential backoff)
            uint32_t delay_ms = workflow->retry_config.base_delay_ms;
            if (workflow->retry_config.exponential_backoff) {
                // Exponential: 100ms, 200ms, 400ms, 800ms...
                for (uint8_t i = 1; i < node->retry_count; i++) {
                    delay_ms *= 2;
                }
            }

            kprintf("[WORKFLOW] Retry %u/%u for event %u after %u ms (transient error)\n",
                    node->retry_count, workflow->retry_config.max_retries,
                    event_index, delay_ms);

            // Reset state for retry
            node->error = 0;
            node->ready = 0;

            // TODO: Schedule retry after delay (for now, immediate retry)
            // In production, we'd create a timer and retry when it fires

            // Immediate retry (simplified for now)
            node->ready = 1;
            uint64_t new_event_id = workflow_submit_event(workflow, event_index);

            if (new_event_id == 0) {
                kprintf("[WORKFLOW] ERROR: Failed to submit retry for event %u\n", event_index);
                node->error = 1;
                workflow->context->error_count++;
                workflow->context->failed_event_index = event_index;
            } else {
                node->event_id = new_event_id;
                workflow->context->running_events++;
            }

            return;  // Don't continue with normal error handling
        }

        // No retry - permanent failure
        node->error = 1;
        workflow->context->error_count++;
        workflow->context->failed_event_index = event_index;

        // Apply error policy
        switch (workflow->error_policy) {
            case ERROR_POLICY_ABORT:
                kprintf("[WORKFLOW] ERROR POLICY: ABORT - stopping workflow\n");
                workflow->state = WORKFLOW_STATE_ERROR;
                return;  // Stop processing

            case ERROR_POLICY_CONTINUE:
                kprintf("[WORKFLOW] ERROR POLICY: CONTINUE - proceeding with other events\n");
                // Continue to process dependencies
                break;

            case ERROR_POLICY_SKIP:
                kprintf("[WORKFLOW] ERROR POLICY: SKIP - skipping dependent events\n");
                // Mark all dependent events as skipped (error)
                for (uint32_t i = 0; i < workflow->event_count; i++) {
                    if (workflow->events[i].completed || workflow->events[i].error) {
                        continue;
                    }

                    // Check if this event depends on the failed event
                    for (uint32_t j = 0; j < workflow->events[i].dependency_count; j++) {
                        if (workflow->events[i].dependencies[j] == event_index) {
                            workflow->events[i].error = 1;
                            workflow->events[i].last_error_code = ERROR_WORKFLOW_DEPENDENCY_FAILED;
                            kprintf("[WORKFLOW] Event %u skipped (dependency %u failed)\n", i, event_index);
                            break;
                        }
                    }
                }
                break;

            case ERROR_POLICY_RETRY:
                // Already handled above
                break;

            default:
                kprintf("[WORKFLOW] WARNING: Unknown error policy %d\n", workflow->error_policy);
                break;
        }
    } else {
        // Event succeeded
        node->completed = 1;
        node->result = result;  // Transfer ownership to workflow
        node->result_size = result_size;

        workflow->context->completed_events++;

        kprintf("[WORKFLOW] Event %u (id=%lu) COMPLETED (result=%p, size=%lu)\n",
                event_index, event_id, result, result_size);
    }

    workflow->context->running_events--;

    // CRITICAL: Check if new events can be activated (dependency chain)!
    // Find events whose dependencies are now met
    for (uint32_t i = 0; i < workflow->event_count; i++) {
        if (workflow->events[i].completed || workflow->events[i].error) {
            continue;  // Skip already processed events
        }

        if (workflow->events[i].ready) {
            continue;  // Already submitted
        }

        // Check if this event's dependencies are now met
        if (workflow_dependencies_met(workflow, i)) {
            kprintf("[WORKFLOW] Event %u dependencies now met, submitting...\n", i);

            workflow->events[i].ready = 1;

            uint64_t new_event_id = workflow_submit_event(workflow, i);

            if (new_event_id == 0) {
                kprintf("[WORKFLOW] ERROR: Failed to submit dependent event %u\n", i);
                workflow->events[i].error = 1;
                workflow->context->error_count++;
                continue;
            }

            workflow->events[i].event_id = new_event_id;
            workflow->context->running_events++;
        }
    }

    // Check if workflow is complete
    if (workflow_is_complete(workflow)) {
        workflow->state = WORKFLOW_STATE_COMPLETED;

        uint64_t exec_time = rdtsc() - workflow->context->activation_time;
        workflow->total_execution_time += exec_time;

        kprintf("[WORKFLOW] Workflow '%s' (ID=%lu) COMPLETED! (time=%lu cycles, errors=%u)\n",
                workflow->name, workflow_id, exec_time, workflow->context->error_count);
    }
}

// ============================================================================
// DAG ANALYSIS
// ============================================================================

int workflow_analyze_dag(Workflow* workflow) {
    if (!workflow) return -1;

    // Analyze DAG structure for optimization opportunities
    int has_cycles = 0;
    int max_parallel = 0;

    // Simple analysis: check if any events have no dependencies
    // Those can potentially run in parallel
    int independent_count = 0;
    for (uint32_t i = 0; i < workflow->event_count; i++) {
        if (workflow->events[i].dependency_count == 0) {
            independent_count++;
        }
    }

    if (independent_count > 1) {
        workflow->parallel_safe = 1;
        max_parallel = independent_count;
    } else {
        workflow->parallel_safe = 0;
    }

    kprintf("[WORKFLOW] DAG analysis: parallel_safe=%d, max_parallel=%d\n",
            workflow->parallel_safe, max_parallel);

    return 0;
}

int workflow_find_parallel_events(Workflow* workflow, uint32_t* event_indices,
                                   uint32_t max_events) {
    if (!workflow || !event_indices) return 0;

    uint32_t count = 0;

    // Find all events that:
    // 1. Are not completed
    // 2. Have dependencies met
    // 3. Are not currently running

    for (uint32_t i = 0; i < workflow->event_count && count < max_events; i++) {
        if (!workflow->events[i].completed &&
            !workflow->events[i].error &&
            workflow_dependencies_met(workflow, i)) {

            event_indices[count++] = i;
        }
    }

    return count;
}

int workflow_dependencies_met(Workflow* workflow, uint32_t event_index) {
    if (!workflow || event_index >= workflow->event_count) {
        return 0;
    }

    WorkflowNode* node = &workflow->events[event_index];

    // No dependencies? Always ready
    if (node->dependency_count == 0) {
        return 1;
    }

    // Check all dependencies are completed
    for (uint32_t i = 0; i < node->dependency_count; i++) {
        uint32_t dep_idx = node->dependencies[i];

        if (dep_idx >= workflow->event_count) {
            kprintf("[WORKFLOW] ERROR: Invalid dependency index %u\n", dep_idx);
            return 0;
        }

        if (!workflow->events[dep_idx].completed) {
            return 0;  // Dependency not met
        }

        if (workflow->events[dep_idx].error) {
            return 0;  // Dependency failed
        }
    }

    return 1;  // All dependencies met
}

// ============================================================================
// STATISTICS & MONITORING
// ============================================================================

void workflow_print_stats(uint64_t workflow_id) {
    Workflow* workflow = workflow_get(workflow_id);
    if (!workflow) {
        kprintf("[WORKFLOW] Workflow ID=%lu not found\n", workflow_id);
        return;
    }

    kprintf("\n[WORKFLOW] Statistics for '%s' (ID=%lu):\n",
            workflow->name, workflow->workflow_id);
    kprintf("  Owner PID: %lu\n", workflow->owner_pid);
    kprintf("  Events: %u\n", workflow->event_count);
    kprintf("  State: %d\n", workflow->state);
    kprintf("  Activations: %lu\n", workflow->activation_count);
    kprintf("  Total execution time: %lu cycles\n", workflow->total_execution_time);
    kprintf("  Parallel safe: %s\n", workflow->parallel_safe ? "yes" : "no");

    if (workflow->context) {
        kprintf("  Execution context:\n");
        kprintf("    Completed: %u / %u\n",
                workflow->context->completed_events, workflow->context->total_events);
        kprintf("    Running: %u\n", workflow->context->running_events);
        kprintf("    Errors: %u\n", workflow->context->error_count);
    }
}

void workflow_print_all(void) {
    kprintf("\n[WORKFLOW] Registered workflows: %lu\n", registry.workflow_count);

    // DYNAMIC WORKFLOW REGISTRY: Walk linked list
    Workflow* current = registry.head;
    uint64_t index = 0;
    while (current) {
        kprintf("  [%lu] '%s' (ID=%lu, events=%u, state=%d)\n",
                index, current->name, current->workflow_id, current->event_count, current->state);
        current = current->next;
        index++;
    }
}

// ============================================================================
// CLEANUP
// ============================================================================

void workflow_cleanup_completed(void) {
    // Clean up workflows in COMPLETED state
    // (Keep them for a while for result retrieval)

    uint64_t cleaned = 0;

    // DYNAMIC WORKFLOW REGISTRY: Walk linked list
    Workflow* current = registry.head;
    while (current) {
        if (current->state == WORKFLOW_STATE_COMPLETED && current->context) {
            // Check if result has been retrieved
            // For now, just clean up old contexts

            uint64_t age = rdtsc() - current->context->activation_time;

            // If older than ~1 second (2.4B cycles on 2.4GHz), clean up
            if (age > 2400000000ULL) {
                if (current->context->final_result) {
                    kfree(current->context->final_result);
                }
                kfree(current->context);
                current->context = NULL;
                current->state = WORKFLOW_STATE_REGISTERED;
                cleaned++;
            }
        }
        current = current->next;
    }

    if (cleaned > 0) {
        kprintf("[WORKFLOW] Cleaned up %lu completed workflows\n", cleaned);
    }
}
