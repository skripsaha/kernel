#include "vmm.h"
#include "pmm.h"
#include "klib.h"
#include "io.h"


// ========== GLOBAL VARIABLES ==========
static vmm_context_t* kernel_context = NULL;
static vmm_context_t* current_context = NULL;
static bool vmm_initialized = false;
static char last_error[256] = {0};
static vmm_stats_t global_stats = {0};
static spinlock_t vmm_global_lock = {0};

// Kernel heap tracking
static uintptr_t kernel_heap_current = VMM_KERNEL_HEAP_BASE;
static spinlock_t kernel_heap_lock = {0};

// vmalloc tracking (simple linked list)
typedef struct vmalloc_entry {
    void* virt_base;
    size_t pages;
    struct vmalloc_entry* next;
} vmalloc_entry_t;

static vmalloc_entry_t* vmalloc_list = NULL;
static spinlock_t vmalloc_lock = {0};

// ========== ERROR HANDLING ==========
void vmm_set_error(const char* error) {
    if (error) {
        strncpy(last_error, error, sizeof(last_error) - 1);
        last_error[sizeof(last_error) - 1] = '\0';
    } else {
        last_error[0] = '\0';
    }
}

const char* vmm_get_last_error(void) {
    return last_error;
}

// ========== PHYSICAL MEMORY INTEGRATION ==========
uintptr_t vmm_alloc_page_table(void) {
    void* page = pmm_alloc_zero(1);  // One page, zero-initialized (returns phys pointer)
    if (!page) {
        vmm_set_error("Failed to allocate page table from PMM");
        return 0;
    }
    spin_lock(&vmm_global_lock);
    global_stats.page_tables_allocated++;
    spin_unlock(&vmm_global_lock);
    return (uintptr_t)page;
}

void vmm_free_page_table(uintptr_t phys_addr) {
    if (!phys_addr) return;
    pmm_free((void*)phys_addr, 1);
    spin_lock(&vmm_global_lock);
    if (global_stats.page_tables_allocated > 0) global_stats.page_tables_allocated--;
    spin_unlock(&vmm_global_lock);
}

// ========== TLB MANAGEMENT ==========
void vmm_flush_tlb(void) {
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    spin_lock(&vmm_global_lock);
    global_stats.tlb_flushes++;
    spin_unlock(&vmm_global_lock);
}

void vmm_flush_tlb_page(uintptr_t virt_addr) {
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    spin_lock(&vmm_global_lock);
    global_stats.tlb_flushes++;
    spin_unlock(&vmm_global_lock);
}

void vmm_invalidate_page(uintptr_t virt_addr) {
    vmm_flush_tlb_page(virt_addr);
}

// ========== PAGE TABLE MANIPULATION (helpers) ==========

// Internal: walk and create intermediate tables up to `level` (1..3). Return pointer to that table (virtual).
// level==1 -> return PDPT, level==2 -> return PD, level==3 -> return PT.
page_table_t* vmm_get_or_create_table(vmm_context_t* ctx, uintptr_t virt_addr, int level) {
    if (!ctx || !ctx->pml4) return NULL;

    page_table_t* current_table = ctx->pml4;
    uint32_t indices[4] = {
        VMM_PML4_INDEX(virt_addr),
        VMM_PDPT_INDEX(virt_addr),
        VMM_PD_INDEX(virt_addr),
        VMM_PT_INDEX(virt_addr)
    };

    // Walk down to the requested level
    for (int i = 0; i < level; i++) {
        pte_t* entry = &current_table->entries[indices[i]];

        if (!(*entry & VMM_FLAG_PRESENT)) {
            // Need to create new table
            uintptr_t new_table_phys = vmm_alloc_page_table();
            if (!new_table_phys) {
                vmm_set_error("Failed to allocate page table");
                return NULL;
            }

            // Map it with kernel + user flags (intermediate tables need USER bit!)
            // CRITICAL: All levels of page tables must have USER bit for Ring 3 access
            *entry = vmm_make_pte(new_table_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
        }

        // Move to next level (phys -> virtual pointer)
        // CRITICAL: Using vmm_phys_to_virt() to convert physical to virtual
        // Currently relies on identity mapping (0-2GB), future: proper higher-half mapping
        uintptr_t next_table_phys = vmm_pte_to_phys(*entry);
        current_table = (page_table_t*)vmm_phys_to_virt(next_table_phys);
    }

    return current_table;
}

// Internal: get existing (no allocation) PTE. Returns NULL if PT or parent tables not present.
static pte_t* vmm_get_pte_noalloc(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx || !ctx->pml4) return NULL;

    uint32_t pml4_idx = VMM_PML4_INDEX(virt_addr);
    uint32_t pdpt_idx = VMM_PDPT_INDEX(virt_addr);
    uint32_t pd_idx   = VMM_PD_INDEX(virt_addr);
    uint32_t pt_idx   = VMM_PT_INDEX(virt_addr);

    page_table_t* pml4 = ctx->pml4;
    pte_t pml4_entry = pml4->entries[pml4_idx];
    if (!(pml4_entry & VMM_FLAG_PRESENT)) return NULL;

    page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[pdpt_idx];
    if (!(pdpt_entry & VMM_FLAG_PRESENT)) return NULL;

    // If PDPT entry is large page (1GB), then there's no lower PT
    if (pdpt_entry & VMM_FLAG_LARGE_PAGE) {
        // No PT; represent as PTE not present here
        return NULL;
    }

    page_table_t* pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[pd_idx];
    if (!(pd_entry & VMM_FLAG_PRESENT)) return NULL;

    // If PD entry is large page (2MB), there's no PT
    if (pd_entry & VMM_FLAG_LARGE_PAGE) {
        return NULL;
    }

    page_table_t* pt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    return &pt->entries[pt_idx];
}

// Get PTE and create page tables if necessary
pte_t* vmm_get_or_create_pte(vmm_context_t* ctx, uintptr_t virt_addr) {
    page_table_t* pt = vmm_get_or_create_table(ctx, virt_addr, 3);
    if (!pt) return NULL;
    return &pt->entries[VMM_PT_INDEX(virt_addr)];
}

// Public wrappers (as declared in header).
// vmm_get_pte -> does NOT create tables (safe for translations)
pte_t* vmm_get_pte(vmm_context_t* ctx, uintptr_t virt_addr) {
    return vmm_get_pte_noalloc(ctx, virt_addr);
}



