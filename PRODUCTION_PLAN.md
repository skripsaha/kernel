# Production Plan - Reliability-First Approach

**Цель:** Создать стабильную, надежную систему для реального использования
**Приоритет:** Надежность > Performance > Features
**Timeline:** Качество важнее скорости
**Scope v1.0:** БЕЗ сети (focus на core stability)

---

## 🎯 Философия Разработки

### Принципы:

1. **Stability First** - Лучше меньше features, но они работают на 100%
2. **Test Everything** - Каждая функция покрыта тестами
3. **Fail Gracefully** - Система НИКОГДА не крашится, только логирует ошибки
4. **Defensive Coding** - Проверять ВСЕ входные данные, никогда не trust user input
5. **Incremental Progress** - Маленькие изменения + тестирование после каждого

### Anti-Patterns (чего избегать):

❌ Добавлять новые features пока старые не стабильны
❌ Optimization до того как есть benchmarks
❌ Commit без тестирования
❌ "Работает у меня" - нужны automated tests

---

## 📋 Phase 1: Core Correctness (4-6 недель)

### Week 1-2: Multi-Process Execution (P0)

**Цель:** Процессы должны КОРРЕКТНО переключаться, без race conditions

#### Task 1.1: Context Switching
```c
// scheduler.c - DEFENSIVE implementation
void scheduler_schedule(interrupt_frame_t* frame) {
    // Validation (defensive!)
    if (!frame) {
        kprintf("[SCHEDULER] CRITICAL: NULL frame!\n");
        return;
    }

    // Save current context (if any)
    if (current_running) {
        // VERIFY process state before saving
        if (current_running->state != PROCESS_STATE_RUNNING &&
            current_running->state != PROCESS_STATE_READY) {
            kprintf("[SCHEDULER] WARNING: Invalid state %d for PID %lu\n",
                    current_running->state, current_running->pid);
        }

        save_context(current_running, frame);
        current_running->state = PROCESS_STATE_READY;
    }

    // Pick next process
    process_t* next = scheduler_pick_next();

    if (!next) {
        // NO PANIC! Just idle
        kprintf("[SCHEDULER] No ready processes, idling...\n");
        asm volatile("hlt");
        return;
    }

    // Sanity checks before switch
    if (next->state != PROCESS_STATE_READY) {
        kprintf("[SCHEDULER] ERROR: Process PID %lu not ready (state=%d)\n",
                next->pid, next->state);
        // Skip this process
        scheduler_remove_process(next);
        return;
    }

    // Verify VMM context exists
    if (!next->vmm_context) {
        kprintf("[SCHEDULER] CRITICAL: Process PID %lu has no VMM context!\n",
                next->pid);
        next->state = PROCESS_STATE_ERROR;
        return;
    }

    // Switch page tables (with validation)
    vmm_switch_context(next->vmm_context);

    // Restore context
    restore_context(next, frame);

    next->state = PROCESS_STATE_RUNNING;
    current_running = next;

    // Stats
    stats.context_switches++;
}
```

**Testing Strategy:**
- [ ] Test 1: Single process (should not crash)
- [ ] Test 2: Two processes alternating (visible in logs)
- [ ] Test 3: Process exits gracefully (cleanup)
- [ ] Test 4: Process crashes (should not kill kernel)
- [ ] Test 5: Stress test - 10 processes, 1000 switches
- [ ] Test 6: Edge case - all processes waiting (idle loop)

**Success Criteria:**
```
✅ 10 processes run for 1 hour without crash
✅ Context switches logged correctly
✅ No memory leaks (check PMM stats)
✅ Process crash doesn't affect others (isolation)
```

---

#### Task 1.2: Robust Process Lifecycle

**Problem:** Процессы могут зависнуть, крашнуться, или leak resources

**Solution:** Comprehensive lifecycle management

```c
// process.c - Safe process termination
void process_exit(process_t* proc, int exit_code) {
    if (!proc) return;

    kprintf("[PROCESS] PID %lu exiting with code %d\n", proc->pid, exit_code);

    // 1. Clean up workflows
    workflow_cleanup_for_process(proc->pid);

    // 2. Clean up file descriptors
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].in_use && fd_table[i].process_id == proc->pid) {
            fs_close(fd_table[i].fd);
        }
    }

    // 3. Flush ring buffers (process results)
    if (proc->result_ring) {
        // Ensure user gets all pending results
        result_ring_flush(proc->result_ring);
    }

    // 4. Free memory (VMM)
    if (proc->vmm_context) {
        vmm_destroy_context(proc->vmm_context);
    }

    // 5. Free physical pages
    if (proc->code_phys) {
        pmm_free((void*)proc->code_phys, proc->code_pages);
    }
    if (proc->stack_phys) {
        pmm_free((void*)proc->stack_phys, USER_STACK_SIZE / PMM_PAGE_SIZE);
    }
    if (proc->rings_phys) {
        pmm_free((void*)proc->rings_phys, proc->rings_pages);
    }

    // 6. Remove from scheduler
    scheduler_remove_process(proc);

    // 7. Mark as terminated
    proc->state = PROCESS_STATE_TERMINATED;
    proc->exit_code = exit_code;

    kprintf("[PROCESS] PID %lu cleanup complete\n", proc->pid);
}
```

