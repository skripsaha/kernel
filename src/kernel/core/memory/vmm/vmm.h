#ifndef VMM_H
#define VMM_H

#include "klib.h"

// ========== VMM CONSTANTS ==========
#define VMM_PAGE_SIZE           4096
#define VMM_PAGE_MASK           0xFFFFFFFFFFFFF000ULL
#define VMM_PAGE_OFFSET_MASK    0x0000000000000FFFULL

// Virtual address space layout
#define VMM_KERNEL_BASE         0xFFFF800000000000ULL  // -128TB
#define VMM_KERNEL_HEAP_BASE    0xFFFF800000000000ULL  // Kernel heap start
#define VMM_KERNEL_HEAP_SIZE    (1ULL << 30)           // 1GB kernel heap
#define VMM_USER_BASE           0x0000000000400000ULL  // 4MB (standard ELF base)
#define VMM_USER_STACK_TOP      0x00007FFFFFFFE000ULL  // ~128TB user space top
#define VMM_USER_HEAP_BASE      0x0000000001000000ULL  // 16MB user heap start

// Memory protection flags
#define VMM_FLAG_PRESENT        (1ULL << 0)   // Page is present
#define VMM_FLAG_WRITABLE       (1ULL << 1)   // Page is writable
#define VMM_FLAG_USER           (1ULL << 2)   // User accessible
#define VMM_FLAG_WRITE_THROUGH  (1ULL << 3)   // Write-through caching
#define VMM_FLAG_CACHE_DISABLE  (1ULL << 4)   // Cache disabled
#define VMM_FLAG_ACCESSED       (1ULL << 5)   // Page was accessed
#define VMM_FLAG_DIRTY          (1ULL << 6)   // Page was written to
#define VMM_FLAG_LARGE_PAGE     (1ULL << 7)   // 2MB/1GB page
#define VMM_FLAG_GLOBAL         (1ULL << 8)   // Global page
#define VMM_FLAG_NO_EXECUTE     (1ULL << 63)  // No execute (NX bit)

// Convenience flag combinations
#define VMM_FLAGS_KERNEL_RW     (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_FLAGS_KERNEL_RO     (VMM_FLAG_PRESENT)
#define VMM_FLAGS_KERNEL_CODE   (VMM_FLAG_PRESENT | VMM_FLAG_GLOBAL)
#define VMM_FLAGS_USER_RW       (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define VMM_FLAGS_USER_RO       (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define VMM_FLAGS_USER_CODE     (VMM_FLAG_PRESENT | VMM_FLAG_USER)

// Page table entry masks
#define VMM_PTE_ADDR_MASK       0x000FFFFFFFFFF000ULL
#define VMM_PTE_FLAGS_MASK      0x8000000000000FFFULL

// Virtual address indices
#define VMM_PML4_INDEX(addr)    (((addr) >> 39) & 0x1FF)
#define VMM_PDPT_INDEX(addr)    (((addr) >> 30) & 0x1FF)
#define VMM_PD_INDEX(addr)      (((addr) >> 21) & 0x1FF)
#define VMM_PT_INDEX(addr)      (((addr) >> 12) & 0x1FF)

// ========== STRUCTURES ==========

// Page table entry (64-bit)
typedef uint64_t pte_t;

// Page table (512 entries)
typedef struct {
    pte_t entries[512];
} __attribute__((aligned(4096))) page_table_t;

// Virtual memory context (address space)
typedef struct {
    page_table_t* pml4;           // Top-level page table
    uintptr_t pml4_phys;          // Physical address of PML4
    spinlock_t lock;              // Protection lock
    
    // Memory statistics
    size_t mapped_pages;          // Number of mapped pages
    size_t kernel_pages;          // Kernel pages in this context
    size_t user_pages;            // User pages in this context
    
    // Memory regions tracking (simple version)
    uintptr_t heap_start;         // Current heap start
    uintptr_t heap_end;           // Current heap end
    uintptr_t stack_top;          // Stack top for user processes
} vmm_context_t;

