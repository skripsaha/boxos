# Низкоуровневые правила: Память, Адреса, Стеки

Данный документ описывает все критичные низкоуровневые детали BoxOS, связанные с физической и виртуальной адресацией, загрузкой ядра, стеками, BSS и cabin-маппингом. Нарушение любого из этих правил приводит к triple fault, бесконечным page fault loop или тихому повреждению данных.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 1. Физическая карта памяти (Boot → Kernel)

```
Адрес          Размер    Назначение
─────────────  ────────  ──────────────────────────────────────────
0x0000-0x04FF  ~1.3KB    BIOS data area, IVT
0x0500-0x04FE  ~2 байта  E820 count + size
0x0500+        var       E820 memory map (до 24 байт * N записей)
0x7C00         512B      Stage1 bootloader (MBR)
0x8000-0x9FFF  8KB       Stage2 bootloader (16 секторов)
0x9000         ~16B      BOOT_INFO структура для ядра
0x9100         512B      TagFS superblock buffer
0x9300         512B      TagFS metadata buffer
0x10000-0x17FFF 32KB     Bounce buffer (Unreal Mode, чанки INT 13h)
0x100000       до 7MB    Kernel РАБОЧИЙ адрес (1MB, linked, Unreal Mode)
динамический   -         Конец загруженного ядра (KERNEL_RUN_ADDR + kernel_loaded_bytes)
0x15D000       -         Фактический конец BSS ядра (~)
0x800000       -         Максимальный адрес конца ядра (KERNEL_MAX_ADDR)
0x820000       16KB      Boot page tables (PML4 + PDPT + PD)
0x900000       -         Стек ядра (растёт вниз)
```

### КРИТИЧНОЕ ПРАВИЛО: Загрузка ядра через Unreal Mode

Ядро **линкуется** на адрес `0x100000` (1MB). Stage2 использует **Unreal Mode**
(Big Real Mode) для загрузки ядра напрямую на финальный адрес без промежуточного
копирования.

**Как работает Unreal Mode:**

1. Stage2 входит в Protected Mode, загружает DS/ES селектором с 4GB лимитом,
   возвращается в Real Mode. Descriptor cache сохраняет 4GB лимит.
2. BIOS INT 13h читает чанки по 32KB (64 секторов) в bounce buffer (0x10000).
3. Каждый чанк копируется `a32 rep movsd` напрямую на 0x100000+ через Unreal Mode.
4. При переходе в Long Mode ядро уже на месте — копирование не нужно.

```nasm
; stage2.asm — tagfs_load_kernel_file:
mov edi, KERNEL_RUN_ADDR       ; 0x100000 — финальный адрес
.load_loop:
    ; INT 13h → bounce buffer (0x10000, 32KB)
    mov esi, KERNEL_BOUNCE_ADDR
    mov ecx, bytes_read / 4
    a32 rep movsd               ; Unreal Mode: 32-bit addressing в Real Mode
    cmp edi, KERNEL_MAX_ADDR    ; не выйти за 0x800000
    jae .size_error
    ; ... loop ...
mov [kernel_loaded_bytes], eax  ; фактический размер

; stage2.asm — long_mode_start:
; Ядро УЖЕ на 0x100000 — копирование не нужно!
; boot_info.kernel_end = KERNEL_RUN_ADDR + kernel_loaded_bytes (выровненный на 4KB)
jmp 0x100000                    ; прыжок на _start ядра
```

**Преимущества Unreal Mode:**
- Нет потолка 512KB — ядро может расти до ~7MB (ограничено KERNEL_MAX_ADDR = 0x800000)
- Размер определяется динамически из TagFS метаданных файла
- Нет дублирования памяти (ядро грузится сразу на целевой адрес)
- BIOS прерывания (INT 0x13, 0x15, 0x10) продолжают работать

