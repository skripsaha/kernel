#ifndef TAGFS_H
#define TAGFS_H

#include "klib.h"  // Для spinlock_t и других типов
#include "../core/atomics.h"

// ============================================================================
// TagFS - Tag-Based Filesystem для BoxOS
// ============================================================================
//
// Революционная файловая система БЕЗ папок и директорий!
// Файлы организованы через ТЕГИ (key:value pairs)
//
// Философия:
// - Пользователь помнит СМЫСЛ файла, а не имя
// - Теги описывают файл: "type:image", "project:boxos", "date:08.11.2025"
// - Быстрый поиск по любой комбинации тегов
// - Файлы могут иметь произвольное количество тегов
//
// Примеры:
//   Фото: ["type:image", "format:jpg", "size:large", "date:2025-11-08", "trashed:false"]
//   Код:  ["type:code", "language:c", "project:boxos", "module:kernel", "trashed:false"]
//   Док:  ["type:document", "format:txt", "topic:design", "status:draft", "trashed:false"]
//
// ============================================================================

// ============================================================================
// CONSTANTS
// ============================================================================

#define TAGFS_MAGIC             0x54414746535632  // "TAGFSV2"
#define TAGFS_VERSION           2
#define TAGFS_BLOCK_SIZE        4096

#define TAGFS_MAX_TAGS_PER_FILE 32     // Максимум тегов на файл
#define TAGFS_TAG_KEY_SIZE      32     // Размер ключа тега (например "type")
#define TAGFS_TAG_VALUE_SIZE    64     // Размер значения тега (например "image")
#define TAGFS_MAX_TAG_INDEX     1024   // Максимум уникальных тегов в индексе

#define TAGFS_MAX_FILES         65536  // Максимум файлов
#define TAGFS_MAX_FILE_SIZE     (1ULL << 32)  // 4GB на файл
#define TAGFS_INODE_SIZE        512    // Размер структуры FileInode на диске

#define TAGFS_INVALID_INODE     0

// ============================================================================
// TAG STRUCTURE - Тег (key:value пара)
// ============================================================================

typedef struct {
    char key[TAGFS_TAG_KEY_SIZE];      // Ключ: "type", "size", "date", "project"
    char value[TAGFS_TAG_VALUE_SIZE];  // Значение: "image", "large", "2025-11-08"
} Tag;

// ============================================================================
// FILE INODE - Метаданные файла
// ============================================================================

typedef struct {
    uint64_t inode_id;                  // Уникальный ID файла (аналог inode number)
    uint64_t size;                      // Размер файла в байтах
    uint64_t creation_time;             // Время создания (RDTSC)
    uint64_t modification_time;         // Время последней модификации

    uint32_t tag_count;                 // Количество тегов
    uint32_t flags;                     // Флаги (reserved)

    Tag tags[TAGFS_MAX_TAGS_PER_FILE];  // Массив тегов

    // Указатели на блоки данных (простая схема - для v1)
    uint64_t direct_blocks[12];         // Прямые указатели (12 * 4KB = 48KB)
    uint64_t indirect_block;            // Косвенный указатель (для файлов > 48KB)
    uint64_t double_indirect_block;     // Двойная косвенность (для файлов > 2MB)

    uint8_t padding[128];               // Padding до 512 байт
} FileInode;

// ============================================================================
// TAG INDEX - Индекс для быстрого поиска по тегам
// ============================================================================

// Entry в индексе тегов - список файлов с определенным тегом
typedef struct {
    Tag tag;                            // Тег (key:value)
    uint32_t file_count;                // Количество файлов с этим тегом
    uint32_t capacity;                  // Вместимость массива
    uint64_t* inode_ids;                // Массив ID файлов с этим тегом (динамический)
} TagIndexEntry;

// Глобальный индекс тегов
typedef struct {
    uint32_t entry_count;               // Количество уникальных тегов
    uint32_t capacity;                  // Вместимость массива
    TagIndexEntry entries[TAGFS_MAX_TAG_INDEX];  // Массив индексов
} TagIndex;

// ============================================================================
// TAGFS SUPERBLOCK - Метаданные файловой системы
// ============================================================================

