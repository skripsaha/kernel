#include "pmm.h"
#include "e820.h"
#include "klib.h"

typedef struct {
    uintptr_t base;
    size_t pages;
    uint8_t* bitmap;
    spinlock_t lock;
    size_t last_free;
} pmm_zone_t;

static pmm_zone_t pmm_zone;
static bool pmm_initialized = false;

// Внутренние функции
static void pmm_reserve_region(uintptr_t base, uintptr_t end, const char* name);
static void pmm_set_bit(size_t bit, pmm_frame_state_t state);
static pmm_frame_state_t pmm_get_bit(size_t bit);
static size_t pmm_find_free_sequence(size_t count);


void pmm_init(void) {
    if (pmm_initialized) {
        kprintf("[PMM] Already initialized!\n");
        return;
    }

    kprintf("[PMM] Fetching e820 memory map...\n");
    e820_entry_t* entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();
    kprintf("[PMM] e820 entry count = %d\n", entry_count);

    if (entry_count == 0) {
        panic("[PMM] ERROR: e820 map is empty!");
    }

    // Print all entries for debug
    for (size_t i = 0; i < entry_count; i++) {
        if(entries[i].type != 0){
            kprintf("  #%d: base=0x%p len=0x%p type=%d\n", i, (void*)entries[i].base, (void*)entries[i].length, entries[i].type);
        }
    }
    kprintf("  Other entries are ZERO.\n");
    kprintf("  %[W]Do not use type 2 entries. Only type 1 - only RAM.%[D]\n\n");

    // Find the highest usable RAM address
    uintptr_t mem_end = 0;
    kprintf("[PMM] DEBUG: Scanning %zu entries for usable RAM...\n", entry_count);
    for (size_t i = 0; i < entry_count; i++) {
        kprintf("[PMM] Entry %zu: base=0x%llx len=0x%llx type=%u\n",
                i, entries[i].base, entries[i].length, entries[i].type);
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            kprintf("[PMM] --> USABLE!\n");
            uintptr_t region_end = entries[i].base + entries[i].length;
            if (region_end > mem_end) {
                mem_end = region_end;
                kprintf("[PMM] --> New mem_end = 0x%llx\n", (unsigned long long)mem_end);
            }
        }
    }
    kprintf("[PMM] mem_end = 0x%p\n", (void*)mem_end);

    if (mem_end <= 0x100000) {
        panic("[PMM] ERROR: Not enough usable RAM!");
    }

    // PMM will manage memory starting from 1MB (0x100000)
    pmm_zone.base = 0x100000;
    pmm_zone.pages = (mem_end - pmm_zone.base) / PMM_PAGE_SIZE;
    kprintf("[PMM] Managing pages from 0x%p, total pages = %d\n", (void*)pmm_zone.base, (int)pmm_zone.pages);

    if (pmm_zone.pages == 0) {
        panic("[PMM] ERROR: No usable pages!");
    }

    // Bitmap size (1 bit per page)
    size_t bitmap_size = (pmm_zone.pages + 7) / 8;
    kprintf("[PMM] Bitmap size = %d bytes\n", (int)bitmap_size);

    // Place bitmap at the end of RAM
    // pmm_zone.bitmap = (uint8_t*)(mem_end - bitmap_size);
    // kprintf("[PMM] Bitmap placed at %p\n", pmm_zone.bitmap);
    pmm_zone.bitmap = (uint8_t*)ALIGN_UP((uintptr_t)&_kernel_end, 4096);
    kprintf("[PMM] Bitmap placed at %p (after kernel)\n", pmm_zone.bitmap);

    // Ensure the bitmap is within managed range
    if ((uintptr_t)pmm_zone.bitmap < pmm_zone.base) {
        panic("[PMM] ERROR: Bitmap is outside managed memory!");
    }

    // Mark all pages as used (safe default)
    memset(pmm_zone.bitmap, 0xFF, bitmap_size);

    // Free all usable regions from e820 (except below 1MB)
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].type == E820_USABLE && entries[i].length > 0) {
            uintptr_t start = entries[i].base;
            uintptr_t end = entries[i].base + entries[i].length;
            if (end <= pmm_zone.base) continue;
            if (start < pmm_zone.base) start = pmm_zone.base;
            if (start >= end) continue;

            size_t start_page = (start - pmm_zone.base) / PMM_PAGE_SIZE;
            size_t end_page = (end - pmm_zone.base) / PMM_PAGE_SIZE;
            if (end_page > pmm_zone.pages) end_page = pmm_zone.pages;

            kprintf("[PMM] Freeing pages: %d .. %d\n", (int)start_page, (int)end_page - 1);

            for (size_t j = start_page; j < end_page; j++) {
                pmm_set_bit(j, PMM_FRAME_FREE);
            }
        }
    }

    // Optionally, reserve kernel and bitmap here, if you have _kernel_start/_kernel_end
    extern uintptr_t _kernel_start;
    extern uintptr_t _kernel_end;
    pmm_reserve_region((uintptr_t)&_kernel_start, (uintptr_t)&_kernel_end, "Kernel");
    pmm_reserve_region((uintptr_t)pmm_zone.bitmap, (uintptr_t)pmm_zone.bitmap + bitmap_size, "Bitmap");

    spinlock_init(&pmm_zone.lock);
    pmm_initialized = true;

    kprintf("[PMM] Initialized: %d MB available, %d pages.\n",
        (int)((pmm_zone.pages * PMM_PAGE_SIZE) / (1024 * 1024)),
        (int)pmm_zone.pages
    );
}


