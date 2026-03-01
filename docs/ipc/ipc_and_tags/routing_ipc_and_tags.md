# BoxOS: Snowball Routing & Unified Tags

**Версия:** 1.0  
**Статус:** Финальный анализ  
**Область:** IPC, ввод/вывод, теги, GUI-совместимость

---

## Часть 1: Теги — два формата, одна система

### Форматы тегов

В BoxOS существуют два формата тегов. Оба — полноценные теги. Оба участвуют в выборке, наследуются, хранятся одинаково.

**Парные теги (key:value):**
```
project:mars
type:photo
status:draft
author:alex
year:2025
```

Ключ (`key`) — это **категория**. Значение (`value`) — конкретика. Двоеточие — разделитель, который ядро понимает и парсит.

**Простые теги (label):**
```
urgent
draft
favorite
app
system
trashed
```

Просто слово. Без ключа, без двоеточия. Используются когда категория не нужна — тег сам по себе описывает свойство.

### Почему оба формата нужны

Парные теги дают **категоризацию**. Когда у файла тег `type:photo`, ты знаешь что `type` — это категория, а `photo` — значение внутри неё. Можно спросить: "покажи всё с ключом `type`" и получить фотографии, документы, код — всё, что имеет категорию типа.

Простые теги дают **свободу**. Иногда файл просто "urgent". Заставлять писать `priority:urgent` — это трение. BoxOS уничтожает трение.

### Хранение в ядре

Для ядра тег — это структура:

```c
typedef struct {
    char key[16];      // Ключ (пустая строка "" для простых тегов)
    char value[16];    // Значение (для простых тегов = сам тег)
} tag_t;
```

Примеры хранения:

| Тег пользователя | key | value |
|-------------------|-----|-------|
| `project:mars` | `"project"` | `"mars"` |
| `type:photo` | `"type"` | `"photo"` |
| `urgent` | `""` | `"urgent"` |
| `app` | `""` | `"app"` |
| `trashed` | `""` | `"trashed"` |

Парсинг тривиален: есть двоеточие → split по первому `:`. Нет двоеточия → key = "", value = слово.

### Wildcard: оператор `...`

В Linux `*` означает "всё". В BoxOS `...` означает "любое значение".

**Синтаксис:** `key:...` — совпадает с любым тегом, у которого ключ = `key`.

**Примеры:**

```
~ files type:...
Files
  photo1.jpg   [type:photo, year:2025]
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos]

# Все файлы, у которых ЕСТЬ ключ "type", независимо от значения
```

```
~ files project:...
Files
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos]
  plan.txt     [type:document, project:mars]

# Все файлы, у которых ЕСТЬ ключ "project"
```

```
~ use type:...
[type:...] ~ files
Files
  photo1.jpg   [type:photo, year:2025]
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos]

# Контекст: все файлы с любым type. Процессы с любым type: тегом получают +20 баллов.
```

**Комбинации:**

```
~ files project:boxos type:...
Files
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos]

# Пересечение: project точно boxos И type любой
```

```
~ files project:... urgent
Files
  plan.txt     [project:mars, urgent]

# Пересечение: любой project И тег urgent
```

### Алгоритм сравнения тегов (Tag Matching)

```c
// Сравнение запроса с тегом объекта
bool tag_matches(const tag_t* query, const tag_t* object_tag) {
    // Случай 1: Простой тег (key пустой)
    // query: {key:"", value:"urgent"} vs object: {key:"", value:"urgent"}
    if (query->key[0] == '\0') {
        return (object_tag->key[0] == '\0') &&
               (strcmp(query->value, object_tag->value) == 0);
    }
    
    // Случай 2: Парный тег с wildcard
    // query: {key:"type", value:"..."} vs object: {key:"type", value:"photo"}
    if (strcmp(query->value, "...") == 0) {
        return strcmp(query->key, object_tag->key) == 0;
        // Ключ совпадает — значение любое. Совпадение!
    }
    
    // Случай 3: Точное совпадение парного тега
    // query: {key:"project", value:"mars"} vs object: {key:"project", value:"mars"}
    return (strcmp(query->key, object_tag->key) == 0) &&
           (strcmp(query->value, object_tag->value) == 0);
}
```

### Wildcard в планировщике (use context)

Когда пользователь вводит `use type:...`, планировщик даёт +20 баллов **всем** процессам, у которых есть любой тег с ключом `type`. Это широкий фокус.

Когда `use project:mars` — +20 только процессам с точным тегом `project:mars`. Это узкий фокус.

Пользователь сам выбирает ширину фокуса:

```
use project:...        → широкий: все проекты в приоритете
use project:mars       → узкий: только Mars
use project:mars urgent → ещё уже: Mars + urgent
```

### Wildcard в ROUTE_TAG

ROUTE_TAG (FF:41) тоже поддерживает `...`:

```
route_tag = "output:..."
→ Доставить всем процессам с ЛЮБЫМ тегом output:*
→ output:text, output:log, output:debug — все получат
```

```
route_tag = "output:text"
→ Доставить только процессам с точным тегом output:text
```

### Защищённые теги

Следующие теги могут быть добавлены/удалены **только** процессами с тегом `system`:

```c
static const char* PROTECTED_TAGS[] = {
    "system",       // Критический компонент ОС
    "app",          // Право на запуск как процесс
    "utility",      // Повышенные привилегии
    "display",      // Право на управление экраном/вводом
    NULL
};
```

`trashed` — может менять любой процесс (не защищён, это просто пометка).

Все остальные теги — свободные. Пользователь и процессы создают любые теги без ограничений.

### Пример сеанса

