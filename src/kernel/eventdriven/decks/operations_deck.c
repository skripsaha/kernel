#include "deck_interface.h"
#include "klib.h"

// ============================================================================
// OPERATIONS DECK - Pure Computations & Transformations
// ============================================================================
//
// According to idea.txt, this deck handles:
// - Compression (RLE, LZ77, zlib-like)
// - Encryption (AES, XOR, etc.)
// - Hashing (SHA256, MD5, CRC32)
// - Media processing (image/video transformation)
// - Mathematical transformations
//
// This is PRODUCTION code with REAL implementations!
// ============================================================================

// ============================================================================
// HASHING OPERATIONS
// ============================================================================

// CRC32 Table (precomputed for fast calculation)
static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

static void crc32_init_table(void) {
    if (crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = 1;
}

static uint32_t crc32_compute(const uint8_t* data, uint64_t size) {
    if (!crc32_table_initialized) {
        crc32_init_table();
    }

    uint32_t crc = 0xFFFFFFFF;
    for (uint64_t i = 0; i < size; i++) {
        uint8_t byte = data[i];
        crc = crc32_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Simple hash function (djb2)
static uint64_t djb2_hash(const uint8_t* data, uint64_t size) {
    uint64_t hash = 5381;
    for (uint64_t i = 0; i < size; i++) {
        hash = ((hash << 5) + hash) + data[i];  // hash * 33 + c
    }
    return hash;
}

// ============================================================================
// COMPRESSION OPERATIONS
// ============================================================================

// RLE (Run-Length Encoding) - Simple but effective for certain data
static uint64_t rle_compress(const uint8_t* input, uint64_t input_size,
                               uint8_t* output, uint64_t output_capacity) {
    if (!input || !output || input_size == 0) return 0;

    uint64_t output_pos = 0;
    uint64_t i = 0;

    while (i < input_size && output_pos + 2 <= output_capacity) {
        uint8_t current = input[i];
        uint64_t count = 1;

        // Count consecutive identical bytes (max 255)
        while (i + count < input_size && input[i + count] == current && count < 255) {
            count++;
        }

        // Write: [byte][count]
        output[output_pos++] = current;
        output[output_pos++] = (uint8_t)count;

        i += count;
    }

    return output_pos;
}

static uint64_t rle_decompress(const uint8_t* input, uint64_t input_size,
                                 uint8_t* output, uint64_t output_capacity) {
    if (!input || !output || input_size == 0 || input_size % 2 != 0) return 0;

    uint64_t output_pos = 0;

    for (uint64_t i = 0; i + 1 < input_size; i += 2) {
        uint8_t byte = input[i];
        uint8_t count = input[i + 1];

        if (output_pos + count > output_capacity) {
            return 0;  // Not enough space
        }

        for (uint8_t j = 0; j < count; j++) {
            output[output_pos++] = byte;
        }
    }

    return output_pos;
}

// ============================================================================
// ENCRYPTION OPERATIONS
// ============================================================================

// XOR cipher (simple but fast)
static void xor_encrypt(uint8_t* data, uint64_t size, const uint8_t* key, uint64_t key_size) {
    if (!data || !key || size == 0 || key_size == 0) return;

    for (uint64_t i = 0; i < size; i++) {
        data[i] ^= key[i % key_size];
    }
}

// Same function for decryption (XOR is symmetric)
static void xor_decrypt(uint8_t* data, uint64_t size, const uint8_t* key, uint64_t key_size) {
    xor_encrypt(data, size, key, key_size);
}

// ============================================================================
// MATHEMATICAL OPERATIONS
// ============================================================================

// Vector addition (element-wise)
static void vector_add(const uint64_t* a, const uint64_t* b, uint64_t* result, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        result[i] = a[i] + b[i];
    }
}

// Vector multiplication (element-wise)
static void vector_multiply(const uint64_t* a, const uint64_t* b, uint64_t* result, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        result[i] = a[i] * b[i];
    }
}

// Scalar multiplication
static void vector_scale(const uint64_t* input, uint64_t scalar, uint64_t* output, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        output[i] = input[i] * scalar;
    }
}

// ============================================================================
// EVENT PROCESSING
// ============================================================================