// ========== CONTEXT MANAGEMENT ==========
vmm_context_t* vmm_create_context(void) {
    vmm_context_t* ctx = kmalloc(sizeof(vmm_context_t));
    if (!ctx) {
        vmm_set_error("Failed to allocate VMM context");
        return NULL;
    }

    memset(ctx, 0, sizeof(vmm_context_t));

    // Allocate PML4 table
    uintptr_t pml4_phys = vmm_alloc_page_table();
    if (!pml4_phys) {
        kfree(ctx);
        vmm_set_error("Failed to allocate PML4 table");
        return NULL;
    }

    ctx->pml4 = (page_table_t*)pml4_phys;
    ctx->pml4_phys = pml4_phys;
    ctx->heap_start = VMM_USER_HEAP_BASE;
    ctx->heap_end = VMM_USER_HEAP_BASE;
    ctx->stack_top = VMM_USER_STACK_TOP;

    spinlock_init(&ctx->lock);

    // Copy kernel mappings from kernel_context if we have one
    if (kernel_context && kernel_context->pml4) {

        // CRITICAL FIX: Don't share PDPT for PML4[0]!
        // Problem: User space (0x20000000 = 512MB) is in PML4[0] range (0-512GB)
        // If we copy PML4[0] entry directly, all processes share the same PDPT,
        // causing Process 1's mappings to contaminate Process 2's address space.
        //
        // Solution: Create a NEW PDPT for this context and copy only the
        // identity-mapped large pages (0-256MB) from kernel's PDPT.
        // Allocate new PDPT for this context
        uintptr_t new_pdpt_phys = vmm_alloc_page_table();
        if (!new_pdpt_phys) {
            vmm_free_page_table(pml4_phys);
            kfree(ctx);
            vmm_set_error("Failed to allocate PDPT for new context");
            return NULL;
        }

        page_table_t* new_pdpt = (page_table_t*)new_pdpt_phys;
        // Get kernel's PDPT for PML4[0]

        pte_t kernel_pml4_entry = kernel_context->pml4->entries[0];
        if (kernel_pml4_entry & VMM_FLAG_PRESENT) {
            page_table_t* kernel_pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kernel_pml4_entry));

            // Copy only the large page entries (identity mapping 0-256MB)
            // PDPT entry 0 covers 0-1GB, and within the corresponding PD,
            // entries 0-127 are 2MB large pages covering 0-256MB
            // We need to copy PDPT[0] which points to the PD with large pages

            pte_t kernel_pdpt_entry = kernel_pdpt->entries[0];
            if (kernel_pdpt_entry & VMM_FLAG_PRESENT) {
                // Check if it points to a PD (not a 1GB large page)
                if (!(kernel_pdpt_entry & VMM_FLAG_LARGE_PAGE)) {
                    // Allocate new PD for this context

                    uintptr_t new_pd_phys = vmm_alloc_page_table();
                    if (!new_pd_phys) {
                        vmm_free_page_table(new_pdpt_phys);
                        vmm_free_page_table(pml4_phys);
                        kfree(ctx);
                        vmm_set_error("Failed to allocate PD for identity mapping");
                        return NULL;
                    }

                    page_table_t* new_pd = (page_table_t*)new_pd_phys;
                    page_table_t* kernel_pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(kernel_pdpt_entry));

                    // Copy large page entries (0-127 = 0-256MB)

                    for (int i = 0; i < 128; i++) {

                        new_pd->entries[i] = kernel_pd->entries[i];

                    }

                    // Entries 128-511 (256MB-2GB) remain zero (unmapped in new context)

                    // Set new PD in new PDPT
                    new_pdpt->entries[0] = vmm_make_pte(new_pd_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);
                }
            }
        }

        // Set new PDPT in PML4[0]
        ctx->pml4->entries[0] = vmm_make_pte(new_pdpt_phys, VMM_FLAGS_KERNEL_RW | VMM_FLAG_USER);


        // Copy upper half (kernel space indexes 256..511) - these are safe to share
        for (int i = 256; i < 512; i++) {
            ctx->pml4->entries[i] = kernel_context->pml4->entries[i];
        }
    }

    spin_lock(&vmm_global_lock);
    global_stats.total_contexts++;
    spin_unlock(&vmm_global_lock);

    return ctx;
}

// Helper: free user-space page tables & mapped pages for a context
static void vmm_free_user_space_tables(vmm_context_t* ctx) {
    if (!ctx || !ctx->pml4) return;

    kprintf("[VMM] Freeing user space tables for context CR3=0x%lx\n", ctx->pml4_phys);

    // Only iterate low half (user space): PML4 indices 0..255
        // NOTE: After the fix, PML4[0] has its own PDPT/PD per context (not shared)

    // We still skip freeing kernel identity-mapped large pages (0-256MB)

    for (int p4 = 0; p4 < 256; p4++) {

        pte_t pml4_entry = ctx->pml4->entries[p4];
        if (!(pml4_entry & VMM_FLAG_PRESENT)) continue;
 
        uintptr_t pdpt_phys = vmm_pte_to_phys(pml4_entry);
        page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(pdpt_phys);

        // No longer shared - this context owns its PDPT for PML4[0]
        int is_shared_kernel = 0;  // Always free PDPT/PD now

        // iterate PDPT entries
        for (int p3 = 0; p3 < 512; p3++) {
            pte_t pdpt_entry = pdpt->entries[p3];
            if (!(pdpt_entry & VMM_FLAG_PRESENT)) continue;

            // SKIP 1GB large pages - these are kernel mappings
            if (pdpt_entry & VMM_FLAG_LARGE_PAGE) {
                continue;  // Skip kernel large pages
            }

            uintptr_t pd_phys = vmm_pte_to_phys(pdpt_entry);
            page_table_t* pd = (page_table_t*)vmm_phys_to_virt(pd_phys);

            // iterate PD entries
            for (int p2 = 0; p2 < 512; p2++) {
                pte_t pd_entry = pd->entries[p2];
                if (!(pd_entry & VMM_FLAG_PRESENT)) continue;

                // SKIP 2MB large pages - these are kernel identity mappings
                if (pd_entry & VMM_FLAG_LARGE_PAGE) {
                    continue;  // Skip kernel large pages
                }

                uintptr_t pt_phys = vmm_pte_to_phys(pd_entry);
                page_table_t* pt = (page_table_t*)vmm_phys_to_virt(pt_phys);

                // iterate PT entries (leaf pages) - FREE DATA PAGES
                int freed_pages = 0;
                for (int p1 = 0; p1 < 512; p1++) {
                    pte_t pt_entry = pt->entries[p1];
                    if (!(pt_entry & VMM_FLAG_PRESENT)) continue;

                    uintptr_t phys = vmm_pte_to_phys(pt_entry);

                    // CRITICAL: Only free if it's a user page (not kernel/shared)
                    // User pages are below 0x20000000 (512MB) in our memory map
                    if (phys >= 0x100000 && phys < 0x20000000) {
                        pmm_free((void*)phys, 1);
                        freed_pages++;
                    }

                    pt->entries[p1] = 0;
                }

                if (freed_pages > 0) {
                    kprintf("[VMM]   Freed %d data pages from PT\n", freed_pages);
                }

                // Free PT table itself (always safe - user-allocated)
                vmm_free_page_table(pt_phys);
                pd->entries[p2] = 0;
            }

            // Free PD table (only if not shared with kernel)
            if (!is_shared_kernel) {
                vmm_free_page_table(pd_phys);
                pdpt->entries[p3] = 0;
            }
        }

        // Free PDPT table (only if not shared with kernel)
        if (!is_shared_kernel) {
            vmm_free_page_table(pdpt_phys);
            ctx->pml4->entries[p4] = 0;
        }
    }

    kprintf("[VMM] User space tables freed\n");
}