```
~ create photo.jpg  type:photo  year:2025  vacation
~ create report.doc  type:document  project:boxos
~ create main.c  type:code  project:boxos  urgent

~ files type:...
Files
  photo.jpg    [type:photo, year:2025, vacation]
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos, urgent]

~ files type:code
Files
  main.c       [type:code, project:boxos, urgent]

~ files project:... urgent
Files
  main.c       [type:code, project:boxos, urgent]

~ files vacation
Files
  photo.jpg    [type:photo, year:2025, vacation]

~ use project:boxos
[project:boxos] ~ files
Files
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos, urgent]

[project:boxos] ~ files type:...
Files
  report.doc   [type:document, project:boxos]
  main.c       [type:code, project:boxos, urgent]
# type:... внутри контекста project:boxos → пересечение обоих условий

[project:boxos] ~ use
~ 
```

### Проверка: Не ломается ли что-то?

| Компонент | Влияние | Статус |
|-----------|---------|--------|
| TagFS intersection | tag_matches() заменяет strcmp() — поддерживает оба формата и wildcard | ✅ OK |
| Security gate | Проверяет конкретные строки из PROTECTED_TAGS | ✅ OK |
| Scheduler (use context) | tag_matches() с поддержкой `...` для широкого фокуса | ✅ OK |
| Shell команда `use` | Парсит key:value, key:..., label — три случая | ✅ OK |
| PROC_SPAWN tags | Comma-separated, парсит при создании | ✅ OK |
| TAG_ADD/REMOVE/CHECK | Работает с tag_t структурой | ✅ OK |
| ROUTE_TAG | tag_matches() для поиска получателей — wildcard работает | ✅ OK |

**Вердикт:** Два формата + wildcard `...` дают и структуру, и свободу, и мощную фильтрацию.

---

## Часть 2: Snowball Routing — IPC через цепочку деков

### Постановка проблемы

BoxOS умеет: Процесс → Ядро → Процесс (через notify/ResultPage).  
BoxOS НЕ умеет:
- Процесс → Другой процесс
- Устройство → Процесс (ввод с клавиатуры)
- Один источник → Много получателей

### Принцип решения

IPC — это **не отдельная подсистема**. Это ещё один шаг в Snowball chain. Данные трансформируются внутри ядра деками и потом доставляются получателю. Получатель не знает, какой путь данные прошли.

Доставка происходит через **существующий** Execution Deck. Никаких новых механизмов доставки.

### Ключевая механика: ROUTE меняет event->pid

Execution Deck всегда делает одно и то же: берёт `event->pid`, находит PCB, кладёт `event->data` в ResultPage этого процесса.

ROUTE просто **меняет `event->pid`** на PID получателя. После этого Execution Deck доставит данные другому процессу, не подозревая, что что-то изменилось. Ноль изменений в Execution Deck.

### Проблема: Где хранить адрес получателя?

Каждый дек в цепочке читает аргументы из `event->data` и **перезаписывает** data своим результатом. Если ROUTE стоит после Storage Deck, данные уже перезаписаны содержимым файла. Куда делся target_pid?

**Решение: Routing Header — отдельное поле в Event и NotifyPage.**

Информация о маршрутизации хранится **вне** `event->data`, в отдельных полях. Деки не трогают эти поля. ROUTE читает только их.

### Изменения в структурах

#### NotifyPage (0x1000) — дополнение

```c
struct NotifyPage {
    uint32_t magic;             // Сигнатура валидности
    
    // === ROUTING HEADER (36 bytes) — NEW ===
    uint32_t route_target;      // PID получателя (0 = доставить себе, по умолчанию)
    char     route_tag[32];     // Тег получателя для fan-out ("" = не используется, "..." поддерживается)
    
    // === EXISTING ===
    uint16_t prefixes[128];     // Цепочка Snowball
    uint8_t  data[3800];        // Полезная нагрузка (было 3836, -36 на routing header)
};
```

#### Event — дополнение

```c
typedef struct Event {
    // ... существующие поля (magic, pid, prefixes, data, state...) ...
    
    // === ROUTING HEADER — NEW ===
    uint32_t route_target;      // 0 = self (по умолчанию)
    char     route_tag[32];     // "" = unused, поддерживает "key:..." wildcard
    uint8_t  route_flags;       // FLAG_ERROR и др.
} Event;
```

#### ResultEntry — дополнение

```c
struct ResultEntry {
    uint8_t  source;        // NEW: 0=KERNEL, 1=ROUTE, 2=HARDWARE
    uint8_t  sender_pid;    // NEW: PID отправителя (если source=ROUTE)
    uint16_t status;        // Код успеха/ошибки
    uint16_t size;          // Размер данных
    uint8_t  payload[250];  // Данные (было 252, отдали 2 байта)
};
```

Поле `source` критически важно: процесс-получатель должен различать ответы на свои собственные notify() (source=KERNEL) и входящие сообщения от других процессов (source=ROUTE).

### Поток данных в notify_gateway

Когда ядро обрабатывает notify():

```
1. Создать Event
2. Скопировать prefixes[] из NotifyPage в event->prefixes[]
3. Скопировать data[] из NotifyPage в event->data[]
4. Скопировать route_target из NotifyPage в event->route_target  ← NEW
5. Скопировать route_tag из NotifyPage в event->route_tag        ← NEW
6. event->pid = current_pid
7. Положить в EventRing
```

Шаги 4-5 — единственное добавление. Остальное без изменений.

---

## Часть 3: Три новых опкода System Deck (0xFF)

### 0x40 — ROUTE (Прямая доставка)

**Что делает:** Меняет `event->pid` на `event->route_target`.