// Define new event types for Operations Deck
#define EVENT_OP_HASH_CRC32     100
#define EVENT_OP_HASH_DJB2      101
#define EVENT_OP_COMPRESS_RLE   110
#define EVENT_OP_DECOMPRESS_RLE 111
#define EVENT_OP_ENCRYPT_XOR    120
#define EVENT_OP_DECRYPT_XOR    121
#define EVENT_OP_VECTOR_ADD     130
#define EVENT_OP_VECTOR_MUL     131
#define EVENT_OP_VECTOR_SCALE   132

int operations_deck_process(RoutingEntry* entry) {
    Event* event = &entry->event_copy;

    switch (event->type) {
        // ====================================================================
        // HASHING OPERATIONS
        // ====================================================================

        case EVENT_OP_HASH_CRC32: {
            // Payload: [size:8][data:...]
            uint64_t size = *(uint64_t*)event->data;
            const uint8_t* data = event->data + 8;

            if (size > EVENT_DATA_SIZE - 8) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 1);
                return 0;
            }

            uint32_t hash = crc32_compute(data, size);

            uint32_t* result = (uint32_t*)kmalloc(sizeof(uint32_t));
            *result = hash;

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] CRC32 hash: 0x%x (size=%lu)\n", hash, size);
            return 1;
        }

        case EVENT_OP_HASH_DJB2: {
            // Payload: [size:8][data:...]
            uint64_t size = *(uint64_t*)event->data;
            const uint8_t* data = event->data + 8;

            if (size > EVENT_DATA_SIZE - 8) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 2);
                return 0;
            }

            uint64_t hash = djb2_hash(data, size);

            uint64_t* result = (uint64_t*)kmalloc(sizeof(uint64_t));
            *result = hash;

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] DJB2 hash: 0x%lx (size=%lu)\n", hash, size);
            return 1;
        }

        // ====================================================================
        // COMPRESSION OPERATIONS
        // ====================================================================

        case EVENT_OP_COMPRESS_RLE: {
            // Payload: [input_size:8][data:...]
            uint64_t input_size = *(uint64_t*)event->data;
            const uint8_t* input_data = event->data + 8;

            if (input_size > EVENT_DATA_SIZE - 8) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 3);
                return 0;
            }

            // Allocate output buffer (worst case: 2x input size)
            uint8_t* output = (uint8_t*)kmalloc(input_size * 2);
            uint64_t output_size = rle_compress(input_data, input_size, output, input_size * 2);

            if (output_size == 0) {
                kfree(output);
                deck_error(entry, DECK_PREFIX_OPERATIONS, 4);
                return 0;
            }

            deck_complete(entry, DECK_PREFIX_OPERATIONS, output, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] RLE compress: %lu -> %lu bytes (%.1f%% ratio)\n",
                    input_size, output_size, (float)output_size * 100 / input_size);
            return 1;
        }

        case EVENT_OP_DECOMPRESS_RLE: {
            // Payload: [compressed_size:8][output_capacity:8][data:...]
            uint64_t compressed_size = *(uint64_t*)event->data;
            uint64_t output_capacity = *(uint64_t*)(event->data + 8);
            const uint8_t* compressed_data = event->data + 16;

            if (compressed_size > EVENT_DATA_SIZE - 16) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 5);
                return 0;
            }

            uint8_t* output = (uint8_t*)kmalloc(output_capacity);
            uint64_t output_size = rle_decompress(compressed_data, compressed_size,
                                                   output, output_capacity);

            if (output_size == 0) {
                kfree(output);
                deck_error(entry, DECK_PREFIX_OPERATIONS, 6);
                return 0;
            }

            deck_complete(entry, DECK_PREFIX_OPERATIONS, output, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] RLE decompress: %lu -> %lu bytes\n",
                    compressed_size, output_size);
            return 1;
        }

        // ====================================================================
        // ENCRYPTION OPERATIONS
        // ====================================================================

        case EVENT_OP_ENCRYPT_XOR: {
            // Payload: [data_size:8][key_size:2][data:...][key:...]
            uint64_t data_size = *(uint64_t*)event->data;
            uint16_t key_size = *(uint16_t*)(event->data + 8);

            if (data_size + key_size + 10 > EVENT_DATA_SIZE) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 7);
                return 0;
            }

            uint8_t* data = event->data + 10;
            const uint8_t* key = data + data_size;

            // Allocate result buffer
            uint8_t* result = (uint8_t*)kmalloc(data_size);
            memcpy(result, data, data_size);

            xor_encrypt(result, data_size, key, key_size);

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] XOR encrypt: %lu bytes (key_size=%u)\n", data_size, key_size);
            return 1;
        }

        case EVENT_OP_DECRYPT_XOR: {
            // Same as encryption (XOR is symmetric)
            uint64_t data_size = *(uint64_t*)event->data;
            uint16_t key_size = *(uint16_t*)(event->data + 8);

            if (data_size + key_size + 10 > EVENT_DATA_SIZE) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 8);
                return 0;
            }

            uint8_t* data = event->data + 10;
            const uint8_t* key = data + data_size;

            uint8_t* result = (uint8_t*)kmalloc(data_size);
            memcpy(result, data, data_size);

            xor_decrypt(result, data_size, key, key_size);

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] XOR decrypt: %lu bytes (key_size=%u)\n", data_size, key_size);
            return 1;
        }

        // ====================================================================
        // MATHEMATICAL OPERATIONS
        // ====================================================================

        case EVENT_OP_VECTOR_ADD: {
            // Payload: [count:8][vector_a:...][vector_b:...]
            uint64_t count = *(uint64_t*)event->data;

            if (count * 2 * sizeof(uint64_t) + 8 > EVENT_DATA_SIZE) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 9);
                return 0;
            }

            const uint64_t* vector_a = (const uint64_t*)(event->data + 8);
            const uint64_t* vector_b = vector_a + count;

            uint64_t* result = (uint64_t*)kmalloc(count * sizeof(uint64_t));
            vector_add(vector_a, vector_b, result, count);

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] Vector add: %lu elements\n", count);
            return 1;
        }

        case EVENT_OP_VECTOR_MUL: {
            uint64_t count = *(uint64_t*)event->data;

            if (count * 2 * sizeof(uint64_t) + 8 > EVENT_DATA_SIZE) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 10);
                return 0;
            }

            const uint64_t* vector_a = (const uint64_t*)(event->data + 8);
            const uint64_t* vector_b = vector_a + count;

            uint64_t* result = (uint64_t*)kmalloc(count * sizeof(uint64_t));
            vector_multiply(vector_a, vector_b, result, count);

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] Vector multiply: %lu elements\n", count);
            return 1;
        }

        case EVENT_OP_VECTOR_SCALE: {
            // Payload: [count:8][scalar:8][vector:...]
            uint64_t count = *(uint64_t*)event->data;
            uint64_t scalar = *(uint64_t*)(event->data + 8);

            if (count * sizeof(uint64_t) + 16 > EVENT_DATA_SIZE) {
                deck_error(entry, DECK_PREFIX_OPERATIONS, 11);
                return 0;
            }

            const uint64_t* input = (const uint64_t*)(event->data + 16);

            uint64_t* result = (uint64_t*)kmalloc(count * sizeof(uint64_t));
            vector_scale(input, scalar, result, count);

            deck_complete(entry, DECK_PREFIX_OPERATIONS, result, RESULT_TYPE_KMALLOC);
            kprintf("[OPERATIONS] Vector scale: %lu elements * %lu\n", count, scalar);
            return 1;
        }

        default:
            kprintf("[OPERATIONS] Unknown event type %d\n", event->type);
            deck_error(entry, DECK_PREFIX_OPERATIONS, 99);
            return 0;
    }
}

// ============================================================================
// INITIALIZATION & RUN
// ============================================================================

DeckContext operations_deck_context;

void operations_deck_init(void) {
    // Initialize CRC32 table
    crc32_init_table();

    deck_init(&operations_deck_context, "Operations", DECK_PREFIX_OPERATIONS, operations_deck_process);

    kprintf("[OPERATIONS] Initialized with real algorithms:\n");
    kprintf("[OPERATIONS]   - Hashing: CRC32, DJB2\n");
    kprintf("[OPERATIONS]   - Compression: RLE\n");
    kprintf("[OPERATIONS]   - Encryption: XOR\n");
    kprintf("[OPERATIONS]   - Math: Vector operations\n");
}

int operations_deck_run_once(void) {
    return deck_run_once(&operations_deck_context);
}

void operations_deck_run(void) {
    deck_run(&operations_deck_context);
}
