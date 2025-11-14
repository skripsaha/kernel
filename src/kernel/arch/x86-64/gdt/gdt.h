#ifndef GDT_H
#define GDT_H

#include "ktypes.h"

// GDT селекторы
#define GDT_KERNEL_CODE   0x08
#define GDT_KERNEL_DATA   0x10  
#define GDT_USER_CODE     0x18
#define GDT_USER_DATA     0x20
#define GDT_TSS           0x28

// Структура GDT записи
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

// // Структура для TSS (пока заглушка)
// typedef struct {
//     uint32_t reserved1;
//     uint64_t rsp0;      // Stack pointer for ring 0
//     uint64_t rsp1;      // Stack pointer for ring 1  
//     uint64_t rsp2;      // Stack pointer for ring 2
//     uint64_t reserved2;
//     uint64_t ist1;      // Interrupt Stack Table 1
//     uint64_t ist2;
//     uint64_t ist3;
//     uint64_t ist4;
//     uint64_t ist5;
//     uint64_t ist6;
//     uint64_t ist7;
//     uint64_t reserved3;
//     uint16_t reserved4;
//     uint16_t iomap_base;
// } __attribute__((packed)) tss_t;

// Дескриптор для загрузки GDT
typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdt_descriptor_t;

// Функции
void gdt_init(void);
void gdt_set_entry(int index, uint64_t base, uint64_t limit, uint8_t access, uint8_t flags);
void gdt_set_tss_entry(int index, uint64_t base, uint64_t limit);
void gdt_load(void);
void gdt_test(void);

#endif // GDT_H