#include "deck_interface.h"
#include "pmm.h"  // Physical memory manager
#include "vmm.h"  // Virtual memory manager
#include "klib.h"
#include "../storage/tagfs.h"  // TagFS - Tag-based filesystem

// ============================================================================
// STORAGE DECK - Memory & Filesystem Operations
// ============================================================================

// Результат аллокации памяти
typedef struct {
    void* address;
    uint64_t size;
} MemoryAllocResult;

// REAL FILE DESCRIPTOR TABLE
typedef struct {
    int fd;                    // File descriptor number
    uint64_t inode_id;         // TagFS inode ID
    char path[256];            // File path/name
    uint64_t size;             // File size
    uint64_t position;         // Current read/write position
    int flags;                 // Open flags (O_RDONLY, O_WRONLY, O_RDWR)
    int in_use;                // 1 if FD is active, 0 if free
} FileDescriptor;

// Глобальная таблица открытых файлов
#define MAX_OPEN_FILES 256
static FileDescriptor fd_table[MAX_OPEN_FILES];
static spinlock_t fd_table_lock;

// Глобальный счетчик FD
static volatile uint64_t next_fd = 100;

// File stat structure (returned by fs_stat)
typedef struct {
    uint64_t inode_id;           // Inode ID
    uint64_t size;               // File size in bytes
    uint64_t creation_time;      // Creation timestamp (RDTSC)
    uint64_t modification_time;  // Last modification timestamp
    uint32_t tag_count;          // Number of tags
    uint32_t flags;              // File flags
} FileStat;

// ============================================================================
// MEMORY OPERATIONS
// ============================================================================

static void* memory_alloc(uint64_t size) {
    // Вычисляем количество страниц (4KB каждая)
    size_t page_count = (size + 4095) / 4096;

    // Аллоцируем память через VMM (используем kernel context)
    void* addr = vmm_alloc_pages(vmm_get_kernel_context(), page_count,
                                 VMM_FLAGS_KERNEL_RW);

    if (addr) {
        kprintf("[STORAGE] Allocated %lu bytes (%lu pages) at %p\n",
                size, page_count, addr);
    } else {
        kprintf("[STORAGE] Failed to allocate %lu bytes\n", size);
    }

    return addr;
}

static void memory_free(void* addr, uint64_t size) {
    size_t page_count = (size + 4095) / 4096;
    vmm_free_pages(vmm_get_kernel_context(), addr, page_count);
    kprintf("[STORAGE] Freed memory at %p (%lu pages)\n", addr, page_count);
}

// ============================================================================
// FILE DESCRIPTOR TABLE MANAGEMENT
// ============================================================================

static int allocate_fd(uint64_t inode_id, const char* path, int flags) {
    spin_lock(&fd_table_lock);

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fd_table[i].in_use) {
            // Found free slot
            fd_table[i].in_use = 1;
            fd_table[i].fd = atomic_increment_u64(&next_fd);
            fd_table[i].inode_id = inode_id;
            fd_table[i].position = 0;
            fd_table[i].flags = flags;

            // Copy path
            int j = 0;
            while (path[j] && j < 255) {
                fd_table[i].path[j] = path[j];
                j++;
            }
            fd_table[i].path[j] = 0;

            // Get file size from inode
            FileInode* inode = tagfs_get_inode(inode_id);
            fd_table[i].size = inode ? inode->size : 0;

            int fd = fd_table[i].fd;
            spin_unlock(&fd_table_lock);
            return fd;
        }
    }

    spin_unlock(&fd_table_lock);
    return -1;  // No free slots
}

static FileDescriptor* find_fd(int fd) {
    spin_lock(&fd_table_lock);

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].in_use && fd_table[i].fd == fd) {
            spin_unlock(&fd_table_lock);
            return &fd_table[i];
        }
    }

    spin_unlock(&fd_table_lock);
    return NULL;  // Not found
}

static void free_fd(int fd) {
    spin_lock(&fd_table_lock);

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i].in_use && fd_table[i].fd == fd) {
            fd_table[i].in_use = 0;
            break;
        }
    }

    spin_unlock(&fd_table_lock);
}

// ============================================================================
// REAL FILESYSTEM OPERATIONS - Using TagFS!
// ============================================================================

