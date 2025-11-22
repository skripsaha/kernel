#include "routing_table.h"
#include "workflow_rings.h"
#include "klib.h"

// ============================================================================
// GLOBAL ROUTING TABLE
// ============================================================================

RoutingTable global_routing_table;

// ============================================================================
// INITIALIZATION
// ============================================================================

void routing_table_init(RoutingTable* table) {
    kprintf("[ROUTING_TABLE] Initializing...\n");

    // Initialize all buckets
    memset(table, 0, sizeof(RoutingTable));

    kprintf("[ROUTING_TABLE] Initialized (size=%d buckets, UNLIMITED capacity via linked lists)\n",
            ROUTING_TABLE_SIZE);
}

// ============================================================================
// INSERT - Вставка routing entry (DYNAMIC VERSION with linked list)
// ============================================================================

int routing_table_insert(RoutingTable* table, RoutingEntry* entry) {
    uint64_t index = routing_table_index(entry->event_id);
    RoutingBucket* bucket = &table->buckets[index];

    // Allocate new entry on heap (CRITICAL: allows unlimited entries!)
    RoutingEntry* new_entry = (RoutingEntry*)kmalloc(sizeof(RoutingEntry));
    if (!new_entry) {
        kprintf("[ROUTING_TABLE] ERROR: Out of memory for routing entry!\n");
        return 0;  // Out of memory
    }

    // Copy entry data
    *new_entry = *entry;
    new_entry->next = NULL;

    bucket_lock(bucket);

    // Insert at head of linked list (O(1) insertion)
    new_entry->next = bucket->head;
    bucket->head = new_entry;
    bucket->count++;
    atomic_increment_u64(&table->total_entries);

    bucket_unlock(bucket);
    return 1;  // Success
}

// ============================================================================
// LOOKUP - Поиск routing entry (DYNAMIC VERSION with linked list)
// ============================================================================

RoutingEntry* routing_table_lookup(RoutingTable* table, uint64_t event_id) {
    uint64_t index = routing_table_index(event_id);
    RoutingBucket* bucket = &table->buckets[index];

    bucket_lock(bucket);

    // Walk linked list to find entry
    RoutingEntry* current = bucket->head;
    while (current) {
        if (current->event_id == event_id) {
            bucket_unlock(bucket);
            return current;  // Found
        }
        current = current->next;
    }

    bucket_unlock(bucket);
    return NULL;  // Not found
}

// ============================================================================
// REMOVE - Удаление routing entry (DYNAMIC VERSION with linked list)
// ============================================================================

int routing_table_remove(RoutingTable* table, uint64_t event_id) {
    uint64_t index = routing_table_index(event_id);
    RoutingBucket* bucket = &table->buckets[index];

    bucket_lock(bucket);

    // Walk linked list to find and remove entry
    RoutingEntry* current = bucket->head;
    RoutingEntry* prev = NULL;

    while (current) {
        if (current->event_id == event_id) {
            // Found - remove from list
            if (prev) {
                // Not head - update previous node's next pointer
                prev->next = current->next;
            } else {
                // Head node - update bucket head
                bucket->head = current->next;
            }

            bucket->count--;
            atomic_decrement_u64(&table->total_entries);
            bucket_unlock(bucket);

            // CRITICAL: Free the entry memory (was allocated in insert)
            kfree(current);

            return 1;  // Success
        }

        prev = current;
        current = current->next;
    }

    bucket_unlock(bucket);
    return 0;  // Not found
}

// ============================================================================
// ADD RING EVENT - Create RoutingEntry from RingEvent and insert
// ============================================================================

int routing_table_add_event(RoutingTable* table, void* ring_event_ptr) {
    RingEvent* ring_event = (RingEvent*)ring_event_ptr;

    // Create RoutingEntry from RingEvent
    RoutingEntry entry;
    entry.event_id = ring_event->id;

    // Copy route from RingEvent to RoutingEntry prefixes
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        entry.prefixes[i] = ring_event->route[i];
    }

    entry.current_index = 0;
    entry.completion_flags = 0;
    entry.state = EVENT_STATUS_PROCESSING;
    entry.created_at = ring_event->timestamp;
    entry.abort_flag = 0;
    entry.error_code = 0;

    // Initialize deck_results and deck_timestamps
    for (int i = 0; i < MAX_ROUTING_STEPS; i++) {
        entry.deck_results[i] = 0;
        entry.deck_timestamps[i] = 0;
    }

    // Copy RingEvent data to Event structure
    entry.event_copy.id = ring_event->id;
    entry.event_copy.user_id = ring_event->workflow_id;  // workflow_id maps to user_id
    entry.event_copy.timestamp = ring_event->timestamp;
    entry.event_copy.type = ring_event->type;
    entry.event_copy.flags = 0;

    // Copy payload (up to EVENT_DATA_SIZE = 224 bytes)
    uint32_t copy_size = ring_event->payload_size;
    if (copy_size > EVENT_DATA_SIZE) {
        copy_size = EVENT_DATA_SIZE;
    }
    memcpy(entry.event_copy.data, ring_event->payload, copy_size);

    // Zero remaining data
    if (copy_size < EVENT_DATA_SIZE) {
        memset(entry.event_copy.data + copy_size, 0, EVENT_DATA_SIZE - copy_size);
    }

    // Insert into routing table
    int result = routing_table_insert(table, &entry);

    if (result == 0) {
        // Calculate which bucket this event is in
        uint64_t bucket_index = ring_event->id % ROUTING_TABLE_SIZE;
        kprintf("[ROUTING] Added event ID=%lu to bucket %lu (route=[%u,%u,%u,%u])\n",
                ring_event->id, bucket_index,
                entry.prefixes[0], entry.prefixes[1], entry.prefixes[2], entry.prefixes[3]);
    }

    return result;
}

// ============================================================================
// STATISTICS
// ============================================================================

void routing_table_print_stats(RoutingTable* table) {
    uint64_t total = atomic_load_u64(&table->total_entries);
    uint64_t collisions = atomic_load_u64(&table->collisions);

    // Calculate average chain length per bucket
    uint64_t avg_chain_length = total / ROUTING_TABLE_SIZE;

    kprintf("[ROUTING_TABLE] entries=%lu collisions=%lu avg_chain=%lu (UNLIMITED capacity)\n",
            total, collisions, avg_chain_length);
}
