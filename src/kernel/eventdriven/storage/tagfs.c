#include "tagfs.h"
#include "klib.h"  // Для kprintf, memset, strcmp, и т.д.
#include "ata.h"    // Для работы с диском

// ============================================================================
// GLOBAL STATE
// ============================================================================

TagFSContext global_tagfs;

// Простое хранилище блоков в памяти (fallback если диск недоступен)
#define TAGFS_MEM_BLOCKS 128  // 512MB виртуального диска (128 * 4KB)
static uint8_t tagfs_storage[TAGFS_MEM_BLOCKS][TAGFS_BLOCK_SIZE];

// Использовать реальный диск или память?
static int use_disk = 0;  // 0 = память, 1 = диск

// ============================================================================
// HELPER FUNCTIONS - Работа с битмапами
// ============================================================================

static inline void bitmap_set_bit(uint8_t* bitmap, uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear_bit(uint8_t* bitmap, uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test_bit(uint8_t* bitmap, uint64_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

// Найти первый свободный бит
static uint64_t bitmap_find_free(uint8_t* bitmap, uint64_t max_bits) {
    for (uint64_t i = 0; i < max_bits; i++) {
        if (!bitmap_test_bit(bitmap, i)) {
            return i;
        }
    }
    return (uint64_t)-1;  // Не найдено
}

// ============================================================================
// DISK I/O - Чтение/запись блоков с диска или из памяти
// ============================================================================

// Читать блок из диска или памяти
static int tagfs_read_block_raw(uint64_t block_num, uint8_t* buffer) {
    if (block_num >= TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Block %lu out of bounds\n", block_num);
        return -1;
    }

    if (use_disk) {
        // Читаем с реального диска
        return ata_read_block(block_num, buffer);
    } else {
        // Копируем из памяти
        memcpy(buffer, tagfs_storage[block_num], TAGFS_BLOCK_SIZE);
        return 0;
    }
}

// Записать блок на диск или в память
static int tagfs_write_block_raw(uint64_t block_num, const uint8_t* buffer) {
    if (block_num >= TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Block %lu out of bounds\n", block_num);
        return -1;
    }

    if (use_disk) {
        // Пишем на реальный диск
        return ata_write_block(block_num, buffer);
    } else {
        // Копируем в память
        memcpy(tagfs_storage[block_num], buffer, TAGFS_BLOCK_SIZE);
        return 0;
    }
}

// ============================================================================
// PERSISTENCE - Синхронизация метаданных с диском
// ============================================================================

// Записать superblock на диск
int tagfs_sync_superblock(void) {
    if (!use_disk) {
        return 0;  // В режиме памяти ничего не делаем
    }

    kprintf("[TAGFS] Syncing superblock to disk...\n");

    // Superblock всегда в блоке 0
    return tagfs_write_block_raw(0, (const uint8_t*)global_tagfs.superblock);
}

// Загрузить superblock с диска
int tagfs_load_superblock(void) {
    if (!use_disk) {
        return 0;  // В режиме памяти ничего не делаем
    }

    kprintf("[TAGFS] Loading superblock from disk...\n");

    // Читаем блок 0
    return tagfs_read_block_raw(0, (uint8_t*)tagfs_storage[0]);
}

// Записать inode table на диск
int tagfs_sync_inode_table(void) {
    if (!use_disk) {
        return 0;
    }

    kprintf("[TAGFS] Syncing inode table to disk...\n");

    // Inode table начинается с блока inode_table_block
    uint64_t start_block = global_tagfs.superblock->inode_table_block;
    uint64_t end_block = global_tagfs.superblock->tag_index_block;

    // Пишем все блоки inode table
    for (uint64_t block = start_block; block < end_block; block++) {
        if (tagfs_write_block_raw(block, tagfs_storage[block]) != 0) {
            kprintf("[TAGFS] ERROR: Failed to sync inode table block %lu\n", block);
            return -1;
        }
    }

    return 0;
}

// Загрузить inode table с диска
int tagfs_load_inode_table(void) {
    if (!use_disk) {
        return 0;
    }

    kprintf("[TAGFS] Loading inode table from disk...\n");

    uint64_t start_block = global_tagfs.superblock->inode_table_block;
    uint64_t end_block = global_tagfs.superblock->tag_index_block;

    for (uint64_t block = start_block; block < end_block; block++) {
        if (tagfs_read_block_raw(block, tagfs_storage[block]) != 0) {
            kprintf("[TAGFS] ERROR: Failed to load inode table block %lu\n", block);
            return -1;
        }
    }

    return 0;
}

// Полная синхронизация файловой системы с диском
int tagfs_sync(void) {
    if (!use_disk) {
        kprintf("[TAGFS] Sync skipped (memory mode)\n");
        return 0;
    }

    kprintf("[TAGFS] Full sync to disk...\n");

    // 1. Sync superblock
    if (tagfs_sync_superblock() != 0) {
        kprintf("[TAGFS] ERROR: Failed to sync superblock\n");
        return -1;
    }

    // 2. Sync inode table
    if (tagfs_sync_inode_table() != 0) {
        kprintf("[TAGFS] ERROR: Failed to sync inode table\n");
        return -1;
    }

    // 3. Sync data blocks (все блоки с данными)
    uint64_t data_start = global_tagfs.superblock->data_blocks_start;
    uint64_t total_blocks = global_tagfs.superblock->total_blocks;

    kprintf("[TAGFS] Syncing data blocks (%lu-%lu)...\n", data_start, total_blocks - 1);

    for (uint64_t block = data_start; block < total_blocks; block++) {
        // Проверяем bitmap - пишем только занятые блоки
        if (bitmap_test_bit(global_tagfs.block_bitmap, block)) {
            if (tagfs_write_block_raw(block, tagfs_storage[block]) != 0) {
                kprintf("[TAGFS] ERROR: Failed to sync data block %lu\n", block);
                return -1;
            }
        }
    }

    kprintf("[TAGFS] Sync complete!\n");
    return 0;
}

// ============================================================================
// DISK MODE CONTROL
// ============================================================================

// Включить/выключить режим работы с диском
void tagfs_set_disk_mode(int enable) {
    use_disk = enable;
    if (enable) {
        kprintf("[TAGFS] Disk mode ENABLED - using ATA driver\n");
    } else {
        kprintf("[TAGFS] Disk mode DISABLED - using memory storage\n");
    }
}

// ============================================================================
// BLOCK ALLOCATION - Аллокация блоков данных
// ============================================================================

static uint64_t tagfs_alloc_block(void) {
    uint64_t block = bitmap_find_free(global_tagfs.block_bitmap, global_tagfs.superblock->total_blocks);
    if (block != (uint64_t)-1) {
        // Bounds check to prevent out-of-bounds access
        if (block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Block allocation out of bounds (%lu >= %u)\n",
                    block, TAGFS_MEM_BLOCKS);
            return (uint64_t)-1;
        }

        bitmap_set_bit(global_tagfs.block_bitmap, block);
        global_tagfs.superblock->free_blocks--;

        // Очищаем блок
        memset(tagfs_storage[block], 0, TAGFS_BLOCK_SIZE);
    }
    return block;
}

static void tagfs_free_block(uint64_t block) {
    if (block < global_tagfs.superblock->total_blocks && block < TAGFS_MEM_BLOCKS) {
        bitmap_clear_bit(global_tagfs.block_bitmap, block);
        global_tagfs.superblock->free_blocks++;
    } else if (block >= TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Attempt to free invalid block %lu (>= %u)\n",
                block, TAGFS_MEM_BLOCKS);
    }
}

// ============================================================================
// INDIRECT BLOCKS - Поддержка файлов > 48KB
// ============================================================================

// Количество указателей в одном indirect блоке
#define PTRS_PER_BLOCK (TAGFS_BLOCK_SIZE / sizeof(uint64_t))  // 4096 / 8 = 512

// Получить блок данных по глобальному индексу (с учётом direct/indirect/double_indirect)
// Возвращает номер блока или 0 если блок не выделен
static uint64_t tagfs_get_block_by_index(FileInode* inode, uint64_t block_idx) {
    // Direct blocks: 0-11 (48KB)
    if (block_idx < 12) {
        return inode->direct_blocks[block_idx];
    }

    block_idx -= 12;

    // Single indirect: 12-523 (512 blocks, до 2MB)
    if (block_idx < PTRS_PER_BLOCK) {
        if (inode->indirect_block == 0) {
            return 0;  // Indirect block не выделен
        }

        // Проверка bounds
        if (inode->indirect_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid indirect_block %lu\n", inode->indirect_block);
            return 0;
        }

        // Читаем указатель из indirect блока
        uint64_t* indirect_table = (uint64_t*)tagfs_storage[inode->indirect_block];
        return indirect_table[block_idx];
    }

    block_idx -= PTRS_PER_BLOCK;

    // Double indirect: 524-262667 (512*512 blocks, до 1GB)
    if (block_idx < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode->double_indirect_block == 0) {
            return 0;  // Double indirect block не выделен
        }

        // Проверка bounds
        if (inode->double_indirect_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid double_indirect_block %lu\n", inode->double_indirect_block);
            return 0;
        }

        // Первый уровень
        uint64_t level1_idx = block_idx / PTRS_PER_BLOCK;
        uint64_t level2_idx = block_idx % PTRS_PER_BLOCK;

        uint64_t* level1_table = (uint64_t*)tagfs_storage[inode->double_indirect_block];
        uint64_t level2_block = level1_table[level1_idx];

        if (level2_block == 0) {
            return 0;  // Level2 block не выделен
        }

        if (level2_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid level2_block %lu\n", level2_block);
            return 0;
        }

        // Второй уровень
        uint64_t* level2_table = (uint64_t*)tagfs_storage[level2_block];
        return level2_table[level2_idx];
    }

    kprintf("[TAGFS] ERROR: Block index %lu too large (file too big)\n", block_idx + 12 + PTRS_PER_BLOCK);
    return 0;
}