void vmm_destroy_context(vmm_context_t* ctx) {
    if (!ctx || ctx == kernel_context) return;

    spin_lock(&ctx->lock);

    // Free all user-space mappings and associated page tables
    vmm_free_user_space_tables(ctx);

    // Finally free the PML4 itself
    if (ctx->pml4_phys) {
        vmm_free_page_table(ctx->pml4_phys);
    }

    spin_unlock(&ctx->lock);

    kfree(ctx);

    spin_lock(&vmm_global_lock);
    if (global_stats.total_contexts > 0) global_stats.total_contexts--;
    spin_unlock(&vmm_global_lock);
}

vmm_context_t* vmm_get_kernel_context(void) {
    return kernel_context;
}

vmm_context_t* vmm_get_current_context(void) {
    return current_context ? current_context : kernel_context;
}

void vmm_switch_context(vmm_context_t* ctx) {
    if (!ctx || !ctx->pml4_phys) return;

    current_context = ctx;
    asm volatile("mov %0, %%cr3" : : "r"(ctx->pml4_phys) : "memory");
    vmm_flush_tlb();
}

// ========== MEMORY MAPPING ==========
vmm_map_result_t vmm_map_page(vmm_context_t* ctx, uintptr_t virt_addr,
                              uintptr_t phys_addr, uint64_t flags) {
    vmm_map_result_t result = {0};

    if (!ctx) {
        result.error_msg = "Invalid context";
        return result;
    }

    if (!vmm_is_page_aligned(virt_addr) || !vmm_is_page_aligned(phys_addr)) {
        result.error_msg = "Address not page-aligned";
        return result;
    }

    spin_lock(&ctx->lock);

    // We need to create page table for mapping
    pte_t* pte = vmm_get_or_create_pte(ctx, virt_addr);
    if (!pte) {
        spin_unlock(&ctx->lock);
        result.error_msg = "Failed to get/create page table entry";
        return result;
    }

    // Если уже был присутствующий PTE
    if (*pte & VMM_FLAG_PRESENT) {
        uintptr_t existing_phys = vmm_pte_to_phys(*pte);
        uint64_t existing_flags = vmm_pte_to_flags(*pte);

        spin_unlock(&ctx->lock);

        if (existing_phys == phys_addr && existing_flags == flags) {
            result.success = true;
            result.virt_addr = virt_addr;
            result.phys_addr = phys_addr;
            result.pages_mapped = 1;
            return result;
        }

        char error_buf[256];
        ksnprintf(error_buf, sizeof(error_buf),
                  "Page already mapped (virt=0x%p: existing_phys=0x%p, new_phys=0x%p, existing_flags=0x%llx, new_flags=0x%llx)",
                  (void*)virt_addr, (void*)existing_phys, (void*)phys_addr,
                  (unsigned long long)existing_flags, (unsigned long long)flags);
        kprintf("[VMM] %s\n", error_buf);
        result.error_msg = "Page already mapped with different address/flags";
        return result;
    }

    // Map the page
    *pte = vmm_make_pte(phys_addr, flags);

    // Update statistics
    ctx->mapped_pages++;
    if (flags & VMM_FLAG_USER) {
        ctx->user_pages++;
        spin_lock(&vmm_global_lock);
        global_stats.user_mapped_pages++;
        global_stats.total_mapped_pages++;
        spin_unlock(&vmm_global_lock);
    } else {
        ctx->kernel_pages++;
        spin_lock(&vmm_global_lock);
        global_stats.kernel_mapped_pages++;
        global_stats.total_mapped_pages++;
        spin_unlock(&vmm_global_lock);
    }

    spin_unlock(&ctx->lock);

    // Invalidate TLB for this page
    vmm_flush_tlb_page(virt_addr);

    result.success = true;
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;
    result.pages_mapped = 1;

    return result;
}

vmm_map_result_t vmm_map_pages(vmm_context_t* ctx, uintptr_t virt_addr,
                               uintptr_t phys_addr, size_t page_count, uint64_t flags) {
    vmm_map_result_t result = {0};
    result.virt_addr = virt_addr;
    result.phys_addr = phys_addr;

    for (size_t i = 0; i < page_count; i++) {
        vmm_map_result_t single_result = vmm_map_page(ctx,
            virt_addr + i * VMM_PAGE_SIZE,
            phys_addr + i * VMM_PAGE_SIZE,
            flags);

        if (!single_result.success) {
            // Rollback previous mappings
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(ctx, virt_addr + j * VMM_PAGE_SIZE);
            }
            result.error_msg = single_result.error_msg;
            return result;
        }

        result.pages_mapped++;
    }

    result.success = true;
    return result;
}

