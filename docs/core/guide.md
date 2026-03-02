Название: The Guide (Диспетчер маршрутизации) Статус: Core Component (Сердце системы) Входные данные: EventRing (Kernel Space) Выходные данные: Вызов функций конкретных Деков и доставка результатов через Execution Deck.

1. Назначение
   Guide — это центральный batch-обработчик ядра, который обеспечивает работу архитектуры «Снежного кома». Он не выполняет полезную работу сам, а управляет потоком данных (Event), передавая его между функциональными модулями (Decks) на основе массива префиксов, сформированного пользователем.

   Важно: Guide не является отдельным потоком или бесконечным циклом. Это функция guide_run(), вызываемая из планировщика (scheduler_yield_from_interrupt) при определённых условиях.

2. Когда вызывается Guide
   guide_run() запускается из scheduler при:
   - Переходе в idle (текущий процесс → idle process)
   - Периодически каждые 5 тиков (50мс) для гарантированной обработки событий
   - Наличии процессов в wait queue на EventRing
   - Непустом EventRing (есть необработанные события)

3. Алгоритм работы (Batch Processing)
   guide_run() обрабатывает все доступные события в EventRing за один вызов:

```c
void guide_run(void) {
    while (!event_ring_is_empty(kernel_event_ring)) {
        Event event;
        event_ring_pop(kernel_event_ring, &event);

        if (!event_validate(&event)) continue;

        event.state = EVENT_STATE_PROCESSING;

        while (event.current_prefix_idx < event.prefix_count) {
            uint16_t prefix = event_current_prefix(&event);

            // 1. Конец маршрута (терминатор)
            if (prefix == 0x0000) {
                execution_deck_handler(&event);
                break;
            }

            uint8_t deck_id = event_get_deck_id(&event, idx);
            uint8_t opcode  = event_get_opcode(&event, idx);

            // 2. Проверка прав (Security Gate)
            if (!system_security_gate(event.pid, deck_id, opcode)) {
                event.state = EVENT_STATE_ACCESS_DENIED;
                execution_deck_handler(&event);
                break;
            }

            // 3. Диспетчеризация в Deck
            deck_handler_t handler = guide_get_deck_handler(deck_id);
            error_t err = handler(&event);

            // 4. Ошибка → прекращаем цепочку
            if (IS_ERROR(err)) {
                event.state = EVENT_STATE_ERROR;
                execution_deck_handler(&event);
                break;
            }

            event_advance(&event);  // Следующий префикс
        }

        // Если цепочка завершена без ошибок и без терминатора
        if (need_execution_deck) {
            event.state = EVENT_STATE_COMPLETED;
            execution_deck_handler(&event);
        }
    }

    // Пробуждение процессов из wait queue
    if (processed_count > 0 && event_ring_waiters.count > 0) {
        guide_wakeup_waiters();
    }

    // Обработка AHCI I/O completions
    guide_process_ahci_completions();

    // Backpressure: обновить event_ring_full флаг во всех Notify Pages
    update_backpressure_flags();
}
```

4. Deck Table (Зарегистрированные обработчики)

| Deck ID | Название | Обработчик |
|---------|----------|------------|
| 0xFF | System Deck | system_deck_handler — процессы, теги, маршрутизация, буферы |
| 0x01 | Operations Deck | operations_deck_handler — трансформация данных |
| 0x02 | Storage Deck | storage_deck_handler — TagFS операции |
| 0x03 | Hardware Deck | hardware_deck_handler — прямой доступ к оборудованию |
| 0x00 | Execution Deck | execution_deck_handler — финализатор (вызывается напрямую, не через deck_table) |

5. Обработка ошибок в цепочке
   В BoxOS ошибка в одном звене «Снежного кома» останавливает всё движение:

- Если любой Дек возвращает ошибку, Guide прекращает дальнейшую маршрутизацию
- Игнорирует оставшиеся в массиве префиксы
- Передает событие в Execution Deck с первой зафиксированной ошибкой (first_error)
- Execution Deck доставляет результат (с ошибкой) в Result Page процесса

Это гарантирует, что битые или неполные данные никогда не попадут в сеть или на диск.

6. Error Tracking
   Event имеет двухуровневую систему ошибок:
   - `error_code` — текущая ошибка (может перезаписываться)
   - `first_error` — первая ошибка в цепочке (записывается один раз)
   - `error_deck_idx` — индекс дека, вызвавшего первую ошибку

7. Wait Queue и Backpressure
   Если EventRing был полон во время notify():
   - Процесс помещается в event_ring_waiters (linked list)
   - guide_wakeup_waiters() пробуждает процессы когда появляются свободные слоты
   - Backpressure флаг (event_ring_full) обновляется во всех Notify Pages при >90% заполненности

8. Почему это эффективно?
   Конвейерность: Guide не делает лишних копирований. Event обрабатывается in-place.

Низкий оверхед: Между операциями «Прочитать» и «Записать» нет переключения контекста Ring 3 ↔ Ring 0. Всё происходит внутри одного вызова guide_run().

Batch Processing: Все накопившиеся события обрабатываются за один вызов, минимизируя overhead входа/выхода.

Предсказуемость: Весь путь данных (Pipeline) определен массивом префиксов заранее.
