#ifndef SERIAL_H
#define SERIAL_H

#include "ktypes.h"

// COM1 serial port (standard)
#define SERIAL_COM1 0x3F8

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char* str);

#endif // SERIAL_H