**Опасность**: Если изменить linker.ld (`. = 0x100000`) без обновления stage2.asm
(KERNEL_RUN_ADDR), ядро будет выполняться с неправильных адресов → garbage code → crash.

**Файлы, которые ОБЯЗАНЫ быть синхронизированы:**

| Файл | Константа | Текущее значение |
|------|-----------|-----------------|
| `src/kernel/entry/linker.ld` | `. = ...` | `0x100000` |
| `src/boot/stage2/stage2.asm` | `KERNEL_RUN_ADDR` | `0x100000` |
| `src/boot/stage2/stage2.asm` | `KERNEL_BOUNCE_ADDR` | `0x10000` (bounce buffer, 32KB) |
| `src/boot/stage2/stage2.asm` | `KERNEL_MAX_ADDR` | `0x800000` (макс. конец ядра, до page tables) |
| `src/kernel/config/kernel_config.h` | `CONFIG_KERNEL_LOAD_ADDR` | `0x100000ULL` |
| `Makefile` | `KERNEL_MAX_BYTES` | `7340032` (7MB) |

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 2. Cabin — Виртуальная карта процесса

Каждый процесс живёт в своём виртуальном адресном пространстве (свой CR3/PML4).
Раскладка фиксирована:

```
Виртуальный адрес      Размер    Назначение
─────────────────────  ────────  ────────────────────────────
0x0000 - 0x0FFF        4KB       NULL trap zone (не маппится)
0x1000 - 0x2FFF        8KB       Notify Page (User → Kernel)
0x3000 - 0x9FFF        28KB      Result Page (Kernel → User)
0xA000 - ...           var       Code + Data + BSS (entry point)
0x20000 - ...          var       Heap (растёт вверх)
...                    ...       (свободно)
0x7FFFF000             4KB       CPU Caps Page (read-only)
0x7FFFFFFFE000         var       User Stack (растёт вниз)
```

**Определяющие файлы:**

| Файл | Что определяет |
|------|---------------|
| `src/include/cabin_layout.h` | Все адреса и размеры регионов |
| `src/include/boxos_sizes.h` | RESULT_RING_SIZE (должен влезть в Result Page) |
| `src/userspace/boxlib/user.ld` | `USER_BASE` = entry point (= CABIN_CODE_START_ADDR) |
| `src/kernel/core/memory/vmm/vmm.h` | VMM_CABIN_* алиасы |
| `src/include/boxos_addresses.h` | NOTIFY_PAGE_ADDR, RESULT_PAGE_ADDR, CPU_CAPS_PAGE_ADDR |

### КРИТИЧНОЕ ПРАВИЛО: cabin_layout.h — единственный источник правды

Все адреса Cabin определены ТОЛЬКО в `cabin_layout.h`. Остальные файлы
ссылаются на эти макросы. Если нужно изменить раскладку — менять ТОЛЬКО
`cabin_layout.h`, затем проверить все зависимые файлы.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 3. Перекрытие адресов Ядро/Userspace (Баг, март 2026)

### Суть проблемы

Ядро identity-mapped по адресу X (virt = phys = X).
Пользовательский код в cabin маппится с адреса `CABIN_CODE_START_ADDR`.
Если user binary достаточно большой, его виртуальные адреса **перекрывают**
адреса ядра в cabin page tables.

### Как это происходит

1. VMM создаёт cabin с копией kernel identity mapping (2MB large pages)
2. При маппинге user code, 2MB page **разбивается** на 512 × 4KB pages
3. `vmm_map_page()` видит identity-mapped entry (phys == virt) и **разрешает** перезапись
4. User code страницы **затирают** kernel code в page tables cabin'а
5. Timer interrupt (INT 0x20) → ISR пытается выполнить kernel code → выполняет мусор
6. Page fault → ISR тоже затёрт → recursive fault → triple fault

### Текущие адреса и безопасные границы

