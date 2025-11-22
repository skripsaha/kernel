#include "errors.h"
#include "atomics.h"
#include "klib.h"

// ============================================================================
// ERROR STRING MAPPING
// ============================================================================

const char* error_to_string(uint32_t error_code) {
    switch (error_code) {
        case ERROR_NONE: return "No error";

        // Generic errors
        case ERROR_UNKNOWN: return "Unknown error";
        case ERROR_INVALID_PARAMETER: return "Invalid parameter";
        case ERROR_OUT_OF_MEMORY: return "Out of memory";
        case ERROR_TIMEOUT: return "Operation timed out";
        case ERROR_NOT_IMPLEMENTED: return "Not implemented";
        case ERROR_RESOURCE_BUSY: return "Resource busy";
        case ERROR_PERMISSION_DENIED: return "Permission denied";

        // Operations Deck errors
        case ERROR_OP_INVALID_OPERATION: return "Invalid operation";
        case ERROR_OP_BUFFER_TOO_SMALL: return "Buffer too small";
        case ERROR_OP_INVALID_INPUT: return "Invalid input data";
        case ERROR_OP_COMPRESSION_FAILED: return "Compression failed";
        case ERROR_OP_DECOMPRESSION_FAILED: return "Decompression failed";

        // Storage Deck errors
        case ERROR_STORAGE_FILE_NOT_FOUND: return "File not found";
        case ERROR_STORAGE_PERMISSION_DENIED: return "Storage permission denied";
        case ERROR_STORAGE_DISK_FULL: return "Disk full";
        case ERROR_STORAGE_INVALID_FD: return "Invalid file descriptor";
        case ERROR_STORAGE_READ_FAILED: return "Read operation failed";
        case ERROR_STORAGE_WRITE_FAILED: return "Write operation failed";
        case ERROR_STORAGE_SEEK_FAILED: return "Seek operation failed";
        case ERROR_STORAGE_TAG_NOT_FOUND: return "Tag not found";
        case ERROR_STORAGE_INODE_NOT_FOUND: return "Inode not found";

        // Hardware Deck errors
        case ERROR_HW_TIMER_SLOTS_FULL: return "No free timer slots";
        case ERROR_HW_TIMER_NOT_FOUND: return "Timer not found";
        case ERROR_HW_DEVICE_NOT_FOUND: return "Device not found";
        case ERROR_HW_DEVICE_BUSY: return "Device busy";
        case ERROR_HW_IOCTL_FAILED: return "IOCTL operation failed";

        // Network Deck errors
        case ERROR_NET_NOT_CONNECTED: return "Not connected";
        case ERROR_NET_CONNECTION_REFUSED: return "Connection refused";
        case ERROR_NET_TIMEOUT: return "Network timeout";
        case ERROR_NET_HOST_UNREACHABLE: return "Host unreachable";

        // Workflow errors
        case ERROR_WORKFLOW_NOT_FOUND: return "Workflow not found";
        case ERROR_WORKFLOW_ALREADY_RUNNING: return "Workflow already running";
        case ERROR_WORKFLOW_DEPENDENCY_FAILED: return "Dependency failed";
        case ERROR_WORKFLOW_SUBMIT_FAILED: return "Failed to submit event";
        case ERROR_WORKFLOW_ABORTED: return "Workflow aborted";

        default:
            return "Unknown error code";
    }
}

// ============================================================================
// TRANSIENT ERROR DETECTION
// ============================================================================

bool error_is_transient(uint32_t error_code) {
    // Transient errors are those that might succeed on retry
    switch (error_code) {
        case ERROR_TIMEOUT:
        case ERROR_RESOURCE_BUSY:
        case ERROR_STORAGE_DISK_FULL:     // Might become available
        case ERROR_HW_DEVICE_BUSY:
        case ERROR_NET_TIMEOUT:
        case ERROR_NET_HOST_UNREACHABLE:
            return true;

        default:
            return false;  // Most errors are permanent
    }
}

// ============================================================================
// ERROR CONTEXT MANAGEMENT
// ============================================================================

void error_context_init(ErrorContext* ctx, uint32_t error_code,
                       uint8_t deck_prefix, uint64_t event_id,
                       uint64_t workflow_id, const char* message) {
    if (!ctx) return;

    ctx->error_code = error_code;
    ctx->deck_prefix = deck_prefix;
    ctx->event_id = event_id;
    ctx->workflow_id = workflow_id;
    ctx->timestamp = rdtsc();

    // Determine severity based on error code
    if (error_code == ERROR_NONE) {
        ctx->severity = ERROR_SEVERITY_INFO;
    } else if (error_code >= ERROR_WORKFLOW_NOT_FOUND) {
        ctx->severity = ERROR_SEVERITY_FATAL;
    } else if (error_is_transient(error_code)) {
        ctx->severity = ERROR_SEVERITY_WARNING;
    } else {
        ctx->severity = ERROR_SEVERITY_ERROR;
    }

    // Copy message (with bounds checking)
    if (message) {
        size_t len = strlen(message);
        if (len >= sizeof(ctx->message)) {
            len = sizeof(ctx->message) - 1;
        }
        memcpy(ctx->message, message, len);
        ctx->message[len] = '\0';
    } else {
        // Use default message from error_to_string
        const char* default_msg = error_to_string(error_code);
        size_t len = strlen(default_msg);
        if (len >= sizeof(ctx->message)) {
            len = sizeof(ctx->message) - 1;
        }
        memcpy(ctx->message, default_msg, len);
        ctx->message[len] = '\0';
    }
}

// ============================================================================
// ERROR LOGGING
// ============================================================================

void error_log(const ErrorContext* ctx) {
    if (!ctx) return;

    const char* severity_str;
    uint8_t color_code;

    switch (ctx->severity) {
        case ERROR_SEVERITY_INFO:
            severity_str = "INFO";
            color_code = 'H';  // Hint color
            break;
        case ERROR_SEVERITY_WARNING:
            severity_str = "WARNING";
            color_code = 'W';  // Warning color
            break;
        case ERROR_SEVERITY_ERROR:
            severity_str = "ERROR";
            color_code = 'E';  // Error color
            break;
        case ERROR_SEVERITY_FATAL:
            severity_str = "FATAL";
            color_code = 'E';  // Error color
            break;
        default:
            severity_str = "UNKNOWN";
            color_code = 'D';  // Default color
            break;
    }

    kprintf("%%[%c]", color_code);
    kprintf("[%s] Error 0x%04x in Deck %u\n", severity_str, ctx->error_code, ctx->deck_prefix);
    kprintf("%%[D]");  // Reset to default color

    kprintf("  Event ID: %lu, Workflow ID: %lu\n", ctx->event_id, ctx->workflow_id);
    kprintf("  Message: %s\n", ctx->message);
    kprintf("  Time: %lu TSC cycles\n", ctx->timestamp);

    if (error_is_transient(ctx->error_code)) {
        kprintf("  (This error might be transient - retry recommended)\n");
    }
}
