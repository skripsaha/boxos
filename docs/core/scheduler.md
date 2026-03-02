Название: Smart Score Scheduler (Семантический планировщик) Тип: Балльно-приоритетная система распределения ресурсов CPU. Главный принцип: Фокус на пользователе и актуальности данных (Hot Data).

1. Философия планирования
В отличие от классических ОС, которые используют сложные формулы квантования времени, Scheduler в BoxOS опирается на Баллы Приоритета (Scores). Система всегда отдает квант времени тому процессу, у которого в данный момент высший балл.

2. Система начисления баллов (The Scoring System)
Баллы динамически пересчитываются при каждом вызове scheduler_calculate_score().

| Категория | Баллы | Триггер (Условие) |
|-----------|-------|-------------------|
| Hot Result | +50 | Процесс только что получил данные (result_there = true). Бонус затухает с consecutive_runs и временем простоя. |
| Result Overflow Recovery | +200 | Процесс восстанавливается после переполнения Result Ring (WAIT_OVERFLOW). |
| Context Match | +20 | Теги процесса совпадают с текущим глобальным use контекстом. |
| Starvation (Mild) | +10 | Процесс не получал CPU более 10 тиков (~100мс при 100Hz). |
| Starvation (Severe) | +30 | Процесс не получал CPU более 50 тиков (~500мс). |
| Starvation (Critical) | +100 | Процесс не получал CPU более 100 тиков (~1 секунда). |
| System/Utility | +5 | Процесс имеет системный тег utility или system. |
| Consecutive Run Penalty | -100 | Процесс занимал CPU 5+ раз подряд (SCHEDULER_MAX_CONSECUTIVE_RUNS). |

Обоснование баллов:

Hot Result (+50 с затуханием): Самый высокий стандартный приоритет. Если данные только что вышли из ядра, они ещё лежат в кэше процессора (L1/L2). Бонус уменьшается на 5 за каждый consecutive_run и дополнительно за время простоя, предотвращая монополизацию CPU.

Overflow Recovery (+200): Экстренный приоритет для процессов, потерявших результаты из-за переполнения Result Ring. Должны получить CPU как можно быстрее.

Context Match (+20): Если пользователь ввел `use project:mars`, то все процессы с этим тегом становятся важнее. Планировщик «понимает», над чем работает человек.

Multi-tier Starvation (+10/+30/+100): Три уровня защиты от «голодания» обеспечивают плавное повышение приоритета забытых процессов.

Consecutive Run Penalty (-100): Если процесс занимал CPU 5+ раз подряд, он получает штраф, давая шанс другим.

3. Механика Контекста (use) и Наследования
Scheduler тесно связан с командой `use`.

Глобальный контекст: Пользователь устанавливает активный набор тегов (максимум 16).

Сверка тегов: При выборе процесса планировщик сравнивает теги в process_t.tags с active_tags через scheduler_matches_use_context().

Наследование: При spawn() дочерний процесс наследует теги родителя. Это гарантирует, что если ты запустил компилятор внутри «рабочего контекста», все его подпроцессы тоже будут в приоритете.

4. Алгоритм выбора процесса (The Selection Loop)
Планировщик вызывается из scheduler_yield_from_interrupt() при каждом timer interrupt (INT 0x20, 100Hz).

```c
process_t* scheduler_select_next(void) {
    process_t* best = NULL;
    int32_t best_score = INT32_MIN;

    for (each process in ProcessList) {
        if (state == CRASHED || state == DONE) continue;
        if (state == WORKING || state == CREATED) {
            int32_t score = scheduler_calculate_score(proc);
            if (score > best_score) {
                best_score = score;
                best = proc;
            }
        }
    }

    if (!best) best = idle_process;  // fallback to HLT loop
    return best;
}
```

Ключевые особенности реализации:
- Три уровня starvation (mild/severe/critical) вместо линейного накопления
- Hot Result бонус затухает с consecutive_runs
- Consecutive run penalty при 5+ подряд запусках
- Overflow Recovery (+200) как экстренный приоритет
- Фильтрация по состоянию процесса (CRASHED, DONE пропускаются)
- Score clamped to INT32_MAX/2 для предотвращения overflow

5. Оптимизация Idle (Энергоэффективность)
Если не найден ни один подходящий процесс, scheduler возвращает idle process:

- Idle process выполняет `hlt` в бесконечном цикле
- Пробуждение происходит по timer interrupt (каждые 10мс при 100Hz)
- Idle process имеет минимальный stack (1 page, 4KB)

6. Guide Integration
Планировщик вызывает guide_run() при определённых условиях:
- Переход в idle (текущий процесс → idle)
- Периодически каждые 5 тиков (50мс)
- Если есть процессы в wait queue на EventRing
- Если EventRing не пуст

Это гарантирует своевременную обработку событий даже когда все процессы заняты.

7. Fairness Audit
Каждые SCHEDULER_CRITICAL_STARVATION_TICKS (100 тиков ≈ 1 сек) scheduler проводит аудит:
- Выявляет процессы, не получавшие CPU дольше SEVERE_STARVATION_TICKS
- Логирует в debug_printf для диагностики

8. Process Lifecycle Management
Scheduler также управляет очисткой завершённых процессов:
- PROC_DONE с ref_count == 0 → переводятся в PROC_CRASHED → destroy
- PROC_CRASHED с ref_count == 0 → немедленный destroy
- Периодический flush pending_results и deferred cleanup

Почему это инновация?

В классических ОС планировщик «слеп». Он не знает, что chrome.exe сейчас важен, а background_update.exe — нет.

В BoxOS планировщик — это часть интерфейса пользователя. Командой `use` ты буквально перенастраиваешь приоритеты под конкретную задачу.