```c
void system_deck_route(Event* event) {
    // 1. Проверка: route_target задан?
    if (event->route_target == 0) {
        event->state = STATE_ERROR;
        event->error_code = SYSTEM_ERR_INVALID_ARGS;
        return;
    }
    
    // 2. Проверка: целевой процесс существует?
    PCB* target = pcb_find(event->route_target);
    if (!target || target->state == TERMINATED) {
        event->state = STATE_ERROR;
        event->error_code = SYSTEM_ERR_PROCESS_NOT_FOUND;
        return;
    }
    
    // 3. Проверка: есть место в ResultPage получателя?
    ResultPage* rp = get_result_page(event->route_target);
    if (result_ring_is_full(rp)) {
        event->state = STATE_ERROR;
        event->error_code = ROUTE_ERR_TARGET_FULL;
        return;
    }
    
    // 4. Сохранить sender_pid для ResultEntry.sender_pid
    uint32_t original_sender = event->pid;
    event->route_flags |= ROUTE_SOURCE_PROCESS;
    
    // 5. КЛЮЧЕВОЕ ДЕЙСТВИЕ: подменить pid
    event->pid = event->route_target;
    
    // 6. Сохранить original sender в метаданных
    //    (Execution Deck прочитает это при формировании ResultEntry)
    event->sender_pid = original_sender;
    
    // Цепочка продолжается. Когда Guide встретит 0x0000,
    // Execution Deck доставит event->data в ResultPage процесса route_target.
}
```

**Безопасность:** Требуется тег `app` или `utility` у отправителя. Нельзя отправить сообщение от процесса без этих тегов.

**Что если цепочка после ROUTE содержит ещё деки?** Они выполнятся нормально, данные продолжат трансформироваться. ROUTE меняет только получателя, не прерывает цепочку.

### 0x41 — ROUTE_TAG (Доставка по тегу)

**Что делает:** Ищет **все** процессы с совпадающим тегом. Для каждого — клонирует event и кладёт в EventRing с единственным префиксом `[0x0000]`.

```c
void system_deck_route_tag(Event* event) {
    // 1. Проверка: route_tag задан?
    if (event->route_tag[0] == '\0') {
        event->state = STATE_ERROR;
        event->error_code = SYSTEM_ERR_INVALID_ARGS;
        return;
    }
    
    // 2. Найти всех получателей по тегу
    uint32_t targets[MAX_ROUTE_TAG_TARGETS]; // лимит: 16 получателей
    uint32_t target_count = 0;
    
    for (each Process p in ProcessList) {
        if (p->state != RUNNING && p->state != READY) continue;
        if (p->pid == event->pid) continue;  // не себе
        
        if (process_tag_matches(p, event->route_tag)) {
            // tag_matches() поддерживает "output:..." (wildcard) и "output:text" (точное)
            // Если активен use-контекст — дополнительная фильтрация
            if (global_use_context_active()) {
                if (!process_matches_use_context(p)) continue;
            }
            targets[target_count++] = p->pid;
            if (target_count >= MAX_ROUTE_TAG_TARGETS) break;
        }
    }
    
    // 3. Никого не нашли?
    if (target_count == 0) {
        event->state = STATE_ERROR;
        event->error_code = ROUTE_ERR_NO_SUBSCRIBERS;
        return;
    }
    
    // 4. Для каждого получателя — клон event
    uint32_t original_sender = event->pid;
    
    for (uint32_t i = 0; i < target_count; i++) {
        Event* clone = event_clone(event);
        clone->pid = targets[i];
        clone->sender_pid = original_sender;
        clone->route_flags |= ROUTE_SOURCE_PROCESS;
        
        // Клон завершается немедленно (только доставка)
        clone->prefixes[0] = 0x0000;
        clone->current_prefix_idx = 0;
        clone->prefix_count = 1;
        
        event_ring_push(clone);
    }
    
    // 5. Оригинальный event продолжает свою цепочку
    //    (pid остаётся оригинальным — результат вернётся отправителю)
}
```

**Важный нюанс:** ROUTE_TAG **не меняет** pid оригинального event. Оригинал продолжает цепочку и результат вернётся отправителю. Клоны — это копии текущего состояния data, которые доставляются получателям.

Это отличается от ROUTE (0x40), который **перенаправляет** оригинал. ROUTE_TAG **рассылает копии**, оставляя оригинал нетронутым.

**Взаимодействие с `use`:** Если установлен глобальный контекст `use project:mars`, ROUTE_TAG ищет получателей **сначала** среди процессов с тегом `project:mars`. Это автоматическая область видимости IPC.

**Лимиты:** Максимум 16 получателей за одну рассылку. Это защита от бесконтрольного fan-out.

### 0x42 — LISTEN (Привязка аппаратного ввода)

**Что делает:** Регистрирует процесс как получатель данных от аппаратного источника (клавиатура, мышь, сетевая карта).

```c
typedef struct {
    uint8_t  source_type;   // LISTEN_KEYBOARD, LISTEN_MOUSE, LISTEN_NETWORK и т.д.
    uint8_t  flags;         // 0 = обычный, LISTEN_FLAG_EXCLUSIVE = только я
    uint16_t reserved;
} listen_args_t;
```

```c
void system_deck_listen(Event* event) {
    listen_args_t* args = (listen_args_t*)event->data;
    
    // Добавить запись в Listen Table
    listen_entry_t entry = {
        .pid = event->pid,
        .source_type = args->source_type,
        .flags = args->flags
    };
    
    listen_table_add(&entry);
}
```

**Когда срабатывает IRQ (например, клавиатура):**

```c
void keyboard_irq_handler(uint8_t scancode) {
    char character = scancode_to_char(scancode);
    
    // Найти процессы, слушающие клавиатуру
    uint32_t listeners[8];
    uint32_t count = listen_table_find(LISTEN_KEYBOARD, listeners, 8);
    
    // Фильтр по use-контексту: если активен, отправить только тем,
    // кто в текущем контексте
    if (global_use_context_active()) {
        count = filter_by_use_context(listeners, count);
    }
    
    // Создать event-доставку для каждого слушателя
    for (uint32_t i = 0; i < count; i++) {
        Event* ev = event_alloc();
        ev->pid = listeners[i];
        ev->prefixes[0] = 0x0000;  // Только доставка
        ev->prefix_count = 1;
        ev->sender_pid = 0;  // 0 = hardware
        ev->route_flags = ROUTE_SOURCE_HARDWARE;
        
        // Данные: нажатый символ
        ev->data[0] = character;
        ev->data_size = 1;
        
        event_ring_push(ev);
        // Guide подберёт → Execution Deck доставит в ResultPage
    }
}
```