```
User code:    0xA000 → 0xA000 + binary_size
Kernel code:  0x100000 → ~0x15D000 (растёт с ростом ядра, до 0x800000 max)

Зазор:        0xA000 .. 0x100000 = 0xF6000 = 984KB
```

**Максимальный безопасный размер user binary: ~984KB**

При бинарнике > 984KB user code дойдёт до 0x100000 и затрёт ядро.

Примечание: С Unreal Mode ядро может занимать до 7MB (0x100000-0x800000).
Это не влияет на зазор user↔kernel, т.к. зазор определяется _началом_ ядра (0x100000).

### Что проверять

- При увеличении `CABIN_CODE_START_ADDR` (сдвиг вниз) — зазор уменьшается
- При увеличении размера ядра — зазор уменьшается
- При добавлении больших static массивов в ядро — BSS растёт, `_kernel_end` растёт
- `vmm_map_page()` строка ~508: allows overwrite of identity-mapped pages — это **by design**, но опасно

### Как диагностировать

В QEMU логе (`-d int` или `-d cpu_reset`):

```
RIP = адрес внутри kernel range (0x100000-0x15D000)
НО CR3 = cabin PML4 (не kernel PML4)
→ Kernel code overwritten by user binary
```

Признаки:
- Бесконечный page fault loop (одинаковые CR2, RIP)
- RIP попадает в середину инструкции (не на границу)
- Triple fault сразу после первого timer interrupt в userspace

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 4. Стеки: Кто, Где, Сколько

В BoxOS одновременно существуют **четыре разных стека** на разных этапах:

### 4.1 Bootloader stack (stage2)

```
Адрес:    0x900000 (STACK_BASE в stage2.asm)
Размер:   Ограничен снизу адресом 0x820000 (page tables)
Когда:    Unreal mode → Protected mode → Long mode (до прыжка в kernel)
Файл:     src/boot/stage2/stage2.asm строки 94, 120
```

### 4.2 Kernel initial stack (kernel_entry.asm)

```
Адрес:    0x900000 (hardcoded в kernel_entry.asm)
Размер:   ~7.6MB (от BSS end ~0x15D000 до 0x900000)
Когда:    От _start до конца kernel_main (включая все тесты)
Файл:     src/kernel/entry/kernel_entry.asm строка 34
```

**ПРАВИЛО**: Этот адрес ОБЯЗАН быть ВЫШЕ `__bss_end` ядра.
Текущий `__bss_end` ≈ 0x15D000. Стек на 0x900000 — запас 7.6MB.

**Как проверить**:
```bash
x86_64-elf-objdump -h build/kernel.elf | grep bss
# .bss VMA + Size = BSS end. Должен быть < 0x900000
```

### 4.3 Per-process kernel stack (scheduler)

```
Адрес:    Динамически выделяется через pmm_alloc()
Размер:   CONFIG_KERNEL_STACK_PAGES × 4KB = 16KB (4 pages)
          + CONFIG_KERNEL_STACK_GUARD_PAGES × 4KB = 4KB guard
Когда:    Для каждого process_t при создании
Файл:     src/kernel/core/process/process.c (process_create)
Назначение: RSP0 в TSS — куда CPU переключает стек при переходе Ring3→Ring0
```

**Важно**: `tss_set_rsp0()` вызывается в `context_restore()` / `context_restore_to_frame()`.
Если kernel_stack_top неправильный → стек при interrupt уходит в мусор.

### 4.4 User stack (per-process)

```
Адрес:    0x7FFFFFFFE000 (VMM_USER_STACK_TOP) — растёт вниз
Размер:   CONFIG_USER_STACK_SIZE = 64KB (16 pages)
          + 1 guard page
Когда:    Маппится при создании cabin
Файл:     src/kernel/core/memory/vmm/vmm.h, kernel_config.h
```

### Опасности

1. **BSS перерастает стек**: Добавление больших `static` массивов в kernel
   увеличивает BSS. Если BSS end > 0x900000, стек ядра пишет в BSS.
   **Решение**: Следить за `objdump -h kernel.elf | grep bss`

