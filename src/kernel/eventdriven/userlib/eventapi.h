#ifndef EVENTAPI_H
#define EVENTAPI_H

#include "../core/events.h"
#include "../core/ringbuffer.h"

// ============================================================================
// USER SPACE EVENT API - Асинхронный интерфейс для user programs
// ============================================================================
//
// Пример использования:
//
//   // Инициализация
//   eventapi_init();
//
//   // Отправка события (асинхронно!)
//   uint64_t event_id = eventapi_memory_alloc(4096);
//
//   // Продолжаем работу (НЕ БЛОКИРУЕМСЯ!)
//   do_other_work();
//
//   // Проверяем результат (polling)
//   Response* resp = eventapi_poll_response(event_id);
//   if (resp) {
//       void* addr = *(void**)resp->result;
//       use_memory(addr);
//   }
//
// ============================================================================

// ============================================================================
// INITIALIZATION
// ============================================================================

// Инициализирует доступ к kernel ring buffers
// NOTE: В реальной системе это должно делаться через shared memory mapping
void eventapi_init(EventRingBuffer* to_kernel, ResponseRingBuffer* from_kernel);

// ============================================================================
// EVENT SUBMISSION - Отправка событий (асинхронно!)
// ============================================================================

// Память
uint64_t eventapi_memory_alloc(uint64_t size);
uint64_t eventapi_memory_free(void* addr);

// Файлы
uint64_t eventapi_file_open(const char* path);
uint64_t eventapi_file_close(int fd);
uint64_t eventapi_file_read(int fd, uint64_t size);
uint64_t eventapi_file_write(int fd, const void* data, uint64_t size);

// Generic event submission
uint64_t eventapi_submit_event(Event* event);

// ============================================================================
// RESPONSE POLLING - Проверка результатов
// ============================================================================

// Проверяет наличие ответа для данного event_id
// Возвращает NULL если ответ ещё не готов
Response* eventapi_poll_response(uint64_t event_id);

// Ждёт ответа (blocking!) - НЕ РЕКОМЕНДУЕТСЯ
Response* eventapi_wait_response(uint64_t event_id);

// ============================================================================
// HELPERS
// ============================================================================

// Возвращает количество pending событий
int eventapi_pending_count(void);

#endif // EVENTAPI_H
