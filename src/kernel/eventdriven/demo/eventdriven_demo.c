#include "eventdriven_demo.h"
#include "../eventdriven_system.h"
#include "../userlib/eventapi.h"
#include "../task/task.h"
#include "klib.h"

// ============================================================================
// DEMO - Симуляция user space приложения
// ============================================================================

// ============================================================================
// TASK TEST FUNCTIONS
// ============================================================================

// Test task 1: Simple task that increments counter
static volatile int test_counter = 0;

void test_task_simple(void* args) {
    (void)args;
    uint64_t task_id = task_get_current_id();

    kprintf("[TASK_TEST] Simple task started (ID=%lu)\n", task_id);

    for (int i = 0; i < 5; i++) {
        test_counter++;
        kprintf("[TASK_TEST] Task %lu: counter = %d\n", task_id, test_counter);

        // Simulate some work
        for (volatile int j = 0; j < 100000; j++);
    }

    kprintf("[TASK_TEST] Simple task completed (ID=%lu)\n", task_id);
}

// Test task 2: Task with sleep
void test_task_sleepy(void* args) {
    uint64_t sleep_ms = (uint64_t)args;
    uint64_t task_id = task_get_current_id();

    kprintf("[TASK_TEST] Sleepy task started (ID=%lu, will sleep %lu ms)\n",
            task_id, sleep_ms);

    // Sleep for specified time
    task_sleep(task_id, sleep_ms);

    kprintf("[TASK_TEST] Sleepy task woke up! (ID=%lu)\n", task_id);
}

// Test task 3: High energy task
void test_task_high_energy(void* args) {
    (void)args;
    uint64_t task_id = task_get_current_id();

    kprintf("[TASK_TEST] High-energy task started (ID=%lu, energy=90)\n", task_id);

    for (int i = 0; i < 3; i++) {
        kprintf("[TASK_TEST] High-energy working hard! (%d/3)\n", i + 1);
        for (volatile int j = 0; j < 200000; j++);
    }

    kprintf("[TASK_TEST] High-energy task completed (ID=%lu)\n", task_id);
}

// Test task 4: Low energy task
void test_task_low_energy(void* args) {
    (void)args;
    uint64_t task_id = task_get_current_id();

    kprintf("[TASK_TEST] Low-energy task started (ID=%lu, energy=20)\n", task_id);

    for (int i = 0; i < 2; i++) {
        kprintf("[TASK_TEST] Low-energy working slowly... (%d/2)\n", i + 1);
        for (volatile int j = 0; j < 100000; j++);
    }

    kprintf("[TASK_TEST] Low-energy task completed (ID=%lu)\n", task_id);
}

