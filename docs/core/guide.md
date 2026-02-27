Название: The Guide (Диспетчер маршрутизации) Статус: Core Component (Сердце системы) Входные данные: EventRing (Kernel Space) Выходные данные: Вызов функций конкретных Деков или Execution Deck.

1. Назначение
   Guide — это центральный бесконечный цикл ядра, который обеспечивает работу архитектуры «Снежного кома». Он не выполняет полезную работу сам, а управляет потоком данных (Event), передавая его между функциональными модулями (Decks) на основе массива префиксов, сформированного пользователем.

2. Алгоритм работы (The Core Loop)
   Guide работает по принципу Sequential Dispatch (Последовательная диспетчеризация):

Idle State: Если EventRing пуст, Guide переходит в режим ожидания (halt/sleep), минимизируя потребление ресурсов.

Wake Up: При вызове notify() и появлении нового Event, Guide извлекает его из головы кольцевого буфера.

Prefix Fetch: Читает текущую инструкцию из event->prefixes[event->current_prefix_idx].

Validation:

Если префикс == 0x0000 — цепочка завершена успешно. Переход к Execution Deck (вызов напрямую, не через deck_table).

Если префикс некорректен или current_prefix_idx вышел за пределы — переход к Execution Deck со статусом CRITICAL_ERROR.

JIT Security (System Check):

Guide передает pid, Deck_ID и Opcode в System Deck (0xFF).

Если проверка прав провалена — мгновенная остановка цепочки, статус ACCESS_DENIED.

Dispatch:

Guide находит нужный Дек по ID.

Вызывает обработчик Дека, передавая ему указатель на event->data.

Increment: После возврата управления от Дека, Guide инкрементирует current_prefix_idx и возвращается к пункту 3.

3. Обработка ошибок в цепочке
   В BoxOS ошибка в одном звене «Снежного кома» останавливает всё движение:

Если любой Дек возвращает флаг ошибки, Guide прекращает дальнейшую маршрутизацию.

Он игнорирует оставшиеся в массиве префиксы и сразу передает событие в Execution Deck.

Это гарантирует, что битые или неполные данные никогда не попадут в сеть или на диск.

4.  Псевдокод (Архитектурная логика)
    C
    void guide_loop() {
    while(true) {
    // Ожидание события
    while(EventRing.is_empty()) cpu_relax();

            Event* ev = EventRing.pop();

            while(ev->state == STATE_PROCESSING) {
                uint16_t prefix = ev->prefixes[ev->current_prefix_idx];

                // 1. Конец маршрута
                if (prefix == 0x0000) {
                    dispatch_to_deck(DECK_EXECUTION, ev);
                    break;
                }

                uint8_t d_id = (prefix >> 8);
                uint8_t op   = (prefix & 0xFF);

                // 2. Проверка прав (System Deck)
                if (!system_security_gate(ev->pid, d_id, op)) {
                    ev->state = STATE_ACCESS_DENIED;
                    dispatch_to_deck(DECK_EXECUTION, ev);
                    break;
                }

                // 3. Выполнение
                bool success = dispatch_to_deck(d_id, ev, op);

                if (!success) {
                    ev->state = STATE_ERROR;
                    dispatch_to_deck(DECK_EXECUTION, ev);
                    break;
                }

                ev->current_prefix_idx++;
            }
        }

    }

5.  Почему это эффективно?
    Конвейерность: Guide не делает лишних копирований. Он передает только указатель на структуру события.

Низкий оверхед: Между операциями «Прочитать» и «Сжать» нет переключения контекста Ring 3 <-> Ring 0. Всё происходит внутри одного прохода Guide.

Предсказуемость: Весь путь данных (Pipeline) определен заранее. Процессор может эффективно использовать предсказание переходов.
