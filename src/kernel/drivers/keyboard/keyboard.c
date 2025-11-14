#include "keyboard.h"
#include "klib.h"

// ============================================================================
// KEYBOARD RING BUFFER
// ============================================================================

#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;  // Write position
static volatile uint32_t kb_tail = 0;  // Read position
static spinlock_t kb_lock = {0};       // Spinlock

static keyboard_state_t kb_state = {0};

// ============================================================================
// SCANCODE TO ASCII TABLE (US layout)
// ============================================================================

static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  // Ctrl
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,  // Left Shift
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,  // Right Shift
    '*',
    0,  // Alt
    ' ',  // Space
    0,  // Caps Lock
};

static const char scancode_to_ascii_shifted[] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  // Ctrl
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,  // Left Shift
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,  // Right Shift
    '*',
    0,  // Alt
    ' ',  // Space
    0,  // Caps Lock
};

// ============================================================================
// KEYBOARD INITIALIZATION
// ============================================================================

void keyboard_init(void) {
    kb_head = 0;
    kb_tail = 0;
    kb_state.shift_pressed = 0;
    kb_state.ctrl_pressed = 0;
    kb_state.alt_pressed = 0;
    kb_state.caps_lock = 0;
    kb_state.num_lock = 0;
    kb_state.scroll_lock = 0;
    kb_state.last_keycode = 0;
}

// ============================================================================
// SCANCODE PROCESSING (called from IRQ handler)
// ============================================================================

void keyboard_handle_scancode(uint8_t scancode) {
    // Handle key release (bit 7 set)
    uint8_t is_release = scancode & 0x80;
    uint8_t key = scancode & 0x7F;

    // Handle modifier keys
    if (key == 0x2A || key == 0x36) {  // Left/Right Shift
        kb_state.shift_pressed = !is_release;
        return;
    }
    if (key == 0x1D) {  // Ctrl
        kb_state.ctrl_pressed = !is_release;
        return;
    }
    if (key == 0x38) {  // Alt
        kb_state.alt_pressed = !is_release;
        return;
    }
    if (key == 0x3A && !is_release) {  // Caps Lock (toggle on press)
        kb_state.caps_lock = !kb_state.caps_lock;
        return;
    }

    // Ignore key releases for regular keys
    if (is_release) return;

    // Convert scancode to ASCII
    char ascii = 0;
    if (key < sizeof(scancode_to_ascii)) {
        if (kb_state.shift_pressed) {
            ascii = scancode_to_ascii_shifted[key];
        } else {
            ascii = scancode_to_ascii[key];
            // Apply caps lock to letters
            if (kb_state.caps_lock && ascii >= 'a' && ascii <= 'z') {
                ascii -= 32;  // To uppercase
            }
        }
    }

    // Add to ring buffer if valid ASCII
    if (ascii != 0) {
        spin_lock(&kb_lock);
        uint32_t next_head = (kb_head + 1) % KEYBOARD_BUFFER_SIZE;
        if (next_head != kb_tail) {  // Buffer not full
            keyboard_buffer[kb_head] = ascii;
            kb_head = next_head;
        }
        spin_unlock(&kb_lock);
    }
}

// ============================================================================
// KEYBOARD INPUT API
// ============================================================================

int keyboard_has_input(void) {
    return kb_head != kb_tail;
}

char keyboard_getchar(void) {
    if (kb_head == kb_tail) {
        return 0;  // No input
    }

    spin_lock(&kb_lock);
    char c = keyboard_buffer[kb_tail];
    kb_tail = (kb_tail + 1) % KEYBOARD_BUFFER_SIZE;
    spin_unlock(&kb_lock);

    return c;
}

char keyboard_getchar_blocking(void) {
    while (!keyboard_has_input()) {
        asm("hlt");  // Wait for interrupt
    }
    return keyboard_getchar();
}

void keyboard_flush(void) {
    spin_lock(&kb_lock);
    kb_head = kb_tail = 0;
    spin_unlock(&kb_lock);
}