// Выделить блок данных по глобальному индексу (создаёт indirect блоки при необходимости)
// Возвращает номер блока или (uint64_t)-1 при ошибке
static uint64_t tagfs_alloc_block_by_index(FileInode* inode, uint64_t block_idx) {
    // Direct blocks: 0-11
    if (block_idx < 12) {
        if (inode->direct_blocks[block_idx] == 0) {
            inode->direct_blocks[block_idx] = tagfs_alloc_block();
        }
        return inode->direct_blocks[block_idx];
    }

    block_idx -= 12;

    // Single indirect: 12-523
    if (block_idx < PTRS_PER_BLOCK) {
        // Выделяем indirect block если ещё нет
        if (inode->indirect_block == 0) {
            inode->indirect_block = tagfs_alloc_block();
            if (inode->indirect_block == (uint64_t)-1) {
                return (uint64_t)-1;
            }
        }

        if (inode->indirect_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid indirect_block %lu\n", inode->indirect_block);
            return (uint64_t)-1;
        }

        uint64_t* indirect_table = (uint64_t*)tagfs_storage[inode->indirect_block];

        // Выделяем data block если ещё нет
        if (indirect_table[block_idx] == 0) {
            indirect_table[block_idx] = tagfs_alloc_block();
        }

        return indirect_table[block_idx];
    }

    block_idx -= PTRS_PER_BLOCK;

    // Double indirect: 524-262667
    if (block_idx < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        // Выделяем double indirect block если ещё нет
        if (inode->double_indirect_block == 0) {
            inode->double_indirect_block = tagfs_alloc_block();
            if (inode->double_indirect_block == (uint64_t)-1) {
                return (uint64_t)-1;
            }
        }

        if (inode->double_indirect_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid double_indirect_block %lu\n", inode->double_indirect_block);
            return (uint64_t)-1;
        }

        uint64_t level1_idx = block_idx / PTRS_PER_BLOCK;
        uint64_t level2_idx = block_idx % PTRS_PER_BLOCK;

        uint64_t* level1_table = (uint64_t*)tagfs_storage[inode->double_indirect_block];

        // Выделяем level2 block если ещё нет
        if (level1_table[level1_idx] == 0) {
            level1_table[level1_idx] = tagfs_alloc_block();
            if (level1_table[level1_idx] == (uint64_t)-1) {
                return (uint64_t)-1;
            }
        }

        uint64_t level2_block = level1_table[level1_idx];

        if (level2_block >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid level2_block %lu\n", level2_block);
            return (uint64_t)-1;
        }

        uint64_t* level2_table = (uint64_t*)tagfs_storage[level2_block];

        // Выделяем data block если ещё нет
        if (level2_table[level2_idx] == 0) {
            level2_table[level2_idx] = tagfs_alloc_block();
        }

        return level2_table[level2_idx];
    }

    kprintf("[TAGFS] ERROR: Block index %lu too large (max file size exceeded)\n", block_idx + 12 + PTRS_PER_BLOCK);
    return (uint64_t)-1;
}

// Освободить все indirect блоки файла
static void tagfs_free_indirect_blocks(FileInode* inode) {
    // Освобождаем single indirect
    if (inode->indirect_block != 0 && inode->indirect_block < TAGFS_MEM_BLOCKS) {
        uint64_t* indirect_table = (uint64_t*)tagfs_storage[inode->indirect_block];

        // Освобождаем все data blocks
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
            if (indirect_table[i] != 0) {
                tagfs_free_block(indirect_table[i]);
            }
        }

        // Освобождаем сам indirect block
        tagfs_free_block(inode->indirect_block);
        inode->indirect_block = 0;
    }

    // Освобождаем double indirect
    if (inode->double_indirect_block != 0 && inode->double_indirect_block < TAGFS_MEM_BLOCKS) {
        uint64_t* level1_table = (uint64_t*)tagfs_storage[inode->double_indirect_block];

        // Проходим по всем level2 блокам
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++) {
            uint64_t level2_block = level1_table[i];

            if (level2_block != 0 && level2_block < TAGFS_MEM_BLOCKS) {
                uint64_t* level2_table = (uint64_t*)tagfs_storage[level2_block];

                // Освобождаем все data blocks
                for (uint32_t j = 0; j < PTRS_PER_BLOCK; j++) {
                    if (level2_table[j] != 0) {
                        tagfs_free_block(level2_table[j]);
                    }
                }

                // Освобождаем level2 block
                tagfs_free_block(level2_block);
            }
        }

        // Освобождаем level1 block
        tagfs_free_block(inode->double_indirect_block);
        inode->double_indirect_block = 0;
    }
}

