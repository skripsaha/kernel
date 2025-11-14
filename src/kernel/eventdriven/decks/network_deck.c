#include "deck_interface.h"
#include "klib.h"

// ============================================================================
// NETWORK DECK - Network Operations (STUB для v1)
// ============================================================================
// В первой версии ядра нет полноценного сетевого стека.
// Network deck реализуется в будущих версиях.
// ============================================================================

int network_deck_process(RoutingEntry* entry) {
    Event* event = &entry->event_copy;

    switch (event->type) {
        case EVENT_NET_SOCKET:
            kprintf("[NETWORK] Event %lu: socket() - STUB\n", event->id);
            deck_complete(entry, DECK_PREFIX_NETWORK, (void*)100);  // Fake socket fd
            return 1;

        case EVENT_NET_CONNECT: {
            // Payload: [socket_fd:4][addr:...]
            int socket_fd = *(int*)event->data;
            kprintf("[NETWORK] Event %lu: connect(fd=%d) - STUB\n",
                    event->id, socket_fd);
            deck_complete(entry, DECK_PREFIX_NETWORK, 0);
            return 1;
        }

        case EVENT_NET_SEND: {
            // Payload: [socket_fd:4][size:8][data:...]
            int socket_fd = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            kprintf("[NETWORK] Event %lu: send(fd=%d, size=%lu) - STUB\n",
                    event->id, socket_fd, size);
            deck_complete(entry, DECK_PREFIX_NETWORK, (void*)size);  // Return bytes sent
            return 1;
        }

        case EVENT_NET_RECV: {
            // Payload: [socket_fd:4][max_size:8]
            int socket_fd = *(int*)event->data;
            uint64_t max_size = *(uint64_t*)(event->data + 4);
            kprintf("[NETWORK] Event %lu: recv(fd=%d, max_size=%lu) - STUB\n",
                    event->id, socket_fd, max_size);
            deck_complete(entry, DECK_PREFIX_NETWORK, 0);  // Return 0 bytes received
            return 1;
        }

        default:
            kprintf("[NETWORK] Unknown event type %d - STUB\n", event->type);
            deck_error(entry, DECK_PREFIX_NETWORK, 1);
            return 0;
    }
}

// ============================================================================
// INITIALIZATION & RUN
// ============================================================================

DeckContext network_deck_context;

void network_deck_init(void) {
    deck_init(&network_deck_context, "Network", DECK_PREFIX_NETWORK, network_deck_process);
    kprintf("[NETWORK] Initialized (STUB - no network stack in v1)\n");
}

int network_deck_run_once(void) {
    return deck_run_once(&network_deck_context);
}

void network_deck_run(void) {
    deck_run(&network_deck_context);
}