// Open file: search by name tag, return FD
static int fs_open(const char* path) {
    // Strategy: search for file with tag "name:path"
    Tag search_tag;
    strcpy(search_tag.key, "name");
    strcpy(search_tag.value, path);

    uint64_t result_inodes[10];
    uint32_t count = 0;

    int ret = tagfs_query_single(&search_tag, result_inodes, &count, 10);

    if (ret == 0 && count > 0) {
        // Found file - open first match
        uint64_t inode_id = result_inodes[0];
        int fd = allocate_fd(inode_id, path, 0);  // flags=0 for now

        if (fd >= 0) {
            kprintf("[STORAGE] Opened file '%s' (inode=%lu, fd=%d)\n",
                    path, inode_id, fd);
            return fd;
        } else {
            kprintf("[STORAGE] ERROR: Failed to allocate FD for '%s'\n", path);
            return -1;
        }
    } else {
        // File not found - create it!
        Tag tags[2];
        strcpy(tags[0].key, "name");
        strcpy(tags[0].value, path);
        strcpy(tags[1].key, "type");
        strcpy(tags[1].value, "file");

        uint64_t inode_id = tagfs_create_file(tags, 2);

        if (inode_id != TAGFS_INVALID_INODE) {
            // PRODUCTION: Sync to disk immediately!
            tagfs_sync();
            int fd = allocate_fd(inode_id, path, 0);
            kprintf("[STORAGE] Created & opened file '%s' (inode=%lu, fd=%d) - synced to disk\n",
                    path, inode_id, fd);
            return fd;
        } else {
            kprintf("[STORAGE] ERROR: Failed to create file '%s'\n", path);
            return -1;
        }
    }
}

// Close file: free FD
static int fs_close(int fd) {
    FileDescriptor* fd_info = find_fd(fd);

    if (fd_info) {
        kprintf("[STORAGE] Closed fd=%d (inode=%lu, '%s')\n",
                fd, fd_info->inode_id, fd_info->path);
        free_fd(fd);
        return 0;
    } else {
        kprintf("[STORAGE] ERROR: Invalid fd=%d\n", fd);
        return -1;
    }
}

// Read from file: use TagFS
static int fs_read(int fd, void* buffer, uint64_t size) {
    FileDescriptor* fd_info = find_fd(fd);

    if (!fd_info) {
        kprintf("[STORAGE] ERROR: Read: invalid fd=%d\n", fd);
        return -1;
    }

    // Read from current position
    int bytes_read = tagfs_read_file(fd_info->inode_id, fd_info->position,
                                      (uint8_t*)buffer, size);

    if (bytes_read >= 0) {
        fd_info->position += bytes_read;
        kprintf("[STORAGE] Read %d bytes from fd=%d (inode=%lu, pos=%lu)\n",
                bytes_read, fd, fd_info->inode_id, fd_info->position);
        return bytes_read;
    } else {
        kprintf("[STORAGE] ERROR: Read failed from fd=%d\n", fd);
        return -1;
    }
}

// Write to file: use TagFS
static int fs_write(int fd, const void* buffer, uint64_t size) {
    FileDescriptor* fd_info = find_fd(fd);

    if (!fd_info) {
        kprintf("[STORAGE] ERROR: Write: invalid fd=%d\n", fd);
        return -1;
    }

    // Write at current position
    int bytes_written = tagfs_write_file(fd_info->inode_id, fd_info->position,
                                          (const uint8_t*)buffer, size);

    if (bytes_written >= 0) {
        fd_info->position += bytes_written;

        // Update file size in FD
        FileInode* inode = tagfs_get_inode(fd_info->inode_id);
        if (inode) {
            fd_info->size = inode->size;
        }

        // PRODUCTION: Sync to disk after every write!
        tagfs_sync();

        kprintf("[STORAGE] Wrote %d bytes to fd=%d (inode=%lu, pos=%lu, size=%lu) - synced to disk\n",
                bytes_written, fd, fd_info->inode_id, fd_info->position, fd_info->size);
        return bytes_written;
    } else {
        kprintf("[STORAGE] ERROR: Write failed to fd=%d\n", fd);
        return -1;
    }
}

