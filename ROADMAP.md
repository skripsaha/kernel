# Kernel Workflow Engine - Production Roadmap

**Цель:** Довести систему до production-ready состояния

---

## Phase 1: Critical Fixes (Week 1-2) 🔥

### Week 1: Multi-Process Execution

**Задача 1.1: Context Switching в Timer IRQ**
- [ ] Модифицировать `timer_irq_handler()` для вызова `scheduler_schedule()`
- [ ] Реализовать `save_context()` и `restore_context()`
- [ ] Переключение CR3 (page tables) при context switch
- [ ] Тестирование: 2+ процесса должны выполняться параллельно

**Файлы:**
- `src/kernel/drivers/timer/pit.c`
- `src/kernel/scheduler/scheduler.c`
- `src/kernel/arch/x86-64/context/context_switch.asm`

**Acceptance:**
```
✅ Создано 3 процесса
✅ Каждый выводит уникальное сообщение в цикле
✅ Все 3 процесса чередуются (видно в serial log)
```

---

**Задача 1.2: Fix Scheduler Integration**
- [ ] Убедиться что `scheduler_add_process()` вызывается для всех процессов
- [ ] Проверить что `scheduler_pick_next()` работает корректно
- [ ] Добавить `scheduler_yield()` для cooperative scheduling
- [ ] Протестировать NOTIFY_YIELD flag

**Acceptance:**
```
✅ kernel_notify(workflow_id, NOTIFY_YIELD) переключает процесс
✅ Scheduler корректно обрабатывает READY/RUNNING/WAITING states
```

---

### Week 2: Workflow DAG Execution

**Задача 2.1: DAG Analysis Integration**
- [ ] Guide при получении события должен проверять `workflow_get(workflow_id)`
- [ ] Вызывать `workflow_find_parallel_events()` для поиска готовых nodes
- [ ] Автоматически создавать RingEvents для готовых nodes
- [ ] Push их в EventRing (внутри kernel)

**Файлы:**
- `src/kernel/eventdriven/guide/guide.c`
- `src/kernel/eventdriven/workflow/workflow.c`

**Пример:**
```c
void guide_process_workflow_event(RoutingEntry* entry) {
    uint64_t workflow_id = entry->event_copy.user_id;
    Workflow* wf = workflow_get(workflow_id);

    if (wf) {
        // Find ready nodes
        uint32_t ready_indices[16];
        int ready_count = workflow_find_parallel_events(wf, ready_indices, 16);

        // Submit ready nodes as events
        for (int i = 0; i < ready_count; i++) {
            WorkflowNode* node = &wf->events[ready_indices[i]];
            // Create RingEvent from node
            // Submit to routing table
        }
    }

    // Continue normal routing
}
```

**Acceptance:**
```
✅ Workflow с 3 независимыми nodes выполняется параллельно
✅ Workflow с зависимостями (A→B→C) выполняется последовательно
✅ Callback workflow_on_event_completed() триггерит следующие nodes
```

---

**Задача 2.2: Async Guide Processing**
- [ ] Переместить `guide_process_all()` из timer IRQ в dedicated loop
- [ ] ИЛИ: Trigger Guide только когда EventRing получает новые события
- [ ] Benchmark: latency между push в EventRing и начало обработки

**Файлы:**
- `src/kernel/eventdriven/guide/guide.c`
- `src/kernel/drivers/timer/pit.c`

**Acceptance:**
```
✅ Guide обрабатывает события < 1ms после push в EventRing
✅ Нет busy-wait в timer IRQ
```

---

## Phase 2: Stability & Error Handling (Week 3)

### Week 3: Production Hardening

**Задача 3.1: Error Handling**
- [ ] Все decks используют `deck_error_detailed()` вместо kprintf
- [ ] Error context propagate через RoutingEntry
- [ ] Execution Deck возвращает полный error context в ResultRing
- [ ] Retry logic в Guide для transient errors

**Файлы:**
- `src/kernel/eventdriven/decks/*.c`
- `src/kernel/eventdriven/execution/execution_deck.c`
- `src/kernel/eventdriven/guide/guide.c`

**Acceptance:**
```
✅ Ошибка в deck A не крашит kernel
✅ Error code + message возвращаются в ResultRing
✅ Transient error (например, disk busy) приводит к retry
```

---

**Задача 3.2: Resource Limits**
- [ ] Увеличить `PROCESS_MAX_COUNT` → 32
- [ ] Увеличить `RING_BUFFER_SIZE` → 1024
- [ ] Увеличить `DECK_QUEUE_SIZE` → 512
- [ ] Stress test: 100+ events в секунду

**Файлы:**
- `src/kernel/process/process.h`
- `src/kernel/eventdriven/core/ringbuffer.h`
- `src/kernel/eventdriven/guide/guide.h`

**Acceptance:**
```
✅ 32 процесса создаются без проблем
✅ 1000 events в EventRing не переполняют буфер
✅ Deck queues не блокируются при высокой нагрузке
```

