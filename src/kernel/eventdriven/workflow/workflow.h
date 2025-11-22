#ifndef WORKFLOW_H
#define WORKFLOW_H

#include "ktypes.h"
#include "events.h"
#include "errors.h"
#include "klib.h"  // For spinlock_t

// ============================================================================
// WORKFLOW ENGINE - Core Innovation of the Kernel
// ============================================================================
//
// According to idea.txt, Workflows are the fundamental unit of computation.
// User space registers workflows (DAG of events), then activates them via
// the SINGLE syscall: kernel_notify()
//
// Key features:
// - DAG (Directed Acyclic Graph) of events
// - Dependency analysis for parallel execution
// - Zero-copy data passing between decks
// - Execution context tracking
// - Automatic prefetching and optimization
//
// ============================================================================

// Maximum limits
#define WORKFLOW_MAX_EVENTS      16    // Max events per workflow (reduced for v1)
// REMOVED: WORKFLOW_MAX_WORKFLOWS - now unlimited via linked list!
#define WORKFLOW_NAME_MAX        32    // Max workflow name length
#define WORKFLOW_MAX_DEPENDENCIES 8    // Max dependencies per event

// ============================================================================
// WORKFLOW STATE
// ============================================================================

typedef enum {
    WORKFLOW_STATE_REGISTERED = 0,  // Registered but not active
    WORKFLOW_STATE_READY      = 1,  // Ready to execute
    WORKFLOW_STATE_RUNNING    = 2,  // Currently executing
    WORKFLOW_STATE_WAITING    = 3,  // Waiting for dependencies
    WORKFLOW_STATE_COMPLETED  = 4,  // Execution finished
    WORKFLOW_STATE_ERROR      = 5   // Error occurred
} WorkflowState;

// ============================================================================
// WORKFLOW DAG NODE
// ============================================================================

// Each node in the DAG represents an event to be executed
typedef struct {
    // Event definition
    EventType type;                             // Type of event
    uint8_t data[EVENT_DATA_SIZE];              // Event payload
    uint64_t data_size;                         // Actual data size

    // Dependencies (DAG edges)
    uint32_t dependency_count;                  // Number of dependencies
    uint32_t dependencies[WORKFLOW_MAX_DEPENDENCIES];  // Indices of dependent events

    // Execution state
    uint8_t ready;                              // 1 if dependencies met
    uint8_t completed;                          // 1 if execution done
    uint8_t error;                              // 1 if error occurred
    uint8_t retry_count;                        // Number of times retried
    uint32_t last_error_code;                   // Last error code (if error occurred)

    // Results
    uint64_t event_id;                          // Event ID when submitted
    void* result;                               // Pointer to result data
    uint64_t result_size;                       // Size of result

} WorkflowNode;

// ============================================================================
// EXECUTION CONTEXT
// ============================================================================

// Tracks execution state of a workflow
typedef struct {
    uint64_t workflow_id;                       // Workflow being executed
    uint64_t activation_time;                   // RDTSC when activated

    // Progress tracking
    uint32_t total_events;                      // Total events in workflow
    uint32_t completed_events;                  // How many completed
    uint32_t running_events;                    // Currently executing

    // Performance metrics
    uint64_t total_cycles;                      // Total CPU cycles used
    uint64_t wait_time;                         // Time spent waiting

    // Result aggregation
    void* final_result;                         // Final workflow result
    uint64_t final_result_size;                 // Size of final result

    // Error handling
    uint32_t error_count;                       // Number of errors
    uint32_t failed_event_index;                // Which event failed

} ExecutionContext;

// ============================================================================
// WORKFLOW DEFINITION
// ============================================================================

typedef struct Workflow {
    // Identity
    uint64_t workflow_id;                       // Unique workflow ID
    char name[WORKFLOW_NAME_MAX];               // Workflow name
    uint64_t owner_pid;                         // User process ID

    // Routing through Decks
    // Example: [1, 0, 0, 0] = Operations → Execution
    // Example: [3, 1, 2, 0] = Storage → Operations → Hardware → Execution
    uint8_t route[MAX_ROUTING_STEPS];           // Path through Decks

    // DAG structure
    uint32_t event_count;                       // Number of events
    WorkflowNode events[WORKFLOW_MAX_EVENTS];   // DAG nodes

    // State
    WorkflowState state;                        // Current state
    ExecutionContext* context;                  // Execution context (if active)

    // Metadata
    uint64_t registration_time;                 // RDTSC when registered
    uint64_t activation_count;                  // Times activated
    uint64_t total_execution_time;              // Cumulative execution time

    // Optimization hints
    uint8_t parallel_safe;                      // Can events run in parallel?
    uint8_t prefetch_enabled;                   // Enable data prefetching?

    // Error handling configuration
    ErrorPolicy error_policy;                   // How to handle errors (abort/continue/retry/skip)
    RetryConfig retry_config;                   // Retry configuration for transient errors

    // DYNAMIC WORKFLOW REGISTRY FIX: Linked list for unlimited workflows
    struct Workflow* next;                      // Next workflow in registry (NULL if last)

} Workflow;

// ============================================================================
// WORKFLOW REGISTRY
// ============================================================================
//
// DYNAMIC WORKFLOW REGISTRY FIX:
// Changed from fixed array to linked list for unlimited capacity!
// Workflows are now allocated on-demand via kmalloc.
//
// ============================================================================

typedef struct {
    Workflow* head;                     // Head of linked list (NULL if empty)
    uint64_t workflow_count;            // Number of registered workflows
    volatile uint64_t next_workflow_id; // Next workflow ID to assign
    spinlock_t lock;                    // Registry protection
} WorkflowRegistry;

// ============================================================================
// WORKFLOW API
// ============================================================================

// === INITIALIZATION ===
void workflow_engine_init(void);

// === WORKFLOW MANAGEMENT ===
// Register a new workflow (returns workflow_id, or 0 on error)
uint64_t workflow_register(const char* name,
                           const uint8_t* route,
                           uint32_t event_count,
                           const WorkflowNode* events,
                           uint64_t owner_pid);

// Unregister a workflow
int workflow_unregister(uint64_t workflow_id);

// Get workflow by ID
Workflow* workflow_get(uint64_t workflow_id);

// === ACTIVATION & EXECUTION ===
// Activate a workflow (called by kernel_notify syscall)
int workflow_activate(uint64_t workflow_id, void* params, uint64_t param_size);

// Process workflow execution (called by Guide)
int workflow_process(Workflow* workflow);

// Check if workflow is complete
int workflow_is_complete(Workflow* workflow);

// Get workflow result
void* workflow_get_result(Workflow* workflow, uint64_t* result_size);

// === DAG ANALYSIS ===
// Analyze DAG for parallel execution opportunities
int workflow_analyze_dag(Workflow* workflow);

// Find events that can execute in parallel
int workflow_find_parallel_events(Workflow* workflow, uint32_t* event_indices,
                                   uint32_t max_events);

// Check if event dependencies are met
int workflow_dependencies_met(Workflow* workflow, uint32_t event_index);

// === EVENT COMPLETION CALLBACK ===
// Called by Execution Deck when event completes
// This integrates the event-driven system with the workflow system!
void workflow_on_event_completed(uint64_t workflow_id, uint64_t event_id,
                                  void* result, uint64_t result_size,
                                  int32_t error_code);

// === STATISTICS & MONITORING ===
void workflow_print_stats(uint64_t workflow_id);
void workflow_print_all(void);

// === CLEANUP ===
// Clean up completed workflows
void workflow_cleanup_completed(void);

#endif  // WORKFLOW_H
