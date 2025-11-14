#ifndef PMM_H
#define PMM_H

#include "klib.h"

#define PMM_PAGE_SIZE       4096
#define PMM_BITMAP_ALIGN    8
#define PMM_MAX_MEMORY      (128ULL * 1024 * 1024 * 1024) // 128GB

typedef enum {
    PMM_FRAME_FREE = 0,
    PMM_FRAME_USED,
    PMM_FRAME_RESERVED,
    PMM_FRAME_KERNEL,
    PMM_FRAME_BAD
} pmm_frame_state_t;

// Инициализация PMM
void pmm_init(void);

// Основные функции
void* pmm_alloc(size_t pages);
void* pmm_alloc_zero(size_t pages);
void pmm_free(void* addr, size_t pages);

// Утилиты
size_t pmm_total_pages(void);
size_t pmm_free_pages(void);
size_t pmm_used_pages(void);
void pmm_dump_stats(void);

// Отладочные функции
void pmm_print_memory_map(void);
bool pmm_check_integrity(void);

#endif // PMM_H