// Get file statistics
static int fs_stat(const char* path, FileStat* stat_buf) {
    if (!stat_buf) {
        return -1;  // Invalid buffer
    }

    // Search for file by name tag
    Tag search_tag;
    strcpy(search_tag.key, "name");
    strcpy(search_tag.value, path);

    uint64_t result_inodes[10];
    uint32_t count = 0;

    int ret = tagfs_query_single(&search_tag, result_inodes, &count, 10);

    if (ret == 0 && count > 0) {
        // Found file - get inode info
        uint64_t inode_id = result_inodes[0];
        FileInode* inode = tagfs_get_inode(inode_id);

        if (inode) {
            // Fill stat buffer
            stat_buf->inode_id = inode->inode_id;
            stat_buf->size = inode->size;
            stat_buf->creation_time = inode->creation_time;
            stat_buf->modification_time = inode->modification_time;
            stat_buf->tag_count = inode->tag_count;
            stat_buf->flags = inode->flags;

            kprintf("[STORAGE] Stat '%s': inode=%lu, size=%lu bytes, tags=%u\n",
                    path, inode_id, inode->size, inode->tag_count);
            return 0;  // Success
        } else {
            kprintf("[STORAGE] ERROR: Stat '%s': inode not found in memory\n", path);
            return -1;
        }
    } else {
        kprintf("[STORAGE] ERROR: Stat '%s': file not found\n", path);
        return -1;  // File not found
    }
}

// ============================================================================
// PROCESSING FUNCTION
// ============================================================================