// Memory mapping result
typedef struct {
    bool success;
    uintptr_t virt_addr;
    uintptr_t phys_addr;
    size_t pages_mapped;
    const char* error_msg;
} vmm_map_result_t;

// ========== CORE FUNCTIONS ==========

// Initialization
void vmm_init(void);
void vmm_test_basic(void);

// Context management
vmm_context_t* vmm_create_context(void);
void vmm_destroy_context(vmm_context_t* ctx);
vmm_context_t* vmm_get_kernel_context(void);
vmm_context_t* vmm_get_current_context(void);
void vmm_switch_context(vmm_context_t* ctx);

// Memory mapping
vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr, 
                              uintptr_t phys_addr, uint64_t flags);
vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr, 
                               uintptr_t phys_addr, size_t page_count, uint64_t flags);
bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count);

// Memory allocation (high-level)
void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags);
void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count);

// Address translation
uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr);
bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr);
uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr);

// Kernel heap (vmalloc equivalent)
void* vmalloc(size_t size);
void* vzalloc(size_t size);  // Zero-initialized
void vfree(void* addr);

// Memory regions
uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end);
bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags);

// Utilities
void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_dump_context_stats(vmm_context_t* ctx);
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(uintptr_t virt_addr);

// Protection and flags
bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags);
bool vmm_is_user_accessible(uintptr_t virt_addr);
bool vmm_is_kernel_addr(uintptr_t addr);

// ========== INTERNAL FUNCTIONS (for advanced use) ==========

// Page table manipulation
page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level);
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr);
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr);
void vmm_invalidate_page(uintptr_t virt_addr);

// Physical memory integration
uintptr_t vmm_alloc_page_table(void);
void vmm_free_page_table(uintptr_t phys_addr);

// Error handling
const char* vmm_get_last_error(void);
void vmm_set_error(const char* error);

// ========== DEBUGGING & STATISTICS ==========
typedef struct {
    size_t total_contexts;
    size_t total_mapped_pages;
    size_t kernel_mapped_pages;
    size_t user_mapped_pages;
    size_t page_tables_allocated;
    size_t page_faults_handled;
    size_t tlb_flushes;
} vmm_stats_t;

void vmm_get_global_stats(vmm_stats_t* stats);
void vmm_print_stats(void);

// ========== INLINE HELPERS ==========

// Check if address is page-aligned
static inline bool vmm_is_page_aligned(uintptr_t addr) {
    return (addr & VMM_PAGE_OFFSET_MASK) == 0;
}

// Align address down to page boundary
static inline uintptr_t vmm_page_align_down(uintptr_t addr) {
    return addr & VMM_PAGE_MASK;
}

// Align address up to page boundary
static inline uintptr_t vmm_page_align_up(uintptr_t addr) {
    return (addr + VMM_PAGE_SIZE - 1) & VMM_PAGE_MASK;
}

// Get page count for size
static inline size_t vmm_size_to_pages(size_t size) {
    return (size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
}

// Convert pages to bytes
static inline size_t vmm_pages_to_size(size_t pages) {
    return pages * VMM_PAGE_SIZE;
}

// Extract physical address from PTE
static inline uintptr_t vmm_pte_to_phys(pte_t pte) {
    return pte & VMM_PTE_ADDR_MASK;
}

// Extract flags from PTE
static inline uint64_t vmm_pte_to_flags(pte_t pte) {
    return pte & VMM_PTE_FLAGS_MASK;
}

// Create PTE from physical address and flags
static inline pte_t vmm_make_pte(uintptr_t phys_addr, uint64_t flags) {
    return (phys_addr & VMM_PTE_ADDR_MASK) | (flags & VMM_PTE_FLAGS_MASK);
}

// Page fault handling
// Returns: 0 on success (handled), -1 on error (unhandled)
int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code);

#endif // VMM_H