**Фильтрация по `use`:** Если пользователь в контексте `use project:mars`, нажатие клавиши получают только слушатели с тегом `project:mars`. Это **автоматический фокус ввода**.

**Security:** LISTEN на клавиатуру и мышь требует тег `app` или `display`. LISTEN на низкоуровневое железо (MMIO, IRQ) — тег `utility` или `system`.

---

## Часть 4: Замена stdin / stdout / stderr / pipes

### Сводная таблица

| Концепция UNIX | Что это на самом деле | Как реализовано в BoxOS |
|----------------|----------------------|------------------------|
| stdin | Поток ввода в процесс | Данные приходят в ResultPage (source=HARDWARE или ROUTE) |
| stdout | Поток вывода из процесса | ROUTE к display-процессу с flag=TEXT |
| stderr | Поток ошибок из процесса | ROUTE к display-процессу с flag=ERROR |
| pipe `A \| B` | Соединение stdout→stdin | A делает ROUTE(pid:B), B читает из ResultPage |
| redirect `> file` | stdout в файл | Shell создаёт writer-процесс, меняет target ROUTE |
| tee | Копирование потока | ROUTE_TAG — все с тегом получают копию (поддержка `...` wildcard) |
| socket | Сетевое соединение | Snowball chain с Network Deck (04:xx) |
| select/poll/epoll | Ожидание на нескольких fd | Одна ResultPage, поле `source` различает источники |

### Почему fd (файловые дескрипторы) не нужны

В UNIX файловые дескрипторы — это **таблица индексов** внутри ядра. Каждый fd указывает на struct file. Процесс оперирует числами: 0, 1, 2, 3... Это уровень абстракции, добавленный для универсальности ("everything is a file").

В BoxOS этот уровень **не нужен**, потому что:

1. **Для файлов** — TagFS. Файлы идентифицируются по тегам или file_id, а не по дескрипторам.
2. **Для ввода/вывода** — ResultPage. Одна точка получения всех данных.
3. **Для IPC** — ROUTE. Прямая доставка по PID или тегу.
4. **Для сети** — Network Deck. Сокеты заменены опкодами NET_BIND/NET_RECV.

Нет таблицы fd → нет утечек fd → нет dup2() → нет ошибок "too many open files".

---

## Часть 5: boxlib — Runtime-библиотека

### Зачем нужна

Программист не должен вручную заполнять NotifyPage и проверять ResultPage. boxlib — это тонкий слой, который скрывает механику за человечным API. Аналог libc, но проще (потому что под капотом всего один syscall).

### Инициализация процесса

При запуске процесса через PROC_SPAWN, родитель передаёт начальные данные в Cabin процесса. boxlib при старте читает их:

```c
// Начальные данные, помещённые родителем в Cabin
typedef struct {
    uint32_t spawner_pid;       // PID родителя
    uint32_t display_pid;      // PID display-процесса
    char     context_tags[64]; // Унаследованные теги контекста
} boxlib_init_t;

// Глобальные переменные boxlib (заполняются при старте)
static uint32_t _self_pid;
static uint32_t _spawner_pid;
static uint32_t _display_pid;
```

**Откуда берётся display_pid?** Родитель (shell или другой процесс) знает PID display-процесса и передаёт его дочерним процессам при SPAWN. Display-процесс стартует при загрузке ОС, его PID известен всей системе (первый пользовательский процесс, PID всегда предсказуем, либо хранится в системной переменной).

### API boxlib

```c
// === Вывод ===

// Отправить текст на экран (ROUTE → display process)
void print(const char* text);

// Отправить ошибку на экран (ROUTE → display, flag=ERROR)
void print_err(const char* text);

// === Ввод ===

// Получить строку ввода (LISTEN + ожидание ResultPage)
// Блокирует выполнение до получения данных
char* input(const char* prompt);

// Проверить, есть ли входящие данные (неблокирующий)
bool has_input(void);

// Прочитать одно сообщение из ResultPage (неблокирующий)
// Возвращает NULL если пусто
result_t* receive(void);

// === IPC ===

// Отправить данные процессу по PID
void send(uint32_t target_pid, const void* data, uint16_t size);

// Отправить данные всем процессам с тегом (поддерживает "key:..." wildcard)
void broadcast(const char* tag, const void* data, uint16_t size);

// === Файлы ===

// Прочитать файл по тегам
void* file_read(const char* tags, uint32_t* out_size);

// Записать файл
void file_write(const char* tags, const void* data, uint32_t size);

// === Snowball (продвинутое) ===

// Прочитать файл, трансформировать, отправить другому процессу — одним notify()
void file_read_and_send(const char* tags, uint32_t target_pid);
```

### Внутренняя реализация print()

```c
void print(const char* text) {
    NotifyPage* np = (NotifyPage*)0x1000;
    
    // Routing header
    np->route_target = _display_pid;
    np->route_tag[0] = '\0';  // Не используем route_tag
    
    // Snowball chain: ROUTE → завершение
    np->prefixes[0] = 0xFF40;   // System Deck: ROUTE
    np->prefixes[1] = 0x0000;   // Терминатор
    
    // Payload
    route_payload_t* payload = (route_payload_t*)np->data;
    payload->flags = FLAG_TEXT;
    payload->size = strlen(text);
    memcpy(payload->content, text, payload->size);
    
    // Activate
    np->magic = NOTIFY_MAGIC;
    __asm__ volatile("int $0x80");  // notify()
}
```

### Внутренняя реализация input()