2. **Kernel stack overflow в interrupt**: 16KB kernel stack на процесс.
   Глубокая рекурсия или большие локальные буферы в обработчиках прерываний
   могут переполнить стек. Guard page поймает, но = process death.

3. **Idle process stack**: Выделяется отдельно в `idle_process_init()` —
   1 page (4KB) через `pmm_alloc(1)`. Минимальный размер.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 5. VMM: Identity Mapping и Page Table Splitting

### Начальный маппинг (boot)

stage2 создаёт identity mapping через **2MB large pages**:
- PML4[0] → PDPT[0] → PD[0..N] (N = кол-во 2MB страниц)
- Минимум 64 записей PD = 128MB mapped
- Каждая запись PD = 2MB, флаг `VMM_FLAG_LARGE_PAGE`

### Kernel VMM переинициализация

`vmm_init()` создаёт **новый** kernel context с:
- Identity mapping всей доступной RAM через 2MB large pages
- Higher-half mapping (0xFFFF800000000000+) для kernel heap и MMIO

### Cabin context creation

`vmm_create_context()` (строки 200-279 vmm.c):
1. Создаёт новый PML4
2. **НЕ шарит** PML4[0] с kernel (user space в этом диапазоне)
3. Создаёт НОВЫЙ PDPT и PD для PML4[0]
4. **Копирует** identity-mapped 2MB large pages из kernel PD (записи 0-127)
5. PML4[256-511] шарит с kernel (higher-half, kernel space)

### Splitting 2MB → 4KB

Когда `vmm_map_page()` вызывается для адреса внутри 2MB large page,
`vmm_get_or_create_table()` (строки 116-143 vmm.c) **разбивает** её:

```
2MB large page (PD entry):  phys=0x000000, flags=PRESENT|WRITABLE|LARGE_PAGE

     ↓ split ↓

512 × 4KB pages (PT entries):
  PT[0]   = phys=0x000000, flags=PRESENT|WRITABLE
  PT[1]   = phys=0x001000, flags=PRESENT|WRITABLE
  ...
  PT[256] = phys=0x100000, flags=PRESENT|WRITABLE  ← ЯДРО ТУТ!
  ...
  PT[511] = phys=0x1FF000, flags=PRESENT|WRITABLE
```

После split, `vmm_map_page()` может **перезаписать** любой PT entry.
На строке ~508 vmm.c есть проверка:

```c
bool is_identity_map = (existing_phys == virt_addr) &&
                       !(existing_flags & VMM_FLAG_USER);
if (!is_identity_map) {
    // error: page already mapped
}
// identity-mapped → разрешаем перезапись user mapping
```

**Это значит**: Любой identity-mapped kernel page может быть перезаписан user mapping.
Это **by design** для low addresses (0x0-0xFFFFF, legacy BIOS area), но **опасно**
для kernel code addresses (0x100000+).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 6. Interrupt handling с cabin CR3

**Ключевой факт**: Когда timer interrupt (INT 0x20) срабатывает во время
выполнения user process, CPU **НЕ меняет CR3**. ISR выполняется с CR3 cabin'а.

### Цепочка обработки interrupt

```
1. User code выполняется (CR3 = cabin PML4, CPL=3)
2. Timer interrupt → CPU автоматически:
   a. Загружает RSP0 из TSS (kernel stack)
   b. Pushes: SS, RSP, RFLAGS, CS, RIP на kernel stack
   c. Загружает CS:RIP из IDT entry
   d. НЕ МЕНЯЕТ CR3!
3. ISR (isr_common в isr.asm) выполняется с cabin CR3
   - push всех регистров
   - call irq_handler → scheduler_yield_from_interrupt()
4. context_save_from_frame() сохраняет состояние текущего процесса
5. context_restore_to_frame() загружает следующий процесс
   - mov CR3, next_process.context.cr3  ← CR3 меняется ТУТ
6. iretq возвращает управление (уже с новым CR3)
```

