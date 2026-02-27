СИСТЕМА АВТОЗАПУСКА ПРОГРАММ В BOXOS

Этот документ описывает механизм запуска пользовательских программ из шелла
по имени — без явного пути и без необходимости встраивать программу в статическую
таблицу команд.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. КОНЦЕПЦИЯ

   В BoxOS нет понятия PATH. Вместо этого TagFS хранит каждый файл с набором
   тегов. Исполняемые программы помечаются системным тегом "app" или "utility".
   Имя файла в метаданных (поле filename[32]) — это и есть имя команды.

   Когда пользователь вводит команду, которой нет в статической таблице шелла,
   шелл не сразу печатает "Unknown command". Сначала он делает один IPC-вызов
   к ядру: "найди в TagFS файл с таким именем и признаком исполняемого, запусти
   его". Ядро ищет, создаёт процесс, возвращает PID. Если файл не найден —
   только тогда шелл печатает сообщение об ошибке.

   Пример:
       > snake
       Шелл не находит "snake" в своей таблице.
       Шелл вызывает proc_exec("snake").
       Ядро находит файл с filename="snake" и системным тегом key="app".
       Ядро создаёт процесс, загружает бинарь, переводит в READY.
       Шелл получает PID и печатает: Running snake (pid 7)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

2. ТРЕБОВАНИЯ К ИСПОЛНЯЕМЫМ ФАЙЛАМ

   Чтобы файл можно было запустить через proc_exec, он должен:

   а) Иметь поле filename (до 32 символов) равное имени команды без расширения.
      Расширения в TagFS не обязательны. Файл должен называться ровно так,
      как его вызывает пользователь: "snake", "calc", "hello".

   б) Иметь хотя бы один из двух системных тегов:
          key="app"     type=TAGFS_TAG_SYSTEM
          key="utility" type=TAGFS_TAG_SYSTEM

      Тег типа TAGFS_TAG_USER с тем же ключом НЕ считается исполняемым.
      Это гарантирует, что пользователь не может случайно запустить
      произвольный файл, добавив к нему пользовательский тег.

   в) Иметь ненулевой размер, не превышающий PROC_SPAWN_MAX_BINARY_SIZE
      (определён в kernel_config.h как CONFIG_PROC_MAX_BINARY_SIZE).

   г) Не иметь флага TAGFS_FILE_TRASHED и не иметь флага TAGFS_FILE_HIDDEN.
      tagfs_list_all_files() уже исключает трешованные файлы.

   Минимальный пример создания исполняемого файла через шелл:
       create snake
       tag snake app:yes    # добавить системный тег — только через Storage Deck
                            # с нужными правами, не через shell-команду tag

   Правильнее: устанавливать системный тег через Storage Deck с type=SYSTEM
   при создании файла из билд-системы или инсталлятора.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

3. IPC-ПРОТОКОЛ: SYSTEM_OP_PROC_EXEC (опкод 0x06)

   Дек: DECK_SYSTEM (0xFF)
   Опкод: 0x06

   Этот опкод добавляется в system_deck.h рядом с существующими:
       #define SYSTEM_OP_PROC_SPAWN   0x01
       #define SYSTEM_OP_PROC_KILL    0x02
       #define SYSTEM_OP_PROC_INFO    0x03
       #define SYSTEM_OP_PROC_EXEC    0x06   // <-- новый

   3.1. Структура запроса (proc_exec_event_t) — 192 байта:

       typedef struct __packed {
           char     filename[32];    // имя файла для поиска (null-terminated)
           uint8_t  reserved[160];   // зарезервировано, нули
       } proc_exec_event_t;

       Правила:
       - filename должен быть null-terminated в пределах 32 байт.
       - filename[0] != '\0' (пустое имя — ошибка SYSTEM_ERR_INVALID_ARGS).
       - Если strnlen(filename, 32) == 32 — не null-terminated, ошибка.

   3.2. Структура ответа (proc_exec_response_t) — 192 байта:

       typedef struct __packed {
           uint32_t new_pid;         // PID созданного процесса (0 если ошибка)
           uint32_t reserved;        // выравнивание
           uint8_t  reserved2[184];  // зарезервировано
       } proc_exec_response_t;

       При ошибке: event->state = EVENT_STATE_ERROR, new_pid = 0.
       При успехе: event->state = EVENT_STATE_COMPLETED, new_pid = PID.

   3.3. Права доступа (security gate):

       Процесс, вызывающий SYSTEM_OP_PROC_EXEC, должен иметь один из тегов:
           "proc_spawn", "utility", "system"

       Это те же права, что у SYSTEM_OP_PROC_SPAWN.
       Шелл запускается с тегом "utility", поэтому имеет доступ.

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