void eventdriven_demo_run(void) {
    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  EVENT-DRIVEN ARCHITECTURE DEMONSTRATION\n");
    kprintf("============================================================\n");
    kprintf("\n");

    // 1. Получаем доступ к ring buffers
    EventRingBuffer* to_kernel = eventdriven_get_user_to_kernel_ring();
    ResponseRingBuffer* from_kernel = eventdriven_get_kernel_to_user_ring();

    // 2. Инициализируем user API
    eventapi_init(to_kernel, from_kernel);

    kprintf("[DEMO] User API initialized\n");
    kprintf("\n");

    // ========================================================================
    // ТЕСТ 1: Memory Allocation
    // ========================================================================

    kprintf("--- TEST 1: Asynchronous Memory Allocation ---\n");
    kprintf("[DEMO] Requesting memory allocation (4096 bytes)...\n");

    uint64_t event_id_1 = eventapi_memory_alloc(4096);
    kprintf("[DEMO] Event submitted (async, no blocking!)\n");

    // Симулируем другую работу (не блокируемся!)
    kprintf("[DEMO] Doing other work while kernel processes request...\n");
    for (int i = 0; i < 5; i++) {
        kprintf("[DEMO] Working... (%d/5)\n", i + 1);

        // Обрабатываем события в фоне (в реальной системе это работает на других ядрах)
        eventdriven_process_events(50);  // 50 итераций pipeline

        delay(1000);  // Симуляция работы
    }

    kprintf("[DEMO] Checking for response...\n");
    Response* resp1 = eventapi_poll_response(event_id_1);

    if (resp1) {
        if (resp1->status == EVENT_STATUS_SUCCESS) {
            kprintf("[DEMO] SUCCESS! Memory allocated\n");
            kprintf("[DEMO] Result: %p\n", *(void**)resp1->result);
        } else {
            kprintf("[DEMO] FAILED: status=%d\n", resp1->status);
        }
    } else {
        kprintf("[DEMO] Response not ready yet (would continue polling in real app)\n");
    }

    kprintf("\n");

    // ========================================================================
    // ТЕСТ 2: File Open
    // ========================================================================

    kprintf("--- TEST 2: Asynchronous File Open ---\n");
    kprintf("[DEMO] Opening file '/test.txt'...\n");

    uint64_t event_id_2 = eventapi_file_open("/test.txt");
    kprintf("[DEMO] Event submitted (async!)\n");

    kprintf("[DEMO] Doing other work...\n");
    eventdriven_process_events(100);  // Обрабатываем события
    delay(2000);

    kprintf("[DEMO] Checking for response...\n");
    Response* resp2 = eventapi_poll_response(event_id_2);

    if (resp2) {
        if (resp2->status == EVENT_STATUS_SUCCESS) {
            kprintf("[DEMO] SUCCESS! File opened\n");
        } else {
            kprintf("[DEMO] FAILED\n");
        }
    } else {
        kprintf("[DEMO] Response not ready yet\n");
    }

    kprintf("\n");

    // ========================================================================
    // ТЕСТ 3: Batch Events
    // ========================================================================

    kprintf("--- TEST 3: Batch Event Submission ---\n");
    kprintf("[DEMO] Submitting 10 events in rapid succession...\n");

    for (int i = 0; i < 10; i++) {
        uint64_t size = 1024 * (i + 1);
        eventapi_memory_alloc(size);
        kprintf("[DEMO] Event %d submitted (size=%lu)\n", i, size);
    }

    kprintf("[DEMO] All 10 events submitted WITHOUT BLOCKING!\n");
    kprintf("[DEMO] Kernel will process them in parallel...\n");

    // Обрабатываем все события
    eventdriven_process_events(200);

    kprintf("\n");

    // ========================================================================
    // СТАТИСТИКА
    // ========================================================================

    kprintf("--- SYSTEM STATISTICS ---\n");
    eventdriven_print_full_stats();

    kprintf("\n");

    // ========================================================================
    // ТЕСТ 4: TASK SYSTEM
    // ========================================================================

    kprintf("============================================================\n");
    kprintf("  TASK SYSTEM DEMONSTRATION\n");
    kprintf("============================================================\n");
    kprintf("\n");

    kprintf("--- TEST 4: Task Creation & Execution ---\n");

    // Test 1: Spawn simple task
    kprintf("\n[DEMO] Creating simple task with medium energy (50)...\n");
    Task* task1 = task_spawn("SimpleTask", test_task_simple, 50);
    if (task1) {
        kprintf("[DEMO] Task spawned! ID=%lu\n", task1->task_id);
        task_print_stats(task1->task_id);

        // Give it time to run
        kprintf("[DEMO] Letting task run...\n");
        for (volatile int i = 0; i < 1000000; i++);

        kprintf("[DEMO] Test counter value: %d (should be > 0)\n", test_counter);
    }

    kprintf("\n--- TEST 5: Task Energy System ---\n");

    // Test 2: High energy task
    kprintf("\n[DEMO] Creating high-energy task (energy=90)...\n");
    Task* task2 = task_spawn("HighEnergyTask", test_task_high_energy, 90);
    if (task2) {
        kprintf("[DEMO] High-energy task spawned! ID=%lu\n", task2->task_id);
    }

    // Test 3: Low energy task
    kprintf("\n[DEMO] Creating low-energy task (energy=20)...\n");
    Task* task3 = task_spawn("LowEnergyTask", test_task_low_energy, 20);
    if (task3) {
        kprintf("[DEMO] Low-energy task spawned! ID=%lu\n", task3->task_id);
    }

    // Let them run
    kprintf("\n[DEMO] Letting tasks run...\n");
    for (volatile int i = 0; i < 2000000; i++);

    kprintf("\n--- TEST 6: Task Sleep & Wake ---\n");

    // Test 4: Sleepy task
    kprintf("\n[DEMO] Creating sleepy task (will sleep 1000ms)...\n");
    Task* task4 = task_spawn_with_args("SleepyTask", test_task_sleepy, (void*)1000, 60);
    if (task4) {
        kprintf("[DEMO] Sleepy task spawned! ID=%lu\n", task4->task_id);
        kprintf("[DEMO] Task is now sleeping... (state=%d)\n", task4->state);

        // Wait a bit
        kprintf("[DEMO] Waiting for task to wake naturally...\n");
        delay(2000);

        kprintf("[DEMO] Task state after delay: %d (should be awake)\n", task4->state);
    }

    kprintf("\n--- TEST 7: Task Control Operations ---\n");

    // Test 5: Pause/Resume
    kprintf("\n[DEMO] Testing pause/resume...\n");
    Task* task5 = task_spawn("PausableTask", test_task_simple, 50);
    if (task5) {
        kprintf("[DEMO] Task created (ID=%lu)\n", task5->task_id);

        // Pause it
        kprintf("[DEMO] Pausing task...\n");
        task_pause(task5->task_id);
        kprintf("[DEMO] Task state: %d (should be THROTTLED=7)\n", task5->state);

        delay(500);

        // Resume it
        kprintf("[DEMO] Resuming task...\n");
        task_resume(task5->task_id);
        kprintf("[DEMO] Task state: %d (should be RUNNING=0)\n", task5->state);
    }

    kprintf("\n--- TEST 8: Task Groups ---\n");

    // Test 6: Task groups
    kprintf("\n[DEMO] Creating task group 'WorkerGroup'...\n");
    uint64_t group_id = task_group_create("WorkerGroup");
    if (group_id > 0) {
        kprintf("[DEMO] Group created! ID=%lu\n", group_id);

        // Create 3 tasks and add to group
        kprintf("[DEMO] Adding 3 tasks to group...\n");
        for (int i = 0; i < 3; i++) {
            kprintf("[DEMO] Creating worker %d...\n", i);
            Task* worker = task_spawn("Worker", test_task_simple, 50);
            if (worker) {
                task_group_add(group_id, worker->task_id);
                kprintf("[DEMO] Added task %lu to group\n", worker->task_id);
            }
        }

        // Set memory limit for group
        kprintf("[DEMO] Setting group memory limit to 1MB...\n");
        task_group_set_memory_limit(group_id, 1024 * 1024);
    }

    kprintf("\n--- TEST 9: Task Health & Statistics ---\n");

    // Print stats for all active tasks
    kprintf("\n[DEMO] Printing statistics for active tasks...\n");
    if (task1) {
        kprintf("\n");
        task_print_stats(task1->task_id);
    }
    if (task2) {
        kprintf("\n");
        task_print_stats(task2->task_id);
    }

    kprintf("\n");
    kprintf("============================================================\n");
    kprintf("  DEMONSTRATION COMPLETE\n");
    kprintf("============================================================\n");
    kprintf("\n");

    kprintf("KEY TAKEAWAYS:\n");
    kprintf("1. Events submitted ASYNCHRONOUSLY - no blocking!\n");
    kprintf("2. User can continue working while kernel processes\n");
    kprintf("3. Multiple events can be in flight simultaneously\n");
    kprintf("4. Results retrieved via polling (non-blocking)\n");
    kprintf("5. NO SYSCALLS - only lock-free ring buffers!\n");
    kprintf("\n");
    kprintf("6. TASK SYSTEM: Lightweight tasks with energy management\n");
    kprintf("7. Tasks spawn in microseconds (vs milliseconds for processes)\n");
    kprintf("8. Energy-based scheduling adapts to task efficiency\n");
    kprintf("9. Health monitoring tracks responsiveness & stability\n");
    kprintf("10. Task groups enable resource limits & broadcasts\n");
    kprintf("\n");
}