**Testing:**
- [ ] Test: Process exits normally (kernel_notify EXIT)
- [ ] Test: Process crashes (divide by zero)
- [ ] Test: Process infinite loop (timeout kill)
- [ ] Test: Process leaks memory (detect and cleanup)

---

### Week 3-4: Workflow DAG Execution (P0)

**Цель:** DAG работает КОРРЕКТНО, без deadlocks или race conditions

#### Task 2.1: DAG Analysis with Validation

```c
// workflow.c - SAFE DAG execution
int workflow_submit_ready_nodes(Workflow* wf) {
    if (!wf) return -1;

    // Defensive: check workflow state
    if (wf->state != WORKFLOW_STATE_READY &&
        wf->state != WORKFLOW_STATE_RUNNING) {
        kprintf("[WORKFLOW] ERROR: Workflow %lu not ready (state=%d)\n",
                wf->workflow_id, wf->state);
        return -1;
    }

    uint32_t ready_indices[WORKFLOW_MAX_EVENTS];
    int ready_count = 0;

    // Find nodes that are ready (all dependencies met)
    for (uint32_t i = 0; i < wf->event_count; i++) {
        WorkflowNode* node = &wf->events[i];

        // Skip if already completed or running
        if (node->completed || !node->ready) {
            continue;
        }

        // Verify ALL dependencies are met
        int deps_met = 1;
        for (uint32_t j = 0; j < node->dependency_count; j++) {
            uint32_t dep_idx = node->dependencies[j];

            // DEFENSIVE: validate dependency index
            if (dep_idx >= wf->event_count) {
                kprintf("[WORKFLOW] ERROR: Invalid dependency %u for node %u\n",
                        dep_idx, i);
                node->error = 1;
                node->last_error_code = ERROR_INVALID_WORKFLOW;
                deps_met = 0;
                break;
            }

            if (!wf->events[dep_idx].completed) {
                deps_met = 0;
                break;
            }

            // CRITICAL: Check if dependency FAILED
            if (wf->events[dep_idx].error) {
                kprintf("[WORKFLOW] Node %u skipped (dependency %u failed)\n",
                        i, dep_idx);
                node->error = 1;
                node->last_error_code = ERROR_DEPENDENCY_FAILED;
                deps_met = 0;
                break;
            }
        }

        if (deps_met && ready_count < WORKFLOW_MAX_EVENTS) {
            ready_indices[ready_count++] = i;
        }
    }

    // Submit ready nodes as events
    for (int i = 0; i < ready_count; i++) {
        // Create and submit event (через Guide)
        int result = workflow_submit_node(wf, ready_indices[i]);
        if (result != 0) {
            kprintf("[WORKFLOW] WARNING: Failed to submit node %u\n",
                    ready_indices[i]);
        }
    }

    return ready_count;
}
```

**Deadlock Prevention:**
```c
// workflow.c - Cycle detection
int workflow_validate_dag(Workflow* wf) {
    // Check for cycles using DFS
    uint8_t visited[WORKFLOW_MAX_EVENTS] = {0};
    uint8_t rec_stack[WORKFLOW_MAX_EVENTS] = {0};

    for (uint32_t i = 0; i < wf->event_count; i++) {
        if (!visited[i]) {
            if (dfs_cycle_check(wf, i, visited, rec_stack)) {
                kprintf("[WORKFLOW] ERROR: Cycle detected in workflow %lu!\n",
                        wf->workflow_id);
                return -1;  // REJECT workflow!
            }
        }
    }

    return 0;  // DAG is valid
}
```

**Testing:**
- [ ] Test: Linear workflow (A→B→C)
- [ ] Test: Parallel workflow (A→B, A→C, B+C→D)
- [ ] Test: Diamond dependency (A→B+C, B+C→D)
- [ ] Test: CYCLE DETECTION (should reject)
- [ ] Test: Invalid dependency index (should reject)
- [ ] Test: Dependency fails (dependent node should not run)

---

### Week 5-6: Error Handling & Recovery (P1)

**Цель:** Система НИКОГДА не крашится, всегда восстанавливается

#### Task 3.1: Comprehensive Error Handling

