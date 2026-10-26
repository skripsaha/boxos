# Source map and authority order

## Authority order

1. The live Git tree, current branch, and actual verification output.
2. The tracked files in this `memory-bank/`, `GOALS.md`, and `PLANS.md`.
3. Preserved Claude memory and the v2 planning archive, used for history and
   decisions that are not visible in code.
4. Raw Claude JSONL and tool-result logs, used only for forensic questions.

## Preserved Claude sources

Project memory index:

`/Users/skripsaha/.claude/projects/-Users-skripsaha-Projects-boxos/memory/MEMORY.md`

Core BoxOS context:

- `memory/project_boxos.md`
- `memory/boxos_philosophy.md`
- `memory/boxos_production_constitution.md`
- `memory/boxos_master_table.md`
- `memory/feedback_v2_first.md`
- `memory/feedback_discuss_before_acting.md`
- `memory/feedback_tagfs_rebuild_what_we_remove.md`

Quill design and current v2 plan:

- `memory/tagfs_quill_design_2026_09_26.md`
- `memory/tagfs_wave1_map_2026_09_29.md`
- `memory/next_session_handoff_2026_10_14.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/spec_v2.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/plan_final.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/NOTES.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/j2/NOTES.md`

The full session transcripts are the `*.jsonl` files beside the memory
directory. They are valuable historical evidence but must not be injected in
full into every task.

## Imported Codex layer

- `.codex/config.toml` — project-scoped MCP definitions imported from Claude.
- `.codex/agents/` — project-scoped architect, debugger, and developer agents.
- `~/.codex/AGENTS.md` — imported global user instructions.
- `~/.codex/hooks.json` — imported machine-level Cortex hooks; keep outside Git.
- `~/.codex/external_agent_session_imports.json` — hashes and source paths for
  the fourteen imported Claude sessions.

## Full Claude memory coverage

Generated read-only from `/Users/skripsaha/.claude/projects/-Users-skripsaha-Projects-boxos/memory` on 2026-10-26. The catalog is a manifest, not a replacement for source files. Git/current handoffs remain authoritative.

- Files: 330
- Bytes: 7251758
- Lines: 55172
- Referenced by MEMORY.md: 229
- Not referenced by MEMORY.md: 101
- Duplicate content hashes: 0

