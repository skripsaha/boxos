Название: Анатомия процесса и интерфейс Userspace Тип: Спецификация программного окружения (Runtime Environment) Базовый принцип: Изоляция через Cabin и коммуникация через фиксированные Shared-страницы.

1. Карта памяти процесса (Virtual Memory Map)
   Каждый процесс в BoxOS видит стандартную топологию своего виртуального адресного пространства. Это упрощает компиляцию и делает работу с памятью предсказуемой.

```
Адрес (Virtual)          Размер   Название              Доступ     Описание
─────────────────────    ──────   ───────────────       ──────     ────────────────────────────────
0x0000 - 0x0FFF          4KB      NULL Trap Zone        No Access  Защита от разыменования NULL-указателя
0x1000 - 0x2FFF          8KB      Notify Page           Read/Write Шлюз для отправки команд в ядро
0x3000 - 0x9FFF          28KB     Result Page           Read-Only  Кольцевой буфер ответов от ядра
0xA000 - ...             var      Code + Data + BSS     R / RW     Тело программы и глобальные переменные
0x20000 - ...            var      Heap                  RW         Динамическая память процесса
...                      ...      (свободно)            ...        ...
0x7FFFF000               4KB      CPU Caps Page         Read-Only  Информация о возможностях CPU
0x7FFFFFFFE000           var      User Stack            RW         Стек (растёт вниз)
```

Определяющие файлы: `cabin_layout.h`, `boxos_addresses.h`, `vmm.h`

2. Структура Notify Page (0x1000 - 0x2FFF, 8KB)
   Эта страница — «бланк заказа», который процесс заполняет перед вызовом notify().

```c
typedef struct __packed {
    uint32_t magic;                          // 0x4E4F5449 "NOTI"
    uint8_t  prefix_count;                   // 0-16
    uint8_t  flags;                          // NOTIFY_FLAG_CHECK_STATUS | NOTIFY_FLAG_YIELD
    uint8_t  status;                         // NOTIFY_STATUS_OK / RING_FULL / INVALID
    uint8_t  reserved1;
    uint16_t prefixes[16];                   // Цепочка команд (Snowball Pipeline)
    uint8_t  data[256];                      // Полезная нагрузка (Payload)
    volatile uint8_t event_ring_full;        // Backpressure: EventRing >90%
    volatile uint8_t result_page_full;       // Backpressure: Result Page full
    uint32_t route_target;                   // PID для direct routing (IPC)
    char     route_tag[32];                  // Tag для fan-out routing (IPC)
    uint32_t spawner_pid;                    // PID родительского процесса
    uint8_t  _reserved[7854];               // Padding до 8192 байт
} notify_page_t;                             // TOTAL: 8192 bytes (2 pages)
```

Как данные попадают в Notify Page:

1. notify_prepare() — обнуляет страницу и устанавливает magic.

2. notify_add_prefix(deck_id, opcode) — добавляет 16-битный префикс в цепочку. Максимум 16 префиксов.

3. notify_write_data(data, size) — копирует payload в буфер data (максимум 256 байт).

4. Routing (опционально): Заполняется route_target (PID) или route_tag (строка) для IPC.

5. notify() — выполняет INT 0x80, передавая управление ядру. Возвращает event_id.

3. Структура Result Page (0x3000 - 0x9FFF, 28KB)
   Страница, куда ядро доставляет результаты. SPSC ring buffer: head управляется процессом, tail — ядром.

```c
typedef struct __packed {
    uint8_t  source;             // ROUTE_SOURCE_KERNEL/PROCESS/HARDWARE
    uint8_t  _reserved1;
    uint16_t size;               // Размер данных в payload
    error_t  error_code;         // Код ошибки (OK = 0)
    uint32_t sender_pid;         // PID отправителя (для IPC)
    uint8_t  payload[244];       // Полезная нагрузка
} result_entry_t;                // TOTAL: 256 bytes

typedef struct __packed {
    uint32_t magic;                          // 0x52455355 "RESU"
    uint32_t _padding;
    volatile uint8_t notification_flag;      // Сигнал: есть новые результаты
    uint8_t _pad_notify[63];                 // Cache-line isolation
    struct {
        volatile uint32_t head;              // Индекс чтения (процесс) — cache-line aligned
        uint8_t _pad1[60];
        volatile uint32_t tail;              // Индекс записи (ядро) — cache-line aligned
        uint8_t _pad2[60];
        result_entry_t entries[111];         // Кольцо из 111 результатов
    } ring;
    uint8_t _reserved[56];
} result_page_t;                             // TOTAL: 28672 bytes (7 pages)
```

4. Жизненный цикл "Запрос-Ответ" (Userspace Side)

1. Подготовка: Приложение вызывает библиотечную функцию, например storage_read("type:config").

2. Заполнение Notify Page: Библиотека вызывает notify_prepare(), notify_add_prefix(0x02, 0x01) (Storage Query), notify_write_data() с данными.

3. Активация: Выполняется notify() — INT 0x80. Процесс переходит в WAITING.

4. Обработка ядром: Guide обрабатывает событие через цепочку Деков. Результат помещается в Result Page.

5. Возобновление: Планировщик возвращает CPU процессу (с бонусом Hot Result +50).

6. Получение: Процесс проверяет notification_flag и ring.tail, забирает данные из entries[head] и продвигает head.

5. Взаимодействие с Buffer Heap
   Для больших данных, которые не помещаются в 244 байта Result payload, ядро маппит буферы в область Buffer Heap (0x40000000+). Процесс получает в Result Page адрес буфера и его размер.

6. Контекст исполнения и Теги
   В PCB (внутри ядра) для каждого процесса хранится его Tag List.

При наследовании: Если parent_process имел тег project:secret, то child_process при создании через PROC_SPAWN автоматически получит этот тег.

При выполнении: Любая попытка доступа к файлам или сети будет неявно фильтроваться планировщиком и ФС на основе этих тегов.

Итог: Почему это удобно для программиста?

Никаких скрытых состояний: Всё общение с ОС происходит через две области памяти (Notify 8KB + Result 28KB). Для отладки достаточно посмотреть дамп памяти по адресам 0x1000 и 0x3000.

Безопасность «из коробки»: Процесс физически не может отправить в ядро ничего, кроме того, что влезет в Notify Page (16 префиксов, 256 байт данных).

Backpressure: Процесс может проверить флаги event_ring_full и result_page_full до вызова notify(), избегая блокировки.