```c
char* input(const char* prompt) {
    // 1. Если есть промпт — вывести его
    if (prompt) print(prompt);
    
    // 2. Ждать данные в ResultPage
    ResultPage* rp = (ResultPage*)0x2000;
    
    while (true) {
        // Проверить: есть ли новые записи?
        if (rp->ring.head != rp->ring.tail) {
            uint32_t idx = rp->ring.head % RESULT_RING_SIZE;
            ResultEntry* entry = &rp->ring.entries[idx];
            
            // Ищем запись с source=HARDWARE (ввод) или source=ROUTE (от другого процесса)
            if (entry->source == SOURCE_HARDWARE || entry->source == SOURCE_ROUTE) {
                // Скопировать данные
                char* result = boxlib_alloc(entry->size + 1);
                memcpy(result, entry->payload, entry->size);
                result[entry->size] = '\0';
                
                // Сдвинуть head (мы прочитали)
                rp->ring.head++;
                return result;
            }
            
            // Пропустить записи source=KERNEL (ответы на наши notify)
            rp->ring.head++;
            continue;
        }
        
        // 3. Пусто — отдать квант времени
        //    Планировщик разбудит нас когда придёт Hot Result (+50)
        boxlib_yield();
    }
}
```

### Внутренняя реализация send()

```c
void send(uint32_t target_pid, const void* data, uint16_t size) {
    NotifyPage* np = (NotifyPage*)0x1000;
    
    np->route_target = target_pid;
    np->route_tag[0] = '\0';
    
    np->prefixes[0] = 0xFF40;   // ROUTE
    np->prefixes[1] = 0x0000;
    
    memcpy(np->data, data, size);
    
    np->magic = NOTIFY_MAGIC;
    __asm__ volatile("int $0x80");
}
```

### Внутренняя реализация file_read_and_send()

```c
// Прочитать файл и отправить другому процессу — ОДНИМ notify()
// Данные никогда не покидают ядро до момента доставки
void file_read_and_send(const char* tags, uint32_t target_pid) {
    NotifyPage* np = (NotifyPage*)0x1000;
    
    np->route_target = target_pid;
    np->route_tag[0] = '\0';
    
    // Snowball: TAG_QUERY → OBJ_READ → ROUTE → terminate
    np->prefixes[0] = 0x0201;   // Storage: TAG_QUERY
    np->prefixes[1] = 0x0205;   // Storage: OBJ_READ
    np->prefixes[2] = 0xFF40;   // System: ROUTE
    np->prefixes[3] = 0x0000;   // Терминатор
    
    // Data: теги для поиска файла
    memcpy(np->data, tags, strlen(tags));
    
    np->magic = NOTIFY_MAGIC;
    __asm__ volatile("int $0x80");
}
```

Это невозможно в UNIX одной операцией. В Linux: open → read → write(pipe)/send(socket) = минимум 3 syscall + 2 копирования. В BoxOS: 1 notify(), данные остаются в ядре.

---

## Часть 6: Обязательные системные компоненты

### Минимальный набор для загрузки

BoxOS при старте должна запустить три процесса:

```
PID 1: init       (теги: system)
PID 2: display    (теги: system, display, app)
PID 3: shell      (теги: system, app)
```

**init** — создаёт display и shell, остаётся фоновым watchdog'ом (перезапускает display/shell при крашах).

**display** — единственный процесс с LISTEN на клавиатуру и мышь. Он принимает данные от приложений через ResultPage (source=ROUTE) и рисует их на экран. Он же отправляет нажатия клавиш процессу, который сейчас в фокусе.

**shell** — интерпретатор команд. Получает ввод от display, отправляет вывод в display.

### Display Process — подробно

Display — это **почтовое отделение** BoxOS. Он:

1. **Владеет экраном**: только у него есть права писать в видеопамять (через Hardware Deck)
2. **Слушает ввод**: LISTEN на клавиатуру и мышь
3. **Маршрутизирует ввод**: отправляет нажатия клавиш процессу в фокусе (по `use` контексту)
4. **Принимает вывод**: процессы делают ROUTE на display_pid с данными для отрисовки

```
Keyboard IRQ → Display (ResultPage, source=HARDWARE)
                  ↓
Display определяет фокус (по use-контексту)
                  ↓
Display → notify([FF:40, 00:00]) → ROUTE к процессу в фокусе
                                          ↓
                                   Процесс получает символ в ResultPage (source=ROUTE)
```

```
Процесс вызывает print("Hello")
    ↓
notify([FF:40, 00:00]) → ROUTE к display_pid
    ↓
Display получает "Hello" в ResultPage (source=ROUTE)
    ↓
Display рисует "Hello" на экране (через Hardware Deck)
```

### Почему не напрямую: Process → Keyboard?

Можно было бы дать каждому процессу LISTEN на клавиатуру. Но тогда:
- Каждый процесс получает **все** нажатия (включая те, что предназначены другим)
- Нет понятия "фокус" — кто сейчас главный?
- Нет буферизации ввода (line editing, backspace, history)

Display решает все три проблемы. Он — единый посредник. Как капитан корабля, который распределяет почту по каютам.

### GUI: Как масштабируется?

В текстовом режиме display — простой: рисует текст, отправляет символы.

В графическом режиме display становится **композитором**:
- Каждое приложение рисует в свой **буфер** (выделенный через BUF_ALLOC, System Deck FF:10)
- Приложение отправляет через ROUTE команду: "мой буфер обновлён, перерисуй"
- Display композитирует буферы на экран

```c
// GUI-приложение:
void draw_frame() {
    // Рисуем в свой буфер (выделен при старте через BUF_ALLOC)
    draw_pixel(buffer, x, y, color);
    // ...
    
    // Уведомляем display: "перерисуй мой буфер"
    send(_display_pid, &(draw_cmd_t){
        .type = CMD_BUFFER_UPDATED,
        .buffer_handle = my_buffer_handle,
        .x = window_x, .y = window_y,
        .w = window_w, .h = window_h
    }, sizeof(draw_cmd_t));
}
```