typedef struct {
    uint64_t magic;                     // TAGFS_MAGIC
    uint32_t version;                   // TAGFS_VERSION
    uint32_t block_size;                // Размер блока (4096)

    uint64_t total_blocks;              // Всего блоков
    uint64_t free_blocks;               // Свободных блоков

    uint64_t total_inodes;              // Всего inodes
    uint64_t free_inodes;               // Свободных inodes

    uint64_t inode_table_block;         // Блок начала таблицы inodes
    uint64_t data_blocks_start;         // Блок начала области данных
    uint64_t tag_index_block;           // Блок хранения индекса тегов

    uint64_t root_flags;                // Флаги корня ФС

    uint8_t padding[4000];              // Padding до 4096 байт
} TagFSSuperblock;

// ============================================================================
// USER CONTEXT - Контекст пользователя для фильтрации файлов
// ============================================================================
// Новая фича! Пользователь может установить контекст (набор тегов)
// и видеть только файлы, у которых есть ВСЕ эти теги.
//
// Пример:
//   use type:image size:small
//   Теперь пользователь видит только маленькие картинки!
//   Все операции (ls, eye и т.д.) работают только с этими файлами.

#define TAGFS_MAX_CONTEXT_TAGS 16

typedef struct {
    Tag tags[TAGFS_MAX_CONTEXT_TAGS];   // Теги контекста
    uint32_t tag_count;                  // Количество тегов в контексте
    bool enabled;                        // Включен ли контекст
} TagFSUserContext;

// ============================================================================
// TAGFS CONTEXT - Runtime состояние файловой системы
// ============================================================================

typedef struct {
    TagFSSuperblock* superblock;        // Superblock в памяти
    FileInode* inode_table;             // Таблица inodes в памяти
    TagIndex tag_index;                 // Индекс тегов в памяти

    uint8_t* block_bitmap;              // Bitmap занятых блоков (для аллокации)
    uint8_t* inode_bitmap;              // Bitmap занятых inodes

    volatile uint64_t next_inode_id;    // Счётчик для генерации inode ID

    spinlock_t lock;                    // Spinlock для синхронизации доступа

    // User Context для фильтрации (NEW!)
    TagFSUserContext user_context;      // Текущий контекст пользователя

    // Статистика
    volatile uint64_t files_created;
    volatile uint64_t files_deleted;
    volatile uint64_t queries_executed;
    volatile uint64_t tags_added;
    volatile uint64_t tags_removed;
} TagFSContext;

// ============================================================================
// QUERY STRUCTURE - Запрос поиска файлов
// ============================================================================

typedef enum {
    QUERY_OP_AND,    // Все теги должны совпадать
    QUERY_OP_OR,     // Хотя бы один тег должен совпадать
    QUERY_OP_NOT,    // Исключить файлы с тегом
} QueryOperator;

typedef struct {
    Tag* tags;              // Массив тегов для поиска
    uint32_t tag_count;     // Количество тегов
    QueryOperator op;       // Оператор (AND/OR/NOT)

    // Результаты
    uint64_t* result_inodes;   // Массив найденных inode ID
    uint32_t result_count;     // Количество найденных файлов
    uint32_t result_capacity;  // Вместимость массива результатов
} TagQuery;

// ============================================================================
// TAGFS INITIALIZATION
// ============================================================================

void tagfs_init(void);
void tagfs_format(uint64_t total_blocks);  // Форматирование новой ФС

// ============================================================================
// DISK PERSISTENCE - Синхронизация с диском
// ============================================================================

// Включить режим работы с диском (0 = память, 1 = диск)
void tagfs_set_disk_mode(int enable);

// Синхронизировать всю ФС на диск
int tagfs_sync(void);

// Синхронизировать superblock на диск
int tagfs_sync_superblock(void);

// Загрузить superblock с диска
int tagfs_load_superblock(void);

// Синхронизировать таблицу inodes на диск
int tagfs_sync_inode_table(void);

// Загрузить таблицу inodes с диска
int tagfs_load_inode_table(void);