---

**Задача 3.3: TagFS Persistence**
- [ ] Включить `use_disk = 1` в tagfs.c
- [ ] Автоматический sync после каждой операции (ИЛИ: periodic sync)
- [ ] Тестирование: создать файл, перезагрузить, файл должен остаться

**Файлы:**
- `src/kernel/eventdriven/storage/tagfs.c`
- `src/kernel/drivers/disk/ata.c`

**Acceptance:**
```
✅ Файлы сохраняются на диск
✅ После перезагрузки файлы восстанавливаются
✅ Нет data corruption
```

---

## Phase 3: Testing & Validation (Week 4)

### Week 4: Test Suite

**Задача 4.1: Automated Tests**
- [ ] Test framework в userspace
- [ ] 10+ unit tests для каждого deck
- [ ] Integration tests для workflow scenarios
- [ ] Stress tests для concurrency

**Новые файлы:**
- `src/userspace/test_framework.asm`
- `src/userspace/tests/test_operations.asm`
- `src/userspace/tests/test_storage.asm`
- `src/userspace/tests/test_workflow_dag.asm`

**Acceptance:**
```
✅ All tests pass
✅ Test runner выводит PASS/FAIL для каждого теста
✅ Coverage > 70% для core components
```

---

**Задача 4.2: Performance Benchmarks**
- [ ] Latency measurements (rdtsc)
- [ ] Throughput tests (events/sec)
- [ ] Memory usage profiling
- [ ] Bottleneck identification

**Acceptance:**
```
✅ Latency: EventRing push → ResultRing result < 10ms (99th percentile)
✅ Throughput: > 10,000 events/sec
✅ Memory: < 100MB для 32 процессов
```

---

## Phase 4: Feature Completeness (Week 5-8)

### Week 5-6: Network Stack (Optional)

**Задача 5.1: Minimal UDP Stack**
- [ ] Ethernet frame parsing
- [ ] IP packet handling
- [ ] UDP socket operations
- [ ] Network Deck integration

**Файлы:**
- `src/kernel/eventdriven/decks/network_deck.c` (заменить stub)
- `src/kernel/drivers/network/` (новые файлы)

**Acceptance:**
```
✅ Send UDP packet
✅ Receive UDP packet
✅ Event routing: [4, 0] works (Network → Execution)
```

---

### Week 7: Security & Permissions

**Задача 6.1: Permission Checks**
- [ ] Process ownership для workflows
- [ ] Resource quotas (memory, file descriptors)
- [ ] Deck permission checks (например, network только для root)

**Файлы:**
- `src/kernel/eventdriven/decks/deck_interface.c`
- `src/kernel/process/process.h`

**Acceptance:**
```
✅ Процесс не может access чужой workflow
✅ Memory quota enforcement работает
✅ Non-privileged процесс не может использовать network deck
```

---

### Week 8: Documentation & Polish

**Задача 7.1: Developer Documentation**
- [ ] API reference (workflow registration, event types)
- [ ] Architecture diagrams
- [ ] Example workflows (видео обработка, веб-сервер)
- [ ] Porting guide (как добавить новый deck)

**Новые файлы:**
- `docs/API.md`
- `docs/ARCHITECTURE.md`
- `docs/EXAMPLES.md`
- `docs/PORTING.md`

---

## Success Metrics

### Minimal Production-Ready (Week 4)
- ✅ Multi-process execution works
- ✅ Workflow DAG automatic execution
- ✅ Error handling robust
- ✅ 10+ automated tests passing
- ✅ TagFS persistence enabled

### Full Production (Week 8)
- ✅ All of above +
- ✅ Network stack working (UDP)
- ✅ Security & permissions
- ✅ Performance benchmarks met
- ✅ Documentation complete

---

## Risk Mitigation

**Risk 1: Context switching bugs**
- Mitigation: Тестировать с 2 процессами сначала, потом масштабировать
- Fallback: Single-process mode для debugging

**Risk 2: Workflow DAG complexity**
- Mitigation: Start с simple linear workflows
- Fallback: Manual event submission (current behavior)

**Risk 3: Performance degradation**
- Mitigation: Benchmark на каждом этапе
- Fallback: Rollback changes если latency spike

---

## Resources Needed

**Development:**
- Time: 4-8 недель (зависит от scope)
- Hardware: QEMU достаточно, real hardware опционально

**Testing:**
- Automated test suite
- CI/CD (GitHub Actions)
- Performance profiling tools

---

## Next Steps (Immediate)

1. **Прочитать этот roadmap** ✅
2. **Выбрать приоритет:**
   - Option A: Minimal (4 weeks) → Multi-process + Workflow DAG + Tests
   - Option B: Full (8 weeks) → All features включая network
3. **Начать с Task 1.1** (Context Switching)
4. **Daily progress tracking** в этом файле

---

**Let's build this! 🚀**