**Переключение фокуса:**
```
Пользователь кликает на окно приложения B
    ↓
Display получает клик (source=HARDWARE)
    ↓
Display определяет: клик попал в окно приложения B
    ↓
Display делает: use <теги приложения B>
    ↓
Эффект:
  - Планировщик: приложение B получает +20 баллов
  - Ввод: клавиатура идёт приложению B
  - Файлы: фильтруются по контексту B (если применимо)
```

**Всё переключается одной командой `use`**. Это и есть уникальность BoxOS.

---

## Часть 7: Пошаговый разбор — `cat text.txt | grep hello`

### Linux (подробно)

```
Шаг 1: Shell парсит команду, видит "|"
Шаг 2: Shell вызывает pipe(fds[2]) → ядро создаёт pipe, возвращает fds[0]=read, fds[1]=write
Шаг 3: Shell вызывает fork() → создаёт процесс-потомок для cat
          Ядро: копирует всё адресное пространство shell (COW), создаёт новый PCB
Шаг 4: Shell вызывает fork() → создаёт процесс-потомок для grep
          Ядро: ещё одна копия (COW)
Шаг 5: Процесс cat: close(fds[0]), dup2(fds[1], STDOUT_FILENO), close(fds[1])
          → stdout теперь указывает на write-конец pipe
Шаг 6: Процесс cat: execve("/bin/cat", ["cat", "text.txt"])
          → заменяет память процесса на бинарник cat
Шаг 7: Процесс grep: close(fds[1]), dup2(fds[0], STDIN_FILENO), close(fds[0])
          → stdin теперь указывает на read-конец pipe
Шаг 8: Процесс grep: execve("/bin/grep", ["grep", "hello"])
          → заменяет память на бинарник grep
Шаг 9: cat: open("text.txt") → VFS lookup → dentry → inode → syscall
Шаг 10: cat: read(file_fd, buf, 4096) → page cache → memcpy в userspace
Шаг 11: cat: write(STDOUT, buf, n) → memcpy в pipe buffer (kernel space)
           context switch: cat → kernel
Шаг 12: grep: read(STDIN, buf, 4096) → memcpy из pipe buffer в userspace
           context switch: kernel → grep
Шаг 13: grep: обрабатывает строки, ищет "hello"
Шаг 14: grep: write(STDOUT, result, n) → через TTY driver → экран
           context switch: grep → kernel → TTY

Итого: 2 fork(), 2 execve(), 1 pipe(), 4 dup2(), 4 close(),
       минимум 5 read()/write(), ~6 context switch Ring3↔Ring0
       Копирования данных: disk → pagecache → cat → pipe → grep → TTY = 5
```

### BoxOS — Вариант 1: Всё через деки (если grep — операция)

Если нам нужно найти строку "hello" в файле, это BUF_FIND (Operations Deck 01:06).

```
Шаг 1: Shell парсит команду
Шаг 2: Shell формирует Notify Page:
          route_target = 0 (результат себе)
          prefixes = [02:01, 02:05, 01:06, 00:00]
                      TAG_Q  OBJ_R  BUF_F  END
          data = { tags: "name:text.txt", find_pattern: "hello" }
Шаг 3: notify() → переход в Ring 0 (один раз)
Шаг 4: Ядро создаёт Event, кладёт в EventRing, iret → shell свободен
Шаг 5: Guide берёт Event:
          prefix 02:01 → Storage Deck: TAG_QUERY
          → находит file_id по тегам "name:text.txt"
Шаг 6: Guide:
          prefix 02:05 → Storage Deck: OBJ_READ
          → содержимое файла в event->data
Шаг 7: Guide:
          prefix 01:06 → Operations Deck: BUF_FIND
          → в event->data остаются только строки с "hello"
Шаг 8: Guide:
          prefix 00:00 → Execution Deck
          → результат в ResultPage shell'а, +50 баллов
Шаг 9: Shell видит обновление в ResultPage, выводит через print()

Итого: 1 notify(), 0 fork(), 0 pipe(), 0 context switch (кроме одного входа/выхода)
       Копирования: disk → event->data → ResultPage = 2
```

### BoxOS — Вариант 2: grep как отдельная программа

Если grep — это полноценный процесс со сложной логикой (regex и т.д.):

```
Шаг 1: Shell парсит команду, видит "|"
Шаг 2: Shell запускает grep:
          notify([FF:01, 00:00], data: {binary_tags: "name:grep, app", spawn_tags: "app"})
          → System Deck создаёт процесс, shell получает grep_pid в ResultPage
Шаг 3: Shell формирует цепочку "прочитать файл и отправить grep'у":
          route_target = grep_pid
          prefixes = [02:01, 02:05, FF:40, 00:00]
                      TAG_Q  OBJ_R  ROUTE  END
          data = { tags: "name:text.txt" }
Шаг 4: notify() — одна операция
Шаг 5: Ядро:
          Storage: TAG_QUERY → находит файл
          Storage: OBJ_READ → содержимое в event->data
          System: ROUTE → event->pid = grep_pid
          Execution Deck → доставляет содержимое файла в ResultPage grep'а
          Grep получает +50 баллов
Шаг 6: Grep просыпается, видит данные в ResultPage (source=ROUTE)
Шаг 7: Grep обрабатывает строки, находит "hello"
Шаг 8: Grep отправляет результат shell'у:
          route_target = shell_pid (полученный при инициализации из boxlib_init)
          prefixes = [FF:40, 00:00]
          data = "строка с hello\n"
Шаг 9: notify() → ROUTE → Execution Deck → ResultPage shell'а
Шаг 10: Shell видит результат, выводит через print()

Итого: 3 notify(), 0 fork(), 0 pipe()
       Процесс grep создан полноценным PROC_SPAWN (не fork+exec)
       Файл прочитан и доставлен grep'у внутри ядра (шаг 5) — одной цепочкой
```

---

## Часть 8: Проверка на стыки и крайние случаи