// ============================================================================
// INODE ALLOCATION
// ============================================================================

static uint64_t tagfs_alloc_inode(void) {
    // Safely cap to prevent overflow
    uint64_t safe_total = global_tagfs.superblock->total_inodes;
    if (safe_total > TAGFS_MAX_FILES) {
        safe_total = TAGFS_MAX_FILES;
    }

    uint64_t inode_num = bitmap_find_free(global_tagfs.inode_bitmap, safe_total);
    if (inode_num != (uint64_t)-1) {
        bitmap_set_bit(global_tagfs.inode_bitmap, inode_num);
        global_tagfs.superblock->free_inodes--;

        // Генерируем уникальный ID (комбинация номера и timestamp)
        uint64_t inode_id = atomic_increment_u64(&global_tagfs.next_inode_id);
        return inode_id;
    }
    return TAGFS_INVALID_INODE;
}

static void tagfs_free_inode(uint64_t inode_id) {
    // Safely cap to prevent overflow
    uint32_t safe_max = global_tagfs.superblock->total_inodes;
    if (safe_max > TAGFS_MAX_FILES) {
        safe_max = TAGFS_MAX_FILES;
    }

    // Ищем inode по ID
    for (uint32_t i = 0; i < safe_max; i++) {
        if (global_tagfs.inode_table[i].inode_id == inode_id) {
            FileInode* inode = &global_tagfs.inode_table[i];

            // Освобождаем все direct blocks
            for (uint32_t j = 0; j < 12; j++) {
                if (inode->direct_blocks[j] != 0) {
                    tagfs_free_block(inode->direct_blocks[j]);
                }
            }

            // Освобождаем indirect и double indirect blocks
            tagfs_free_indirect_blocks(inode);

            bitmap_clear_bit(global_tagfs.inode_bitmap, i);
            global_tagfs.superblock->free_inodes++;
            memset(inode, 0, sizeof(FileInode));
            break;
        }
    }
}

// ============================================================================
// TAG UTILITIES
// ============================================================================

Tag tagfs_tag_from_string(const char* str) {
    Tag tag;
    memset(&tag, 0, sizeof(Tag));

    // Парсим строку формата "key:value"
    const char* colon = str;
    while (*colon && *colon != ':') colon++;

    if (*colon == ':') {
        uint32_t key_len = colon - str;
        if (key_len >= TAGFS_TAG_KEY_SIZE) key_len = TAGFS_TAG_KEY_SIZE - 1;

        memcpy(tag.key, str, key_len);
        tag.key[key_len] = '\0';

        const char* value = colon + 1;
        uint32_t value_len = 0;
        while (value[value_len] && value_len < TAGFS_TAG_VALUE_SIZE - 1) {
            value_len++;
        }
        memcpy(tag.value, value, value_len);
        tag.value[value_len] = '\0';
    }

    return tag;
}

int tagfs_tag_equal(const Tag* a, const Tag* b) {
    return (strcmp(a->key, b->key) == 0) && (strcmp(a->value, b->value) == 0);
}

// ============================================================================
// INITIALIZATION
// ============================================================================

void tagfs_init(void) {
    kprintf("[TAGFS] Initializing tag-based filesystem...\n");

    memset(&global_tagfs, 0, sizeof(TagFSContext));

    // Allocate superblock in memory
    global_tagfs.superblock = (TagFSSuperblock*)tagfs_storage[0];

    // Проверяем наличие ATA диска
    int disk_available = 0;
    if (ata_primary_master.exists) {
        kprintf("[TAGFS] ATA disk detected: %s (%lu MB)\n",
                ata_primary_master.model, ata_primary_master.size_mb);
        disk_available = 1;
    }

    // Если диск доступен, пытаемся загрузить ФС с диска
    int loaded_from_disk = 0;
    if (disk_available) {
        tagfs_set_disk_mode(1);  // Включаем режим диска

        kprintf("[TAGFS] Attempting to load filesystem from disk...\n");
        if (tagfs_load_superblock() == 0) {
            if (global_tagfs.superblock->magic == TAGFS_MAGIC) {
                kprintf("[TAGFS] Valid superblock found on disk (version %u)\n",
                        global_tagfs.superblock->version);

                // Загружаем таблицу inodes
                if (tagfs_load_inode_table() == 0) {
                    kprintf("[TAGFS] Successfully loaded filesystem from disk!\n");
                    loaded_from_disk = 1;
                } else {
                    kprintf("[TAGFS] Failed to load inode table from disk\n");
                }
            } else {
                kprintf("[TAGFS] No valid filesystem on disk (magic=0x%lx)\n",
                        global_tagfs.superblock->magic);
            }
        } else {
            kprintf("[TAGFS] Failed to read superblock from disk\n");
        }
    }

    // Если не загрузили с диска, форматируем
    if (!loaded_from_disk) {
        kprintf("[TAGFS] Creating new filesystem...\n");
        tagfs_format(TAGFS_MEM_BLOCKS);

        // Если диск доступен, записываем новую ФС на диск
        if (disk_available) {
            kprintf("[TAGFS] Writing new filesystem to disk...\n");
            if (tagfs_sync() == 0) {
                kprintf("[TAGFS] Filesystem synced to disk successfully!\n");
            } else {
                kprintf("[TAGFS] WARNING: Failed to sync filesystem to disk\n");
            }
        }
    }

    // Validate superblock values to prevent out-of-bounds access
    if (global_tagfs.superblock->inode_table_block >= TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Invalid inode_table_block (%lu >= %u), reformatting...\n",
                global_tagfs.superblock->inode_table_block, TAGFS_MEM_BLOCKS);
        tagfs_format(TAGFS_MEM_BLOCKS);
    }

    if (global_tagfs.superblock->data_blocks_start > TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Invalid data_blocks_start (%lu > %u), reformatting...\n",
                global_tagfs.superblock->data_blocks_start, TAGFS_MEM_BLOCKS);
        tagfs_format(TAGFS_MEM_BLOCKS);
    }

    if (global_tagfs.superblock->total_blocks > TAGFS_MEM_BLOCKS) {
        kprintf("[TAGFS] ERROR: Invalid total_blocks (%lu > %u), reformatting...\n",
                global_tagfs.superblock->total_blocks, TAGFS_MEM_BLOCKS);
        tagfs_format(TAGFS_MEM_BLOCKS);
    }

    // Validate total_inodes doesn't exceed available space
    uint64_t available_inode_blocks = 0;
    if (global_tagfs.superblock->tag_index_block > global_tagfs.superblock->inode_table_block) {
        available_inode_blocks = global_tagfs.superblock->tag_index_block - global_tagfs.superblock->inode_table_block;
    }
    uint64_t max_possible_inodes = (available_inode_blocks * TAGFS_BLOCK_SIZE) / TAGFS_INODE_SIZE;

    if (global_tagfs.superblock->total_inodes > max_possible_inodes) {
        kprintf("[TAGFS] ERROR: Invalid total_inodes (%lu > max %lu), reformatting...\n",
                global_tagfs.superblock->total_inodes, max_possible_inodes);
        tagfs_format(TAGFS_MEM_BLOCKS);
    }

    // Setup inode table (starts at block 1)
    global_tagfs.inode_table = (FileInode*)tagfs_storage[global_tagfs.superblock->inode_table_block];

    // Allocate bitmaps
    uint64_t block_bitmap_size = (global_tagfs.superblock->total_blocks + 7) / 8;
    uint64_t inode_bitmap_size = (global_tagfs.superblock->total_inodes + 7) / 8;

    kprintf("[TAGFS] Allocating bitmaps: block_bitmap=%lu bytes, inode_bitmap=%lu bytes\n",
            block_bitmap_size, inode_bitmap_size);

    global_tagfs.block_bitmap = (uint8_t*)kmalloc(block_bitmap_size);
    if (!global_tagfs.block_bitmap) {
        panic("[TAGFS] FATAL: Failed to allocate block_bitmap (%lu bytes)", block_bitmap_size);
    }

    global_tagfs.inode_bitmap = (uint8_t*)kmalloc(inode_bitmap_size);
    if (!global_tagfs.inode_bitmap) {
        kfree(global_tagfs.block_bitmap);
        panic("[TAGFS] FATAL: Failed to allocate inode_bitmap (%lu bytes)", inode_bitmap_size);
    }

    memset(global_tagfs.block_bitmap, 0, block_bitmap_size);
    memset(global_tagfs.inode_bitmap, 0, inode_bitmap_size);

    // Mark reserved blocks as used
    for (uint64_t i = 0; i < global_tagfs.superblock->data_blocks_start; i++) {
        bitmap_set_bit(global_tagfs.block_bitmap, i);
    }

    // Initialize tag index
    global_tagfs.tag_index.entry_count = 0;
    global_tagfs.tag_index.capacity = TAGFS_MAX_TAG_INDEX;

    // Rebuild index from existing files
    tagfs_index_rebuild();

    global_tagfs.next_inode_id = 1;

    // Initialize spinlock for thread-safe access
    spinlock_init(&global_tagfs.lock);

    kprintf("[TAGFS] Initialized: %lu blocks (%lu free), %lu inodes (%lu free)\n",
            global_tagfs.superblock->total_blocks,
            global_tagfs.superblock->free_blocks,
            global_tagfs.superblock->total_inodes,
            global_tagfs.superblock->free_inodes);
}