bool vmm_unmap_page(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx || !vmm_is_page_aligned(virt_addr)) return false;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return false;
    }

    // Update statistics (only counts, do not free physical pages here)
    uint64_t flags = vmm_pte_to_flags(*pte);
    if (flags & VMM_FLAG_USER) {
        if (ctx->user_pages > 0) ctx->user_pages--;
        spin_lock(&vmm_global_lock);
        if (global_stats.user_mapped_pages > 0) global_stats.user_mapped_pages--;
        if (global_stats.total_mapped_pages > 0) global_stats.total_mapped_pages--;
        spin_unlock(&vmm_global_lock);
    } else {
        if (ctx->kernel_pages > 0) ctx->kernel_pages--;
        spin_lock(&vmm_global_lock);
        if (global_stats.kernel_mapped_pages > 0) global_stats.kernel_mapped_pages--;
        if (global_stats.total_mapped_pages > 0) global_stats.total_mapped_pages--;
        spin_unlock(&vmm_global_lock);
    }

    if (ctx->mapped_pages > 0) ctx->mapped_pages--;

    // Clear the PTE
    *pte = 0;

    spin_unlock(&ctx->lock);

    // Invalidate TLB
    vmm_flush_tlb_page(virt_addr);

    return true;
}

bool vmm_unmap_pages(vmm_context_t* ctx, uintptr_t virt_addr, size_t page_count) {
    bool success = true;

    for (size_t i = 0; i < page_count; i++) {
        if (!vmm_unmap_page(ctx, virt_addr + i * VMM_PAGE_SIZE)) {
            success = false;
        }
    }

    return success;
}

// ========== HIGH-LEVEL ALLOCATION ==========
void* vmm_alloc_pages(vmm_context_t* ctx, size_t page_count, uint64_t flags) {
    if (!ctx || page_count == 0) {
        kprintf("[VMM] vmm_alloc_pages: invalid parameters (ctx=%p, count=%zu)\n", ctx, page_count);
        return NULL;
    }

    kprintf("[VMM] vmm_alloc_pages: requesting %zu pages with flags 0x%llx\n", page_count, (unsigned long long)flags);

    // Allocate physical pages first (returns pointer to physical memory)
    void* phys_pages = pmm_alloc(page_count);
    if (!phys_pages) {
        vmm_set_error("Failed to allocate physical pages");
        kprintf("[VMM] PMM allocation failed for %zu pages\n", page_count);
        return NULL;
    }

    kprintf("[VMM] PMM allocated %zu pages at physical 0x%p\n", page_count, phys_pages);

    uintptr_t phys_base = (uintptr_t)phys_pages;
    uintptr_t virt_base;

    // Find virtual address space
    if (flags & VMM_FLAG_USER) {
        virt_base = vmm_find_free_region(ctx, vmm_pages_to_size(page_count),
                                         VMM_USER_BASE, VMM_USER_STACK_TOP);
        if (!virt_base) {
            pmm_free(phys_pages, page_count);
            vmm_set_error("Failed to find user virtual address space");
            kprintf("[VMM] Failed to find user virtual space for %zu pages\n", page_count);
            return NULL;
        }
        kprintf("[VMM] Found user virtual space at 0x%p\n", (void*)virt_base);
    } else {
        // Kernel allocation - use simple sequential allocation
        spin_lock(&kernel_heap_lock);
        virt_base = kernel_heap_current;

        kprintf("[VMM] Current kernel heap pointer: 0x%p\n", (void*)kernel_heap_current);
        kprintf("[VMM] Kernel heap base: 0x%p\n", (void*)VMM_KERNEL_HEAP_BASE);
        kprintf("[VMM] Kernel heap size: 0x%llx\n", (unsigned long long)VMM_KERNEL_HEAP_SIZE);

        // Check if we have enough space (basic check)
        if (virt_base + vmm_pages_to_size(page_count) > VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE) {
            spin_unlock(&kernel_heap_lock);
            pmm_free(phys_pages, page_count);
            vmm_set_error("Kernel heap exhausted");
            kprintf("[VMM] ERROR: Kernel heap exhausted! Current: 0x%p, need: 0x%llx, limit: 0x%p\n",
                   (void*)virt_base, (unsigned long long)vmm_pages_to_size(page_count),
                   (void*)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
            return NULL;
        }

        kernel_heap_current += vmm_pages_to_size(page_count);
        spin_unlock(&kernel_heap_lock);

        kprintf("[VMM] Kernel allocation: virt=0x%p, phys=0x%p, pages=%zu\n",
               (void*)virt_base, (void*)phys_base, page_count);
    }

    // Map the pages individually for better error handling
    for (size_t i = 0; i < page_count; i++) {
        uintptr_t virt_addr = virt_base + i * VMM_PAGE_SIZE;
        uintptr_t phys_addr = phys_base + i * VMM_PAGE_SIZE;

        kprintf("[VMM] Mapping page %zu/%zu: virt=0x%p -> phys=0x%p\n",
               i + 1, page_count, (void*)virt_addr, (void*)phys_addr);

        vmm_map_result_t result = vmm_map_page(ctx, virt_addr, phys_addr, flags);

        if (!result.success) {
            kprintf("[VMM] ERROR: Failed to map page %zu/%zu (virt=0x%p, phys=0x%p): %s\n",
                   i + 1, page_count, (void*)virt_addr, (void*)phys_addr,
                   result.error_msg ? result.error_msg : "unknown error");

            // Rollback previous mappings
            for (size_t j = 0; j < i; j++) {
                vmm_unmap_page(ctx, virt_base + j * VMM_PAGE_SIZE);
            }

            pmm_free(phys_pages, page_count);
            vmm_set_error(result.error_msg);
            return NULL;
        }
    }

    kprintf("[VMM] SUCCESS: Allocated %zu pages at virtual 0x%p\n", page_count, (void*)virt_base);
    return (void*)virt_base;
}

void vmm_free_pages(vmm_context_t* ctx, void* virt_addr, size_t page_count) {
    if (!ctx || !virt_addr || page_count == 0) return;

    uintptr_t virt_base = (uintptr_t)virt_addr;

    // Collect physical addresses
    uintptr_t* phys_addrs = kmalloc(page_count * sizeof(uintptr_t));
    if (phys_addrs) {
        for (size_t i = 0; i < page_count; i++) {
            phys_addrs[i] = vmm_virt_to_phys(ctx, virt_base + i * VMM_PAGE_SIZE);
        }
    }

    // Unmap virtual pages (does not free physical)
    vmm_unmap_pages(ctx, virt_base, page_count);

    // Free physical pages
    if (phys_addrs) {
        for (size_t i = 0; i < page_count; i++) {
            if (phys_addrs[i]) {
                pmm_free((void*)phys_addrs[i], 1);
            }
        }
        kfree(phys_addrs);
    }
}

// ========== KERNEL HEAP (vmalloc) ==========
void* vmalloc(size_t size) {
    if (size == 0) {
        kprintf("[VMM] vmalloc: size is 0\n");
        return NULL;
    }

    if (!vmm_initialized) {
        kprintf("[VMM] vmalloc: VMM not initialized\n");
        return NULL;
    }

    size_t page_count = vmm_size_to_pages(size);
    kprintf("[VMM] vmalloc: requested %zu bytes (%zu pages)\n", size, page_count);

    vmm_context_t* ctx = vmm_get_current_context();
    if (!ctx) {
        kprintf("[VMM] vmalloc: no current context\n");
        return NULL;
    }

    void* virt = vmm_alloc_pages(ctx, page_count, VMM_FLAGS_KERNEL_RW);
    if (!virt) {
        kprintf("[VMM] vmalloc FAILED: %s\n", vmm_get_last_error());
        return NULL;
    }

    uintptr_t phys_first = vmm_virt_to_phys(ctx, (uintptr_t)virt);
    kprintf("[VMM] vmalloc: allocated virt=%p phys=%p pages=%zu\n",
            virt, (void*)phys_first, page_count);

    // Register allocation for vfree
    vmalloc_entry_t* ent = kmalloc(sizeof(vmalloc_entry_t));
    if (ent) {
        ent->virt_base = virt;
        ent->pages = page_count;
        spin_lock(&vmalloc_lock);
        ent->next = vmalloc_list;
        vmalloc_list = ent;
        spin_unlock(&vmalloc_lock);
        kprintf("[VMM] vmalloc: recorded allocation (%p, %zu pages)\n", virt, page_count);
    } else {
        kprintf("[VMM] vmalloc: WARNING: could not record allocation for vfree()\n");
    }

    kprintf("[VMM] vmalloc SUCCESS: %p (%zu pages)\n", virt, page_count);
    return virt;
}


void* vzalloc(size_t size) {
    void* addr = vmalloc(size);
    if (addr) {
        memset(addr, 0, size);
    }
    return addr;
}

void vfree(void* addr) {
    if (!addr) {
        kprintf("[VMM] vfree: null pointer, nothing to free\n");
        return;
    }

    spin_lock(&vmalloc_lock);
    vmalloc_entry_t* prev = NULL;
    vmalloc_entry_t* cur = vmalloc_list;

    while (cur) {
        if (cur->virt_base == addr) {
            // Found allocation
            if (prev) prev->next = cur->next;
            else vmalloc_list = cur->next;
            spin_unlock(&vmalloc_lock);

            kprintf("[VMM] vfree: freeing allocation at %p (%zu pages)\n",
                    addr, cur->pages);

            vmm_free_pages(vmm_get_current_context(), addr, cur->pages);
            kfree(cur);

            kprintf("[VMM] vfree: successfully freed %p\n", addr);
            return;
        }
        prev = cur;
        cur = cur->next;
    }

    spin_unlock(&vmalloc_lock);

    // Not found — fallback mode
    kprintf("[VMM] vfree: allocation not found in list, fallback free at %p\n", addr);

    uintptr_t phys = vmm_virt_to_phys(vmm_get_current_context(), (uintptr_t)addr);
    if (phys) {
        kprintf("[VMM] vfree: unmapped single page virt=%p phys=%p\n", addr, (void*)phys);
        vmm_unmap_page(vmm_get_current_context(), (uintptr_t)addr);
        pmm_free((void*)phys, 1);
    } else {
        kprintf("[VMM] vfree: WARNING: could not resolve physical address for %p\n", addr);
    }
}


// ========== ADDRESS TRANSLATION ==========
uintptr_t vmm_virt_to_phys(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return 0;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uintptr_t phys_base = vmm_pte_to_phys(*pte);
    uintptr_t offset = virt_addr & VMM_PAGE_OFFSET_MASK;

    spin_unlock(&ctx->lock);

    return phys_base + offset;
}

bool vmm_is_mapped(vmm_context_t* ctx, uintptr_t virt_addr) {
    return vmm_virt_to_phys(ctx, virt_addr) != 0;
}

uint64_t vmm_get_page_flags(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return 0;

    spin_lock(&ctx->lock);

    pte_t* pte = vmm_get_pte(ctx, virt_addr); // noalloc
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return 0;
    }

    uint64_t flags = vmm_pte_to_flags(*pte);
    spin_unlock(&ctx->lock);

    return flags;
}