4. ПОТОК ВЫПОЛНЕНИЯ

   Пользовательская сторона (шелл → boxlib):

       1. Пользователь вводит "snake".
       2. executor_run() перебирает g_commands[] — совпадения нет.
       3. Вызывается proc_exec("snake") из box/system.h.
       4. proc_exec() формирует событие:
              box_notify_prepare()
              box_notify_add_prefix(0xFF, 0x06)
              data[0..31] = "snake\0..."
              box_notify_write_data(data, 192)
              box_notify_execute()
       5. proc_exec() ждёт ответа: box_result_wait(&result, 5000).
       6. Если result.error_code == BOX_OK:
              читает new_pid из result.payload[0..3]
              возвращает (int)new_pid
          Иначе возвращает -1.
       7. executor_run() проверяет результат:
              pid > 0  → печатает "Running snake (pid N)"
              pid <= 0 → печатает "Unknown command: snake"

   Ядерная сторона (system_deck_proc_exec):

       1. Получает событие с опкодом 0x06.
       2. Копирует proc_exec_event_t из event->data.
       3. Валидирует filename: не пустой, null-terminated.
       4. Проверяет лимит процессов (process_get_count() >= PROCESS_MAX_COUNT).
       5. Получает список всех активных файлов:
              uint32_t file_ids[TAGFS_MAX_FILES];
              int count = tagfs_list_all_files(file_ids, TAGFS_MAX_FILES);
       6. Для каждого file_id:
              TagFSMetadata* meta = tagfs_get_metadata(file_id);
              if (!meta) continue;
              if (!(meta->flags & TAGFS_FILE_ACTIVE)) continue;
              if (strcmp(meta->filename, filename) != 0) continue;
              // Проверить теги
              bool is_exec = false;
              for (int t = 0; t < meta->tag_count; t++) {
                  if (meta->tags[t].type == TAGFS_TAG_SYSTEM) {
                      if (strcmp(meta->tags[t].key, "app") == 0 ||
                          strcmp(meta->tags[t].key, "utility") == 0) {
                          is_exec = true;
                          break;
                      }
                  }
              }
              if (is_exec) { found_id = file_id; break; }
       7. Если не найден → deliver_response(SYSTEM_ERR_EXEC_NOT_FOUND).
       8. Проверяет размер файла (meta->size > 0 и <= PROC_SPAWN_MAX_BINARY_SIZE).
       9. Выделяет буфер: pmm_alloc_zero(pages_needed).
      10. Открывает и читает файл:
              TagFSFileHandle* fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
              tagfs_read(fh, virt_buf, file_size);
              tagfs_close(fh);
      11. Создаёт процесс с тегом найденного типа:
              process_t* proc = process_create(exec_tag);
              // exec_tag = "app" или "utility" в зависимости от найденного тега
      12. Загружает бинарь:
              process_load_binary(proc, virt_buf, file_size);
      13. Освобождает буфер: pmm_free(buf, pages_needed).
      14. Переводит процесс в готовность:
              process_set_state(proc, PROC_READY);
      15. Возвращает PID: deliver_response(SYSTEM_ERR_SUCCESS, &response).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

5. КОДЫ ОШИБОК

   Добавляются в system_deck_process.h:

       #define SYSTEM_ERR_NOT_EXECUTABLE  0x000D
           // Файл найден, но не имеет системного тега "app" или "utility".
           // На практике это не должно возникать через proc_exec, так как
           // поиск идёт именно по наличию этих тегов. Резервируется для
           // будущего использования или прямых вызовов.

       #define SYSTEM_ERR_EXEC_NOT_FOUND  0x000E
           // Файл с указанным именем и исполняемым тегом не найден в TagFS.
           // Именно этот код означает "Unknown command" на уровне шелла.

   Коды, уже существующие и используемые proc_exec:

       SYSTEM_ERR_SUCCESS           0x0000  — успех, new_pid содержит PID
       SYSTEM_ERR_INVALID_ARGS      0x0001  — пустое или не-terminated имя
       SYSTEM_ERR_NO_MEMORY         0x0002  — pmm_alloc_zero вернул NULL
       SYSTEM_ERR_PROCESS_LIMIT     0x0003  — достигнут PROCESS_MAX_COUNT
       SYSTEM_ERR_LOAD_FAILED       0x0005  — process_load_binary вернул != 0
       SYSTEM_ERR_CABIN_FAILED      0x0006  — process_create вернул NULL
       SYSTEM_ERR_SIZE_LIMIT        0x0007  — файл слишком большой или нулевой

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