void tagfs_format(uint64_t total_blocks) {
    kprintf("[TAGFS] Formatting filesystem with %lu blocks...\n", total_blocks);

    // Clear storage
    memset(tagfs_storage, 0, sizeof(tagfs_storage));

    // Initialize superblock
    TagFSSuperblock* sb = (TagFSSuperblock*)tagfs_storage[0];
    sb->magic = TAGFS_MAGIC;
    sb->version = TAGFS_VERSION;
    sb->block_size = TAGFS_BLOCK_SIZE;
    sb->total_blocks = total_blocks;

    // Layout: [SB][Inode Table][Tag Index][Data Blocks]
    sb->inode_table_block = 1;

    // Reserve space for tag index (64 blocks)
    uint64_t tag_index_blocks = 64;

    // Calculate available blocks for inodes
    // total_blocks - superblock(1) - tag_index(64) - minimum_data_blocks(10)
    uint64_t available_for_inodes = 0;
    if (total_blocks > (1 + tag_index_blocks + 10)) {
        available_for_inodes = total_blocks - 1 - tag_index_blocks - 10;
    } else {
        kprintf("[TAGFS] ERROR: Not enough blocks for filesystem!\n");
        available_for_inodes = 1;  // Minimum
    }

    // Calculate how many inodes fit in available blocks
    // Each inode is 512 bytes, each block is 4096 bytes (8 inodes per block)
    uint64_t max_inodes = (available_for_inodes * TAGFS_BLOCK_SIZE) / TAGFS_INODE_SIZE;

    // Cap at TAGFS_MAX_FILES to avoid overflow
    if (max_inodes > TAGFS_MAX_FILES) {
        max_inodes = TAGFS_MAX_FILES;
    }

    sb->total_inodes = max_inodes;

    // Calculate actual blocks needed for inodes
    uint64_t inode_blocks = (max_inodes * TAGFS_INODE_SIZE + TAGFS_BLOCK_SIZE - 1) / TAGFS_BLOCK_SIZE;

    sb->tag_index_block = sb->inode_table_block + inode_blocks;
    sb->data_blocks_start = sb->tag_index_block + tag_index_blocks;

    // Ensure data_blocks_start doesn't exceed total_blocks
    if (sb->data_blocks_start > total_blocks) {
        kprintf("[TAGFS] ERROR: Filesystem layout exceeds available blocks!\n");
        sb->data_blocks_start = total_blocks;
        sb->free_blocks = 0;
    } else {
        sb->free_blocks = total_blocks - sb->data_blocks_start;
    }

    sb->free_inodes = max_inodes;

    kprintf("[TAGFS] Format complete: inodes=%lu (in %lu blocks), tag_index=%lu, data_start=%lu\n",
            max_inodes, inode_blocks, sb->tag_index_block, sb->data_blocks_start);
}

// ============================================================================
// FILE OPERATIONS
// ============================================================================

uint64_t tagfs_create_file(Tag* tags, uint32_t tag_count) {
    if (tag_count > TAGFS_MAX_TAGS_PER_FILE) {
        kprintf("[TAGFS] Error: too many tags (%u > %u)\n", tag_count, TAGFS_MAX_TAGS_PER_FILE);
        return TAGFS_INVALID_INODE;
    }

    spin_lock(&global_tagfs.lock);

    // Allocate inode
    uint64_t inode_id = tagfs_alloc_inode();
    if (inode_id == TAGFS_INVALID_INODE) {
        spin_unlock(&global_tagfs.lock);
        kprintf("[TAGFS] Error: no free inodes\n");
        return TAGFS_INVALID_INODE;
    }

    // Find inode slot
    // Safely cap to prevent overflow
    uint32_t safe_max = global_tagfs.superblock->total_inodes;
    if (safe_max > TAGFS_MAX_FILES) {
        safe_max = TAGFS_MAX_FILES;
    }

    FileInode* inode = NULL;
    for (uint32_t i = 0; i < safe_max; i++) {
        if (global_tagfs.inode_table[i].inode_id == 0) {
            inode = &global_tagfs.inode_table[i];
            break;
        }
    }

    if (!inode) {
        spin_unlock(&global_tagfs.lock);
        kprintf("[TAGFS] Error: inode table full\n");
        return TAGFS_INVALID_INODE;
    }

    // Initialize inode
    memset(inode, 0, sizeof(FileInode));
    inode->inode_id = inode_id;
    inode->size = 0;
    inode->creation_time = rdtsc();
    inode->modification_time = inode->creation_time;
    inode->tag_count = tag_count;

    // Copy tags
    for (uint32_t i = 0; i < tag_count; i++) {
        inode->tags[i] = tags[i];
    }

    // Add to tag index
    tagfs_index_add_file(inode_id, tags, tag_count);

    atomic_increment_u64(&global_tagfs.files_created);

    spin_unlock(&global_tagfs.lock);

    kprintf("[TAGFS] Created file inode=%lu with %u tags\n", inode_id, tag_count);
    return inode_id;
}