// ========== UTILITIES ==========
uintptr_t vmm_find_free_region(vmm_context_t* ctx, size_t size, uintptr_t start, uintptr_t end) {
    if (!ctx || size == 0 || start >= end) return 0;

    size_t pages_needed = vmm_size_to_pages(size);
    uintptr_t current = vmm_page_align_up(start);

    while (current + vmm_pages_to_size(pages_needed) <= end) {
        bool region_free = true;

        // Check pages
        for (size_t i = 0; i < pages_needed; i++) {
            if (vmm_is_mapped(ctx, current + i * VMM_PAGE_SIZE)) {
                region_free = false;
                // skip past this mapped page
                uintptr_t next = current + (i + 1) * VMM_PAGE_SIZE;

                // Check for overflow and ensure we're making progress
                if (next <= current || next >= end) {
                    return 0;  // No free region found (overflow or out of bounds)
                }

                current = vmm_page_align_up(next);
                break;
            }
        }

        if (region_free) {
            return current;
        }
    }

    return 0;
}

bool vmm_is_kernel_addr(uintptr_t addr) {
    return addr >= VMM_KERNEL_BASE;
}

bool vmm_is_user_accessible(uintptr_t virt_addr) {
    vmm_context_t* ctx = vmm_get_current_context();
    if (!ctx) return false;

    uint64_t flags = vmm_get_page_flags(ctx, virt_addr);
    return (flags & VMM_FLAG_USER) != 0;
}