6. СПИСОК ФАЙЛОВ ДЛЯ ИЗМЕНЕНИЯ

   Ядро:
       src/kernel/core/guide/system_deck.h
           — добавить: #define SYSTEM_OP_PROC_EXEC 0x06

       src/kernel/core/guide/system_deck_process.h
           — добавить: SYSTEM_ERR_NOT_EXECUTABLE = 0x000D
                       SYSTEM_ERR_EXEC_NOT_FOUND  = 0x000E
                       proc_exec_event_t
                       proc_exec_response_t
                       int system_deck_proc_exec(Event* event);

       src/kernel/core/guide/system_deck_process.c
           — добавить: реализацию system_deck_proc_exec()
           — добавить: #include "tagfs.h"

       src/kernel/core/guide/system_deck.c
           — в system_deck_handler(): добавить case SYSTEM_OP_PROC_EXEC
           — в system_deck_check_permission(): добавить case SYSTEM_OP_PROC_EXEC
             с теми же правами, что у SYSTEM_OP_PROC_SPAWN

   Userspace (boxlib):
       src/userspace/boxlib/include/box/system.h
           — добавить: #define BOX_SYSTEM_PROC_EXEC 0x06
           — добавить: int proc_exec(const char* filename);

       src/userspace/boxlib/src/system.c
           — добавить: реализацию proc_exec()
           — ВНИМАНИЕ: именно src/userspace/boxlib/src/system.c,
             а не src/core/system.c. В src/system.c живут proc_info, exit,
             proc_tag_*. В src/core/system.c — reboot, shutdown, defrag, etc.
             proc_exec логически относится к src/system.c (операции с процессами).

   Шелл:
       src/userspace/shell/executor.c
           — добавить: #include "box/system.h"
           — в executor_run(): после цикла for, перед установкой g_error_message,
             вызвать proc_exec() и по результату либо сообщить PID, либо
             установить "Unknown command: X"

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