int tagfs_delete_file(uint64_t inode_id) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return 0;
    }

    // В TagFS удаление = добавление тега "trashed:true"
    // Реальное удаление произойдет через garbage collector
    Tag trash_tag = tagfs_tag_from_string("trashed:true");
    int result = tagfs_add_tag(inode_id, &trash_tag);

    if (result) {
        atomic_increment_u64(&global_tagfs.files_deleted);
        kprintf("[TAGFS] File inode=%lu marked as trashed\n", inode_id);
    }

    return result;
}

FileInode* tagfs_get_inode(uint64_t inode_id) {
    // Safely cap to prevent overflow
    uint32_t safe_max = global_tagfs.superblock->total_inodes;
    if (safe_max > TAGFS_MAX_FILES) {
        safe_max = TAGFS_MAX_FILES;
    }

    for (uint32_t i = 0; i < safe_max; i++) {
        if (global_tagfs.inode_table[i].inode_id == inode_id) {
            return &global_tagfs.inode_table[i];
        }
    }
    return NULL;
}

int tagfs_read_file(uint64_t inode_id, uint64_t offset, uint8_t* buffer, uint64_t size) {
    spin_lock(&global_tagfs.lock);

    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        spin_unlock(&global_tagfs.lock);
        return -1;
    }

    if (offset >= inode->size) {
        spin_unlock(&global_tagfs.lock);
        return 0;  // EOF
    }

    if (offset + size > inode->size) {
        size = inode->size - offset;
    }

    uint64_t bytes_read = 0;
    uint64_t current_offset = offset;

    // Полная реализация с поддержкой indirect blocks
    while (bytes_read < size && current_offset < inode->size) {
        uint64_t block_idx = current_offset / TAGFS_BLOCK_SIZE;
        uint64_t block_offset = current_offset % TAGFS_BLOCK_SIZE;

        // Получаем блок (поддерживает direct/indirect/double_indirect)
        uint64_t block_num = tagfs_get_block_by_index(inode, block_idx);

        if (block_num == 0) {
            // Sparse file - читаем нули
            uint64_t to_read = TAGFS_BLOCK_SIZE - block_offset;
            if (to_read > size - bytes_read) {
                to_read = size - bytes_read;
            }
            memset(buffer + bytes_read, 0, to_read);
            bytes_read += to_read;
            current_offset += to_read;
            continue;
        }

        // Bounds check
        if (block_num >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid block number %lu in read (>= %u)\n",
                    block_num, TAGFS_MEM_BLOCKS);
            break;
        }

        uint64_t to_read = TAGFS_BLOCK_SIZE - block_offset;
        if (to_read > size - bytes_read) {
            to_read = size - bytes_read;
        }

        memcpy(buffer + bytes_read, tagfs_storage[block_num] + block_offset, to_read);
        bytes_read += to_read;
        current_offset += to_read;
    }

    spin_unlock(&global_tagfs.lock);
    return bytes_read;
}

int tagfs_write_file(uint64_t inode_id, uint64_t offset, const uint8_t* buffer, uint64_t size) {
    spin_lock(&global_tagfs.lock);

    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        spin_unlock(&global_tagfs.lock);
        return -1;
    }

    uint64_t bytes_written = 0;
    uint64_t current_offset = offset;

    // Полная реализация с поддержкой indirect blocks
    while (bytes_written < size) {
        uint64_t block_idx = current_offset / TAGFS_BLOCK_SIZE;
        uint64_t block_offset = current_offset % TAGFS_BLOCK_SIZE;

        // Выделяем блок если нужно (поддерживает direct/indirect/double_indirect)
        uint64_t block_num = tagfs_alloc_block_by_index(inode, block_idx);

        if (block_num == (uint64_t)-1) {
            kprintf("[TAGFS] Error: failed to allocate block at index %lu\n", block_idx);
            break;
        }

        // Bounds check
        if (block_num >= TAGFS_MEM_BLOCKS) {
            kprintf("[TAGFS] ERROR: Invalid block number %lu in write (>= %u)\n",
                    block_num, TAGFS_MEM_BLOCKS);
            break;
        }

        uint64_t to_write = TAGFS_BLOCK_SIZE - block_offset;
        if (to_write > size - bytes_written) {
            to_write = size - bytes_written;
        }

        memcpy(tagfs_storage[block_num] + block_offset, buffer + bytes_written, to_write);
        bytes_written += to_write;
        current_offset += to_write;
    }

    // Update size and modification time
    if (current_offset > inode->size) {
        inode->size = current_offset;
    }
    inode->modification_time = rdtsc();

    spin_unlock(&global_tagfs.lock);
    return bytes_written;
}

// ============================================================================
// TAG OPERATIONS
// ============================================================================

int tagfs_add_tag(uint64_t inode_id, const Tag* tag) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return 0;
    }

    if (inode->tag_count >= TAGFS_MAX_TAGS_PER_FILE) {
        kprintf("[TAGFS] Error: max tags reached for inode=%lu\n", inode_id);
        return 0;
    }

    // Check if tag already exists
    for (uint32_t i = 0; i < inode->tag_count; i++) {
        if (tagfs_tag_equal(&inode->tags[i], tag)) {
            kprintf("[TAGFS] Tag already exists on inode=%lu\n", inode_id);
            return 1;  // Already exists - success
        }
    }

    // Add tag
    inode->tags[inode->tag_count] = *tag;
    inode->tag_count++;
    inode->modification_time = rdtsc();

    // Update index
    tagfs_index_add_file(inode_id, tag, 1);

    atomic_increment_u64(&global_tagfs.tags_added);
    return 1;
}

int tagfs_remove_tag(uint64_t inode_id, const char* key) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return 0;
    }

    // Find and remove tag
    for (uint32_t i = 0; i < inode->tag_count; i++) {
        if (strcmp(inode->tags[i].key, key) == 0) {
            // Shift remaining tags
            for (uint32_t j = i; j < inode->tag_count - 1; j++) {
                inode->tags[j] = inode->tags[j + 1];
            }
            inode->tag_count--;
            inode->modification_time = rdtsc();

            atomic_increment_u64(&global_tagfs.tags_removed);

            // NOTE: Индекс не обновляем - произойдет при следующем rebuild
            return 1;
        }
    }

    return 0;
}

int tagfs_get_tags(uint64_t inode_id, Tag* tags_out, uint32_t* count_out) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return 0;
    }

    for (uint32_t i = 0; i < inode->tag_count; i++) {
        tags_out[i] = inode->tags[i];
    }
    *count_out = inode->tag_count;

    return 1;
}

int tagfs_file_has_tag(uint64_t inode_id, const Tag* tag) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return 0;
    }

    for (uint32_t i = 0; i < inode->tag_count; i++) {
        if (tagfs_tag_equal(&inode->tags[i], tag)) {
            return 1;
        }
    }
    return 0;
}

// ============================================================================
// TAG INDEX MANAGEMENT
// ============================================================================