// ============================================================================
// FILE OPERATIONS
// ============================================================================

// Создать файл с тегами
uint64_t tagfs_create_file(Tag* tags, uint32_t tag_count);

// Создать файл с данными
uint64_t tagfs_create_file_with_data(Tag* tags, uint32_t tag_count,
                                     const uint8_t* data, uint64_t size);

// Удалить файл (помечает trashed:true, мягкое удаление - в корзину)
int tagfs_trash_file(uint64_t inode_id);

// Полностью удалить файл с диска (стирает данные и метаданные)
int tagfs_erase_file(uint64_t inode_id);

// Восстановить файл из корзины
int tagfs_restore_file(uint64_t inode_id);

// Чтение данных файла
int tagfs_read_file(uint64_t inode_id, uint64_t offset, uint8_t* buffer, uint64_t size);

// Запись данных в файл
int tagfs_write_file(uint64_t inode_id, uint64_t offset, const uint8_t* buffer, uint64_t size);

// Получить весь контент файла (удобная обертка)
uint8_t* tagfs_read_file_content(uint64_t inode_id, uint64_t* size_out);

// Записать весь контент файла (удобная обертка)
int tagfs_write_file_content(uint64_t inode_id, const uint8_t* data, uint64_t size);

// Получить метаданные файла
FileInode* tagfs_get_inode(uint64_t inode_id);

// ============================================================================
// TAG OPERATIONS
// ============================================================================

// Добавить тег к файлу
int tagfs_add_tag(uint64_t inode_id, const Tag* tag);

// Удалить тег из файла
int tagfs_remove_tag(uint64_t inode_id, const char* key);

// Получить все теги файла
int tagfs_get_tags(uint64_t inode_id, Tag* tags_out, uint32_t* count_out);

// ============================================================================
// QUERY OPERATIONS - Поиск файлов по тегам
// ============================================================================

// Поиск файлов по одному тегу
int tagfs_query_single(const Tag* tag, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results);

// Поиск файлов по нескольким тегам (AND/OR/NOT)
int tagfs_query(TagQuery* query);

// Удобные wrappers для частых запросов
int tagfs_find_by_type(const char* type, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results);
int tagfs_find_by_date(const char* date, uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results);
int tagfs_find_not_trashed(uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results);

// ============================================================================
// TAG INDEX MANAGEMENT
// ============================================================================

void tagfs_index_add_file(uint64_t inode_id, const Tag* tags, uint32_t tag_count);
void tagfs_index_remove_file(uint64_t inode_id);
void tagfs_index_rebuild(void);  // Пересборка индекса (если повреждён)

// ============================================================================
// USER CONTEXT OPERATIONS (NEW!)
// ============================================================================

// Установить контекст пользователя (фильтр по тегам)
// После этого все операции (поиск, список) работают только с файлами,
// у которых есть ВСЕ указанные теги
int tagfs_context_set(Tag* tags, uint32_t tag_count);

// Очистить контекст (показать все файлы)
void tagfs_context_clear(void);

// Получить текущий контекст
TagFSUserContext* tagfs_context_get(void);

// Проверить, подходит ли файл под текущий контекст
bool tagfs_context_matches(uint64_t inode_id);

// Получить список файлов в текущем контексте
int tagfs_context_list_files(uint64_t* result_inodes, uint32_t* count_out, uint32_t max_results);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

// Создать тег из строки формата "key:value"
Tag tagfs_tag_from_string(const char* str);

// Сравнить два тега
int tagfs_tag_equal(const Tag* a, const Tag* b);

// Проверить есть ли тег у файла
int tagfs_file_has_tag(uint64_t inode_id, const Tag* tag);

// Найти файл по имени (тегу name:xxx) в текущем контексте
uint64_t tagfs_find_by_name(const char* name);

// ============================================================================
// STATISTICS & DEBUGGING
// ============================================================================

void tagfs_print_stats(void);
void tagfs_print_file_info(uint64_t inode_id);
void tagfs_print_tag_index(void);

// ============================================================================
// GLOBAL CONTEXT
// ============================================================================

extern TagFSContext global_tagfs;

#endif // TAGFS_H
