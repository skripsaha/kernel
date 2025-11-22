#ifndef ERRORS_H
#define ERRORS_H

#include "ktypes.h"

// ============================================================================
// STANDARDIZED ERROR CODES - Centralized error code system
// ============================================================================

// Error code format: 0xDDCC
// DD = Deck prefix (01-04)
// CC = Error code (00-FF)

#define ERROR_NONE                  0x0000

// === GENERIC ERRORS (00xx) ===
#define ERROR_UNKNOWN               0x0001
#define ERROR_INVALID_PARAMETER     0x0002
#define ERROR_OUT_OF_MEMORY         0x0003
#define ERROR_TIMEOUT               0x0004
#define ERROR_NOT_IMPLEMENTED       0x0005
#define ERROR_RESOURCE_BUSY         0x0006
#define ERROR_PERMISSION_DENIED     0x0007

// === OPERATIONS DECK ERRORS (01xx) ===
#define ERROR_OP_INVALID_OPERATION  0x0101
#define ERROR_OP_BUFFER_TOO_SMALL   0x0102
#define ERROR_OP_INVALID_INPUT      0x0103
#define ERROR_OP_COMPRESSION_FAILED 0x0104
#define ERROR_OP_DECOMPRESSION_FAILED 0x0105

// === STORAGE DECK ERRORS (02xx) ===
#define ERROR_STORAGE_FILE_NOT_FOUND    0x0201
#define ERROR_STORAGE_PERMISSION_DENIED 0x0202
#define ERROR_STORAGE_DISK_FULL         0x0203
#define ERROR_STORAGE_INVALID_FD        0x0204
#define ERROR_STORAGE_READ_FAILED       0x0205
#define ERROR_STORAGE_WRITE_FAILED      0x0206
#define ERROR_STORAGE_SEEK_FAILED       0x0207
#define ERROR_STORAGE_TAG_NOT_FOUND     0x0208
#define ERROR_STORAGE_INODE_NOT_FOUND   0x0209

// === HARDWARE DECK ERRORS (03xx) ===
#define ERROR_HW_TIMER_SLOTS_FULL   0x0301
#define ERROR_HW_TIMER_NOT_FOUND    0x0302
#define ERROR_HW_DEVICE_NOT_FOUND   0x0303
#define ERROR_HW_DEVICE_BUSY        0x0304
#define ERROR_HW_IOCTL_FAILED       0x0305

// === NETWORK DECK ERRORS (04xx) ===
#define ERROR_NET_NOT_CONNECTED     0x0401
#define ERROR_NET_CONNECTION_REFUSED 0x0402
#define ERROR_NET_TIMEOUT           0x0403
#define ERROR_NET_HOST_UNREACHABLE  0x0404

// === WORKFLOW ERRORS (05xx) ===
#define ERROR_WORKFLOW_NOT_FOUND    0x0501
#define ERROR_WORKFLOW_ALREADY_RUNNING 0x0502
#define ERROR_WORKFLOW_DEPENDENCY_FAILED 0x0503
#define ERROR_WORKFLOW_SUBMIT_FAILED 0x0504
#define ERROR_WORKFLOW_ABORTED      0x0505

// ============================================================================
// ERROR SEVERITY LEVELS
// ============================================================================

typedef enum {
    ERROR_SEVERITY_INFO = 0,      // Informational, not really an error
    ERROR_SEVERITY_WARNING = 1,   // Warning, operation continued
    ERROR_SEVERITY_ERROR = 2,     // Error, operation failed but recoverable
    ERROR_SEVERITY_FATAL = 3      // Fatal error, system unstable
} ErrorSeverity;

// ============================================================================
// ERROR CONTEXT - Detailed error information
// ============================================================================

typedef struct {
    uint32_t error_code;          // Standardized error code
    ErrorSeverity severity;       // Error severity level
    uint8_t deck_prefix;          // Deck that generated the error
    uint64_t event_id;            // Event that failed
    uint64_t workflow_id;         // Workflow that failed
    uint64_t timestamp;           // When error occurred (TSC)
    char message[128];            // Human-readable error message
} ErrorContext;

// ============================================================================
// ERROR POLICY - How to handle errors in workflows
// ============================================================================

typedef enum {
    ERROR_POLICY_ABORT = 0,       // Abort workflow on first error (default)
    ERROR_POLICY_CONTINUE = 1,    // Continue with other events, mark failed event
    ERROR_POLICY_RETRY = 2,       // Retry failed event (with exponential backoff)
    ERROR_POLICY_SKIP = 3         // Skip failed event and its dependents
} ErrorPolicy;

// ============================================================================
// RETRY CONFIGURATION
// ============================================================================

typedef struct {
    uint8_t enabled;              // Is retry enabled?
    uint8_t max_retries;          // Maximum retry attempts (default: 3)
    uint32_t base_delay_ms;       // Base delay between retries (default: 100ms)
    uint8_t exponential_backoff;  // Use exponential backoff? (default: yes)
} RetryConfig;

// ============================================================================
// ERROR HELPERS
// ============================================================================

// Get human-readable error string
const char* error_to_string(uint32_t error_code);

// Get deck prefix from error code
static inline uint8_t error_get_deck(uint32_t error_code) {
    return (error_code >> 8) & 0xFF;
}

// Get error number from error code
static inline uint8_t error_get_number(uint32_t error_code) {
    return error_code & 0xFF;
}

// Check if error is transient (can be retried)
bool error_is_transient(uint32_t error_code);

// Initialize error context
void error_context_init(ErrorContext* ctx, uint32_t error_code,
                       uint8_t deck_prefix, uint64_t event_id,
                       uint64_t workflow_id, const char* message);

// Log error with full context
void error_log(const ErrorContext* ctx);

#endif // ERRORS_H