void tagfs_index_add_file(uint64_t inode_id, const Tag* tags, uint32_t tag_count) {
    for (uint32_t i = 0; i < tag_count; i++) {
        const Tag* tag = &tags[i];

        // Find or create index entry for this tag
        TagIndexEntry* entry = NULL;
        for (uint32_t j = 0; j < global_tagfs.tag_index.entry_count; j++) {
            if (tagfs_tag_equal(&global_tagfs.tag_index.entries[j].tag, tag)) {
                entry = &global_tagfs.tag_index.entries[j];
                break;
            }
        }

        if (!entry) {
            // Create new entry
            if (global_tagfs.tag_index.entry_count >= TAGFS_MAX_TAG_INDEX) {
                kprintf("[TAGFS] Warning: tag index full\n");
                continue;
            }

            entry = &global_tagfs.tag_index.entries[global_tagfs.tag_index.entry_count];
            global_tagfs.tag_index.entry_count++;

            entry->tag = *tag;
            entry->file_count = 0;
            entry->capacity = 16;  // Start small
            entry->inode_ids = (uint64_t*)kmalloc(entry->capacity * sizeof(uint64_t));

            if (!entry->inode_ids) {
                kprintf("[TAGFS] ERROR: Failed to allocate inode_ids array for tag %s:%s\n",
                        tag->key, tag->value);
                global_tagfs.tag_index.entry_count--;  // Rollback
                continue;
            }
        }

        // Check if file already in list
        int found = 0;
        for (uint32_t j = 0; j < entry->file_count; j++) {
            if (entry->inode_ids[j] == inode_id) {
                found = 1;
                break;
            }
        }

        if (!found) {
            // Resize if needed
            if (entry->file_count >= entry->capacity) {
                uint32_t new_capacity = entry->capacity * 2;
                uint64_t* new_array = (uint64_t*)kmalloc(new_capacity * sizeof(uint64_t));

                if (!new_array) {
                    kprintf("[TAGFS] ERROR: Failed to resize inode_ids array (capacity %u -> %u)\n",
                            entry->capacity, new_capacity);
                    continue;  // Skip adding this file to avoid corruption
                }

                memcpy(new_array, entry->inode_ids, entry->file_count * sizeof(uint64_t));
                kfree(entry->inode_ids);
                entry->inode_ids = new_array;
                entry->capacity = new_capacity;
            }

            entry->inode_ids[entry->file_count] = inode_id;
            entry->file_count++;
        }
    }
}

void tagfs_index_remove_file(uint64_t inode_id) {
    // Remove file from all tag index entries
    for (uint32_t i = 0; i < global_tagfs.tag_index.entry_count; i++) {
        TagIndexEntry* entry = &global_tagfs.tag_index.entries[i];

        for (uint32_t j = 0; j < entry->file_count; j++) {
            if (entry->inode_ids[j] == inode_id) {
                // Shift remaining entries
                for (uint32_t k = j; k < entry->file_count - 1; k++) {
                    entry->inode_ids[k] = entry->inode_ids[k + 1];
                }
                entry->file_count--;
                break;
            }
        }
    }
}

void tagfs_index_rebuild(void) {
    kprintf("[TAGFS] Rebuilding tag index...\n");

    // Clear existing index
    for (uint32_t i = 0; i < global_tagfs.tag_index.entry_count; i++) {
        if (global_tagfs.tag_index.entries[i].inode_ids) {
            kfree(global_tagfs.tag_index.entries[i].inode_ids);
        }
    }
    global_tagfs.tag_index.entry_count = 0;

    // OPTIMIZATION: Skip scanning for freshly formatted filesystem
    if (global_tagfs.superblock->free_inodes == global_tagfs.superblock->total_inodes) {
        kprintf("[TAGFS] Filesystem is empty, skipping inode scan\n");
        kprintf("[TAGFS] Index rebuilt: 0 unique tags\n");
        return;
    }

    // Calculate maximum safe inodes we can access
    // inode_table starts at block inode_table_block
    // Each block is 4096 bytes, each inode is 512 bytes (8 inodes per block)
    uint64_t inode_start_block = global_tagfs.superblock->inode_table_block;
    uint64_t inode_end_block = global_tagfs.superblock->tag_index_block;

    // Bounds check to prevent accessing memory outside tagfs_storage
    if (inode_end_block > TAGFS_MEM_BLOCKS) {
        inode_end_block = TAGFS_MEM_BLOCKS;
        kprintf("[TAGFS] WARNING: Limiting inode scan to %lu blocks\n", inode_end_block);
    }

    uint64_t available_inode_blocks = inode_end_block - inode_start_block;
    uint64_t max_safe_inodes = (available_inode_blocks * TAGFS_BLOCK_SIZE) / TAGFS_INODE_SIZE;

    // Use the smaller of declared total_inodes or what fits in memory
    uint64_t inodes_to_scan = global_tagfs.superblock->total_inodes;
    if (inodes_to_scan > max_safe_inodes) {
        kprintf("[TAGFS] WARNING: total_inodes=%lu exceeds safe limit %lu, capping scan\n",
                inodes_to_scan, max_safe_inodes);
        inodes_to_scan = max_safe_inodes;
    }

    kprintf("[TAGFS] Scanning %lu inodes for index rebuild...\n", inodes_to_scan);

    // Scan all inodes and rebuild
    uint32_t scanned = 0;
    for (uint64_t i = 0; i < inodes_to_scan && scanned < 10000; i++) {
        FileInode* inode = &global_tagfs.inode_table[i];
        if (inode->inode_id != 0) {
            tagfs_index_add_file(inode->inode_id, inode->tags, inode->tag_count);
        }
        scanned++;
    }

    kprintf("[TAGFS] Index rebuilt: %u unique tags\n", global_tagfs.tag_index.entry_count);
}

// ============================================================================
// QUERY OPERATIONS
// ============================================================================

int tagfs_query_single(const Tag* tag, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results) {
    *count_out = 0;

    // Find tag in index
    for (uint32_t i = 0; i < global_tagfs.tag_index.entry_count; i++) {
        TagIndexEntry* entry = &global_tagfs.tag_index.entries[i];
        if (tagfs_tag_equal(&entry->tag, tag)) {
            uint32_t to_copy = entry->file_count;
            if (to_copy > max_results) {
                to_copy = max_results;
            }

            memcpy(result_inodes, entry->inode_ids, to_copy * sizeof(uint64_t));
            *count_out = to_copy;

            atomic_increment_u64(&global_tagfs.queries_executed);
            return 1;
        }
    }

    return 0;  // Tag not found
}