### 8.1 — ResultPage переполнена у получателя

**Ситуация:** Процесс A делает ROUTE к процессу B, но ResultPage B полна (ring full).

**Решение:** System Deck при ROUTE проверяет заполненность. Если полна — устанавливает `event->state = STATE_ERROR`, `error_code = ROUTE_ERR_TARGET_FULL`. Execution Deck доставит ошибку **отправителю** (pid не был изменён, т.к. ROUTE не выполнился).

Отправитель получает в своём ResultPage: `status = ROUTE_ERR_TARGET_FULL`. Может повторить позже.

### 8.2 — Получатель умер между ROUTE и доставкой

**Ситуация:** ROUTE изменил pid, но до Execution Deck процесс получатель был убит.

**Решение:** Execution Deck при доставке проверяет PCB. Если процесс TERMINATED — event утилизируется без доставки. Данные теряются. Это нормально: получатель мёртв, доставлять некуда.

Если отправителю важно знать об этом, он должен использовать PROC_INFO для проверки статуса получателя перед отправкой.

### 8.3 — Циклическая пересылка (A → B → A → B...)

**Ситуация:** Процесс A отправляет ROUTE к B. B при получении автоматически отправляет ROUTE к A. Бесконечный цикл?

**Нет проблемы.** Каждый ROUTE — это отдельный notify(). Каждый notify() создаёт новый Event. Нет автоматической пересылки. B должен сознательно вызвать notify() чтобы ответить. Это не рекурсия — это обмен сообщениями.

Если программист специально напишет бесконечный цикл отправки — это его проблема (как while(true) в любом языке). Защита: квоты на количество Event в EventRing (из dynamic_architecture.md).

### 8.4 — ROUTE_TAG: никого с таким тегом нет

**Ситуация:** Процесс делает ROUTE_TAG с тегом "output", но ни один процесс его не имеет.

**Решение:** `event->state = STATE_ERROR`, `error_code = ROUTE_ERR_NO_SUBSCRIBERS`. Ошибка доставляется отправителю через его ResultPage. Приложение решает: повторить, проигнорировать, или сообщить пользователю.

### 8.5 — ROUTE_TAG: слишком много получателей

**Ситуация:** 100 процессов имеют тег "output".

**Решение:** Жёсткий лимит: MAX_ROUTE_TAG_TARGETS = 16. Первые 16 получат копии. Если нужно больше — архитектурная ошибка приложения. 16 достаточно для любого разумного fan-out (экран + логгер + дебаггер + мониторинг...).

### 8.6 — LISTEN: два процесса слушают клавиатуру

**Ситуация:** Shell и IDE оба сделали LISTEN на клавиатуру.

**Решение:** При доставке ввода display-процесс (или ядро при IRQ) использует `use`-контекст для фильтрации. Если `use project:mars` активен и IDE имеет тег `project:mars`, а shell — нет, ввод идёт в IDE.

Если оба совпадают с контекстом или контекст не установлен — ввод получает процесс с **более высоким приоритетом** (score). В крайнем случае — **последний зарегистрировавший** LISTEN (LIFO).

### 8.7 — Snowball chain: данные перезаписываются между деками

**Ситуация:** В цепочке [02:05, 01:06, 00:00] (READ, FIND) — Storage READ кладёт содержимое файла в event->data. Но Operations FIND нуждается в паттерне для поиска. Где он?

**Это предсуществующая проблема Snowball chain, не связанная с ROUTE.** Но раз мы её обнаружили — зафиксируем решение.

**Конвенция передачи данных между деками:**

Каждый дек в цепочке:
1. Читает свои аргументы из **начала** event->data
2. Выполняет операцию
3. Пишет свой результат в event->data, **сдвигая** данные для следующего дека

Для цепочки с несколькими деками, аргументы всех деков упаковываются последовательно в data при формировании NotifyPage:

```
data layout для [02:05, 01:06]:
  [file_id (8)] [offset (8)] [len (8)] [pattern_len (2)] [pattern (N)]
   ↑ Storage читает эти                 ↑ Ops читает эти
```

Storage Deck: читает file_id/offset/len, пишет содержимое файла в data начиная с определённого смещения, ОБНОВЛЯЕТ метаданные о том, где начинаются данные для следующего дека.

Это требует стандартизации формата data — что является задачей для отдельного документа. Для ROUTE это не проблема, т.к. routing info хранится в отдельных полях Event.

### 8.8 — Обратная совместимость

**Что меняется в существующих структурах:**

| Структура | Изменение | Влияние |
|-----------|-----------|---------|
| NotifyPage | +route_target (4B), +route_tag (32B) | data уменьшается на 36B (3836→3800) |
| Event | +route_target (4B), +route_tag (32B), +sender_pid (4B), +route_flags (1B) | +41 байт |
| ResultEntry | +source (1B), +sender_pid (1B) | payload уменьшается на 2B (252→250) |
| Execution Deck | Читает route_flags для заполнения ResultEntry.source | Минимальное изменение |
| System Deck | +3 новых опкода (0x40, 0x41, 0x42) | Новый код, существующий не затронут |

**Процессы без IPC** (не используют ROUTE) продолжают работать без изменений. route_target=0 по умолчанию → Execution Deck доставляет отправителю как раньше.

### 8.9 — Безопасность ROUTE

**Может ли процесс отправить сообщение куда угодно?**

Да, если у него есть тег `app` или `utility`. Это аналог того, что в Linux любой процесс может сделать write() в pipe, если у него есть fd.

**Может ли процесс подделать sender_pid?**

Нет. `sender_pid` заполняется **ядром** (System Deck) из оригинального `event->pid`, который ядро установило при notify(). Процесс не может изменить свой pid.

**Может ли процесс переполнить чужой ResultPage спамом?**