7. ТОЧНЫЙ КОД ДЛЯ РЕАЛИЗАЦИИ

   7.1. system_deck.h (добавить в группу Process Management):

       #define SYSTEM_OP_PROC_EXEC      0x06

   7.2. system_deck_process.h (добавить после существующих ошибок):

       #define SYSTEM_ERR_NOT_EXECUTABLE  0x000D
       #define SYSTEM_ERR_EXEC_NOT_FOUND  0x000E

       typedef struct __packed {
           char    filename[32];
           uint8_t reserved[160];
       } proc_exec_event_t;

       typedef struct __packed {
           uint32_t new_pid;
           uint32_t reserved;
           uint8_t  reserved2[184];
       } proc_exec_response_t;

       int system_deck_proc_exec(Event* event);

   7.3. system_deck_process.c (новая функция, добавить после system_deck_proc_info):

       int system_deck_proc_exec(Event* event) {
           if (!event) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: NULL event\n");
               return -1;
           }

           proc_exec_event_t exec_event;
           memset(&exec_event, 0, sizeof(exec_event));
           memcpy(&exec_event, event->data, sizeof(proc_exec_event_t));

           if (strnlen(exec_event.filename, sizeof(exec_event.filename))
                   == sizeof(exec_event.filename)) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: filename not null-terminated\n");
               deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
               return -1;
           }

           exec_event.filename[sizeof(exec_event.filename) - 1] = '\0';

           if (exec_event.filename[0] == '\0') {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: empty filename\n");
               deliver_response(event, SYSTEM_ERR_INVALID_ARGS, NULL, 0);
               return -1;
           }

           debug_printf("[SYSTEM_DECK] PROC_EXEC: looking for '%s'\n",
                   exec_event.filename);

           if (process_get_count() >= PROCESS_MAX_COUNT) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: process limit reached\n");
               deliver_response(event, SYSTEM_ERR_PROCESS_LIMIT, NULL, 0);
               return -1;
           }

           // Поиск файла в TagFS по имени + системный тег "app"/"utility"
           uint32_t file_ids[TAGFS_MAX_FILES];
           int file_count = tagfs_list_all_files(file_ids, TAGFS_MAX_FILES);

           uint32_t found_id = 0;
           const char* found_tag = NULL;

           for (int i = 0; i < file_count; i++) {
               TagFSMetadata* meta = tagfs_get_metadata(file_ids[i]);
               if (!meta || !(meta->flags & TAGFS_FILE_ACTIVE)) {
                   continue;
               }
               if (strcmp(meta->filename, exec_event.filename) != 0) {
                   continue;
               }
               for (uint8_t t = 0; t < meta->tag_count; t++) {
                   if (meta->tags[t].type == TAGFS_TAG_SYSTEM) {
                       if (strcmp(meta->tags[t].key, "app") == 0) {
                           found_id  = file_ids[i];
                           found_tag = "app";
                           break;
                       }
                       if (strcmp(meta->tags[t].key, "utility") == 0) {
                           found_id  = file_ids[i];
                           found_tag = "utility";
                           break;
                       }
                   }
               }
               if (found_id != 0) {
                   break;
               }
           }

           if (found_id == 0) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: '%s' not found\n",
                       exec_event.filename);
               deliver_response(event, SYSTEM_ERR_EXEC_NOT_FOUND, NULL, 0);
               return -1;
           }

           TagFSMetadata* meta = tagfs_get_metadata(found_id);
           uint64_t file_size = meta->size;

           if (file_size == 0 || file_size > PROC_SPAWN_MAX_BINARY_SIZE) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: invalid file size %lu\n",
                       file_size);
               deliver_response(event, SYSTEM_ERR_SIZE_LIMIT, NULL, 0);
               return -1;
           }

           size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
           void* phys_buf = pmm_alloc_zero(pages_needed);
           if (!phys_buf) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: pmm_alloc_zero failed\n");
               deliver_response(event, SYSTEM_ERR_NO_MEMORY, NULL, 0);
               return -1;
           }

           void* virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

           TagFSFileHandle* fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
           if (!fh) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_open failed\n");
               pmm_free(phys_buf, pages_needed);
               deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
               return -1;
           }

           int read_result = tagfs_read(fh, virt_buf, file_size);
           tagfs_close(fh);

           if (read_result != 0) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: tagfs_read failed\n");
               pmm_free(phys_buf, pages_needed);
               deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
               return -1;
           }

           process_t* proc = process_create(found_tag);
           if (!proc) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: process_create failed\n");
               pmm_free(phys_buf, pages_needed);
               deliver_response(event, SYSTEM_ERR_CABIN_FAILED, NULL, 0);
               return -1;
           }

           int load_result = process_load_binary(proc, virt_buf, file_size);
           pmm_free(phys_buf, pages_needed);

           if (load_result != 0) {
               debug_printf("[SYSTEM_DECK] PROC_EXEC: process_load_binary failed\n");
               process_destroy(proc);
               deliver_response(event, SYSTEM_ERR_LOAD_FAILED, NULL, 0);
               return -1;
           }

           process_set_state(proc, PROC_READY);

           proc_exec_response_t response;
           memset(&response, 0, sizeof(response));
           response.new_pid = proc->pid;

           debug_printf("[SYSTEM_DECK] PROC_EXEC: SUCCESS - '%s' -> PID %u\n",
                   exec_event.filename, proc->pid);
           deliver_response(event, SYSTEM_ERR_SUCCESS, &response, sizeof(response));
           return 0;
       }

   7.4. system_deck.c — в system_deck_handler(), добавить case:

       case SYSTEM_OP_PROC_EXEC:
           return system_deck_proc_exec(event);

   7.5. system_deck.c — в system_deck_check_permission(), добавить case:

       case SYSTEM_OP_PROC_EXEC:
           return snapshot_has_tag(tags_snapshot, "proc_spawn") ||
                  snapshot_has_tag(tags_snapshot, "utility") ||
                  snapshot_has_tag(tags_snapshot, "system");

   7.6. box/system.h (userspace) — добавить:

       #define BOX_SYSTEM_PROC_EXEC    0x06

       int proc_exec(const char* filename);

   7.7. src/userspace/boxlib/src/system.c — добавить функцию:
        (ВАЖНО: именно в этот файл, не в src/core/system.c)

       int proc_exec(const char* filename) {
           if (!filename || filename[0] == '\0') {
               return -1;
           }

           box_notify_prepare();
           box_notify_add_prefix(BOX_SYSTEM_DECK_ID, BOX_SYSTEM_PROC_EXEC);

           uint8_t data[192];
           memset(data, 0, 192);

           size_t name_len = strlen(filename);
           if (name_len >= 32) {
               return -1;
           }
           memcpy(data, filename, name_len);

           box_notify_write_data(data, 192);
           box_event_id_t event_id = box_notify_execute();
           if (event_id == 0) {
               return -1;
           }

           box_result_entry_t result;
           if (!box_result_wait(&result, 5000)) {
               return -1;
           }

           if (result.error_code != BOX_OK) {
               return -1;
           }

           if (result.size < 4) {
               return -1;
           }

           uint32_t new_pid = 0;
           memcpy(&new_pid, result.payload, 4);
           return (int)new_pid;
       }

   7.8. src/userspace/shell/executor.c — изменить конец executor_run():

       // Вместо текущего кода с g_error_message:
       int pid = proc_exec(command_name);
       if (pid > 0) {
           // Успешный запуск — сообщить пользователю
           // (вывод через соответствующий print-механизм шелла)
           return 0;
       }

       // Файл не найден или ошибка — стандартное сообщение
       memcpy(g_error_message, "Unknown command: ", 17);
       size_t cmd_len = strlen(command_name);
       if (cmd_len > 100) cmd_len = 100;
       memcpy(g_error_message + 17, command_name, cmd_len);
       g_error_message[17 + cmd_len] = '\0';
       return -1;

       Также добавить в начало файла:
       #include "box/system.h"

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