bool vmm_protect(vmm_context_t* ctx, uintptr_t virt_addr, size_t size, uint64_t new_flags) {
    if (!ctx || size == 0) return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t current_addr = vmm_page_align_down(virt_addr);

    spin_lock(&ctx->lock);

    for (size_t i = 0; i < page_count; i++) {
        pte_t* pte = vmm_get_pte(ctx, current_addr); // noalloc
        if (!pte || !(*pte & VMM_FLAG_PRESENT)) {
            spin_unlock(&ctx->lock);
            return false;
        }

        // Update flags while preserving physical address
        uintptr_t phys_addr = vmm_pte_to_phys(*pte);
        // Ensure present bit remains set unless new_flags explicitly clears it
        uint64_t flags_to_set = (new_flags & VMM_PTE_FLAGS_MASK);
        if (!(flags_to_set & VMM_FLAG_PRESENT)) flags_to_set |= VMM_FLAG_PRESENT;
        *pte = vmm_make_pte(phys_addr, flags_to_set);

        vmm_flush_tlb_page(current_addr);
        current_addr += VMM_PAGE_SIZE;
    }

    spin_unlock(&ctx->lock);
    return true;
}

bool vmm_reserve_region(vmm_context_t* ctx, uintptr_t start, size_t size, uint64_t flags) {
    if (!ctx || size == 0) return false;

    size_t page_count = vmm_size_to_pages(size);
    uintptr_t virt_base = vmm_page_align_down(start);

    // Allocate physical pages
    void* phys_pages = pmm_alloc(page_count);
    if (!phys_pages) return false;

    // Map the region
    vmm_map_result_t result = vmm_map_pages(ctx, virt_base, (uintptr_t)phys_pages,
                                           page_count, flags);

    if (!result.success) {
        pmm_free(phys_pages, page_count);
        return false;
    }

    return true;
}

// ========== INITIALIZATION ==========
void vmm_init(void) {
    if (vmm_initialized) {
        kprintf("[VMM] Already initialized!\n");
        return;
    }

    kprintf("[VMM] Initializing Virtual Memory Manager...\n");

    spinlock_init(&vmm_global_lock);
    spinlock_init(&kernel_heap_lock);
    spinlock_init(&vmalloc_lock);

    // Create kernel context
    kernel_context = vmm_create_context();
    if (!kernel_context) {
        panic("Failed to create kernel VMM context");
    }

    kprintf("[VMM] Kernel context created at %p\n", kernel_context);
    kprintf("[VMM] PML4 physical address: 0x%p\n", (void*)kernel_context->pml4_phys);

    // Set up identity mapping for low memory only (first 256MB)
    // CRITICAL: VMM relies on physical = virtual for page table access!
    //
    // IMPORTANT: We limit identity mapping to 256MB to avoid conflicts with
    // user space mappings (which start at 512MB). This is sufficient for:
    // - VGA buffer (0xB8000)
    // - Serial ports and low memory devices
    // - Page table structures allocated by PMM
    // - Boot-time kernel data structures
    //
    // OPTIMIZATION: Use 2MB large pages for identity mapping
    // This reduces mappings from 65K to 128, dramatically speeding up initialization
    #define IDENTITY_MAP_SIZE (256ULL * 1024 * 1024)  // 256MB
    uintptr_t identity_map_end = IDENTITY_MAP_SIZE;

    kprintf("[VMM] Setting up identity mapping for %zu MB (using 2MB large pages)...\n",
            identity_map_end / (1024*1024));

    // Map using 2MB large pages for speed
    #define LARGE_PAGE_SIZE (2 * 1024 * 1024)  // 2MB
    size_t large_pages_mapped = 0;

    for (uintptr_t addr = 0; addr < identity_map_end; addr += LARGE_PAGE_SIZE) {
        // Get or create page directory (PD) entry
        page_table_t* pd = vmm_get_or_create_table(kernel_context, addr, 2);
        if (!pd) {
            if (addr < 0x1000000) { // First 16MB is critical
                panic("Failed to create page directory for identity mapping");
            }
            continue;
        }

        // Calculate PD index
        uint32_t pd_idx = VMM_PD_INDEX(addr);
        pte_t* pd_entry = &pd->entries[pd_idx];

        // Create 2MB large page entry (skip Page Table level)
        // Set LARGE_PAGE bit (bit 7) to indicate 2MB page
        *pd_entry = vmm_make_pte(addr, VMM_FLAGS_KERNEL_RW | VMM_FLAG_LARGE_PAGE);
        large_pages_mapped++;
    }

    kprintf("[VMM] Identity mapped %zu large pages (%zu MB)\n",
           large_pages_mapped, large_pages_mapped * 2);

    kprintf("[VMM] Kernel heap will be mapped on demand starting at 0x%p\n",
           (void*)VMM_KERNEL_HEAP_BASE);

    // Test identity mapping by writing & reading known physical address (1MB mark)
    kprintf("[VMM] Testing identity mapping...\n");
    volatile uint32_t* test_ptr = (volatile uint32_t*)0x100000; // 1MB
    uint32_t old_value = *test_ptr;
    *test_ptr = 0xDEADBEEF;
    if (*test_ptr != 0xDEADBEEF) {
        panic("Identity mapping test failed");
    }
    *test_ptr = old_value; // Restore
    kprintf("[VMM] Identity mapping test: PASSED\n");

    // Switch to our new page tables
    current_context = kernel_context;
    vmm_switch_context(kernel_context);

    // Test that we can still access memory after switch
    *test_ptr = 0xCAFEBABE;
    if (*test_ptr != 0xCAFEBABE) {
        panic("Page table switch broke memory access");
    }
    *test_ptr = old_value; // Restore

    vmm_initialized = true;

    kprintf("[VMM] Virtual memory layout:\n");
    kprintf("[VMM]   Kernel base:      0x%p\n", (void*)VMM_KERNEL_BASE);
    kprintf("[VMM]   Kernel heap:      0x%p - 0x%p (on-demand)\n",
           (void*)VMM_KERNEL_HEAP_BASE,
           (void*)(VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE));
    kprintf("[VMM]   User base:        0x%p\n", (void*)VMM_USER_BASE);
    kprintf("[VMM]   User heap:        0x%p\n", (void*)VMM_USER_HEAP_BASE);
    kprintf("[VMM]   User stack top:   0x%p\n", (void*)VMM_USER_STACK_TOP);

    kprintf("[VMM] %[S]Virtual Memory Manager initialized successfully!%[D]\n");
}