Частично. Если процесс отправляет сотни ROUTE подряд, ResultPage получателя заполнится. Защита:
1. ROUTE возвращает ошибку ROUTE_ERR_TARGET_FULL если ResultPage получателя полна
2. Квоты из dynamic_architecture.md ограничивают количество Event в EventRing
3. Получатель может проверять sender_pid и игнорировать нежелательных отправителей

Для будущего: можно добавить TAG_CHECK перед ROUTE — "может ли отправитель слать получателю?" (whitelist по тегам). Но это не обязательно для v1.

---

## Часть 9: Сводка изменений

### Новые опкоды System Deck (0xFF)

| Opcode | Команда | Описание | Security |
|--------|---------|----------|----------|
| 0x40 | ROUTE | Изменить event->pid на route_target | app, utility, system |
| 0x41 | ROUTE_TAG | Клонировать event для всех процессов с тегом | app, utility, system |
| 0x42 | LISTEN | Зарегистрировать процесс как получатель IRQ | app (keyboard/mouse), utility (hw) |

### Новые структуры ядра

```c
// Таблица слушателей
#define MAX_LISTENERS 64

typedef struct {
    uint32_t pid;
    uint8_t  source_type;   // LISTEN_KEYBOARD=0, LISTEN_MOUSE=1, ...
    uint8_t  flags;
} listen_entry_t;

typedef struct {
    listen_entry_t entries[MAX_LISTENERS];
    uint32_t count;
    spinlock_t lock;
} listen_table_t;
```

### Новые коды ошибок

```c
#define ROUTE_ERR_TARGET_FULL    0x0040  // ResultPage получателя переполнена
#define ROUTE_ERR_NO_SUBSCRIBERS 0x0041  // Нет процессов с указанным тегом
#define ROUTE_ERR_SELF           0x0042  // Попытка ROUTE к самому себе (бессмысленно)
#define LISTEN_ERR_TABLE_FULL    0x0043  // Listen table переполнена
#define LISTEN_ERR_ALREADY       0x0044  // Уже слушает этот источник
```

### Константы source для ResultEntry

```c
#define SOURCE_KERNEL   0   // Ответ на собственный notify()
#define SOURCE_ROUTE    1   // Сообщение от другого процесса
#define SOURCE_HARDWARE 2   // Данные от устройства (клавиатура, мышь, сеть)
```

### Флаги для route_flags / payload

```c
#define FLAG_TEXT    0x00   // Обычный текст (stdout)
#define FLAG_ERROR   0x01   // Ошибка (stderr)
#define FLAG_BINARY  0x02   // Бинарные данные
#define FLAG_COMMAND 0x03   // Команда управления (для display)
```

---

## Часть 10: Чего здесь НЕТ и почему

### Нет потоков (streams)

В UNIX pipe — это бесконечный поток байтов. Процесс читает пока не EOF. В BoxOS нет потоков. Есть **дискретные сообщения**. Каждый ROUTE — одно сообщение. Каждый ResultEntry — одно сообщение.

**Почему это лучше:** Потоки требуют буферизации, управления EOF, и блокировок. Сообщения — атомарны. Получил → обработал → следующее. Нет промежуточных состояний.

**Если нужен большой объём:** DMA Region (из dynamic_architecture.md). Процесс создаёт shared buffer, передаёт handle через ROUTE. Получатель маппит buffer. Zero-copy.

### Нет блокирующих операций

В UNIX read(stdin) **блокирует** процесс. В BoxOS ничего не блокирует. Если данных нет — процесс делает yield и занимается другими делами. Когда данные придут (ResultPage обновится), планировщик начислит +50 баллов и процесс проснётся.

boxlib::input() **выглядит** блокирующей для программиста, но внутри это yield-loop. Процесс не висит в ядре. Ядро свободно.

### Нет select/poll/epoll

В UNIX: процесс ждёт данных из **нескольких** источников одновременно (сеть + клавиатура + pipe). Нужен мультиплексор (select/poll/epoll).

В BoxOS: **всё приходит в один ResultPage**. Поле `source` и `sender_pid` говорят откуда. Процесс просто читает ResultPage по кругу. Мультиплексирование **бесплатно и автоматически**. 

Это следствие архитектуры: один ResultPage = один входящий буфер = нет проблемы мультиплексирования.

### Нет файловых дескрипторов

Числа 0, 1, 2 (stdin, stdout, stderr) — это не концепция BoxOS. Вместо таблицы fd у процесса есть:
- **route_target** (куда отправлять вывод) — одно число, PID
- **ResultPage** (откуда получать всё) — одна страница

Две точки вместо таблицы на 1024 записи.

---

## Часть 11: Финальная проверка

| Критерий | Статус | Обоснование |
|----------|--------|-------------|
| Стабильность | ✅ | ROUTE — простая операция (смена pid). Execution Deck не меняется. Крайние случаи обработаны (dead process, full ResultPage, no subscribers). |
| Не UNIX | ✅ | Нет fd, нет pipe, нет fork+dup2, нет streams, нет select/poll. Полностью иная модель. |
| Уникальность | ✅ | IPC через Snowball chain + доставка по тегам процессов + автоматический фокус через `use` — не существует ни в одной ОС. |
| Не сломается | ✅ | ROUTE — атомарная операция внутри Guide loop. Ошибки обрабатываются стандартным путём (STATE_ERROR → Execution Deck). Нет новых механизмов, которые могут зависнуть. |
| Не переусложнит | ✅ | 3 новых опкода. 0 новых деков. 0 новых страниц. ~200 строк нового кода в System Deck. |
| Вписывается в философию | ✅ | "Снежный ком" катится через деки — ROUTE просто ещё один дек в цепочке. Асинхронность сохранена. Notify/Result модель не нарушена. |
| Человечность | ✅ | Программист видит print(), input(), send(). Не видит prefixes, ROUTE, ResultPage. boxlib скрывает всё. |
| Без больших минусов | ✅ | Единственный компромисс: ResultEntry.payload уменьшился на 2 байта (252→250). Незначительно. |