8. ВАЖНЫЕ ЗАМЕЧАНИЯ ПО РЕАЛИЗАЦИИ

   а) Два файла system.c в boxlib.
      src/userspace/boxlib/src/system.c    — proc_info, exit, proc_tag_*
      src/userspace/boxlib/src/core/system.c — reboot, shutdown, defrag, etc.
      Функция proc_exec должна идти в src/system.c (операции с процессами).

   б) deliver_response() — статическая функция в system_deck_process.c.
      Она уже доступна внутри этого файла. Новая функция system_deck_proc_exec
      добавляется в тот же .c файл и использует её напрямую.

   в) tagfs.h нужно включить в system_deck_process.c.
      Сейчас там нет #include "tagfs.h". Его нужно добавить.
      Аналогично нужен #include "vmm.h" (уже есть) для vmm_phys_to_virt.

   г) Освобождение pmm-буфера.
      pmm_free вызывается ПЕРЕД process_destroy в случае ошибки process_load_binary.
      Это критично: если освободить после destroy, буфер всё равно правильно
      освобождается, но последовательность должна быть явной.

   д) Теги процесса.
      process_create(found_tag) передаёт строку "app" или "utility" как теги
      нового процесса. Это корректно — строка передаётся как единственный тег,
      что соответствует тому, как шелл создаётся с тегом "utility".

   е) Права шелла.
      Шелл должен иметь тег "utility" или "proc_spawn" для вызова PROC_EXEC.
      Проверить, что при создании процесса шелла этот тег передаётся.

   ж) Процесс остаётся фоновым.
      proc_exec запускает программу и немедленно возвращает PID.
      Шелл не ждёт завершения программы. Если нужно ожидание —
      это отдельная задача, требующая механизма wait/join (не реализован).

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

9. ДИАГРАММА ПОТОКА

   [Пользователь] "snake"
         |
         v
   [Шелл: executor_run()]
         |
         |-- совпадение в g_commands[]? --> ДА --> выполнить built-in
         |
         НЕТ
         |
         v
   [proc_exec("snake")]   -- boxlib userspace
         |
         | IPC: DECK_SYSTEM / 0x06 / data="snake"
         v
   [system_deck_proc_exec()]  -- ядро
         |
         |-- tagfs_list_all_files()
         |-- цикл: strcmp(meta->filename, "snake")
         |         && TAGFS_TAG_SYSTEM && (key=="app" || key=="utility")
         |
         |-- НЕ НАЙДЕН --> SYSTEM_ERR_EXEC_NOT_FOUND --> return -1
         |
         НАЙДЕН: file_id=5, tag="app"
         |
         |-- pmm_alloc_zero()
         |-- tagfs_open() + tagfs_read() + tagfs_close()
         |-- process_create("app")
         |-- process_load_binary()
         |-- pmm_free()
         |-- process_set_state(PROC_READY)
         |-- deliver_response(SUCCESS, new_pid=7)
         |
         v
   [proc_exec] возвращает 7
         |
         v
   [Шелл] печатает: Running snake (pid 7)
