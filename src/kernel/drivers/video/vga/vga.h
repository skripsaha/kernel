#ifndef VGA_H
#define VGA_H

#include "ktypes.h"

#define VGA 0xB8000 //если так не получится - сделать replace "VGA" на "vga"
extern unsigned char *vga;


/* Размеры экрана */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define BYTES_FOR_EACH_ELEMENT 2
#define VGA_SIZE (VGA_WIDTH * VGA_HEIGHT * BYTES_FOR_EACH_ELEMENT)


#define TEXT_ATTR_DEFAULT    0x07
#define TEXT_ATTR_CURSOR     0x09
#define TEXT_ATTR_ERROR      0x0C
#define TEXT_ATTR_WARNING    0x0E
#define TEXT_ATTR_HINT       0x0B
#define TEXT_ATTR_SUCCESS    0x0A

//эти перепемнные могут быть определены в других файлах, и при подулючении vga.h будет ошибка multiple definition и first defined here
// unsigned int current_loc;
// unsigned int last_loc;
// unsigned int current_line;
// unsigned int line_size;

void vga_init(void);

void vga_print(const char *str);
void vga_print_char(char ch, const unsigned char attr);
void vga_print_newline(void);
void vga_clear_screen(void);
void vga_clear_line(int line);
void vga_clear_to_eol();
void vga_print_error(const char *str);
void vga_print_success(const char *str);
void vga_print_hint(const char *str);
void vga_scroll_up(void);
void vga_change_background(unsigned char attr);

void vga_update_cursor(void);
void vga_set_cursor_position(int x, int y);
int vga_get_cursor_position_x();
int vga_get_cursor_position_y();
#endif // VGA_H