int tagfs_query(TagQuery* query) {
    if (query->tag_count == 0) {
        return 0;
    }

    query->result_count = 0;

    if (query->op == QUERY_OP_AND) {
        // AND: файл должен иметь ВСЕ теги
        // Стратегия: начинаем с первого тега, затем фильтруем

        uint64_t* candidates = (uint64_t*)kmalloc(TAGFS_MAX_FILES * sizeof(uint64_t));
        if (!candidates) {
            kprintf("[TAGFS] ERROR: Failed to allocate candidates array for AND query\n");
            return 0;
        }

        uint32_t candidate_count = 0;

        // Get files with first tag
        tagfs_query_single(&query->tags[0], candidates, &candidate_count, TAGFS_MAX_FILES);

        // Filter by remaining tags
        for (uint32_t i = 1; i < query->tag_count && candidate_count > 0; i++) {
            uint32_t filtered_count = 0;
            for (uint32_t j = 0; j < candidate_count; j++) {
                if (tagfs_file_has_tag(candidates[j], &query->tags[i])) {
                    candidates[filtered_count++] = candidates[j];
                }
            }
            candidate_count = filtered_count;
        }

        // Copy results
        uint32_t to_copy = candidate_count;
        if (to_copy > query->result_capacity) {
            to_copy = query->result_capacity;
        }
        memcpy(query->result_inodes, candidates, to_copy * sizeof(uint64_t));
        query->result_count = to_copy;

        kfree(candidates);

    } else if (query->op == QUERY_OP_OR) {
        // OR: файл должен иметь ХОТЯ БЫ ОДИН тег

        uint8_t* seen = (uint8_t*)kmalloc(TAGFS_MAX_FILES);
        if (!seen) {
            kprintf("[TAGFS] ERROR: Failed to allocate seen array for OR query\n");
            return 0;
        }

        memset(seen, 0, TAGFS_MAX_FILES);

        uint32_t result_count = 0;

        for (uint32_t i = 0; i < query->tag_count; i++) {
            uint64_t temp_results[256];
            uint32_t temp_count = 0;

            tagfs_query_single(&query->tags[i], temp_results, &temp_count, 256);

            for (uint32_t j = 0; j < temp_count; j++) {
                uint64_t inode_id = temp_results[j];
                if (inode_id < TAGFS_MAX_FILES && !seen[inode_id]) {
                    seen[inode_id] = 1;
                    if (result_count < query->result_capacity) {
                        query->result_inodes[result_count++] = inode_id;
                    }
                }
            }
        }

        query->result_count = result_count;
        kfree(seen);
    }

    atomic_increment_u64(&global_tagfs.queries_executed);
    return 1;
}

// Convenient wrappers
int tagfs_find_by_type(const char* type, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results) {
    char tag_str[128];
    ksnprintf(tag_str, sizeof(tag_str), "type:%s", type);
    Tag tag = tagfs_tag_from_string(tag_str);
    return tagfs_query_single(&tag, result_inodes, count_out, max_results);
}

int tagfs_find_by_date(const char* date, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results) {
    char tag_str[128];
    ksnprintf(tag_str, sizeof(tag_str), "date:%s", date);
    Tag tag = tagfs_tag_from_string(tag_str);
    return tagfs_query_single(&tag, result_inodes, count_out, max_results);
}

int tagfs_find_not_trashed(uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results) {
    // Возвращаем все файлы БЕЗ тега "trashed:true"
    uint32_t result_count = 0;

    // Safely cap iteration to prevent buffer overflow
    uint32_t safe_max_inodes = global_tagfs.superblock->total_inodes;
    if (safe_max_inodes > TAGFS_MAX_FILES) {
        safe_max_inodes = TAGFS_MAX_FILES;
    }

    for (uint32_t i = 0; i < safe_max_inodes && result_count < max_results; i++) {
        FileInode* inode = &global_tagfs.inode_table[i];
        if (inode->inode_id != 0) {
            Tag trash_tag = tagfs_tag_from_string("trashed:true");
            if (!tagfs_file_has_tag(inode->inode_id, &trash_tag)) {
                result_inodes[result_count++] = inode->inode_id;
            }
        }
    }

    *count_out = result_count;
    return 1;
}

// ============================================================================
// STATISTICS & DEBUGGING
// ============================================================================

void tagfs_print_stats(void) {
    kprintf("[TAGFS] === Statistics ===\n");
    kprintf("  Files created:   %lu\n", global_tagfs.files_created);
    kprintf("  Files deleted:   %lu\n", global_tagfs.files_deleted);
    kprintf("  Queries executed: %lu\n", global_tagfs.queries_executed);
    kprintf("  Tags added:      %lu\n", global_tagfs.tags_added);
    kprintf("  Tags removed:    %lu\n", global_tagfs.tags_removed);
    kprintf("  Unique tags:     %u\n", global_tagfs.tag_index.entry_count);
    kprintf("  Free blocks:     %lu / %lu\n",
            global_tagfs.superblock->free_blocks,
            global_tagfs.superblock->total_blocks);
    kprintf("  Free inodes:     %lu / %lu\n",
            global_tagfs.superblock->free_inodes,
            global_tagfs.superblock->total_inodes);
}

void tagfs_print_file_info(uint64_t inode_id) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] File not found: inode=%lu\n", inode_id);
        return;
    }

    kprintf("[TAGFS] === File Info (inode=%lu) ===\n", inode_id);
    kprintf("  Size:         %lu bytes\n", inode->size);
    kprintf("  Created:      %lu\n", inode->creation_time);
    kprintf("  Modified:     %lu\n", inode->modification_time);
    kprintf("  Tags (%u):\n", inode->tag_count);

    for (uint32_t i = 0; i < inode->tag_count; i++) {
        kprintf("    %s:%s\n", inode->tags[i].key, inode->tags[i].value);
    }
}

void tagfs_print_tag_index(void) {
    kprintf("[TAGFS] === Tag Index (%u entries) ===\n", global_tagfs.tag_index.entry_count);

    for (uint32_t i = 0; i < global_tagfs.tag_index.entry_count; i++) {
        TagIndexEntry* entry = &global_tagfs.tag_index.entries[i];
        kprintf("  %s:%s -> %u files\n",
                entry->tag.key,
                entry->tag.value,
                entry->file_count);
    }
}

// ============================================================================
// USER CONTEXT OPERATIONS - NEW!
// ============================================================================

// Установить контекст пользователя
int tagfs_context_set(Tag* tags, uint32_t tag_count) {
    if (tag_count > TAGFS_MAX_CONTEXT_TAGS) {
        kprintf("[TAGFS] ERROR: Too many context tags (%u > %u)\n",
                tag_count, TAGFS_MAX_CONTEXT_TAGS);
        return -1;
    }

    spin_lock(&global_tagfs.lock);

    // Копируем теги
    for (uint32_t i = 0; i < tag_count; i++) {
        global_tagfs.user_context.tags[i] = tags[i];
    }
    global_tagfs.user_context.tag_count = tag_count;
    global_tagfs.user_context.enabled = true;

    spin_unlock(&global_tagfs.lock);

    kprintf("[TAGFS] Context set: %u tags\n", tag_count);
    for (uint32_t i = 0; i < tag_count; i++) {
        kprintf("  - %s:%s\n", tags[i].key, tags[i].value);
    }

    return 0;
}

// Очистить контекст
void tagfs_context_clear(void) {
    spin_lock(&global_tagfs.lock);
    global_tagfs.user_context.enabled = false;
    global_tagfs.user_context.tag_count = 0;
    spin_unlock(&global_tagfs.lock);

    kprintf("[TAGFS] Context cleared (showing all files)\n");
}

// Получить текущий контекст
TagFSUserContext* tagfs_context_get(void) {
    return &global_tagfs.user_context;
}