// void pmm_init(void) {
//     if (pmm_initialized) return;

//     // Получаем память из E820
//     e820_entry_t* entries = memory_map_get_entries();
//     size_t entry_count = memory_map_get_entry_count();

//     // Находим максимальный доступный регион
//     uintptr_t mem_end = 0;
//     for (size_t i = 0; i < entry_count; i++) {
//         if (entries[i].type == E820_USABLE && 
//             entries[i].base + entries[i].length > mem_end) {
//             mem_end = entries[i].base + entries[i].length;
//         }
//     }

//     // Инициализируем зону памяти (начинаем с 1MB)
//     pmm_zone.base = 0x100000;
//     pmm_zone.pages = (mem_end - pmm_zone.base) / PMM_PAGE_SIZE;
    
//     // Размещаем битовую карту в конце памяти
//     size_t bitmap_size = (pmm_zone.pages + PMM_BITMAP_ALIGN - 1) / PMM_BITMAP_ALIGN;
//     pmm_zone.bitmap = (uint8_t*)(mem_end - bitmap_size);
    
//     // Помечаем всю память как занятую
//     memset(pmm_zone.bitmap, 0xFF, bitmap_size);
    
//     // Освобождаем доступные регионы
//     for (size_t i = 0; i < entry_count; i++) {
//         if (entries[i].type == E820_USABLE) {
//             uintptr_t start = MAX(entries[i].base, pmm_zone.base);
//             uintptr_t end = MIN(entries[i].base + entries[i].length, mem_end);
            
//             if (start < end) {
//                 size_t start_page = (start - pmm_zone.base) / PMM_PAGE_SIZE;
//                 size_t end_page = (end - pmm_zone.base) / PMM_PAGE_SIZE;
                
//                 for (size_t j = start_page; j < end_page; j++) {
//                     pmm_set_bit(j, PMM_FRAME_FREE);
//                 }
//             }
//         }
//     }
    
//     // Резервируем системные области
//     extern uintptr_t _kernel_start;
//     extern uintptr_t _kernel_end;
//     pmm_reserve_region((uintptr_t)&_kernel_start, (uintptr_t)&_kernel_end, "Kernel");
//     pmm_reserve_region((uintptr_t)pmm_zone.bitmap, (uintptr_t)pmm_zone.bitmap + bitmap_size, "Bitmap");
    
//     spinlock_init(&pmm_zone.lock);
//     pmm_initialized = true;
    
//     kprintf("PMM: Initialized with %d MB available\n", 
//            (pmm_zone.pages * PMM_PAGE_SIZE) / (1024 * 1024));
// }

void* pmm_alloc(size_t pages) {
    if (!pages || !pmm_initialized) return NULL;
    
    spin_lock(&pmm_zone.lock);
    
    size_t start = pmm_find_free_sequence(pages);
    if (start == (size_t)-1) {
        spin_unlock(&pmm_zone.lock);
        return NULL;
    }
    
    for (size_t i = 0; i < pages; i++) {
        pmm_set_bit(start + i, PMM_FRAME_USED);
    }
    
    void* addr = (void*)(pmm_zone.base + start * PMM_PAGE_SIZE);
    spin_unlock(&pmm_zone.lock);
    return addr;
}

void* pmm_alloc_zero(size_t pages) {
    void* addr = pmm_alloc(pages);
    if (addr) memset(addr, 0, pages * PMM_PAGE_SIZE);
    return addr;
}

void pmm_free(void* addr, size_t pages) {
    if (!addr || !pages || !pmm_initialized) return;
    
    uintptr_t base = (uintptr_t)addr;
    if (base < pmm_zone.base || base >= pmm_zone.base + pmm_zone.pages * PMM_PAGE_SIZE) {
        panic("PMM: Invalid free address %p", addr);
    }
    
    size_t first = (base - pmm_zone.base) / PMM_PAGE_SIZE;
    
    spin_lock(&pmm_zone.lock);
    
    // Проверка на двойное освобождение
    for (size_t i = first; i < first + pages; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            panic("PMM: Double free detected at page %d", i);
        }
    }
    
    // Освобождение
    for (size_t i = first; i < first + pages; i++) {
        pmm_set_bit(i, PMM_FRAME_FREE);
    }
    
    // Обновляем last_free для оптимизации
    if (first < pmm_zone.last_free) {
        pmm_zone.last_free = first;
    }
    
    spin_unlock(&pmm_zone.lock);
}