**Следствие**: ISR код (адреса 0x100000+) ОБЯЗАН быть доступен через cabin page tables.
Если user mapping перезаписал kernel pages → ISR выполняет мусор → crash.

### context_switch.c — критичные функции

| Функция | Что делает | Файл:строка |
|---------|-----------|-------------|
| `context_save_from_frame()` | Сохраняет регистры из interrupt frame | context_switch.c:56 |
| `context_restore_to_frame()` | Восстанавливает регистры + **меняет CR3** + FPU | context_switch.c:97 |
| `tss_set_rsp0()` | Устанавливает kernel stack для ring3→ring0 перехода | context_switch.c:35 |

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 7. Notify/Result страницы — размеры и static_assert

Структуры Notify и Result page имеют `_Static_assert` на точный размер:

### Notify Page (8KB, 2 pages)

```
Файлы:
  - src/kernel/core/ipc/notify_page.h     (kernel)
  - src/userspace/boxlib/include/box/notify.h (userspace)

Структура notify_page_t:
  - Заголовок + поля ~434 байт
  - _reserved[7854] padding
  - Итого = 8192 байт = CABIN_NOTIFY_PAGE_SIZE

_Static_assert(sizeof(notify_page_t) == CABIN_NOTIFY_PAGE_SIZE)
```

### Result Page (28KB, 7 pages)

```
Файлы:
  - src/kernel/core/ipc/result_page.h     (kernel)
  - src/userspace/boxlib/include/box/result.h (userspace)
  - src/include/boxos_sizes.h              (RESULT_RING_SIZE)

Структура result_page_t:
  - Заголовок 24 байта
  - result_entry_t entries[RESULT_RING_SIZE]  (RESULT_RING_SIZE = 111)
  - Итого ≤ 28672 байт = CABIN_RESULT_PAGE_SIZE

_Static_assert(sizeof(result_page_t) <= CABIN_RESULT_PAGE_SIZE)
```

### ПРАВИЛО: Kernel и Userspace структуры ОБЯЗАНЫ быть идентичны

Изменение полей в `notify_page.h` (kernel) **ОБЯЗАТЕЛЬНО** требует
такого же изменения в `box/notify.h` (userspace), и наоборот.
То же для `result_page.h` / `box/result.h`.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 8. Чеклист: Что проверять при изменениях

### При изменении Cabin Layout (адреса/размеры)

- [ ] `cabin_layout.h` — все адреса и размеры
- [ ] `boxos_sizes.h` — RESULT_RING_SIZE влезает в Result Page
- [ ] `vmm.h` — комментарии VMM_CABIN_* актуальны
- [ ] `vmm.c` — `pmm_alloc()` в `vmm_create_cabin()` использует правильное кол-во pages
- [ ] `vmm.c` — `vmm_map_notify_page()` и `vmm_map_result_page()` маппят правильное кол-во pages
- [ ] `notify_page.h` (kernel) — `_reserved[]` padding корректен, static_assert проходит
- [ ] `result_page.h` (kernel) — static_assert на размер проходит
- [ ] `box/notify.h` (userspace) — `_reserved[]` идентичен ядерному
- [ ] `box/result.h` (userspace) — static_assert идентичен
- [ ] `user.ld` — `USER_BASE` = `CABIN_CODE_START_ADDR`
- [ ] `boxos_addresses.h` — комментарии актуальны
- [ ] **User binary не перекрывает kernel**: `CABIN_CODE_START_ADDR + max_binary_size < 0x100000`

### При изменении адреса загрузки ядра