| category | file | bytes | lines | modified | hash | index | markers | headings |
|---|---|---:|---:|---|---|---|---|---|
| architecture | boxos_audit_remaining.md | 29623 | 201 | 2026-09-26 | d247cc8caee2 | yes | active,open,next | Оставшиеся правки аудита BoxOS (после CRIT+HIGH). Owner: «исправим качественно в новой сессии».; ★★ СЕССИЯ 2026-06-22 ИТОГ + NEXT-SESSION START HERE (ветка boxcxx) ★★ |
| architecture | boxos_concept_inventory.md | 30738 | 278 | 2026-08-31 | 31245af97b18 | yes | active,open,next | Полный инвентарь концепций BoxOS (обход 2026-08-31, HEAD `8e5ac3b`); A. Прибытие: загрузка и то, что загрузчик говорит ядру |
| architecture | boxos_constants_audit.md | 8985 | 106 | 2026-09-26 | e6b8d19f6f46 | yes | active,open,next | Ревизия границ: убрать числа, которые стоят вместо механизма; Критерий (полностью — в [[feedback-dynamic-unbounded]]) |
| architecture | boxos_master_table.md | 42009 | 241 | 2026-09-26 | 62ac75b9e892 | yes | active,open,next | Главная таблица BoxOS; ‼ ПРАВИЛО ПРИНУЖДЕНИЯ |
| architecture | boxos_philosophy.md | 1933 | 23 | 2026-05-05 | 52b44f04c75f | yes |  |  |
| architecture | boxos_production_constitution.md | 4756 | 37 | 2026-06-22 | cf4780cdc588 | yes | current,next | ‼ КОНСТИТУЦИЯ BoxOS — обязательна ПРИ КАЖДОМ действии (owner 2026-06-22, кричал, повторял); Как это меняет моё поведение (анти-паттерны, которые owner ловил у меня) |
| architecture | boxos_shell_pf_uefi16c.md | 41122 | 426 | 2026-09-26 | 6820e14093e0 | yes | active,open,next | Падение shell на UEFI 16c — открытая охота; Симптом (факты из лога, 2026-08-16) |
| architecture | boxos_smp_scaling_epic.md | 27975 | 185 | 2026-09-26 | eaf070fb86a5 | yes | active,current,open,next,wip,critical | ‼ AMP NEGATIVE-SCALING ЭПИК + остаток — START HERE для новой сессии (owner 2026-06-22); ★★★ UPDATE 2026-06-22 (II): write_stress hang КОРЕНЬ ВСКРЫТ+ИСПРАВЛЕН (НЕ gate-race — потерянное AHCI-completion) |
| architecture | boxos_strandtest_16c_flake.md | 26944 | 267 | 2026-06-23 | 07da89e4a06c | no | open,next | strandtest UEFI-16c flake — ROOT CAUSE CONFIRMED 2026-06-23 (reap-vs-switch stack free); ★★★ SESSION #2 2026-06-23 — TRUE ROOT FOUND+FIXED: DOUBLE-DISPATCH (read FIRST) |
| architecture | boxos_writestress_wake_bug.md | 11663 | 95 | 2026-06-22 | 9359605eccb3 | no | open,next,wip | write_stress hang = ПОТЕРЯННОЕ AHCI-completion на async-READ (НЕ gate-race! старая гипотеза ОПРОВЕРГНУТА); ★★★ КОРЕНЬ ВСКРЫТ И ДОКАЗАН (сессия 2026-06-22, wsprobe gate-read probe) — START HERE ★★★ |
| bugs | bug_autostart_starts_unreadable_image.md | 7602 | 106 | 2026-08-29 | d18acf066233 | yes | critical | AUTOSTART запускает программу, образ которой не читается; ✅ ЗАКРЫТО — проверено чтением дерева 2026-08-29 (HEAD `0ea7958`) |
| bugs | bug_child_kill_eternal_16c.md | 7350 | 70 | 2026-09-06 | 95a39c76ad87 | yes | active,current,next | cxxtest 58.8: kill вечного ребёнка не подействовал, wait() навсегда (BIOS 16c); Улики (машина заморожена, рентген через монитор QEMU) |
| bugs | bug_console_lane_lost_2026_09_06.md | 16311 | 147 | 2026-09-06 | 61126f3cdbbf | yes | active,open,next | «Клин cxxtest 58 на 16c» = потерянная полоса консоли, три дефекта; Как ловилось (минуты, не матрица) |
| bugs | bug_console_order_lost_after_replug.md | 7031 | 82 | 2026-09-26 | ce40128f5a48 | yes | active,open,next,critical | Порядок консоли теряется после реплага носителя; Что видно, дословно |
| bugs | bug_machine_never_idles.md | 15856 | 197 | 2026-09-05 | 2eb892fda108 | yes | active,open,next | Машина не засыпает — знание есть, кода нет (откачено 2026-08-31); Корень (подтверждён, не изменился) |
| bugs | bug_parked_strand_holds_core.md | 11783 | 158 | 2026-08-30 | 4741a186e084 | yes | active,next | ✅ ЗАКРЫТ: припаркованный strand держал ядро — одно поле, два смысла; Корень (доказан, не выведен) |
| bugs | bug_phase41_a5_wallclock_ratio.md | 8336 | 106 | 2026-08-23 | ce0f41f8409c | yes | active,open,next | ✅ ЗАКРЫТ `0e77852` — отношение двух настенных замеров не свойство аллокатора; Что показала матрица |
| bugs | bug_pocket_ring_double_pop.md | 7015 | 74 | 2026-09-05 | 1c6d34b5050a | yes | next | Двойной pop кольца карманов — корень клина «ANSWER OWED» на 16 ядрах; Улики (BIOS STRICT 16c, `bench` второй прогон, pid 3 gen 6, uptime ~20 с) |
| bugs | bug_power_button_counts_itself.md | 5116 | 64 | 2026-09-26 | a1c581d9b493 | yes | open,next | Кнопка питания ждёт сама себя; ‼ КОРЕНЬ (проверен лично по коду, четыре точки) |
| bugs | bug_reboot_pf_efi_resetsystem.md | 8687 | 102 | 2026-09-26 | 1459552fdaf2 | yes | active,next | #PF при перезагрузке — сильнейший кандидат, ещё НЕ доказан; ‼‼‼ ГИПОТЕЗА (2026-09-24, три факта, каждый проверен в дереве лично) |
| bugs | bug_replug_autostart_silent_read.md | 8596 | 104 | 2026-09-26 | 131f18b472d1 | yes | active,current,next | replug: чтение `shell.bin` принято, не завершилось и НИЧЕГО не сказало; ‼ ПЕРЕДАЧА ПРОШЛОЙ СЕССИИ НАЗЫВАЛА КОРЕНЬ НЕВЕРНО |
| bugs | bug_strand_spawn_pmm_free_16c.md | 8176 | 106 | 2026-08-17 | 6f24fcc386a9 | yes | active,open,next,critical | ✅ ИСПРАВЛЕН: strand_spawn → process_free_kernel_stack → pmm_free → паника; Корень (доказан по коду И воспроизведён зондом, не гипотеза) |
| bugs | bug_tagfs_silent_mount_failure.md | 8948 | 121 | 2026-08-29 | 786d5157f004 | yes | active,next | Том был не смонтирован — и молчал об этом; ‼ КАК ЭТО БЫЛО ДОКАЗАНО (метод, переиспользуемый) |
| bugs | bug_uefi16c_kfree_canary_race.md | 8935 | 100 | 2026-09-01 | 0836da0fbf75 | yes | active,next | Порча кучи на 16c: эвристика принадлежности slab — корень найден и закрыт; Корень (доказан детерминированной мутацией) |
| bugs | bug_uefi_boot_panic_efi_runtime.md | 10166 | 127 | 2026-08-29 | ace5c26fc9e8 | yes | active,open | UEFI на живом железе: паника в efi_runtime_init — ЗАКРЫТ; Что было на экране (ДВЕ разные машины, одна подпись) |
| bugs | bug_umwait_control_register.md | 6389 | 94 | 2026-08-23 | 07c1230ccfe9 | yes | open | UMWAIT: управляющий регистр был тем же, где лежал дедлайн; Дефект |
| bugs | bug_xhci_port_given_up_forever.md | 4765 | 71 | 2026-08-29 | 0401ffe726ac | yes | active,next | Порт, от которого отказались, не оживал никогда; Доказательство — чтением, `grep enum_attempts` по всему ядру |
| bugs | open_decisions_2026_09_11.md | 7809 | 80 | 2026-09-26 | f20341853780 | yes | active,current,open,next | Открыто после сессии 2026-09-11; Решения владельца |
| bugs | open_findings_2026_09_17.md | 19152 | 218 | 2026-09-26 | c82485b46851 | yes | active,open,next | Открытые находки на починку (собрано 2026-09-17); ‼‼‼ ЧАСТЬ 1 — КОРЕНЬ «30 СЕКУНД ПРИ ВЫКЛЮЧЕНИИ» (владелец недоволен, и прав) |
| cxx | cxx_cmath_correctly_rounded.md | 21664 | 88 | 2026-08-19 | 2cab77550deb | yes | active,current,open,next,wip | Ф27e — cmath *l correctly-rounded (≤0.5 ULP) + MPFR mega-sweep; ▶▶▶ NEXT-SESSION START-HERE (owner 2026-07-19: «доделай CR до конца в новой сессии») |
| cxx | cxx_cstdio_one_printf.md | 5174 | 72 | 2026-08-19 | e70c8425df09 | yes | active,open | `<cstdio>`: четыре имени, которые boxlib уже занимал; Факты (перемерены) |
| cxx | cxx_finish_line.md | 14167 | 148 | 2026-09-26 | 759517192070 | yes | active,current,open,next | Финишная прямая C++ — порядок, цена, правила; ✅ ПРАВИЛО НОМЕР ОДИН СРАБОТАЛО ВТОРОЙ РАЗ — ХОСТ-СТЕНД (Ф43-e-2) |
| cxx | cxx_full_architecture_coverage_epic.md | 50766 | 86 | 2026-09-26 | 6dff322d8041 | yes | active,open,next,critical | C++ ПОЛНОЕ ПОКРЫТИЕ АРХИТЕКТУРЫ BoxOS — мастер-план (эпик); КАРТА ПОКРЫТИЯ (22 подсистемы) |
| cxx | cxx_honest_leftovers.md | 267735 | 1108 | 2026-09-26 | 09548b26af8f | yes | active,current,open,next,critical | boxcxx — честные недоделки (постоянный трекер); ‼ ПОПРАВКА К СТАРЫМ ЗАПИСЯМ ЭТОГО ФАЙЛА |
| cxx | cxx_phase10_cmath_progress.md | 16997 | 86 | 2026-06-15 | 7833ba818d9b | no | open,next,critical | Ф10 `<cmath>` — SHIPPED 2026-06-15 ✅; Ф10 — АУДИТ-2 «разберись с узкими местами» (2026-06-15) — КЛЮЧЕВОЙ ПРОРЫВ |
| cxx | cxx_phase12_prep.md | 13369 | 79 | 2026-06-17 | 1c5bbf73faf3 | no | current,open,next,critical | Ф12 ПОДГОТОВКА — FLAGSHIP `<coroutine>` (читать первым в Ф12-сессии); SCOPE Ф12 (owner-план) |
| cxx | cxx_phase15b_prep.md | 9694 | 46 | 2026-06-18 | fc7d09fb6eaf | no | active,open,next | Ф15b ПОДГОТОВКА — TagFS context/snapshot RAII + anchor observer (читать первым); РЕШЕНИЯ ВЛАДЕЛЬЦА (2026-06-18, AskUserQuestion — реализовывать ИМЕННО ТАК) |
| cxx | cxx_phase20_plan.md | 45830 | 367 | 2026-06-23 | 9d5580c22d06 | no | open,next,critical | Ф20 — C++ std потоки (последний блок boxcxx-эпика). План + locked-решения (owner 2026-06-20); ★★★ NEXT SESSION (старт ОТСЮДА, 2026-06-23) — ГЕЙТ ЧИСТ, ВСЕ БЛОКЕРЫ Ф20d СНЯТЫ |
| cxx | cxx_phase20d_active.md | 19731 | 97 | 2026-06-24 | 1a13a99a20d3 | no | open,next | Ф20d ACTIVE — <condition_variable> (d-1) then <future> (d-2); OWNER-DECISIONS (this session, locked) |
| cxx | cxx_phase20e_strandpool.md | 13314 | 128 | 2026-06-24 | e117e862ba80 | no | active,next | Ф20e — per-strand StrandPool (LAST block of Ф20). Owner-approved 2026-06-24. Pairs [[cxx-phase20-plan]] §Ф20e.; GROUND TRUTH (verified against code) |
| cxx | cxx_phase21_strands.md | 13027 | 107 | 2026-06-24 | f15baee28a12 | no | next | Ф21 — box::Strands (нативный C++-фасад). ПОСЛЕДНИЙ блок boxcxx. Решения owner 2026-06-24.; OWNER-LOCKED РЕШЕНИЯ (AskUserQuestion ×4, 2026-06-24) |
| cxx | cxx_phase23_error_model.md | 18934 | 57 | 2026-06-25 | 2534f9de798c | no | active,open,next,critical | Ф23 — BoxOS C++ error-модель (флагман не-Unix). План зафиксирован 2026-06-25.; ★★ ОСТАЛОСЬ ДОДЕЛАТЬ (owner 2026-06-25: «обязательно не забыть ВСЁ доделать», свежей сессией) |
| cxx | cxx_phase24_async.md | 16559 | 49 | 2026-06-26 | 58f2f88dd33c | no | active,open,next | Ф24 — BoxOS async-полнота (co_await всё) [L→split a/b/c; d→Ф26]; ✅ Ф24a SHIPPED `d356bd1` 2026-06-25 |
| cxx | cxx_phase25_coverage.md | 39033 | 110 | 2026-06-28 | 00a47eb1adb0 | yes | active,current,open,next | Ф25 — box:: coverage-полнота (достать ВЕСЬ boxlib + «C++ больше»); ✅ СУБСТРАТ-ФАКТЫ (верифицированы в коде 2026-06-26 architect+owner-session) |
| cxx | cxx_phase26_crosslayer.md | 118603 | 216 | 2026-07-16 | 9f882e212864 | yes | active,current,open,next,wip,critical | Ф26 — Process-supervision + substrate-дыры (CROSS-LAYER kernel+boxlib+C++); ⚠ ВЕРИФИЦ-КОРРЕКЦИИ устаревших memory-claim (current code 2026-06-28): |
| cxx | cxx_phase27_long_double.md | 41799 | 136 | 2026-07-19 | 8ab7a1ca70fe | yes | open,next,critical | Ф27 — long double 80-бит x87 (PART-2 std-полнота, «очень важно»); ✅✅ Ф27 COMPLETE — END-OF-PHASE ЗАКРЫТ (2026-07-19) |
| cxx | cxx_phase28_assoc.md | 15944 | 65 | 2026-07-20 | ab6849f8d7fa | yes | current,open,next | Ф28 — Assoc-полнота (node_handle + heterogeneous), PART-2 std-полнота; ✅✅✅ Ф28 COMPLETE + ALL DEFERRED CLOSED (4 коммита: a=d32c044 rb-tree · b=f7b3ace hash · c=13ee62a heterogeneous · **d=f903b35 deferred-closure**). Матрицы 4/4 каждая. |
| cxx | cxx_phase29_ranges.md | 37546 | 75 | 2026-07-23 | 7f49078fbed9 | yes | active,open,next | Ф29 — Ranges-полнота (PART-2 std-полнота). Часть [[cxx-post-audit-completion-epic]], следует за ✅[[cxx-phase28-assoc]].; ✅ Ф29b-2 ЗАШИПЛЕН `80c604e` (8 файлов, +2202/−7, Phase89, матрица 4/4) |
| cxx | cxx_phase29c_design.md | 10532 | 59 | 2026-07-21 | b6747f33f858 | no | next | Ф29c — factory/simple views (part of [[cxx-phase29-ranges]]). Owner-approved 2026-07-21: **ONE L commit**, 2-file clause-split, counted→subrange (light).; Locked decisions (AskUserQuestion 2026-07-21) |
| cxx | cxx_phase29d_design.md | 24455 | 80 | 2026-07-21 | 94aff89a7431 | yes | next | Ф29d — tuple/multi-range views (part of [[cxx-phase29-ranges]]). Follows ✅Ф29c `eed6030`.; LOCKED decisions (AskUserQuestion 2026-07-21, all 3 = Rec) |
| cxx | cxx_phase29e_design.md | 25193 | 78 | 2026-07-22 | 593cbb78833a | yes | active,current,open,next | Ф29e — decomposition/grouping views (part of [[cxx-phase29-ranges]]). Follows ✅Ф29d `699b6b4`.; LOCKED split (AskUserQuestion 2026-07-21 = Option 1 «3-way по семействам») |
| cxx | cxx_phase30_headers.md | 134107 | 299 | 2026-08-07 | a8e1b9f7f061 | yes | active,current,open,next,critical | Ф30 — Отсутствующие std-заголовки (std-полнота); Split (owner-locked 2026-07-23, 6-way a-f) |
| cxx | cxx_phase31_final.md | 200233 | 539 | 2026-08-14 | d17e899adba5 | yes | active,current,open,next,critical | Ф31 — финальная фаза boxcxx (conformance + flat-контейнеры + CONFORMANCE-док); OWNER-LOCKED РЕШЕНИЯ 2026-08-08 (4×AskUserQuestion) |
| cxx | cxx_phase31e_debts.md | 29942 | 279 | 2026-08-14 | 541ca9a6a122 | yes | active,current,open,next,critical | Ф31e «Долги» — шаг 1 owner-locked порядка (ПЕРЕД Ф32); Подшаги |
| cxx | cxx_phase32_cpp26_uplift.md | 35367 | 319 | 2026-08-15 | ca6fe426ebda | yes | active,current,open,next,wip | Ф32 — C++26-uplift. Шаг 2 owner-locked порядка (после ✅[[cxx-phase31e-debts]], перед решением про `<fstream>`/`<iostream>`).; ‼ Два owner-решения 2026-08-14 (AskUserQuestion, зафиксировать) |
| cxx | cxx_phase33_cpp26_sweep.md | 19696 | 219 | 2026-08-15 | 670d4fd3ed88 | yes | active,open,next | Ф33 — остаток широкого C++26-свипа (шаг «либо остаток свипа» из owner-locked порядка); ✅ ЗАШИПЛЕН `6c252b1` 2026-08-15. Матрица **4/4 ALL-PASS STRICT (MATRIX_RC=0)**, |
| cxx | cxx_phase34_cpp26_sweep2.md | 15449 | 183 | 2026-08-16 | 53d7efaccdf8 | yes | next | Ф34 — остаток C++26-свипа, заход второй (продолжение [[cxx-phase33-cpp26-sweep]]); ✅ ЗАШИПЛЕН `77b456e` 2026-08-15. Матрица **4/4 ALL-PASS STRICT**, |
| cxx | cxx_phase35_indirect_mdspan.md | 16999 | 206 | 2026-08-16 | 77b39875af7c | yes | active,open,next | Ф35 — `indirect`/`polymorphic` + полный `<mdspan>`; ✅ ЗАШИПЛЕН `33d4685` 2026-08-16. Матрица **4/4 ALL CONFIGS PASS STRICT**, |
| cxx | cxx_phase36_fstream_iostream.md | 15002 | 179 | 2026-08-16 | 0810a04c2d85 | yes | active,open,next | Ф36 — `<fstream>` + `<iostream>`, и усечение в TagFS, которого не было; ✅ ЗАШИПЛЕН `e995389` 2026-08-16. Матрица **4/4 ALL-PASS STRICT** |
| cxx | cxx_phase37_nameplate_stacktrace.md | 17653 | 202 | 2026-08-17 | 8977f7fbf45d | yes | active,next | Ф37 — Nameplate + `<stacktrace>`; Развилка и решения владельца (2026-08-16) |
| cxx | cxx_phase38_spanstream_typeindex_scoped.md | 17276 | 209 | 2026-08-18 | 9f6f0564f06f | yes | active,open,next | Ф38 — `<spanstream>` + `<typeindex>` + `<scoped_allocator>`; Решения владельца (2026-08-17, AskUserQuestion) |
| cxx | cxx_phase39_constexpr_string_vector.md | 15658 | 174 | 2026-08-18 | 79a7bf621196 | yes | active,current,next | Ф39 — constexpr-пласт `<string>` / `<vector>` / `<bitset>`; Старт: HEAD `cc1f2a4`, дерево чистое, 86 заголовков / 203 FTM / 210 фаз |
| cxx | cxx_phase40_syncstream_execution_stdfloat.md | 23261 | 263 | 2026-08-18 | 40e526296a2c | yes | active,open,next | Ф40 — `<syncstream>` + `<stdfloat>` + `<execution>`; Старт: HEAD `53dfc7b`, дерево чистое, 86 заголовков / 206 FTM / 213 фаз |
| cxx | cxx_phase41_c_wrappers.md | 55745 | 573 | 2026-08-19 | 4e795927e696 | yes | active,current,open,next,critical | Ф41 — 17 C-обёрток (последняя категория «заголовка нет»); Старт: HEAD `005147f`, дерево чистое, 89 заголовков / 209 FTM / 217 фаз |
| cxx | cxx_phase42_wide_layer.md | 73719 | 715 | 2026-08-20 | 6d30ac922091 | yes | active,current,open,next | Ф42 — wide-слой одним куском; Старт: HEAD `ff67659`, дерево чистое |
| cxx | cxx_phase43_locale_format_freestanding.md | 64219 | 655 | 2026-08-21 | 60693e9bcb33 | yes | active,current,open,next | Ф43 — фасеты, ширина и подпись; Старт: HEAD `0f76210` (Ф42-g), дерево чистое, осиротевших QEMU нет |
| cxx | cxx_phase44_regex_valarray.md | 37305 | 418 | 2026-08-23 | 0c331e9405ce | yes | active,open,next | Ф44 «узор и массив» — `<regex>` + `<valarray>`; Почему исключение Ф0 отменено (это НЕ каприз) |
| cxx | cxx_phase45_timezones.md | 31815 | 344 | 2026-08-23 | 9c623e3a4a70 | yes | active,open,next | Ф45 «узор» закрыт другими — здесь «пояс и секунда»; Решения владельца (все три — его) |
| cxx | cxx_phase46_47_runtime_format_constexpr_cmath.md | 9427 | 120 | 2026-08-23 | b0a99ba66d69 | yes | current,open,next | Ф46 «строка, которой нет до запуска» + Ф47 «constexpr <cmath>»; Что изменилось в цифрах |
| cxx | cxx_post_audit_completion_epic.md | 25368 | 75 | 2026-09-26 | ac1e17e938ee | yes | active,open,next | boxcxx — ПОСТ-АУДИТ ПЛАН ЗАВЕРШЕНИЯ (до настоящих 100%, esp. BoxOS-инструментарий); АУДИТ (workflow `boxcxx-full-audit`, 2026-06-24): 32 агента, 2.33M ток, 462 tool-use, ~13.5мин |
| cxx | cxx_remaining_to_full_support.md | 29997 | 229 | 2026-08-18 | 2e05afca8b95 | no | active,open,next,critical | C++ в BoxOS — что осталось до «полностью поддержан»; ‼‼ АКТУАЛЬНОЕ СОСТОЯНИЕ 2026-08-14 (всё ниже этой секции — снимок 2026-06-14, УСТАРЕЛ на Ф12-Ф31; читать как историю) |
| cxx | cxx_support_roadmap_2026_06_11.md | 60689 | 99 | 2026-09-26 | 38747f805683 | yes | active,open,next,critical | C++ support roadmap (approved 2026-06-11); Final decisions (owner-approved) |
| cxx | cxxtest_timing_flake_2026_07_17.md | 3697 | 16 | 2026-07-17 | 84c64bd554ac | yes | open |  |
| feedback | feedback_agents_self_verify.md | 2301 | 30 | 2026-06-24 | 0430e11bfb60 | yes | active | ‼ Агенты тестируют САМИ, неинтерактивно — owner 2026-06-24; grep build/serial.log for results |
| feedback | feedback_boxlib_structure.md | 1399 | 14 | 2026-04-29 | 681f63f6fab1 | yes |  |  |
| feedback | feedback_canonical_addr.md | 1259 | 22 | 2026-04-29 | d22ddea32ab9 | yes |  |  |
| feedback | feedback_clean_build_before_matrix.md | 5945 | 105 | 2026-08-19 | a6805505bab0 | yes | active,next | Always rebuild clean before stress matrix; The rule |
| feedback | feedback_concurrency_test_16c.md | 2782 | 22 | 2026-06-25 | 58bf8445b711 | yes |  | Concurrency-баги тестировать под истинной 16c-параллельностью (owner 2026-06-25) |
| feedback | feedback_decide_via_analogies.md | 1459 | 14 | 2026-06-19 | f83069d86161 | yes |  |  |
| feedback | feedback_discuss_before_acting.md | 7311 | 87 | 2026-08-31 | fef5380b4b03 | yes | active,open,next | ‼‼ ДОБАВЛЕНО 2026-08-31 — правило нарушено, и вот по каким признакам это было видно; Признак подмены: иду к РАБОТАЮЩЕМУ вместо ПРАВИЛЬНОГО |
| feedback | feedback_diskbook_ata.md | 1654 | 15 | 2026-04-19 | a610c40c2291 | yes |  |  |
| feedback | feedback_dynamic_unbounded.md | 8920 | 106 | 2026-09-26 | 6e46c60b70d9 | yes | active | ‼‼ РАСШИРЕНО 2026-08-31 — владелец повторил и разозлился, и был прав; Правило |
| feedback | feedback_fast_mode_quill.md | 5331 | 42 | 2026-09-30 | 7059ff1e2e20 | yes | active,wip |  |
| feedback | feedback_honest_quality_longterm.md | 7584 | 27 | 2026-06-19 | fa3a1cf254dc | yes | next | НИКОГДА не обманывать · качество · долгосрочные решения (не заплатки) |
| feedback | feedback_naming_arena.md | 1436 | 29 | 2026-06-01 | 9d09f1f6548c | yes |  |  |
| feedback | feedback_never_save_tokens.md | 2923 | 36 | 2026-08-25 | 231055b51c63 | yes | active,open,critical | НИКОГДА не экономить токены ценой качества; Четыре самые важные вещи, по порядку важности |
| feedback | feedback_no_defer.md | 1137 | 19 | 2026-05-05 | 15740cbc45d3 | yes | next |  |
| feedback | feedback_no_feature_dodging.md | 4107 | 87 | 2026-06-07 | 940a26369557 | yes | open | No-feature-dodging rule; Why |
| feedback | feedback_no_flakes_production.md | 2304 | 17 | 2026-06-22 | adc57f4309a1 | yes |  |  |
| feedback | feedback_no_pid_identity.md | 1992 | 33 | 2026-04-26 | b9a9ea9a40fb | yes |  |  |
| feedback | feedback_no_posix_mimicry.md | 6641 | 30 | 2026-06-18 | 01ba9d71ca54 | yes | open,critical |  |
| feedback | feedback_no_shortcuts.md | 2530 | 48 | 2026-06-07 | 6a2296be265b | yes | next | No-shortcuts rule |
| feedback | feedback_non_classic_paradigm.md | 3092 | 25 | 2026-06-15 | 02027191ad6d | yes |  |  |
| feedback | feedback_not_worse_than_others.md | 5844 | 54 | 2026-09-26 | a7830e2a16a6 | yes | next | Не хуже, но и не копия; ‼‼ УСИЛЕНО 2026-09-26 — «лучше, а не только не хуже» |
| feedback | feedback_probe_traps.md | 17687 | 190 | 2026-09-08 | 2a3d07f2f049 | yes | active,open,next | ‼‼ ТРЕТИЙ КАПКАН (2026-08-28): ОСИРОТЕВШИЙ QEMU ПЕРЕХВАТЫВАЕТ МОНИТОР; ‼‼ ТРЕТИЙ КАПКАН (2026-08-31, стоил ШЕСТИ прогонов) |
| feedback | feedback_qemu_testing.md | 2881 | 27 | 2026-05-02 | 92a6bcb2e681 | yes | next |  |
| feedback | feedback_real_hardware_first.md | 2405 | 41 | 2026-05-07 | 523f6eedff6e | yes |  |  |
| feedback | feedback_real_hw_audit_workflow.md | 4278 | 82 | 2026-05-10 | b3750cf4ad11 | yes |  | The seven steps (strict order); Hard rules layered on top of the seven steps |
| feedback | feedback_replace_flimsy_timeouts.md | 5391 | 67 | 2026-09-11 | 6c5aaa844317 | yes | next | Хлипкий таймаут — это не число, это неверный подход; Как выглядит правильная замена (образец, сделано 2026-09-05) |
| feedback | feedback_short_test_runs.md | 3069 | 36 | 2026-08-17 | c71b932bfcaf | no | open |  |
| feedback | feedback_stability_over_momentum.md | 3387 | 38 | 2026-09-02 | be8e5e4e8cdc | yes |  |  |
| feedback | feedback_stop_and_ask.md | 2380 | 49 | 2026-06-07 | 84edc0ac7fda | yes | next | Stop-and-ask rule |
| feedback | feedback_strict_always.md | 1947 | 38 | 2026-06-02 | 9e317ceeb2c0 | yes |  |  |
| feedback | feedback_survey_style.md | 2684 | 23 | 2026-05-05 | 1e511b6adf13 | yes | open |  |
| feedback | feedback_tagfs_rebuild_what_we_remove.md | 4300 | 38 | 2026-10-15 | f9ea0bf6b163 | yes |  |  |
| feedback | feedback_timeouts_irritate.md | 2041 | 26 | 2026-08-27 | 9f53321e8d10 | yes |  |  |
| feedback | feedback_uefi_gop_priority.md | 2804 | 36 | 2026-09-01 | 1af66140c578 | yes | active | UEFI/GOP — приоритетная цель консоли |
| feedback | feedback_uniqueness.md | 1949 | 16 | 2026-04-29 | 367d7ace698f | yes | open |  |
| feedback | feedback_use_our_own_infrastructure.md | 7186 | 54 | 2026-09-09 | 045bc7082c58 | yes | open | Использовать СВОЮ инфраструктуру, а не строить рядом ещё одну; Инвентарь — что у нас есть и для чего (сверено с кодом 2026-08-26) |
| feedback | feedback_v2_first.md | 2237 | 24 | 2026-10-07 | 14a170889840 | yes | active,open,critical |  |
| feedback | feedback_verify_what_changed.md | 3496 | 42 | 2026-09-05 | 94fce782eb36 | yes |  | ‼ Подтверждено владельцем 2026-09-05, поздний вечер (старт сессии «баг → очередь») |
| feedback | feedback_vmm_user_ctx.md | 1809 | 29 | 2026-04-29 | 704a1c8bb6f3 | yes |  |  |
| handoff | next_session_debug.md | 3902 | 68 | 2026-04-29 | 5816ea14e2fc | no | current,next |  |
| handoff | next_session_fix_spec_findings.md | 31849 | 307 | 2026-09-26 | 8d1c68714261 | yes | active,open,next,critical | Починка находок аудита — точка входа; ‼‼‼ ПЕРЕДАЧА 2026-09-20 — ЧИТАТЬ ПЕРВЫМ |
| handoff | next_session_handoff_2026_08_28.md | 24349 | 263 | 2026-08-29 | 50e66c7383ce | yes | active,open,next | Передача: с чего начать новую сессию; ‼ ПЕРВОЕ ДЕЙСТВИЕ, ДО ЛЮБОГО КОДА |
| handoff | next_session_handoff_2026_08_29.md | 16895 | 203 | 2026-08-29 | c27bd93d532b | yes | active,open,next | Передача: с чего начать новую сессию; ‼ ПЕРВОЕ ДЕЙСТВИЕ |
| handoff | next_session_handoff_2026_08_30.md | 7261 | 88 | 2026-08-30 | 8636e6cbf239 | yes | active,open,next | Передача: с чего начать новую сессию; ‼ ПЕРВОЕ ДЕЙСТВИЕ |
| handoff | next_session_handoff_2026_08_31.md | 12245 | 155 | 2026-08-31 | 2939eea371fb | yes | active,open,next | Передача: с чего начать новую сессию; ‼ ПЕРВОЕ ДЕЙСТВИЕ |
| handoff | next_session_handoff_2026_09_01.md | 6646 | 74 | 2026-09-01 | 9fe02a953aa7 | yes | open,next | Передача 2026-09-01: с чего начать; ✅ СДЕЛАНО за сессию (3 коммита, все ворота зелёные) |
| handoff | next_session_handoff_2026_09_01b.md | 7256 | 90 | 2026-09-01 | 480b8f3e1582 | yes | active,next,critical | Передача 2026-09-01 (вечер): с чего начать; ✅ Сделано за сессию |
| handoff | next_session_handoff_2026_09_05.md | 4114 | 53 | 2026-09-05 | 14a721b53e2e | yes | active,open,next | Передача 2026-09-05 — арка сна закрыта; Что теперь ИЗМЕРИМО другое |
| handoff | next_session_handoff_2026_09_05b.md | 6951 | 67 | 2026-09-05 | fec8f330d42f | yes | active,next | Передача 2026-09-05b — ревизия + задача 1 + корень клина 16c; Что сделано |
| handoff | next_session_handoff_2026_09_06.md | 3855 | 41 | 2026-09-06 | 60f2979262e3 | yes | active,open,next | ПЕРЕДАЧА 2026-09-06 (день); Что закрыто (см. [[bug-console-lane-lost-2026-09-06]], раздел «РАЗВЯЗКА») |
| handoff | next_session_handoff_2026_09_06b.md | 6114 | 60 | 2026-09-06 | 3bae32a959e8 | yes | active,next | ПЕРЕДАЧА 2026-09-06 (вечер); Сделано за сессию |
| handoff | next_session_handoff_2026_09_06c.md | 6537 | 62 | 2026-09-06 | 41042de63747 | yes | active,open,next | ПЕРЕДАЧА 2026-09-06 (вечер 2); Сделано (подробно — [[nightwatch-lost-wake-persistence-2026-09-06]]) |
| handoff | next_session_handoff_2026_09_07.md | 5344 | 56 | 2026-09-07 | 69fedceb0733 | yes | active,next | ПЕРЕДАЧА 2026-09-07; Сделано — [[chit-promised-answers-2026-09-07]] (подробности, дизайн, капканы там) |
| handoff | next_session_handoff_2026_09_08.md | 17688 | 144 | 2026-09-08 | ebf304b3f81e | yes | active,open,next,critical | ПЕРЕДАЧА 2026-09-08; ‼ ОЧЕРЕДЬ НА ВЫБОР ВЛАДЕЛЬЦА (следующая сессия; сначала разговор, он выбирает) |
| handoff | next_session_handoff_2026_09_09.md | 7488 | 63 | 2026-09-09 | ee2e5963b9cb | yes | open,next,critical | ПЕРЕДАЧА 2026-09-09 (сессия 4 закрыта); Коммиты сессии (все на BIOS STRICT, узкие оракулы + мутации; fast-матрица в конце) |
| handoff | next_session_handoff_2026_09_09b.md | 5792 | 50 | 2026-09-09 | d9ac3b1eda51 | yes | open,next | ПЕРЕДАЧА 2026-09-09b (сессия 5 закрыта); Коммиты сессии (каждый с оракулом + мутацией, где она возможна) |
| handoff | next_session_handoff_2026_09_09c.md | 10054 | 93 | 2026-09-09 | dec8ed123b7e | yes | open,next | ПЕРЕДАЧА 2026-09-09c (сессия 6); Сделано в сессии 6 |
| handoff | next_session_handoff_2026_09_09d.md | 8817 | 79 | 2026-09-10 | 2190ece6fbb3 | yes | active,open,next | ПЕРЕДАЧА 2026-09-09d (сессия 7); Сделано |
| handoff | next_session_handoff_2026_09_10b.md | 8408 | 102 | 2026-09-10 | a0a76a428f46 | yes | active,open,next | ПЕРЕДАЧА 2026-09-10 (сессия 9) — ТОЧКА ВХОДА; Проверено |
| handoff | next_session_handoff_2026_09_11.md | 8640 | 107 | 2026-09-11 | aaed775faabc | yes | open,next | ПЕРЕДАЧА 2026-09-11 — ‼ ЧАСТИЧНО ОПРОВЕРГНУТА; Состояние дерева |
| handoff | next_session_handoff_2026_09_12.md | 5943 | 68 | 2026-09-26 | 16e8a337d92b | yes | active,open,next | ПЕРЕДАЧА 2026-09-12 — ТОЧКА ВХОДА; Что доказано на этом дереве |
| handoff | next_session_handoff_2026_09_13.md | 6360 | 70 | 2026-09-26 | 059773ba0c17 | yes | active,next | ПЕРЕДАЧА 2026-09-13 — ТОЧКА ВХОДА; Что было сделано (три задачи владельца — ЗАКРЫТЫ) |
| handoff | next_session_handoff_2026_09_23.md | 28086 | 283 | 2026-09-26 | 79f10a1a34dd | yes | active,current,open,next,critical | Точка входа — 2026-09-23; ‼‼‼ УКАЗАНИЕ ВЛАДЕЛЬЦА НА ЭТУ СЕССИЮ — ДОСЛОВНО |
| handoff | next_session_handoff_2026_09_25.md | 13778 | 174 | 2026-09-26 | 017330f15307 | yes | active,open,next | Точка входа — СЕССИЯ ЗАКРЫТА, следующая тема: AMP; ‼ ПОСЛЕДНИЙ ПРОГОН 09-25 (образ `tree 7d29476cee4b3a07`) — ВСЁ ПОДТВЕРЖДЕНО |
| handoff | next_session_handoff_2026_09_26.md | 9537 | 83 | 2026-09-26 | 56cb6d941054 | yes | active,open,next | Точка входа 2026-09-26; ОБНОВЛЕНИЕ 2026-09-26 (после ревизии памяти) |
| handoff | next_session_handoff_2026_09_28.md | 6891 | 64 | 2026-09-28 | d41f99a1fb4e | yes | active,open,next | Точка входа 2026-09-28 — шаг 1.0 закрыт, дальше 1.1; ▶ Владелец 09-28: «продолжим с новой сессии работу, качественно с шага 1.1» |
| handoff | next_session_handoff_2026_09_30.md | 12370 | 97 | 2026-10-01 | 64f0511904c5 | yes | active,open,next,wip | Точка входа 2026-09-30 — этап 1 в коде, стоп на обсуждение; ‼ ПЕРВОЕ ДЕЛО СЛЕДУЮЩЕЙ СЕССИИ |
| handoff | next_session_handoff_2026_10_08.md | 6531 | 54 | 2026-10-08 | 79efd7a78627 | yes | active,open,next,wip,critical | Точка входа 2026-10-08 — проверка №1 закрыта, впереди v2; Цель и правила владельца (действуют в v2) |
| handoff | next_session_handoff_2026_10_13.md | 9934 | 81 | 2026-10-13 | a06698a1776c | yes | active,next,wip | Точка входа 2026-10-13 — v2: 2.1–2.2b готовы, впереди 2.3; Состояние |
| handoff | next_session_handoff_2026_10_14.md | 6924 | 59 | 2026-10-14 | 9903c5ecc148 | yes | active,next,wip | Точка входа 2026-10-14 — v2 шаг 2.3, 7 из 11 подшагов слиты; Состояние |
| handoff | next_session_pending_fixes.md | 13770 | 127 | 2026-04-30 | 8d9e35686b10 | no | open,next | Critical (correctness) — RESOLVED 2026-04-30 in commit f3a4723; Bonus root-cause fix (commit f3a4723) |
| handoff | next_session_tasks_2026_09_12.md | 8417 | 91 | 2026-09-26 | aca8a3b27f6b | yes | active,open,next,critical | Три задачи на сессию 2026-09-12 (поставлены владельцем); ЗАДАЧА 1 — плохие подходы и числа вместо механизмов |
| handoff | revision_2026_09_05.md | 18838 | 156 | 2026-09-26 | c29a595e74f4 | yes | active,open,next | Ревизия 2026-09-05: числа вместо механизмов + где не задействована своя инфраструктура; Класс 1 — числа вместо механизма (место → чем заменить) |
| handoff | revision_batch_2026_09_08.md | 9710 | 69 | 2026-09-09 | 627a3caf3523 | yes | open,next,critical | Пакет ревизии «числа вместо механизмов» — 2026-09-08, сессия 4; Решения владельца (опрос 1 + опрос 2) |
| handoff | revision_batch_2026_09_09.md | 10724 | 78 | 2026-09-09 | 16e5621b270d | yes | open,next | Пакет 2026-09-09 (сессия 5): хвост 9b → Wire → полная матрица; Поправки к памяти, найденные чтением (2026-09-09) |
| handoff | session_2026_04_29_summary.md | 8742 | 87 | 2026-04-30 | 396d18ad1f0d | yes | next | 2026-04-29 long debug + cleanup session; Headline outcomes |
| handoff | session_2026_05_02_disk.md | 2939 | 35 | 2026-05-02 | 74e0af94a37d | no | next | Resolved in commit 1f1d5f6; Deferred (need more invasive work) |
| handoff | session_2026_09_11_progress.md | 31492 | 319 | 2026-09-26 | 5800d7341891 | yes | active,open,next,critical | Сессия 2026-09-11 — живой прогресс; СДЕЛАНО и проверено оракулами |
| handoff | session_2026_09_12_decisions.md | 46819 | 479 | 2026-09-26 | 630b116e41f7 | yes | active,open,next,critical | Сессия 2026-09-12 — решения и прогресс; ‼ ДВЕ ПРЕДПОСЫЛКИ ИЗ ПАМЯТИ ОКАЗАЛИСЬ НЕВЕРНЫ |
| handoff | session_2026_09_22_decisions.md | 21619 | 209 | 2026-09-26 | 15d9b8f55d68 | yes | active,current,open,next,critical | Решения 2026-09-22; Что владелец решил |
| handoff | session_2026_09_23_progress.md | 22888 | 242 | 2026-09-26 | 0ebcb8e6e9f2 | yes | active,open,next,critical | Ход сессии 2026-09-23; Закоммичено (ветка boxmetal) |
| handoff | session_2026_09_27_step10_progress.md | 19336 | 169 | 2026-09-28 | fe2a9e99f620 | yes | active,open,next,wip | Сессия 2026-09-27 — шаг 1.0 «Стенд» (Quill), ход работы; Решения владельца в этой сессии |
| handoff | session_2026_09_28_step11_progress.md | 29113 | 216 | 2026-09-30 | c49607aaea4e | yes | active,open,next,wip,critical | Сессия 2026-09-28 — шаг 1.1 «Замки» (Quill), ход работы; Объём 1.1 (сведено из дизайна [[tagfs-quill-design-2026-09-26]] §5/§6/доп.§5 + архивов) |
| handoff | session_2026_10_01_check1_progress.md | 34978 | 249 | 2026-10-08 | 38c344957d2c | yes | active,open,next,wip,critical | ‼ Слова владельца; Состояние (10-06) |
| handoff | session_2026_10_13_step23_progress.md | 16992 | 127 | 2026-10-16 | b735062945f6 | yes | active,next,wip | Шаг 2.3 — ход (сессия b3f03cd1, 2026-10-13); Где всё (папка `~/.claude/projects/-Users-skripsaha-Projects-boxos/cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/`) |
| handoff | session_validation_2026_09_22.md | 330301 | 2253 | 2026-09-26 | ce79e5172fa1 | yes | active,current,open,next,critical | Валидация сессии 2026-09-22; ‼ ВЕРДИКТ КРИТИКА О ГОТОВНОСТИ К МАТРИЦЕ |
| hardware | acpi_audit_done_2026_05_10.md | 4405 | 99 | 2026-05-10 | 14a17cccae18 | no | next | Files in the subsystem (final); Verification matrix (all clean) |
| hardware | acpi_boxos_ification.md | 2682 | 63 | 2026-05-11 | 4fa188190cfc | yes |  | What changed (vs the "Linux-style" version); Tag map |
| hardware | acpi_deferred_2026_05_10.md | 4596 | 102 | 2026-05-10 | 102656ee7ddc | no |  | Closed in session 1 (commit-equivalent); Closed in session 2 (2026-05-10) |
| hardware | acpi_extension_done_2026_05_10.md | 5519 | 124 | 2026-05-10 | 4c0fef8a061e | no |  | Scope; Phase 1 — ACPI table parsing completion |
| hardware | acpi_o1_uniqueness.md | 2638 | 58 | 2026-05-11 | ee44cf4ad8d4 | no |  | What changed; 1. OpRegion SystemMemory VA cache  (perf — O(N) → O(1)) |
| hardware | acpi_production_target.md | 5347 | 120 | 2026-05-11 | cce5c7901523 | no |  | What BoxOS has today (after 11 sessions ending 2026-05-11); What separates this from "complete" |
| hardware | acpi_prt_routing_epic.md | 15207 | 167 | 2026-09-26 | 1777c1f3f315 | yes | active,open | `_PRT`: библиотеке нужен читатель; Решение владельца 2026-09-19 |
| hardware | acpi_session10_done.md | 3558 | 80 | 2026-05-11 | 7eaf0693ca4c | no | next | AML interpreter (drivers/acpi/aml/aml.{h,c}); VT-d driver (core/iommu/vtd.c) |
| hardware | acpi_session11_done.md | 2756 | 54 | 2026-05-11 | 1ffc10ee695f | no |  | Phase CC — ACPI sleep states; Phase DD — AML TermList executor |
| hardware | acpi_session3_done_2026_05_10.md | 3110 | 71 | 2026-05-10 | a59f939e8d9d | no |  | Scope (4 mini-phases this session); Phase A — MADT x2APIC + AP LAPIC NMI |
| hardware | acpi_session4_done_2026_05_10.md | 2379 | 58 | 2026-05-10 | de955cf30780 | no |  | Phase E — HPET LegacyReplacement tick; Phase F — Full PCIe ECAM enumeration |
| hardware | acpi_session5_done.md | 2277 | 54 | 2026-05-11 | 78b909d064ba | no |  | Phase H — HEST error source iteration; Phase I — NUMA domain lookup |
| hardware | acpi_session6_done.md | 1710 | 42 | 2026-05-11 | dcc7b8abe1bc | no |  | Phase L — GHES per-source decode; Phase M — PMM NUMA helper |
| hardware | acpi_session7_done.md | 1699 | 46 | 2026-05-11 | d459be4e505d | no |  | Phase N — SCI IRQ handler; Phase O — ERST instruction decode |
| hardware | acpi_session8_done.md | 1057 | 26 | 2026-05-11 | 89b110628532 | no |  | Phase Q — GPE drain in SCI handler; Phase R — ERST register-region operand decode |
| hardware | acpi_session9_done.md | 2949 | 66 | 2026-05-11 | 463135d20dc3 | no |  | What this session closed (production); Item 1 — GPE dispatch |
| hardware | ahci_real_hw_audit_2026_05_25.md | 3351 | 39 | 2026-05-25 | 6ed1493bf400 | no | open,next | AHCI real-HW audit (2026-05-25) — COMPLETE & VERIFIED; Commits |
| hardware | amp_real_hw_audit_2026_05_31.md | 6612 | 59 | 2026-05-31 | 67eec92c66b5 | no | next,wip | Closed items; MUST |
| hardware | amp_single_vs_multi_2026_09_25.md | 13218 | 137 | 2026-09-26 | cb2e26efa53b | yes | open,next | 1 ядро против N — что доказано (2026-09-25, HEAD e8f95c37); Замер QEMU TCG (Apple M4 4P+6E, QEMU 10.1.2), медиана `med` |
| hardware | ata_async_dma_2026_05_28.md | 8739 | 54 | 2026-07-16 | ecf4ea0a8cff | no | next |  |
| hardware | ata_real_hw_audit_2026_05_27.md | 18318 | 271 | 2026-05-27 | 3bbb46d85284 | no | next | ATA (legacy IDE) real-HW audit — 2026-05-27; Scope |
| hardware | board_run_2026_08_29.md | 5898 | 74 | 2026-08-29 | 447f3c37488a | yes | active,next | Прогон на живой плате, 2026-08-29 — ядро `13782f6`; ✅ ДЕФЕКТ 1 (BSR=1) ЗАКРЫТ НА ЖЕЛЕЗЕ |
| hardware | board_run_2026_08_29_bios.md | 4987 | 69 | 2026-08-29 | cfadb7e63520 | yes | active,open | Прогон на плате 2026-08-29, BIOS/CSM — ядро `cfefeb4`; ‼ ТРИ ПРАВКИ ЭТОЙ СЕССИИ ПОДТВЕРЖДЕНЫ ЖЕЛЕЗОМ |
| hardware | board_run_2026_08_30.md | 4584 | 58 | 2026-08-30 | 69fb102d4888 | yes | next | Прогон на плате 2026-08-30 — ядро `54b1415`; Числа |
| hardware | board_run_2026_09_20.md | 8163 | 93 | 2026-09-26 | 0793433ae647 | yes | active,open,next | Прогон на плате 2026-09-20 — i5-9400F / B365, UEFI; ✅ IOMMU включился на живом железе с первого раза |
| hardware | board_run_2026_09_22_zenbook.md | 436464 | 2435 | 2026-09-26 | 4dbbf7e21327 | yes | active,current,open,next,critical | Прогон на Asus Zenbook UX303L — 2026-09-22; \_PIC, AML и маршрутизация прерываний (Asus Zenbook UX303L, /tmp/watch_zenbook.log, 2274 строки) — сравнение с i5-9400F/B365 (/tmp/watch_2026_09_20.log, 719 строк) |
| hardware | boot_uefi_real_hw_audit_2026_05_28.md | 11099 | 227 | 2026-05-28 | 918e50df5bf6 | no | open,next | Boot / UEFI / EFI runtime services audit — 2026-05-28; What changed (5 commits expected, all in this session) |
| hardware | efi_runtime_full_2026_05_28.md | 14120 | 300 | 2026-05-28 | 14d6c172da78 | yes | next | EFI runtime — full coverage session, 2026-05-28; What was committed |
| hardware | hw_audit_2026_08_28.md | 29051 | 333 | 2026-09-26 | 203b3542f23f | yes | active,open,next | Аудит железных подсистем 2026-08-28 (HEAD `df3b22d`, ветка `boxmetal`); ✅ ЗАКРЫТ `a29134e` — MUST-FIX 1: ДВА ОЖИДАНИЯ ВНУТРИ IRQ0, ДО EOI |
| hardware | real_hw_bootloader_bringup.md | 67938 | 778 | 2026-09-26 | d6e0e2f96d9b | yes | active,open,next,wip | Real-HW bring-up: загрузка с флешки и переделка загрузчика; ‼‼ ГДЕ МЫ ОСТАНОВИЛИСЬ — ПЕРЕДАЧА В НОВУЮ СЕССИЮ (2026-08-24) |
| hardware | scheduler_real_hw_audit_2026_05_26.md | 7913 | 106 | 2026-05-26 | a30a2f5e5ee5 | no | wip | What shipped (8 files, ~145 insertions); Verification — 4/4 boot matrix green |
| hardware | tagboot_real_hw_audit_2026_05_31.md | 5306 | 62 | 2026-05-31 | dbd777b18e9f | no | next | Closed items; MUST |
| hardware | tagboot_uefi_fixes.md | 3097 | 59 | 2026-04-21 | e30d4ebddfbb | no |  | Summary of fixes applied (all verified working as of 2026-04-20); Fix 1: UEFI ABI mismatch (CRITICAL — was root cause of crash at 0xB0000) |
| hardware | timers_real_hw_audit_2026_05_26.md | 19040 | 187 | 2026-05-26 | 4f47f484837d | no |  | Delivered; Result |
| hardware | xhci_abandoned_transfer.md | 10856 | 118 | 2026-08-28 | fdcdad443c7c | yes | active,open,next | Брошенная передача: одно медленное чтение = мёртвый диск; Как воспроизводится (детерминированно, по команде) |
| hardware | xhci_control_giveup.md | 7056 | 88 | 2026-08-29 | b39ca24744b0 | yes | open,next | Управляющая труба: отказаться от передачи нельзя молча (`7914f63`); Дефект |
| hardware | xhci_isochronous_dropped.md | 5480 | 71 | 2026-08-29 | 24e046e726de | yes | next | Изохронные: чинить было нечего, а молчать было чем (`a893800`); ‼ ПРЕДПОСЫЛКА ЗАДАЧИ ОКАЗАЛАСЬ НЕВЕРНОЙ — И ЭТО ГЛАВНОЕ |
| hardware | xhci_per_controller_slots.md | 6083 | 72 | 2026-08-29 | 971175a540f1 | yes | active,open,next | Контроллер несёт свои устройства (`cfefeb4`); Что было |
| hardware | xhci_production_foundation.md | 217178 | 2311 | 2026-09-26 | 5c1e6d46f2c8 | yes | active,current,open,next,wip,critical | xHCI: причал, на котором стоят обе USB-задачи; ‼ 2026-08-28 — ДЕФЕКТ 1 ЗАКРЫТ В КОДЕ, НО НЕ НА ПЛАТЕ |
| index | MEMORY.md | 26887 | 112 | 2026-10-15 | 12703edba119 | no | active,open,next,critical | BoxOS Memory Index; ‼ ЧИТАТЬ ПЕРВЫМ (конституция + ядро архитектуры) |
| open-work | live_backlog_2026_09_25.md | 33488 | 295 | 2026-10-14 | be4c43a24a51 | yes | active,open,next,critical | Живой остаток — ревизия 2026-09-26 (HEAD 56f9871c); ▶ НАЙДЕНО ШАГОМ 1.0 «Стенд» (2026-09-27/28) — живо, не закрыто шагом |
| open-work | pending_impl.md | 5515 | 111 | 2026-09-26 | e56dbdaaa0f9 | yes | active | 1. Friend Allocator (tag-aware PMM layer); 2. KResult — unified kernel result type |
| open-work | remaining_work_2026_06_08.md | 5384 | 125 | 2026-06-08 | 4792e4837ffa | no | open | Remaining production-grade work — 2026-06-08; 1. TME `pmm_alloc_with_keyid` API + VMM map KeyID preservation |
| open-work | remaining_work_2026_06_08_evening.md | 6555 | 144 | 2026-06-08 | e2ed4126b332 | no | open,next | Remaining production-grade work — 2026-06-08 (evening); Context |
| other | architecture_fragility_verdict_2026_09_24.md | 7048 | 88 | 2026-09-26 | 6b503976aa43 | yes | active,open,next | Вердикт по хлипкости архитектуры (2026-09-24); ОДНОЙ ФРАЗОЙ |
| other | audit_full_codebase_2026_05_17.md | 8392 | 107 | 2026-05-17 | b574781f0a0b | no | open,next | Failure class A — IRQ-context lock violations (instant deadlock under load); Failure class B — Lock inversion + self-deadlock |
| other | audit_week_2026_05_17.md | 5189 | 62 | 2026-05-17 | 688c2f6ad105 | no |  | SOURCE A — IRQ-context lock violations (CRITICAL, multiple commits); SOURCE B — Display daemon duplication (HIGH, partial fix in uncommitted) |
| other | bay_subsystem_2026_06_01.md | 6563 | 142 | 2026-06-02 | ea7b5340afef | yes | open,next | Production hardening pass (2026-06-02); Surface |
| other | bios_16c_write_concurrent_deadlock_fix.md | 5977 | 84 | 2026-06-01 | 68a37b0a5145 | no | active,open | Root cause; Fix (single hunk) |
| other | boarding_pass_sealed_and_claimed.md | 9286 | 109 | 2026-08-29 | f2dc72bb7d06 | yes | next | Пасс: страницы попрошены у прошивки, и блок запечатан (`dd50dbb`); 1. Три страницы, о которых загрузчик никогда не просил |
| other | boarding_pass_subsystem.md | 8479 | 113 | 2026-08-25 | 7e1f0cc78724 | yes | current,next | Boarding Pass — что загрузчик говорит ядру о своём прибытии; Зачем |
| other | boardroom_seat_run.md | 6239 | 83 | 2026-08-29 | d12a230f35c0 | yes |  | Носитель говорит, сколько берёт (`5b7468e`); ‼ ГЛАВНОЕ: КОНСТАНТА НЕ СВЯЗЫВАЛА НИЧЕГО |
| other | boxhash_v2_plan_2026_05_25.md | 8109 | 45 | 2026-05-25 | b84314469c1f | no |  |  |
| other | boxlib_real_hw_audit_2026_05_26.md | 9337 | 195 | 2026-05-26 | bc0a0b68cb07 | no | next | boxlib real-HW audit — 2026-05-26; Why: this turn |
| other | caret_root_cause_2026_09_10.md | 8580 | 102 | 2026-09-10 | d1bee84167d9 | yes | open,next | Курсор: один корень на оба симптома (2026-09-10, сессия 9); ‼ КОРЕНЬ (доказан измерением, не рассуждением) |
| other | cet_codegen_audit_2026_06_08.md | 6835 | 149 | 2026-06-08 | edeaafc004f0 | no | open | CET / IBT code-gen audit — shipped 2026-06-08; Why |
| other | cet_userspace_hardening_2026_06_08.md | 6403 | 76 | 2026-06-08 | 9e01da6087eb | no |  | CET userspace hardening — 2026-06-08; Critical fixes (would crash kernel on real silicon) |
| other | chit_promised_answers_2026_09_07.md | 9613 | 91 | 2026-09-26 | a5b80647607d | yes | active,next | Chit — ответ обещан по факту, не по свидетелю (2026-09-07); Что было не так |
| other | cow_chained_manifest_2026_05_09.md | 2404 | 25 | 2026-05-09 | 7438eecd8a79 | no | next |  |
| other | deck_ports_contract_2026_09_18.md | 28594 | 169 | 2026-09-26 | 4b157baa4f5f | yes | active,next | Контракт портов Hardware Deck — расследование 2026-09-18; ФАКТЫ |
| other | epic_boot_does_not_stand_on_usb.md | 5636 | 61 | 2026-09-26 | 265fb2b1804b | yes | open,next,critical | Загрузка не стоит на USB; Постановка |
| other | hardware_fleet.md | 5389 | 62 | 2026-09-22 | efff22ba13eb | yes | active,critical | Парк живого железа; 1. Настольная: i5-9400F / B365 |
| other | hardware_polish_to_ideal.md | 7444 | 81 | 2026-09-26 | d5ef21c77907 | yes | active,next | Полировать железные подсистемы до идеала, а не до «работает»; Как это делать (режим, а не список) |
| other | honest_remaining_bugs_2026_05_17.md | 5726 | 68 | 2026-05-17 | a054770f5237 | no | open,next | Symptoms STILL reproducing in output.log (mtime 14:09:59 after shell.bin 14:02:02); My partial fixes (NOT rolled back — they are defensive but don't solve the symptoms) |
| other | idt_cr2_read_once.md | 3295 | 40 | 2026-08-29 | 904125a2984a | yes |  | CR2 читается один раз, на входе (`feab3bf`) |
| other | ipc_real_hw_audit_2026_06_03.md | 9649 | 105 | 2026-06-03 | 6d6b2dcad0be | no | next | IPC(full) real-HW audit — 2026-06-03; Delta closed |
| other | ipc_touchring_decision_2026_06_01.md | 4291 | 86 | 2026-06-01 | ea15ca84a83a | no | next |  |
| other | irq_defer_done_2026_05_17.md | 3945 | 56 | 2026-05-17 | a2e0b6318acb | no | open | What landed (uncommitted at write time); Verification (16-core, 8GB) |
| other | kcore_claim_publish_2026_09_06.md | 11162 | 100 | 2026-09-06 | eeb8bb0e22c7 | yes | active,next | Задача B: заявка в очереди K-Core берётся не раньше, чем её положили; Дефект (kcore.c `kcore_queue_pop`, было) |
| other | kcore_hijack_fix_2026_05_03.md | 2223 | 43 | 2026-05-03 | 68a8912f6c58 | no |  | K-Core hijack — closed 2026-05-03; Symptom (existed since Phase 12 commit 9068bab, intermittent) |
| other | klib_kstack_real_hw_audit_2026_05_26.md | 6862 | 128 | 2026-05-26 | b0af0c20e657 | no | next | klib + kernel-stack real-HW audit (2026-05-26); Closed in this session |
| other | knock_irq_defer_removed_2026_09_09.md | 9097 | 85 | 2026-09-09 | c003b636fc42 | yes | active,open,next | Knock и снос irq_defer (2026-09-09, сессия 6); Что было (факты из чтения целиком) |
| other | la57_5level_paging_2026_06_08.md | 9176 | 176 | 2026-06-08 | 969c20d7a925 | no | active | LA57 5-level paging — shipped 2026-06-08 evening; What landed |
| other | manifest_architecture.md | 8766 | 152 | 2026-04-29 | 82e592e56a1e | yes |  | Unified BoxOS Architecture: Manifest + Crate + OpRegistry; Three primitives (the entire system) |
| other | mce_page_migration_2026_06_07.md | 6890 | 148 | 2026-06-07 | dcb803cb163c | no |  | MCE Page Migration — shipped 2026-06-07; Architecture |
| other | multicore_16c_spawn_flake_2026_05_31.md | 3472 | 49 | 2026-05-31 | eaeefa47ad8d | no | open,next | When picking up the fix: |
| other | multicore_hang_2026_05_02.md | 7859 | 114 | 2026-05-02 | 78708ae26d17 | no | next | Multi-core hang — what's confirmed, what to do next; Confirmed facts (2026-05-02 session) |
| other | multicore_hang_2026_05_02_resolved.md | 9465 | 181 | 2026-05-03 | d284d9e6fefa | yes | next | Multi-core hang resolution — what shipped on main; Verified state after the 5 commits |
| other | multicore_hang_2026_05_02_trace.md | 4832 | 102 | 2026-05-02 | 3bdd9dbfa8c7 | no | next | Trace investigation results — what's NOT the cause; Setup |
| other | oracle_poisoned_the_tree_2026_09_11.md | 4968 | 86 | 2026-09-11 | 7a650589b44f | yes | active,next | Оракул отравил дерево, а обвинили код (2026-09-11); Что было |
| other | perf_roadmap.md | 6521 | 104 | 2026-09-26 | f640f5a32fea | yes | active,next | BoxOS performance roadmap; Baseline (commit eb3bc91, QEMU TCG, UEFI) |
| other | pku_stamping_2026_06_07.md | 6521 | 149 | 2026-06-07 | 2ee2114ae139 | no |  | PKU PTE stamping — shipped 2026-06-07; Architecture |
| other | pmm_vmm_real_hw_audit_2026_05_26.md | 7266 | 47 | 2026-05-26 | 800363ed66e0 | no | next | Closed in 135008f (stress_matrix.sh 8/8 PASS); Follow-up — both deferred items closed in 2b326ca |
| other | production_foundation_roadmap.md | 22483 | 366 | 2026-09-26 | 61d7b7f9877c | yes | active,open,next | BoxOS production-foundation roadmap; P0 — Foundation blockers for any production deployment |
| other | project_boxos.md | 1312 | 25 | 2026-04-14 | a551bb3a7499 | yes |  |  |
| other | prompt_after_burst_2026_09_10.md | 7847 | 101 | 2026-09-10 | c0e034042ee4 | yes | open,next | Приглашение вклинивалось в вывод (2026-09-10, сессия 9) — ЗАКРЫТО; ‼ КАК ЕГО ПОЙМАЛИ: лог с флешки, а не рассуждение |
| other | qemu_input_automation.md | 3136 | 56 | 2026-09-06 | 2cf153019f71 | yes |  | QEMU automated input for BoxOS; Subcommands |
| other | race_sysretq_2026_04_30.md | 3046 | 42 | 2026-04-30 | 974f4b5c6fda | no | next | Non-deterministic user page-fault race (FIXED 2026-04-30); Symptom |
| other | reboot_real_hw_audit_2026_05_26.md | 3800 | 26 | 2026-05-26 | 12b403304038 | no |  |  |
| other | roadmap_post_touchring_2026_06_01.md | 11940 | 253 | 2026-09-26 | cc310e2c727c | yes | active,open,next | Gap 1 — Cross-cabin shared memory — DECIDED; Gap 2 — Large-page user API — DECIDED |
| other | root_cause_hpet_irq0_2026_05_19.md | 4484 | 75 | 2026-05-19 | 4d63e3518b7c | no |  | The user's symptoms (all SAME root cause); Bisect chain (2026-05-19 session) |
| other | serial_console_2026_07_16.md | 3358 | 30 | 2026-07-16 | e9cb64d98885 | yes |  |  |
| other | sfmask_smap_mystery_2026_06_08.md | 6594 | 82 | 2026-08-30 | 02e38dc60dde | yes | next | SFMASK + SMAP + UEFI 16c mystery — ЗАКРЫТО; ✅ РАЗВЯЗКА (сверено 2026-08-30 по дереву, HEAD `54b1415`) |
| other | smap_ac_rf_pl0ssp_2026_06_08.md | 7196 | 143 | 2026-06-08 | e33f6ea5a543 | no | next | Real-HW production hardening — SMAP / CET / uaccess (2026-06-08); Root cause of UEFI STRICT 16c hang — found |
| other | spec_audit_2026_09_17.md | 183011 | 774 | 2026-09-26 | 1dc6ace86a8e | yes | active,current,open,next,critical | Находки аудита спецификаций — ЕДИНЫЙ ЖИВОЙ СПИСОК; Состояние на 2026-09-18 (волна CRITICAL закрыта) |
| other | stand_defects_fresh_clone_2026_09_07.md | 4541 | 49 | 2026-09-26 | 7c30c4c5d65a | yes | active,open,next | Дефекты стенда, найденные на чистой копии дерева (2026-09-07) |
| other | strict_qemu_run_bg_2026_06_07.md | 3162 | 65 | 2026-06-07 | b43a4e6c8636 | yes |  | STRICT testing is broken in run-bg → stress matrix → kernel 1 GB-page hang; Two stacked bugs |
| other | tag_books_map_2026_09_07.md | 3979 | 44 | 2026-09-07 | 1ea3726b29ea | yes | active | Книги тегов BoxOS — где какой тег получает номер |
| other | tme_full_impl_2026_06_08.md | 7783 | 174 | 2026-06-08 | 3296ddd98672 | no | open | TME / TME-MK end-to-end + flag hardening — shipped 2026-06-08; Part 1: production-grade compile flags |
| other | tsc_calib_production_2026_05_19.md | 3684 | 68 | 2026-05-19 | 650cb4f02ed3 | no |  | Permanent solution chain; Calibration source preference (cpu_calibrate.c) |
| other | turn_in_and_bell.md | 22581 | 269 | 2026-09-19 | 721359e3d9f7 | yes | active,open,next | Turn In + Bell — сон машины (сессия 2026-09-05); ‼ Поправка к плану, которая стоит трёх сессий отладки |
| other | unattended_read_proof_2026_09_09.md | 7681 | 71 | 2026-09-09 | 0a8ed0cf6e5a | yes | active,open,next | [UNATTENDED READ] — чтение без присмотра, доказанное при загрузке (2026-09-09, сессия 7); Что было неверно в памяти |
| other | user_prefs.md | 1167 | 17 | 2026-04-14 | 055023a163dd | yes |  |  |
| other | userspace_apps_audit_2026_05_26.md | 3765 | 81 | 2026-05-26 | b2dc4fd8811f | no | next | shell / display / utils real-HW audit — 2026-05-26 (session 2); Why: this turn |
| other | utils_pack_2026_09_10.md | 16924 | 193 | 2026-09-10 | 28da69d2662e | yes | active,open,next | Пакет утилит 2026-09-10 (сессия 8); Решения владельца (опрос, 2026-09-10) |
| other | vector_allocator_design.md | 25199 | 263 | 2026-09-22 | 0047ea37b8b0 | no | active,open,next,critical | Распределитель векторов — дизайн; Первоисточник РАЗРЕШАЕТ замену |
| other | vga_fb_fixes.md | 3310 | 45 | 2026-04-21 | 57c67e0f3a5f | no |  | Round 2 fixes — scroll still broken after shadow buffer |
| other | vmm_1gb_guard_page_bug.md | 3746 | 36 | 2026-05-15 | 63b82db92fa2 | no | next | 1. AP-7 boot crash (UEFI+STRICT+16G+16cores) — `vmm.c`; 2. Binary garbage in serial.log under SMP load — `serial.c/h`, `hardware_ops.c` |
| other | watchlog_audit_2026_09_20.md | 788954 | 3163 | 2026-09-26 | 2ab52e4d38d8 | yes | active,current,open,next,wip,critical | Разбор watch.log 2026-09-20 — полный список находок; Молчание AHCI на прогоне 2026-09-20 (i5-9400F / B365 / UEFI, ядро b952961): что на самом деле стоит на портах 4 и 5, почему порты 0-3 отвергнуты по DET=4h, и почему о живых дисках не сказано ни слова |
| other | wedge_userspace_spinner_2026_09_02.md | 26878 | 343 | 2026-09-26 | 3e0e357b76e8 | yes | active,open,next | Клин 16c: спиннер в userspace — форма доказана, корень называется машиной; Ворота на итоговом дереве (первый честный прогон ПОСЛЕ волн 11-12) |
| other | x2apic_msi_fixes_2026_05_09.md | 3440 | 40 | 2026-05-09 | 651fe6088a5f | no |  |  |
| storage | deed_pen_and_volume_growth_2026_09_10.md | 13352 | 158 | 2026-09-10 | 4cbd9d740b57 | yes | active,open,next | DeedPen и рост тома — СДЕЛАНО (2026-09-10, сессия 9); 1. Перо: `src/include/deed_pen.h` |
| storage | diskbook_v2_design_2026_05_25.md | 5091 | 36 | 2026-05-25 | 648f3f18be76 | no |  |  |
| storage | luggage_design_2026_09_07.md | 12479 | 121 | 2026-09-08 | 3ae0091ca7be | yes | next | Luggage — багаж кабины (дизайн-сессия 2026-09-07); Что есть сейчас (факты, прочитано целиком) |
| storage | volume_deed_ground.md | 24425 | 259 | 2026-08-27 | 3684d84c9a76 | yes | active,open,next | Том: Deed, Ground и Ledger; ▶ РЕШЕНИЯ ВЛАДЕЛЬЦА (сохранены) |
| storage | volume_grows_into_its_ground.md | 6107 | 78 | 2026-09-10 | 51b00af5b417 | yes | active,next | ✅ ЗАКРЫТО 2026-09-10 (сессия 9) — см. [[deed-pen-and-volume-growth-2026-09-10]]; Том берёт землю, которую ему дали — задача, поставленная 2026-09-10 |
| subsystem | brook_real_hw_audit_2026_06_03.md | 4619 | 50 | 2026-06-03 | 1bbf6f8a24da | no | open |  |
| subsystem | brook_subsystem_2026_06_02.md | 11973 | 220 | 2026-06-02 | 584864826593 | yes | open,next | Architecture (final); Layout / Surface |
| subsystem | console_input_redesign_2026_09_08.md | 10343 | 86 | 2026-09-08 | 43339efad39b | yes | active,open,next | Перестройка ввода консоли и знакомства с демоном (эпик, 2026-09-08); Решения владельца (опрос, сессия 3) — менять без него нельзя |
| subsystem | console_order_granted_with_lane_2026_09_11.md | 4401 | 60 | 2026-09-11 | eb85a231c64d | yes | active,open,next | Порядок выдаётся вместе с полосой (2026-09-11) — решение владельца; Дыра, которую закрыли |
| subsystem | console_rollcall_2026_09_08.md | 7453 | 77 | 2026-09-08 | 9ade0fbad640 | yes | open,next | Консоль: стренд сдаёт кадр, перевод строки едет в операции (2026-09-08); A/B на HEAD c1fb631, BIOS STRICT 16c, утилита с 4×400 нумерованных строк |
| subsystem | console_s1_rgb_progress.md | 11359 | 135 | 2026-09-01 | 75ea177f3dc5 | yes | active,next | S1 «RGB в ядре» — состояние реализации (2026-09-01, продолжение сессии); Что сделано (рабочее дерево, НЕ закоммичено на момент записи) |
| subsystem | console_s2s3_lanes_progress.md | 39042 | 439 | 2026-09-26 | 8a54bbc62233 | yes | active,current,open,next | S2+S3: Brook-полосы + событийный демон — реализовано (2026-09-01, вечер-2); Что сделано (файлы) |
| subsystem | console_stream_design.md | 6419 | 79 | 2026-09-26 | 636b539438e6 | yes | active,next | Консоль = поток: дизайн утверждён владельцем 2026-08-31; Решения владельца |
| subsystem | current_instability_2026_05_17.md | 2371 | 23 | 2026-05-17 | 3f010e3405d0 | no | open |  |
| subsystem | current_subsystem_2026_06_15.md | 6056 | 31 | 2026-06-15 | 82de9ccf72eb | yes | open,next | Current — BoxOS-native I/O spine (SHIPPED 2026-06-15) |
| subsystem | memtag_phase1_2026_06_05.md | 11270 | 197 | 2026-06-05 | 595bb8022556 | no | next | MemTag Phase 1 — fully complete 2026-06-05; What landed (uncommitted, user to commit) |
| subsystem | memtag_phase2a_2026_06_05.md | 7485 | 133 | 2026-06-05 | c6422a3c25c2 | no | open,next | MemTag Phase 2A — capability enforcement (soft) shipped 2026-06-05; Architecture |
| subsystem | memtag_phase2c_2026_06_06.md | 7410 | 120 | 2026-06-06 | 01a9627ac4a9 | no | open,next | MemTag Phase 2C — Continuous PTE enforcement (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2d_2026_06_06.md | 7282 | 125 | 2026-06-06 | 1246552a335d | no | open,next | MemTag Phase 2D — PTE bits 52-58 region-id fast cache + M5 AP-verify (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2e_2026_06_06.md | 7826 | 124 | 2026-06-06 | b75865205930 | no | next | MemTag Phase 2E — PAT comprehensive + MTRR audit (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2f_2026_06_06.md | 8265 | 129 | 2026-06-06 | 1a0d3e9403c1 | no | next | MemTag Phase 2F — MCE/MCA poisoning auto-tag (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2g_2026_06_06.md | 6013 | 116 | 2026-06-06 | ee061b0d3056 | no | next | MemTag Phase 2G — IOMMU MemTag/Touch integration (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2h_2026_06_06.md | 6830 | 106 | 2026-06-06 | c4c3b3c5e59b | no | next | MemTag Phase 2H — PKU/PKS infrastructure (2026-06-06); What landed (uncommitted) |
| subsystem | memtag_phase2i_2026_06_06.md | 4800 | 95 | 2026-06-06 | e000d8634ef3 | no | next | MemTag Phase 2I — LAM/UAI infrastructure (2026-06-06); What landed |
| subsystem | memtag_phase2j_2026_06_06.md | 4479 | 95 | 2026-06-06 | a30f4c1d866c | no |  | MemTag Phase 2J — TME/TME-MK infrastructure (2026-06-06); What landed |
| subsystem | memtag_phase2k_2026_06_06.md | 4747 | 116 | 2026-06-06 | 2902ca504d47 | no |  | MemTag Phase 2K — CET (Control-flow Enforcement) infrastructure (2026-06-06); What landed |
| subsystem | memtag_post_audit2_2026_06_07.md | 7642 | 123 | 2026-06-07 | 30e3e203e76f | no | next | Second post-Phase-2K audit — latent risk fixes (2026-06-07); All previously-documented risks → fixed |
| subsystem | memtag_post_audit_2026_06_07.md | 6553 | 99 | 2026-06-07 | 7428e31becf2 | no |  | Post-Phase-2K Audit — Architectural cleanup + bug fixes (2026-06-07); What was wrong, what was fixed |
| subsystem | memtag_remaining_work_2026_06_07.md | 7380 | 132 | 2026-06-07 | 7ea20146d421 | no | open | MemTag — Remaining work after audit-2 (2026-06-07); 1. TME `pmm_alloc_with_keyid` + VMM map KeyID preservation |
| subsystem | memtag_resume_handoff.md | 13653 | 100 | 2026-06-06 | 3ae40187ca3d | yes | active,open,next | MemTag — resume handoff; Уже SHIPPED (2026-06-05/06, uncommitted) |
| subsystem | nightwatch_lost_wake_persistence_2026_09_06.md | 10371 | 91 | 2026-09-06 | 2fdc7a369c22 | yes | active,open,next | LOST WAKE — доказательство по двум взглядам, не по одному; Что было в логе (`build/matrix_logs/BIOS_STRICT_16c_153815.log`, копия в scratchpad сессии) |
| subsystem | nightwatch_unanswered_submit.md | 9774 | 123 | 2026-09-05 | 2f6ffd57a8c5 | yes | active,current | Страж говорит только тогда, когда может доказать; Что оказалось неверным в первой реализации (тик) |
| subsystem | process_gone_wait.md | 11281 | 142 | 2026-09-03 | 4140ba71e4ff | yes | active,current,open,next | process.gone — ожидание смерти как состояние; Зачем (корень, доказанный рентгеном) |
| subsystem | rings_claim_and_deadlines.md | 15445 | 199 | 2026-09-04 | 438a16cd20f0 | yes | active,open,next | Кольца: занимать только доказуемо свободное, ждать только настоящую тишину; Доказательство, которое убрало ожидание |
| subsystem | strands_kernel_epic.md | 13908 | 43 | 2026-06-20 | 6a097e800da5 | yes | next | Strands — BoxOS kernel-эпик (потоки), owner-approved 2026-06-19 |
| subsystem | strands_p1_plan.md | 5964 | 36 | 2026-06-19 | bb39f1d00a4c | no | next | Strands P1 — исполнимый план (architect-vetted 2026-06-19) |
| subsystem | strands_p4_audit_findings.md | 9700 | 58 | 2026-06-19 | 008156501dbc | yes | next | Strands P1-P4 — полный ре-аудит конкурентности (4 агента, 2026-06-19) + план фиксов; КОРНИ НАБЛЮДАЕМЫХ 4c-СБОЕВ |
| subsystem | strands_p4_plan.md | 15172 | 65 | 2026-06-19 | 344d935b9a2f | no | current,open,next,critical | Strands P4 — go-live план + локнутые решения (owner 2026-06-19, через аналогии); ЛОКНУТЫЕ РЕШЕНИЯ (owner approved через AskUserQuestion+аналогии) |
| subsystem | strands_p5a_plan.md | 26654 | 245 | 2026-06-20 | 6a23536b6a34 | no | active,open,next,critical | Strands P5a — per-strand IPC rings + TLS (plan v2, после session-1); OWNER-РЕШЕНИЯ (locked, не переспрашивать) |
| subsystem | touch_claim_never_dropped.md | 14153 | 174 | 2026-09-04 | b73c597f46e6 | yes | active,open,next | Touch: заявка — это обещание, и оно не нарушается молча; Что было |
| subsystem | touch_design.md | 12683 | 173 | 2026-05-05 | a3cfe44f9769 | yes | open,next | Touch — BoxOS event mechanism (Phase 1 shipped 2026-05-04); Architecture |
| subsystem | touch_o1_redesign_2026_05_30.md | 6268 | 66 | 2026-05-30 | 5b3794fae7c5 | no |  | Touch O(1) inverted-index + handle ABI (2026-05-30); What changed |
| subsystem | touch_owed_queue.md | 8288 | 103 | 2026-09-03 | 48bc4f7f282d | yes | active,next | Touch: заявка — обещание. Повтор шага 2, взятый; Форма (почему она другая, чем в провалившемся заходе) |
| subsystem | touch_stress_complete_2026_05_05.md | 7238 | 68 | 2026-05-05 | 978749544b8b | no | next |  |
| subsystem | touch_stress_fix_2026_05_05.md | 4087 | 44 | 2026-05-05 | faf9cd6a8f05 | no |  |  |
| subsystem | touch_stress_s3_race_2026_06_02.md | 4609 | 103 | 2026-06-04 | 168266db76ed | no | next | Touch_stress S3 race — closed 2026-06-04; Symptom (observed 2026-06-02, 3× full stress_matrix runs) |
| subsystem | use_context_remembered_2026_09_08.md | 7495 | 69 | 2026-09-08 | 8b51a2b11687 | yes | active,next | Том помнит Use Context («ночные приказы», без нового имени); Решения владельца (AskUserQuestion, 2026-09-08) |
| subsystem | wire_design_2026_09_09.md | 6199 | 64 | 2026-09-09 | a52945c56373 | yes | next | Wire — провод читает лог-ринг (СДЕЛАНО 2026-09-09); Факты из кода (прочитано 2026-09-09) |
| tagfs | quill_checkpoint1_todo.md | 30867 | 226 | 2026-09-30 | 42be1fad6129 | yes | active,open,next | Хостовые сценарии, которых ждут шаги; Пробелы тестов из ревью 1.1 (scratchpad/review11.json — эфемерен; суть здесь) |
| tagfs | tagfs_block_collision_2026_05_06.md | 4449 | 54 | 2026-05-06 | b6c98bbab27a | no | next |  |
| tagfs | tagfs_dedup_index_2026_08_16.md | 9426 | 94 | 2026-08-16 | 5e10f697c7be | yes | active,next | TagFS dedup — блок уходит из индекса вместе с блоком; Как возник |
| tagfs | tagfs_file_visibility_bugs_2026_07_16.md | 2841 | 20 | 2026-07-16 | 6e0dd4e56291 | yes | next |  |
| tagfs | tagfs_mount_barrier.md | 9190 | 114 | 2026-08-30 | 65688388d0e7 | yes | active,open,next | Барьер монтажа TagFS — `HOLDGROUND=on`; Что было не так (перепроверено чтением, шире чем в передаче) |
| tagfs | tagfs_quill_design_2026_09_26.md | 167393 | 1234 | 2026-09-28 | a10b35b1ba15 | yes | active,current,open,next | Quill: ядро записи TagFS. Итоговое предложение владельцу (итоговая версия, с четвёртым дизайном); Что изменилось относительно предыдущей версии |
| tagfs | tagfs_quill_final_wave_archive.md | 313655 | 1643 | 2026-09-26 | b07a69320e35 | yes | active,current,open,next,critical | Архив последней волны (HEAD 56f9871c); АУДИТ: limits |
| tagfs | tagfs_quill_research_archive.md | 396809 | 1480 | 2026-09-26 | 483ea7b8f659 | yes | active,current,open,next,critical | Архив карт разведки волны 1 (HEAD 56f9871c); КАРТА: sync-write |
| tagfs | tagfs_real_hw_audit_2026_05_25.md | 8143 | 41 | 2026-05-25 | 114508b6af4d | yes | next |  |
| tagfs | tagfs_wave1_map_2026_09_29.md | 17279 | 148 | 2026-09-26 | f2eea2947ce3 | yes | active,open,next | Разведка волны 1 TagFS — сводка (2026-09-29); Факты о конкуренции (основа всего) |
| tagfs | v2_format_design_2026_10_08.md | 22900 | 161 | 2026-10-15 | fab068c9e67a | yes | active,open,next,wip | ✅ РЕШЕНИЯ ВЛАДЕЛЬЦА (10-08/09, пакет 1); ✅ РЕШЕНИЯ ВЛАДЕЛЬЦА (10-09, пакет 2) |
| verification | stress_matrix_extended_2026_05_27.md | 6039 | 134 | 2026-05-27 | 3c80fdc1dd0b | no | next | stress_matrix extension + WROTE payload unification — 2026-05-27; Why |