**Deck Error Handling:**
```c
// operations_deck.c - Example
static int operations_process_event(RoutingEntry* entry) {
    Event* event = &entry->event_copy;

    // VALIDATE input
    if (event->type < 100 || event->type >= 200) {
        deck_error_detailed(entry, DECK_PREFIX_OPERATIONS,
                          ERROR_INVALID_EVENT_TYPE,
                          "Invalid operation type");
        return 0;  // Don't crash!
    }

    // Try operation with error handling
    switch (event->type) {
        case EVENT_OP_HASH_CRC32: {
            // Validate payload
            if (event->data[0] == 0 || event->data[0] > 1024*1024) {
                deck_error_detailed(entry, DECK_PREFIX_OPERATIONS,
                                  ERROR_INVALID_PAYLOAD,
                                  "CRC32: invalid data size");
                return 0;
            }

            // Perform operation
            uint32_t crc = crc32_compute(&event->data[8], event->data[0]);

            // Return result
            uint32_t* result = (uint32_t*)kmalloc(sizeof(uint32_t));
            if (!result) {
                deck_error_detailed(entry, DECK_PREFIX_OPERATIONS,
                                  ERROR_OUT_OF_MEMORY,
                                  "Failed to allocate result");
                return 0;
            }

            *result = crc;
            deck_complete(entry, DECK_PREFIX_OPERATIONS,
                         result, RESULT_TYPE_KMALLOC);
            return 1;
        }

        default:
            deck_error_detailed(entry, DECK_PREFIX_OPERATIONS,
                              ERROR_NOT_IMPLEMENTED,
                              "Operation not implemented");
            return 0;
    }
}
```

**Global Error Recovery:**
```c
// errors.c - Centralized error handling
void kernel_error_handler(ErrorContext* ctx) {
    // Log error
    error_log(ctx);

    // Check if critical
    if (ctx->severity == ERROR_SEVERITY_CRITICAL) {
        kprintf("[KERNEL] CRITICAL ERROR: %s\n", ctx->message);

        // Try to save state
        tagfs_sync_all();

        // Kill offending process (if any)
        if (ctx->process_id > 0) {
            process_t* proc = process_get_by_pid(ctx->process_id);
            if (proc) {
                process_exit(proc, -1);
            }
        }

        // DON'T PANIC - system continues!
        return;
    }

    // For transient errors - retry
    if (ctx->severity == ERROR_SEVERITY_TRANSIENT) {
        // Guide will retry event
        return;
    }
}
```

**Testing:**
- [ ] Test: Invalid event type (should log, not crash)
- [ ] Test: Out of memory (should fail gracefully)
- [ ] Test: Divide by zero in user process (kernel survives)
- [ ] Test: Corrupted ring buffer (detect and recover)
- [ ] Test: Disk I/O error (retry logic)

---

## 📋 Phase 2: Comprehensive Testing (2-3 недели)

### Week 7-8: Automated Test Suite

**Goal:** 90%+ code coverage для core components

#### Test Categories:

**1. Unit Tests (каждый deck):**
```asm
; test_operations.asm
_test_crc32_basic:
    ; Submit CRC32 event
    ; Verify result matches expected
    ; Return PASS/FAIL

_test_crc32_empty:
    ; Edge case: empty data
    ; Should return error gracefully

_test_crc32_large:
    ; Edge case: 1MB data
    ; Should handle or reject gracefully
```

**2. Integration Tests (workflows):**
```asm
; test_workflow_linear.asm
_test_linear_workflow:
    ; Create workflow: A→B→C
    ; Verify execution order
    ; Verify results propagate

_test_parallel_workflow:
    ; Create workflow: A→B+C→D
    ; Verify B and C run in parallel
    ; Verify D waits for both
```

**3. Stress Tests (stability):**
```asm
; test_stress.asm
_test_1000_events:
    ; Submit 1000 events rapidly
    ; Verify all complete
    ; Check for memory leaks

_test_process_churn:
    ; Create/destroy 100 processes
    ; Verify no resource leaks
```

**4. Failure Tests (error handling):**
```asm
; test_failures.asm
_test_invalid_event:
    ; Submit malformed event
    ; Kernel should survive

_test_process_crash:
    ; Process divides by zero
    ; Kernel should kill process, continue
```

#### Test Framework:
```c
// test_framework.c (in kernel)
typedef struct {
    const char* name;
    int (*test_func)(void);
    int passed;
} TestCase;

void run_all_tests(void) {
    TestCase tests[] = {
        {"Multi-process switching", test_context_switch, 0},
        {"Workflow linear execution", test_workflow_linear, 0},
        {"Error handling", test_error_recovery, 0},
        // ... 50+ tests
    };

    int total = sizeof(tests) / sizeof(TestCase);
    int passed = 0;

    for (int i = 0; i < total; i++) {
        kprintf("\n[TEST %d/%d] %s...\n", i+1, total, tests[i].name);

        if (tests[i].test_func() == 0) {
            kprintf("✅ PASS\n");
            tests[i].passed = 1;
            passed++;
        } else {
            kprintf("❌ FAIL\n");
        }
    }

    kprintf("\n========================================\n");
    kprintf("Results: %d/%d tests passed (%.1f%%)\n",
            passed, total, (float)passed * 100 / total);
    kprintf("========================================\n");
}
```

