Название: notify() — Универсальный системный шлюз Тип: Единственный системный вызов (Universal Entry Point) Механизм: Переход из Ring 3 в Ring 0 через INT 0x80.

1. Концепция
   В BoxOS отсутствует таблица системных вызовов (syscall table). Вместо сотен функций существует только одна инструкция — notify(). Она не передает параметры через регистры в классическом понимании, а служит сигналом ядру проверить общую страницу памяти процесса (Notify Page) и сформировать команду (Event).

   ВАЖНО: BoxOS — это НЕ event-driven архитектура. Структура называется "Event", но по сути это command packet (пакет команд). Здесь нет подписок на события, нет callbacks, нет pub/sub. Процесс явно отправляет команду через notify() и явно читает результат из Result Page. Архитектура BoxOS — это Snowball command pipeline: команда катится через цепочку обработчиков (Деков).

2. Предварительные условия (Userspace)
   Перед вызовом notify(), процесс обязан подготовить данные на странице Notify Page (0x1000 virtual, 8KB):

Magic: Записать сигнатуру валидности (0x4E4F5449 "NOTI").

Prefixes Array: Сформировать массив 16-битных команд (цепочка маршрута), максимум 16 префиксов.

Data: Поместить необходимые аргументы или payload в буфер данных (256 байт).

Routing (опционально): Заполнить route_target (PID) или route_tag (строка) для IPC.

3. Алгоритм работы notify() (Kernel Side)
   При срабатывании INT 0x80, ядро выполняет следующие шаги строго по порядку:

Save Context: Сохранение состояния регистров текущего процесса через interrupt frame.

Privilege Transition: Переход процессора в Ring 0.

Identify: Получение current_process из scheduler state.

PCB Validation: Проверка magic числа процесса (PROCESS_MAGIC). Если невалидный — мгновенный возврат с ошибкой.

Address Resolution: Получение физического адреса Notify Page через proc->notify_page_phys.

Validation: Проверка magic числа на странице (NOTIFY_PAGE_MAGIC). Если неверный — возврат с ошибкой.

Yield Check: Если установлен флаг NOTIFY_FLAG_YIELD — кооперативный yield без создания события.

Prefix Validation: Проверка prefix_count <= NOTIFY_MAX_PREFIXES (16).

Event Creation: Инициализация новой структуры Event (384 байта) через event_init().

Prefix Ingestion: Копирование массива префиксов из Notify Page в event.prefixes[].
   Ядро добавляет 0x0000 (терминатор) после последнего префикса если есть место.

Data Ingestion: memcpy() буфера данных (256 байт) из Notify Page в event.data.

Routing Copy: Копирование route_target и route_tag из Notify Page в Event.

Queuing: Помещение Event в EventRing через event_ring_push_priority() с приоритетом USER.

Backpressure: Если EventRing переполнен — процесс ставится в wait queue (WAIT_RING_FULL).

Guide Awakening: guide_wake() — установка флага готовности диспетчера.

Synchronous Yield:

   Возврат event_id через RAX.

   context_save_from_frame() — сохранение состояния процесса.

   process_set_state(proc, PROC_WAITING) — процесс ожидает обработки.

   scheduler_yield_from_interrupt() — передача управления планировщику.

   Результат: Процесс отдаёт CPU. Guide обрабатывает событие. Процесс получает результат через Result Page (0x3000) когда планировщик вернёт ему управление.

4. Структура взаимодействия
   ```
   [ Userspace Process ]
   |
   | 1. Заполняет Notify Page (0x1000, 8KB)
   | 2. Вызывает INT 0x80 (notify)
   V
   [ KERNEL: syscall_handler ]
   |
   | 3. Валидация process + notify page magic
   | 4. Копирует Prefixes + Data + Routing -> Event (384 bytes)
   | 5. Кладёт Event в EventRing
   | 6. Будит Guide (guide_wake)
   | 7. Процесс -> PROC_WAITING
   | 8. scheduler_yield_from_interrupt()
   V
   [ Scheduler выбирает следующий процесс ]
   |
   | Guide обрабатывает EventRing (из scheduler)
   | Результат -> Result Page (0x3000, 28KB)
   V
   [ Process получает CPU обратно, читает Result Page ]
   ```

5. Технические особенности
   Синхронность: notify() — это синхронная операция. Процесс отдаёт CPU после вызова и получает управление обратно после обработки события. На single-core это единственно безопасный подход — Guide обрабатывает событие пока процесс ждёт.

Безопасность: Ядро копирует данные (memcpy), а не работает с ними прямо на странице пользователя. Это защищает ядро от TOCTOU-атак (Time-of-check to time-of-use).

Производительность: Единый syscall вектор (INT 0x80) минимизирует логику диспетчеризации. Весь путь от INT до EventRing — несколько десятков тактов.

Backpressure: При переполнении EventRing (>90%) устанавливается флаг event_ring_full в Notify Page. Процесс может проверить его до вызова notify(). При полном заполнении — процесс блокируется в wait queue.

Кооперативный yield: Флаг NOTIFY_FLAG_YIELD (0x80) позволяет процессу отдать CPU без создания события.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## Будущее: Концепция асинхронного notify()

В будущих версиях BoxOS (при поддержке multi-core) возможен переход к асинхронной модели notify(), где процесс продолжает работу сразу после помещения события в EventRing, не отдавая CPU. Эта секция зарезервирована для документации будущей асинхронной реализации.

Основные требования для перехода на async:
- Multi-core поддержка: Guide должен работать на отдельном ядре CPU
- Lock-free EventRing: MPSC очередь без блокировок между ядрами
- Атомарная доставка результатов: Result Page должна поддерживать конкурентную запись ядром и чтение процессом
- Notification механизм: Способ уведомить процесс о готовности результата без прерывания (polling, futex, или signal)
