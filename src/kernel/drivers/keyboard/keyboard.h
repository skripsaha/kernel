#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ktypes.h"
#include "vga.h"

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64

// Структура для хранения состояния клавиатуры
typedef struct {
    uint8_t shift_pressed : 1;
    uint8_t ctrl_pressed : 1;
    uint8_t alt_pressed : 1;
    uint8_t caps_lock : 1;
    uint8_t num_lock : 1;
    uint8_t scroll_lock : 1;
    uint8_t last_keycode; // Для отслеживания предыдущего кода
} keyboard_state_t;

// Initialization
void keyboard_init(void);

// IRQ handler (called from interrupt context)
void keyboard_handle_scancode(uint8_t scancode);

// Input API
int keyboard_has_input(void);          // Returns 1 if input available
char keyboard_getchar(void);           // Non-blocking read (returns 0 if no input)
char keyboard_getchar_blocking(void);  // Blocking read (waits for input)
void keyboard_flush(void);             // Clear input buffer

#endif // KEYBOARD_H