// ========== DEBUGGING & STATISTICS ==========
void vmm_dump_page_tables(vmm_context_t* ctx, uintptr_t virt_addr) {
    if (!ctx) return;

    kprintf("[VMM] Page table dump for virtual address 0x%p:\n", (void*)virt_addr);
    kprintf("[VMM]   PML4 index: %d\n", VMM_PML4_INDEX(virt_addr));
    kprintf("[VMM]   PDPT index: %d\n", VMM_PDPT_INDEX(virt_addr));
    kprintf("[VMM]   PD index:   %d\n", VMM_PD_INDEX(virt_addr));
    kprintf("[VMM]   PT index:   %d\n", VMM_PT_INDEX(virt_addr));

    spin_lock(&ctx->lock);

    page_table_t* pml4 = ctx->pml4;
    if (!pml4) {
        kprintf("[VMM]   PML4: NULL\n");
        spin_unlock(&ctx->lock);
        return;
    }

    pte_t pml4_entry = pml4->entries[VMM_PML4_INDEX(virt_addr)];
    kprintf("[VMM]   PML4 entry: 0x%016llx (present: %s)\n",
           (unsigned long long)pml4_entry, (pml4_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pml4_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pdpt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pml4_entry));
    pte_t pdpt_entry = pdpt->entries[VMM_PDPT_INDEX(virt_addr)];
    kprintf("[VMM]   PDPT entry: 0x%016llx (present: %s)\n",
           (unsigned long long)pdpt_entry, (pdpt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pdpt_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pd = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pdpt_entry));
    pte_t pd_entry = pd->entries[VMM_PD_INDEX(virt_addr)];
    kprintf("[VMM]   PD entry:   0x%016llx (present: %s)\n",
           (unsigned long long)pd_entry, (pd_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (!(pd_entry & VMM_FLAG_PRESENT)) {
        spin_unlock(&ctx->lock);
        return;
    }

    // If PD entry is large page, no PT
    if (pd_entry & VMM_FLAG_LARGE_PAGE) {
        kprintf("[VMM]   PD entry is a large page (2MB). Physical: 0x%p\n", (void*)vmm_pte_to_phys(pd_entry));
        spin_unlock(&ctx->lock);
        return;
    }

    page_table_t* pt = (page_table_t*)vmm_phys_to_virt(vmm_pte_to_phys(pd_entry));
    pte_t pt_entry = pt->entries[VMM_PT_INDEX(virt_addr)];
    kprintf("[VMM]   PT entry:   0x%016llx (present: %s)\n",
           (unsigned long long)pt_entry, (pt_entry & VMM_FLAG_PRESENT) ? "yes" : "no");

    if (pt_entry & VMM_FLAG_PRESENT) {
        uintptr_t phys_addr = vmm_pte_to_phys(pt_entry);
        kprintf("[VMM]   -> Physical: 0x%p\n", (void*)phys_addr);
        kprintf("[VMM]   -> Flags: %s%s%s%s\n",
               (pt_entry & VMM_FLAG_WRITABLE) ? "W" : "R",
               (pt_entry & VMM_FLAG_USER) ? "U" : "K",
               (pt_entry & VMM_FLAG_NO_EXECUTE) ? "NX" : "X",
               (pt_entry & VMM_FLAG_GLOBAL) ? "G" : "");
    }

    spin_unlock(&ctx->lock);
}

void vmm_dump_context_stats(vmm_context_t* ctx) {
    if (!ctx) return;

    spin_lock(&ctx->lock);

    kprintf("[VMM] Context statistics:\n");
    kprintf("[VMM]   PML4 physical:   0x%p\n", (void*)ctx->pml4_phys);
    kprintf("[VMM]   Total pages:     %zu\n", ctx->mapped_pages);
    kprintf("[VMM]   Kernel pages:    %zu\n", ctx->kernel_pages);
    kprintf("[VMM]   User pages:      %zu\n", ctx->user_pages);
    kprintf("[VMM]   Heap start:      0x%p\n", (void*)ctx->heap_start);
    kprintf("[VMM]   Heap end:        0x%p\n", (void*)ctx->heap_end);
    kprintf("[VMM]   Stack top:       0x%p\n", (void*)ctx->stack_top);

    spin_unlock(&ctx->lock);
}

void vmm_get_global_stats(vmm_stats_t* stats) {
    if (!stats) return;

    spin_lock(&vmm_global_lock);
    *stats = global_stats;
    spin_unlock(&vmm_global_lock);
}

void vmm_print_stats(void) {
    vmm_stats_t stats;
    vmm_get_global_stats(&stats);

    kprintf("[VMM] Global statistics:\n");
    kprintf("[VMM]   Total contexts:        %zu\n", stats.total_contexts);
    kprintf("[VMM]   Total mapped pages:    %zu (%zu MB)\n",
           stats.total_mapped_pages,
           (stats.total_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("[VMM]   Kernel mapped pages:   %zu (%zu MB)\n",
           stats.kernel_mapped_pages,
           (stats.kernel_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("[VMM]   User mapped pages:     %zu (%zu MB)\n",
           stats.user_mapped_pages,
           (stats.user_mapped_pages * VMM_PAGE_SIZE) / (1024 * 1024));
    kprintf("[VMM]   Page tables allocated: %zu (%zu KB)\n",
           stats.page_tables_allocated,
           (stats.page_tables_allocated * VMM_PAGE_SIZE) / 1024);
    kprintf("[VMM]   Page faults handled:   %zu\n", stats.page_faults_handled);
    kprintf("[VMM]   TLB flushes:           %zu\n", stats.tlb_flushes);
}

// ========== BASIC TESTING ==========
void vmm_test_basic(void) {
    kprintf("[VMM] %[H]Running basic VMM tests...%[D]\n");

    // Test 1: Create and destroy contexts
    kprintf("[VMM] Test 1: Context creation/destruction...\n");
    vmm_context_t* test_ctx = vmm_create_context();
    if (!test_ctx) {
        kprintf("[VMM] %[E]FAILED: Could not create context%[D]\n");
        return;
    }
    kprintf("[VMM] %[S]PASSED: Context created successfully%[D]\n");

    // Test 2: Map a page
    kprintf("[VMM] Test 2: Page mapping...\n");
    void* phys_page = pmm_alloc(1);
    if (!phys_page) {
        kprintf("[VMM] %[E]FAILED: Could not allocate physical page%[D]\n");
        vmm_destroy_context(test_ctx);
        return;
    }

    uintptr_t test_virt = 0x1000000; // 16MB
    vmm_map_result_t result = vmm_map_page(test_ctx, test_virt, (uintptr_t)phys_page,
                                          VMM_FLAGS_KERNEL_RW);
    if (!result.success) {
        kprintf("[VMM] %[E]FAILED: Could not map page: %s%[D]\n", result.error_msg);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    kprintf("[VMM] %[S]PASSED: Page mapped successfully%[D]\n");

    // Test 3: Address translation
    kprintf("[VMM] Test 3: Address translation...\n");
    uintptr_t translated = vmm_virt_to_phys(test_ctx, test_virt);
    if (translated != (uintptr_t)phys_page) {
        kprintf("[VMM] %[E]FAILED: Translation mismatch (got 0x%p, expected 0x%p)%[D]\n",
               (void*)translated, phys_page);
        vmm_unmap_page(test_ctx, test_virt);
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    kprintf("[VMM] %[S]PASSED: Address translation correct%[D]\n");

    // Test 4: Unmap page
    kprintf("[VMM] Test 4: Page unmapping...\n");
    if (!vmm_unmap_page(test_ctx, test_virt)) {
        kprintf("[VMM] %[E]FAILED: Could not unmap page%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }

    // Verify it's unmapped
    if (vmm_is_mapped(test_ctx, test_virt)) {
        kprintf("[VMM] %[E]FAILED: Page still mapped after unmap%[D]\n");
        pmm_free(phys_page, 1);
        vmm_destroy_context(test_ctx);
        return;
    }
    kprintf("[VMM] %[S]PASSED: Page unmapped successfully%[D]\n");

    // Cleanup
    pmm_free(phys_page, 1);
    vmm_destroy_context(test_ctx);

    // Test 5: Kernel heap allocation
    kprintf("[VMM] Test 5: Kernel heap allocation...\n");

    void* heap_ptr = vmalloc(8192); // 2 pages
    
    if (!heap_ptr) {
        kprintf("[VMM] %[E]FAILED: vmalloc failed%[D]\n");
        return;
    }

    // Try to write to it
    memset(heap_ptr, 0xAA, 8192);
    if (((uint8_t*)heap_ptr)[0] != 0xAA || ((uint8_t*)heap_ptr)[8191] != 0xAA) {
        kprintf("[VMM] %[E]FAILED: Could not write to allocated memory%[D]\n");
        vfree(heap_ptr);
        return;
    }

    kprintf("[VMM] %[S]PASSED: Kernel heap allocation works%[D]\n");

    vfree(heap_ptr);

    kprintf("[VMM] %[S]All basic tests PASSED!%[D]\n");

    // Print statistics
    vmm_print_stats();
    vmm_dump_context_stats(kernel_context);
}
// ============================================================================
// PAGE FAULT HANDLING
// ============================================================================

// Page fault error code bits
#define PF_PRESENT   (1 << 0)  // 0 = not present, 1 = protection fault
#define PF_WRITE     (1 << 1)  // 0 = read, 1 = write
#define PF_USER      (1 << 2)  // 0 = kernel, 1 = user mode
#define PF_RESERVED  (1 << 3)  // 1 = reserved bit set in page table
#define PF_INSTR     (1 << 4)  // 1 = instruction fetch

int vmm_handle_page_fault(uintptr_t fault_addr, uint64_t error_code) {
    // Analyze fault
    bool present = error_code & PF_PRESENT;
    bool write = error_code & PF_WRITE;
    bool user = error_code & PF_USER;
    bool reserved = error_code & PF_RESERVED;
    bool instr_fetch = error_code & PF_INSTR;

    kprintf("[VMM] Page fault at 0x%llx (error=0x%llx)\n", fault_addr, error_code);
    kprintf("[VMM]   present=%d write=%d user=%d reserved=%d instr=%d\n",
            present, write, user, reserved, instr_fetch);

    // Reserved bit violations are always fatal
    if (reserved) {
        kprintf("[VMM] ERROR: Reserved bit violation - cannot handle\n");
        return -1;
    }

    // If page is present, it's a protection fault
    if (present) {
        kprintf("[VMM] ERROR: Protection fault - access denied\n");
        return -1;
    }

    // Page not present - demand paging
    kprintf("[VMM] Page not present - attempting demand paging\n");

    // Get kernel context
    vmm_context_t* ctx = kernel_context;

    // Align fault address to page boundary
    uintptr_t page_addr = fault_addr & ~(VMM_PAGE_SIZE - 1);

    // Check if this is in kernel heap range (higher half)
    if (page_addr >= VMM_KERNEL_HEAP_BASE &&
        page_addr < VMM_KERNEL_HEAP_BASE + VMM_KERNEL_HEAP_SIZE) {

        kprintf("[VMM] Demand paging: mapping kernel heap page at 0x%llx\n", page_addr);

        // Allocate physical page
        void* phys_page = pmm_alloc(1);
        if (!phys_page) {
            kprintf("[VMM] ERROR: Failed to allocate physical page\n");
            return -1;
        }

        // Map the page
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        if (user) {
            flags |= VMM_FLAG_USER;
        }

        vmm_map_result_t result = vmm_map_page(ctx, page_addr, (uintptr_t)phys_page, flags);
        if (!result.success) {
            kprintf("[VMM] ERROR: Failed to map page\n");
            pmm_free(phys_page, 1);
            return -1;
        }

        kprintf("[VMM] SUCCESS: Demand paging successful\n");
        return 0;  // Handled successfully
    }

    // Check if this is in low memory (0-256MB) for kernel data/heap fallback
    if (page_addr < (256ULL * 1024 * 1024)) {
        kprintf("[VMM] Demand paging: mapping low memory page at 0x%llx\n", page_addr);

        // Allocate physical page
        void* phys_page = pmm_alloc(1);
        if (!phys_page) {
            kprintf("[VMM] ERROR: Failed to allocate physical page\n");
            return -1;
        }

        // Map the page (kernel mode, writable)
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

        vmm_map_result_t result = vmm_map_page(ctx, page_addr, (uintptr_t)phys_page, flags);
        if (!result.success) {
            kprintf("[VMM] ERROR: Failed to map page\n");
            pmm_free(phys_page, 1);
            return -1;
        }

        kprintf("[VMM] SUCCESS: Low memory page mapped\n");
        return 0;  // Handled successfully
    }

    // Not in a valid range
    kprintf("[VMM] ERROR: Fault address not in valid range (0x%llx)\n", fault_addr);
    return -1;  // Cannot handle
}