// Проверить, подходит ли файл под текущий контекст
bool tagfs_context_matches(uint64_t inode_id) {
    if (!global_tagfs.user_context.enabled) {
        return true;  // Контекст выключен - показываем все файлы
    }

    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        return false;
    }

    // Проверяем, что файл имеет ВСЕ теги из контекста
    for (uint32_t i = 0; i < global_tagfs.user_context.tag_count; i++) {
        if (!tagfs_file_has_tag(inode_id, &global_tagfs.user_context.tags[i])) {
            return false;  // Нет одного из тегов - не подходит
        }
    }

    return true;  // Все теги есть - подходит!
}

// Получить список файлов в текущем контексте
int tagfs_context_list_files(uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results) {
    uint32_t result_count = 0;

    uint32_t safe_max_inodes = global_tagfs.superblock->total_inodes;
    if (safe_max_inodes > TAGFS_MAX_FILES) {
        safe_max_inodes = TAGFS_MAX_FILES;
    }

    // Перебираем все файлы и фильтруем по контексту
    for (uint32_t i = 0; i < safe_max_inodes && result_count < max_results; i++) {
        FileInode* inode = &global_tagfs.inode_table[i];
        if (inode->inode_id != 0) {
            // Пропускаем удаленные файлы
            Tag trash_tag = tagfs_tag_from_string("trashed:true");
            if (tagfs_file_has_tag(inode->inode_id, &trash_tag)) {
                continue;
            }

            // Проверяем соответствие контексту
            if (tagfs_context_matches(inode->inode_id)) {
                result_inodes[result_count++] = inode->inode_id;
            }
        }
    }

    *count_out = result_count;
    return 0;
}

// ============================================================================
// EXTENDED FILE OPERATIONS - NEW!
// ============================================================================

// Создать файл с данными
uint64_t tagfs_create_file_with_data(Tag* tags, uint32_t tag_count,
                                     const uint8_t* data, uint64_t size) {
    // Создаем файл
    uint64_t inode_id = tagfs_create_file(tags, tag_count);
    if (inode_id == TAGFS_INVALID_INODE) {
        return TAGFS_INVALID_INODE;
    }

    // Записываем данные
    if (tagfs_write_file_content(inode_id, data, size) != 0) {
        kprintf("[TAGFS] ERROR: Failed to write file content\n");
        // Можно удалить файл, но оставим для отладки
        return TAGFS_INVALID_INODE;
    }

    return inode_id;
}

// Удалить файл в корзину (мягкое удаление)
int tagfs_trash_file(uint64_t inode_id) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] ERROR: File not found (inode=%lu)\n", inode_id);
        return -1;
    }

    // Добавляем тег trashed:true
    Tag trash_tag = tagfs_tag_from_string("trashed:true");
    int result = tagfs_add_tag(inode_id, &trash_tag);

    if (result == 0) {
        kprintf("[TAGFS] File moved to trash (inode=%lu)\n", inode_id);
    }

    return result;
}

// Восстановить файл из корзины
int tagfs_restore_file(uint64_t inode_id) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] ERROR: File not found (inode=%lu)\n", inode_id);
        return -1;
    }

    // Удаляем тег trashed:true
    int result = tagfs_remove_tag(inode_id, "trashed");

    if (result == 0) {
        kprintf("[TAGFS] File restored from trash (inode=%lu)\n", inode_id);
    }

    return result;
}

// Полностью удалить файл с диска (жесткое удаление)
int tagfs_erase_file(uint64_t inode_id) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] ERROR: File not found (inode=%lu)\n", inode_id);
        return -1;
    }

    spin_lock(&global_tagfs.lock);

    // Освобождаем все блоки данных
    for (int i = 0; i < 12; i++) {
        if (inode->direct_blocks[i] != 0) {
            bitmap_clear_bit(global_tagfs.block_bitmap, inode->direct_blocks[i]);
            global_tagfs.superblock->free_blocks++;
            inode->direct_blocks[i] = 0;
        }
    }

    // TODO: Освободить indirect и double_indirect блоки

    // Удаляем из индекса тегов
    tagfs_index_remove_file(inode_id);

    // Очищаем inode
    memset(inode, 0, sizeof(FileInode));

    // Освобождаем inode bitmap
    bitmap_clear_bit(global_tagfs.inode_bitmap, inode_id);
    global_tagfs.superblock->free_inodes++;

    global_tagfs.files_deleted++;

    spin_unlock(&global_tagfs.lock);

    kprintf("[TAGFS] File erased completely (inode=%lu)\n", inode_id);
    return 0;
}

// Получить весь контент файла
uint8_t* tagfs_read_file_content(uint64_t inode_id, uint64_t* size_out) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] ERROR: File not found (inode=%lu)\n", inode_id);
        return NULL;
    }

    if (inode->size == 0) {
        *size_out = 0;
        return NULL;
    }

    // Выделяем память для данных
    uint8_t* buffer = (uint8_t*)kmalloc(inode->size + 1);  // +1 для null terminator
    if (!buffer) {
        kprintf("[TAGFS] ERROR: Failed to allocate buffer (%lu bytes)\n", inode->size);
        return NULL;
    }

    // Читаем данные
    int result = tagfs_read_file(inode_id, 0, buffer, inode->size);
    if (result != 0) {
        kprintf("[TAGFS] ERROR: Failed to read file\n");
        kfree(buffer);
        return NULL;
    }

    buffer[inode->size] = '\0';  // Null terminator для строк
    *size_out = inode->size;
    return buffer;
}

// Записать весь контент файла
int tagfs_write_file_content(uint64_t inode_id, const uint8_t* data, uint64_t size) {
    FileInode* inode = tagfs_get_inode(inode_id);
    if (!inode) {
        kprintf("[TAGFS] ERROR: File not found (inode=%lu)\n", inode_id);
        return -1;
    }

    // Записываем данные с начала файла
    int result = tagfs_write_file(inode_id, 0, data, size);
    if (result < 0 || (uint64_t)result != size) {
        kprintf("[TAGFS] ERROR: Failed to write file (wrote %d of %lu bytes)\n", result, size);
        return -1;
    }

    return 0;
}

// Найти файл по имени (тегу name:xxx) в текущем контексте
uint64_t tagfs_find_by_name(const char* name) {
    // Создаем тег name:xxx
    char tag_str[128];
    ksnprintf(tag_str, sizeof(tag_str), "name:%s", name);
    Tag name_tag = tagfs_tag_from_string(tag_str);

    // Ищем файлы с этим тегом
    uint64_t result_inodes[256];
    uint32_t count = 0;
    int result = tagfs_query_single(&name_tag, result_inodes, &count, 256);

    if (result != 0 || count == 0) {
        return TAGFS_INVALID_INODE;  // Не найдено
    }

    // Фильтруем по контексту
    for (uint32_t i = 0; i < count; i++) {
        // Пропускаем удаленные файлы
        Tag trash_tag = tagfs_tag_from_string("trashed:true");
        if (tagfs_file_has_tag(result_inodes[i], &trash_tag)) {
            continue;
        }

        // Проверяем соответствие контексту
        if (tagfs_context_matches(result_inodes[i])) {
            return result_inodes[i];  // Нашли!
        }
    }

    return TAGFS_INVALID_INODE;  // Не найдено в контексте
}