// Внутренние функции
static void pmm_reserve_region(uintptr_t base, uintptr_t end, const char* name) {
    base = ALIGN_DOWN(base, PMM_PAGE_SIZE);
    end = ALIGN_UP(end, PMM_PAGE_SIZE);

    if (base >= end) return;

    size_t start_page = (base - pmm_zone.base) / PMM_PAGE_SIZE;
    size_t end_page = (end - pmm_zone.base) / PMM_PAGE_SIZE;

    // Важно: ограничиваем диапазон резервирования только управляемой PMM зоной
    if (start_page >= pmm_zone.pages) return; // Регион начинается за пределами зоны PMM
    if (end_page > pmm_zone.pages) end_page = pmm_zone.pages; // Регион выходит за пределы зоны PMM

    for (size_t i = start_page; i < end_page; i++) {
        pmm_set_bit(i, PMM_FRAME_RESERVED); // Пометить как занятое (1)
    }

    kprintf("PMM: Reserved %s at %p-%p\n", name, (void*)base, (void*)end);
}

static void pmm_set_bit(size_t bit, pmm_frame_state_t state) {
    size_t byte = bit / PMM_BITMAP_ALIGN;
    size_t offset = bit % PMM_BITMAP_ALIGN; // offset = bit % 8

    switch(state) {
        case PMM_FRAME_FREE:
            pmm_zone.bitmap[byte] &= ~(1 << offset); // Сбросить бит в 0
            break;
        default: // PMM_FRAME_USED, PMM_FRAME_RESERVED, PMM_FRAME_KERNEL, PMM_FRAME_BAD
            pmm_zone.bitmap[byte] |= (1 << offset); // Установить бит в 1
            break;
    }
}

static pmm_frame_state_t pmm_get_bit(size_t bit) {
    size_t byte = bit / PMM_BITMAP_ALIGN;
    size_t offset = bit % PMM_BITMAP_ALIGN; // offset = bit % 8
    // Возвращает PMM_FRAME_USED (1) или PMM_FRAME_FREE (0)
    // Другие состояния не различаются
    return (pmm_zone.bitmap[byte] & (1 << offset)) ? PMM_FRAME_USED : PMM_FRAME_FREE;
}

static size_t pmm_find_free_sequence(size_t count) {
    size_t consecutive = 0;
    size_t start_search_from = pmm_zone.last_free; // Изменил имя переменной для ясности

    // Ищем от last_free до конца
    for (size_t i = start_search_from; i < pmm_zone.pages; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            consecutive++;
            if (consecutive == count) {
                pmm_zone.last_free = i - count + 1; // Обновляем last_free на место найденного блока
                return i - count + 1;
            }
        } else {
            consecutive = 0;
        }
    }

    // Если не нашли, сбрасываем consecutive и ищем с начала до last_free
    consecutive = 0; // Сброс для нового прохода
    for (size_t i = 0; i < start_search_from; i++) { // Ищем только до last_free
        if (pmm_get_bit(i) == PMM_FRAME_FREE) {
            consecutive++;
            if (consecutive == count) {
                pmm_zone.last_free = i - count + 1; // Обновляем last_free на место найденного блока
                return i - count + 1;
            }
        } else {
            consecutive = 0;
        }
    }

    return (size_t)-1;
}

// Утилиты
size_t pmm_total_pages(void) {
    return pmm_zone.pages;
}

size_t pmm_free_pages(void) {
    size_t count = 0;
    spin_lock(&pmm_zone.lock);
    for (size_t i = 0; i < pmm_zone.pages; i++) {
        if (pmm_get_bit(i) == PMM_FRAME_FREE) count++;
    }
    spin_unlock(&pmm_zone.lock);
    return count;
}

size_t pmm_used_pages(void) {
    return pmm_total_pages() - pmm_free_pages();
}

void pmm_dump_stats(void) {
    kprintf("Physical Memory Manager Statistics:\n");
    kprintf("  Total pages: %d (%d MB)\n", 
           pmm_total_pages(), 
           (pmm_total_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Used pages:  %d (%d MB)\n",
           pmm_used_pages(),
           (pmm_used_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("  Free pages:  %d (%d MB)\n",
           pmm_free_pages(),
           (pmm_free_pages() * PMM_PAGE_SIZE) / (1024 * 1024));
}

// Отладочные функции
void pmm_print_memory_map(void) {
    e820_entry_t* entries = memory_map_get_entries();
    size_t entry_count = memory_map_get_entry_count();
    
    kprintf("Memory Map:\n");
    for (size_t i = 0; i < entry_count; i++) {
        kprintf("  %p-%p: %s\n", 
               (void*)entries[i].base,
               (void*)(entries[i].base + entries[i].length),
               entries[i].type == E820_USABLE ? "Usable" : "Reserved");
    }
}

bool pmm_check_integrity(void) {
    // Проверяем, что битовая карта не повреждена
    for (size_t i = 0; i < pmm_zone.pages; i++) {
        uintptr_t addr = pmm_zone.base + i * PMM_PAGE_SIZE;
        
        // Проверяем, что страницы ядра помечены как занятые
        if (addr < (uintptr_t)&_kernel_end && 
            pmm_get_bit(i) != PMM_FRAME_USED) {
            return false;
        }
    }
    return true;
}