---

### Week 9: Memory Safety & Resource Management

**Goal:** ZERO memory leaks, ZERO resource leaks

#### Memory Leak Detection:
```c
// pmm.c - Track allocations
typedef struct {
    uint64_t total_allocs;
    uint64_t total_frees;
    uint64_t bytes_allocated;
    uint64_t bytes_freed;
    uint64_t peak_usage;
} MemStats;

void pmm_print_leak_report(void) {
    if (mem_stats.total_allocs != mem_stats.total_frees) {
        kprintf("⚠️  MEMORY LEAK DETECTED!\n");
        kprintf("   Allocations: %lu\n", mem_stats.total_allocs);
        kprintf("   Frees: %lu\n", mem_stats.total_frees);
        kprintf("   Leaked: %lu blocks\n",
                mem_stats.total_allocs - mem_stats.total_frees);
    } else {
        kprintf("✅ No memory leaks\n");
    }
}
```

**Testing:**
- [ ] Run 1000 workflows, check PMM stats (should be zero leaks)
- [ ] Create/destroy 100 processes, check leaks
- [ ] Open/close 1000 files, check FD leaks

---

## 📋 Phase 3: Production Hardening (2-3 недели)

### Week 10-11: Resource Limits & Quotas

**Goal:** Система защищена от resource exhaustion

```c
// process.h - Resource quotas
typedef struct {
    uint64_t max_memory;      // Max bytes
    uint64_t max_open_files;  // Max FDs
    uint64_t max_workflows;   // Max workflows
    uint64_t cpu_quota;       // Time slice quota
} ResourceQuota;

// Enforcement in decks
int storage_deck_alloc_memory(process_t* proc, uint64_t size) {
    if (proc->memory_used + size > proc->quota.max_memory) {
        return -ERROR_QUOTA_EXCEEDED;
    }

    void* mem = vmm_alloc_pages(...);
    if (mem) {
        proc->memory_used += size;
    }
    return mem ? 0 : -ERROR_OUT_OF_MEMORY;
}
```

---

### Week 12: Observability & Debugging

**Goal:** Easy to diagnose issues in production

```c
// metrics.c - System metrics
typedef struct {
    uint64_t uptime_ticks;
    uint64_t total_events_processed;
    uint64_t total_errors;
    uint64_t context_switches;
    uint64_t page_faults;
    MemStats memory;
    DiskStats disk;
} SystemMetrics;

// Export via special file
// /sys/metrics (read via TagFS)
```

---

## 🎯 Success Criteria for v1.0

### Functional Requirements:
- ✅ Multi-process execution (10+ processes stable)
- ✅ Workflow DAG execution (parallel + sequential)
- ✅ All 4 decks working (Operations, Storage, Hardware, Execution)
- ✅ TagFS persistence (data survives reboot)
- ✅ Process isolation (crash doesn't affect others)

### Reliability Requirements:
- ✅ **90%+ test coverage**
- ✅ **1000+ events processed without crash**
- ✅ **Zero memory leaks** (validated by tests)
- ✅ **All errors handled gracefully** (no panics)
- ✅ **24-hour stress test passes**

### Quality Requirements:
- ✅ All code reviewed
- ✅ All functions documented
- ✅ User manual written
- ✅ Architecture guide complete

---

## 📊 Timeline Estimate

**Optimistic:** 10-12 недель (2.5-3 месяца)
**Realistic:** 12-16 недель (3-4 месяца)
**Pessimistic:** 16-20 недель (4-5 месяцев)

**Why realistic > optimistic?**
- Testing takes time
- Edge cases always appear
- Bugs are sneaky
- Quality can't be rushed

---

## 🚀 Next Immediate Steps

**This Week:**
1. Implement context switching (scheduler.c)
2. Write 5 tests for context switching
3. Run tests until all pass

**Next Week:**
4. Implement workflow DAG execution
5. Write 10 tests for workflows
6. Fix all bugs found

**Philosophy:**
> "Make it work, make it right, make it fast" - в таком порядке!
> Сейчас фокус на "make it RIGHT" (надежность)

---

Готов начать? С чего хотите начать:

**A)** Context switching (critical, opens door for multi-process)
**B)** Workflow DAG (core innovation of your OS)
**C)** Error handling (defensive foundation)
**D)** Test framework (infrastructure for quality)

Рекомендую **A** (context switching), потому что это разблокирует всё остальное.