- [ ] `linker.ld` — `. = NEW_ADDR`
- [ ] `stage2.asm` — `KERNEL_RUN_ADDR equ NEW_ADDR`
- [ ] `stage2.asm` — `KERNEL_MAX_ADDR` всё ещё выше реального конца ядра
- [ ] `stage2.asm` — bounce buffer (KERNEL_BOUNCE_ADDR) не перекрывает Stage2 или BOOT_INFO
- [ ] `kernel_config.h` — `CONFIG_KERNEL_LOAD_ADDR = NEW_ADDR`
- [ ] `kernel_entry.asm` — стек (0x900000) всё ещё ВЫШЕ `__bss_end`
- [ ] `vmm.c` — identity mapping тест НЕ пишет в kernel range
- [ ] **Зазор user↔kernel**: `NEW_ADDR - CABIN_CODE_START_ADDR > max_user_binary_size`

### При добавлении больших static переменных в ядро

- [ ] Собрать ядро: `make clean && make`
- [ ] Проверить BSS: `x86_64-elf-objdump -h build/kernel.elf | grep bss`
- [ ] BSS end (VMA + Size) < 0x900000 (kernel stack)
- [ ] BSS end < 0x820000 (boot page tables) — нужен только при холодном старте
- [ ] kernel.bin size < 7340032 (7MB, KERNEL_MAX_BYTES в Makefile; Unreal Mode загрузка)

### При отладке page fault loop

1. Включить QEMU log: `-d int,cpu_reset -D boxos_qemu.log`
2. Найти первый fault (не повторяющийся): `grep "v=0e" boxos_qemu.log | head -5`
3. Проверить CR3 — это kernel PML4 или cabin PML4?
4. Проверить RIP — попадает ли в kernel range (0x100000-0x15D000)?
5. Если CR3 = cabin И RIP = kernel range → вероятно address overlap
6. Проверить: `CABIN_CODE_START_ADDR + binary_size` vs `kernel start`
7. Дизассемблировать: `x86_64-elf-objdump -d build/kernel.elf | grep "RIP_VALUE"`

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

## 9. Быстрая справка: Ключевые адреса и значения

| Что | Адрес / Значение | Файл |
|-----|-----------------|------|
| Kernel linked address | `0x100000` | linker.ld |
| Bounce buffer (Unreal Mode) | `0x10000` (32KB) | stage2.asm |
| Kernel max binary size | 7MB (7340032) | Makefile, stage2.asm |
| Kernel max end address | `0x800000` (KERNEL_MAX_ADDR) | stage2.asm |
| Kernel BSS end (приблизительно) | `~0x15D000` | objdump kernel.elf |
| Kernel initial stack | `0x900000` | kernel_entry.asm |
| Boot page tables | `0x820000` | stage2.asm |
| Cabin NULL trap | `0x0000-0x0FFF` | cabin_layout.h |
| Cabin Notify | `0x1000-0x2FFF` (8KB) | cabin_layout.h |
| Cabin Result | `0x3000-0x9FFF` (28KB) | cabin_layout.h |
| Cabin Code start | `0xA000` | cabin_layout.h |
| User stack top | `0x7FFFFFFFE000` | vmm.h |
| User heap base | `0x20000` | vmm.h |
| Kernel heap (higher-half) | `0xFFFF800000000000` | vmm.h |
| RESULT_RING_SIZE | 111 entries | boxos_sizes.h |
| Per-process kernel stack | 4 pages (16KB) + 1 guard | kernel_config.h |
| User stack | 16 pages (64KB) + 1 guard | kernel_config.h |
| Idle process stack | 1 page (4KB) | idle.c |
| GDT Kernel Code | `0x08` | gdt.c |
| GDT Kernel Data | `0x10` | gdt.c |
| GDT User Code | `0x1B` (0x18 \| 3) | gdt.c |
| GDT User Data | `0x23` (0x20 \| 3) | gdt.c |
| Syscall vector | `0x80` (INT 128) | isr.asm |
| Timer IRQ vector | `0x20` (INT 32) | isr.asm |