int storage_deck_process(RoutingEntry* entry) {
    // DEFENSIVE: Validate input
    if (!entry) {
        kprintf("[STORAGE] ERROR: NULL routing entry\n");
        return 0;
    }

    Event* event = &entry->event_copy;

    // DEFENSIVE: Validate event type is in storage range (200-299)
    if (event->type < 200 || event->type >= 300) {
        deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                          "Event type out of storage range (200-299)");
        return 0;
    }

    switch (event->type) {
        // === MEMORY OPERATIONS ===
        case EVENT_MEMORY_ALLOC: {
            // DEFENSIVE: Validate size
            uint64_t size = *(uint64_t*)event->data;

            if (size == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory allocation: size is zero");
                return 0;
            }

            // DEFENSIVE: Validate reasonable size (max 16MB per allocation)
            if (size > 16 * 1024 * 1024) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory allocation: size exceeds 16MB limit");
                return 0;
            }

            void* addr = memory_alloc(size);

            if (addr) {
                deck_complete(entry, DECK_PREFIX_STORAGE, addr, RESULT_TYPE_MEMORY_MAPPED);
                kprintf("[STORAGE] Event %lu: allocated %lu bytes\n",
                        event->id, size);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                  "Memory allocation failed");
                return 0;
            }
        }

        case EVENT_MEMORY_FREE: {
            // DEFENSIVE: Validate pointer
            void* addr = *(void**)event->data;
            uint64_t size = *(uint64_t*)(event->data + 8);

            if (!addr) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory free: NULL pointer");
                return 0;
            }

            if (size == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory free: size is zero");
                return 0;
            }

            memory_free(addr, size);
            deck_complete(entry, DECK_PREFIX_STORAGE, 0, RESULT_TYPE_NONE);
            kprintf("[STORAGE] Event %lu: freed memory at %p\n", event->id, addr);
            return 1;
        }

        case EVENT_MEMORY_MAP: {
            // Payload: [size:8][flags:4][fd:4] (fd can be -1 for anonymous mapping)
            uint64_t size = *(uint64_t*)event->data;
            uint32_t flags = *(uint32_t*)(event->data + 8);
            int fd = *(int*)(event->data + 12);

            // DEFENSIVE: Validate size
            if (size == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory map: size is zero");
                return 0;
            }

            // DEFENSIVE: Validate reasonable size (max 64MB)
            if (size > 64 * 1024 * 1024) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Memory map: size exceeds 64MB limit");
                return 0;
            }

            // Real memory mapping implementation
            // For now, implement anonymous mapping (fd == -1)
            // File-backed mapping can be added later

            if (fd == -1) {
                // Anonymous mapping - allocate virtual memory
                void* mapped_addr = vmalloc(size);

                if (mapped_addr) {
                    // Zero-initialize if requested
                    if (flags & 0x01) {  // MAP_ZERO flag
                        memset(mapped_addr, 0, size);
                    }

                    kprintf("[STORAGE] Memory mapped %lu bytes at %p (anonymous)\n",
                            size, mapped_addr);
                    deck_complete(entry, DECK_PREFIX_STORAGE, mapped_addr, RESULT_TYPE_MEMORY_MAPPED);
                    return 1;
                } else {
                    deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                      "Memory mapping failed");
                    return 0;
                }
            } else {
                // File-backed mapping - TODO: implement later
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_NOT_IMPLEMENTED,
                                  "File-backed memory mapping not yet supported");
                return 0;
            }
        }

        // === FILESYSTEM OPERATIONS ===
        case EVENT_FILE_OPEN: {
            // DEFENSIVE: Validate path
            const char* path = (const char*)event->data;

            if (!path || path[0] == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File open: path is NULL or empty");
                return 0;
            }

            // DEFENSIVE: Validate path length
            uint64_t path_len = 0;
            while (path[path_len] && path_len < 255) path_len++;

            if (path_len >= 255) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File open: path exceeds 255 characters");
                return 0;
            }

            int fd = fs_open(path);

            if (fd >= 0) {
                // DEFENSIVE: Check memory allocation
                int* fd_result = (int*)kmalloc(sizeof(int));
                if (!fd_result) {
                    fs_close(fd);  // Clean up
                    deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                      "File open: failed to allocate result buffer");
                    return 0;
                }
                *fd_result = fd;
                deck_complete(entry, DECK_PREFIX_STORAGE, fd_result, RESULT_TYPE_KMALLOC);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_FILE_NOT_FOUND,
                                  "File open failed");
                return 0;
            }
        }

        case EVENT_FILE_CLOSE: {
            // DEFENSIVE: Validate FD
            int fd = *(int*)event->data;

            if (fd < 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File close: invalid file descriptor");
                return 0;
            }

            int result = fs_close(fd);

            if (result == 0) {
                deck_complete(entry, DECK_PREFIX_STORAGE, 0, RESULT_TYPE_NONE);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File close: file descriptor not found");
                return 0;
            }
        }

        case EVENT_FILE_READ: {
            // Payload: [fd:4 bytes][size:8 bytes]
            int fd = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);

            // DEFENSIVE: Validate FD
            if (fd < 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File read: invalid file descriptor");
                return 0;
            }

            // DEFENSIVE: Validate size
            if (size == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File read: size is zero");
                return 0;
            }

            // DEFENSIVE: Validate reasonable size (max 1MB per read)
            if (size > 1024 * 1024) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File read: size exceeds 1MB limit");
                return 0;
            }

            // DEFENSIVE: Check memory allocation
            uint8_t* buffer = (uint8_t*)kmalloc(size);
            if (!buffer) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                  "File read: failed to allocate buffer");
                return 0;
            }

            int bytes_read = fs_read(fd, buffer, size);
            if (bytes_read >= 0) {
                deck_complete(entry, DECK_PREFIX_STORAGE, buffer, RESULT_TYPE_KMALLOC);
                return 1;
            } else {
                kfree(buffer);
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_READ_FAILED,
                                  "File read failed");
                return 0;
            }
        }

        case EVENT_FILE_WRITE: {
            // Payload: [fd:4 bytes][size:8 bytes][data:...]
            int fd = *(int*)event->data;
            uint64_t size = *(uint64_t*)(event->data + 4);
            void* data = event->data + 12;

            // DEFENSIVE: Validate FD
            if (fd < 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File write: invalid file descriptor");
                return 0;
            }

            // DEFENSIVE: Validate size
            if (size == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File write: size is zero");
                return 0;
            }

            // DEFENSIVE: Validate size fits in event payload
            if (size > EVENT_DATA_SIZE - 12) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File write: data exceeds event payload limit");
                return 0;
            }

            // Real write
            int bytes_written = fs_write(fd, data, size);
            if (bytes_written >= 0) {
                // PRODUCTION: Sync to disk immediately after write!
                tagfs_sync();
                // DEFENSIVE: Check memory allocation
                int* result = (int*)kmalloc(sizeof(int));
                if (!result) {
                    deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                      "File write: failed to allocate result buffer");
                    return 0;
                }
                *result = bytes_written;
                deck_complete(entry, DECK_PREFIX_STORAGE, result, RESULT_TYPE_KMALLOC);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_WRITE_FAILED,
                                  "File write failed");
                return 0;
            }
        }

        case EVENT_FILE_STAT: {
            // DEFENSIVE: Validate path
            const char* path = (const char*)event->data;

            if (!path || path[0] == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File stat: path is NULL or empty");
                return 0;
            }

            // DEFENSIVE: Check memory allocation
            FileStat* stat_buf = (FileStat*)kmalloc(sizeof(FileStat));
            if (!stat_buf) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                  "File stat: failed to allocate stat buffer");
                return 0;
            }

            // Real stat implementation
            int ret = fs_stat(path, stat_buf);
            if (ret == 0) {
                // Success - return stat buffer
                deck_complete(entry, DECK_PREFIX_STORAGE, stat_buf, RESULT_TYPE_KMALLOC);
                return 1;
            } else {
                // Failed - free buffer and return error
                kfree(stat_buf);
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_FILE_NOT_FOUND,
                                  "File stat: file not found");
                return 0;
            }
        }

        // === TAGFS OPERATIONS ===
        case 215:  // Storage Deck range: FILE_CREATE_TAGGED (new range 200-299)
        case EVENT_FILE_CREATE_TAGGED: {  // Legacy support (old range 1-99)
            // Payload: [tag_count:4][tags:Tag[]...]
            uint32_t tag_count = *(uint32_t*)event->data;
            Tag* tags = (Tag*)(event->data + 4);

            // DEFENSIVE: Validate tag count
            if (tag_count == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Create tagged file: tag count is zero");
                return 0;
            }

            if (tag_count > TAGFS_MAX_TAGS_PER_FILE) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Create tagged file: tag count exceeds maximum");
                return 0;
            }

            uint64_t inode_id = tagfs_create_file(tags, tag_count);
            if (inode_id != TAGFS_INVALID_INODE) {
                // PRODUCTION: Sync to disk immediately to ensure persistence!
                tagfs_sync();
                deck_complete(entry, DECK_PREFIX_STORAGE, (void*)inode_id, RESULT_TYPE_VALUE);
                kprintf("[STORAGE] Event %lu: created file inode=%lu with %u tags (synced to disk)\n",
                        event->id, inode_id, tag_count);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_INODE_NOT_FOUND,
                                  "Create tagged file: TagFS operation failed");
                return 0;
            }
        }

        case EVENT_FILE_QUERY: {
            // Payload: [tag_count:4][operator:1][tags:Tag[]...]
            uint32_t tag_count = *(uint32_t*)event->data;
            uint8_t op = *(uint8_t*)(event->data + 4);
            Tag* tags = (Tag*)(event->data + 8);

            // DEFENSIVE: Validate tag count
            if (tag_count == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File query: tag count is zero");
                return 0;
            }

            if (tag_count > TAGFS_MAX_TAGS_PER_FILE) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File query: tag count exceeds maximum");
                return 0;
            }

            // DEFENSIVE: Validate operator (AND=0, OR=1)
            if (op > 1) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "File query: invalid query operator");
                return 0;
            }

            // DEFENSIVE: Check memory allocation
            uint64_t* result_inodes = (uint64_t*)kmalloc(256 * sizeof(uint64_t));
            if (!result_inodes) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                  "File query: failed to allocate result buffer");
                return 0;
            }

            TagQuery query;
            query.tags = tags;
            query.tag_count = tag_count;
            query.op = (QueryOperator)op;
            query.result_inodes = result_inodes;
            query.result_count = 0;
            query.result_capacity = 256;

            int success = tagfs_query(&query);
            if (success) {
                // Pass results back (will be in Response)
                deck_complete(entry, DECK_PREFIX_STORAGE, result_inodes, RESULT_TYPE_KMALLOC);
                kprintf("[STORAGE] Event %lu: query found %u files\n",
                        event->id, query.result_count);
                return 1;
            } else {
                kfree(result_inodes);
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_TAG_NOT_FOUND,
                                  "File query: TagFS query failed");
                return 0;
            }
        }

        case EVENT_FILE_TAG_ADD: {
            // Payload: [inode_id:8][tag:Tag]
            uint64_t inode_id = *(uint64_t*)event->data;
            Tag* tag = (Tag*)(event->data + 8);

            // DEFENSIVE: Validate inode ID
            if (inode_id == TAGFS_INVALID_INODE || inode_id == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Add tag: invalid inode ID");
                return 0;
            }

            // DEFENSIVE: Validate tag (key must not be empty)
            if (!tag || tag->key[0] == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Add tag: tag key is empty");
                return 0;
            }

            int success = tagfs_add_tag(inode_id, tag);
            if (success) {
                // PRODUCTION: Sync to disk after tag modification!
                tagfs_sync();
                deck_complete(entry, DECK_PREFIX_STORAGE, 0, RESULT_TYPE_NONE);
                kprintf("[STORAGE] Event %lu: added tag %s:%s to inode=%lu (synced to disk)\n",
                        event->id, tag->key, tag->value, inode_id);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_TAG_NOT_FOUND,
                                  "Add tag: TagFS operation failed");
                return 0;
            }
        }

        case EVENT_FILE_TAG_REMOVE: {
            // Payload: [inode_id:8][key:32]
            uint64_t inode_id = *(uint64_t*)event->data;
            const char* key = (const char*)(event->data + 8);

            // DEFENSIVE: Validate inode ID
            if (inode_id == TAGFS_INVALID_INODE || inode_id == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Remove tag: invalid inode ID");
                return 0;
            }

            // DEFENSIVE: Validate key
            if (!key || key[0] == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Remove tag: key is empty");
                return 0;
            }

            int success = tagfs_remove_tag(inode_id, key);
            if (success) {
                // PRODUCTION: Sync to disk after tag modification!
                tagfs_sync();
                deck_complete(entry, DECK_PREFIX_STORAGE, 0, RESULT_TYPE_NONE);
                kprintf("[STORAGE] Event %lu: removed tag '%s' from inode=%lu (synced to disk)\n",
                        event->id, key, inode_id);
                return 1;
            } else {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_TAG_NOT_FOUND,
                                  "Remove tag: TagFS operation failed");
                return 0;
            }
        }

        case EVENT_FILE_TAG_GET: {
            // Payload: [inode_id:8]
            uint64_t inode_id = *(uint64_t*)event->data;

            // DEFENSIVE: Validate inode ID
            if (inode_id == TAGFS_INVALID_INODE || inode_id == 0) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_INVALID_PARAMETER,
                                  "Get tags: invalid inode ID");
                return 0;
            }

            // DEFENSIVE: Check memory allocation
            Tag* tags = (Tag*)kmalloc(TAGFS_MAX_TAGS_PER_FILE * sizeof(Tag));
            if (!tags) {
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_OUT_OF_MEMORY,
                                  "Get tags: failed to allocate tag buffer");
                return 0;
            }

            uint32_t count = 0;
            int success = tagfs_get_tags(inode_id, tags, &count);
            if (success) {
                deck_complete(entry, DECK_PREFIX_STORAGE, tags, RESULT_TYPE_KMALLOC);
                kprintf("[STORAGE] Event %lu: retrieved %u tags from inode=%lu\n",
                        event->id, count, inode_id);
                return 1;
            } else {
                kfree(tags);
                deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_STORAGE_INODE_NOT_FOUND,
                                  "Get tags: TagFS operation failed");
                return 0;
            }
        }

        default:
            // PRODUCTION: Detailed error for unknown operations
            kprintf("[STORAGE] ERROR: Unknown/unimplemented event type %d\n", event->type);
            deck_error_detailed(entry, DECK_PREFIX_STORAGE, ERROR_NOT_IMPLEMENTED,
                              "Storage operation type not implemented");
            return 0;
    }
}

// ============================================================================
// INITIALIZATION & RUN
// ============================================================================

DeckContext storage_deck_context;

void storage_deck_init(void) {
    deck_init(&storage_deck_context, "Storage", DECK_PREFIX_STORAGE, storage_deck_process);

    // Initialize FD table
    memset(fd_table, 0, sizeof(fd_table));
    spinlock_init(&fd_table_lock);
    kprintf("[STORAGE] FD table initialized (%d slots)\n", MAX_OPEN_FILES);

    // NOTE: TagFS already initialized in main.c - don't reinitialize!
    kprintf("[STORAGE] TagFS ready (using globally initialized instance)\n");
}

int storage_deck_run_once(void) {
    return deck_run_once(&storage_deck_context);
}

void storage_deck_run(void) {
    deck_run(&storage_deck_context);
}
