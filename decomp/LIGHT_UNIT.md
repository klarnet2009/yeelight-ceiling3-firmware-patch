# Light unit / channel / effect manager / trans manager — yeelink.light.ceiling3 2.0.2_0048

Чистая реконструкция: `firmware/include/yglc_light_unit_clean.h`, `firmware/clean_src/yglc_light_unit_clean.c`.
Сборка: `python tools/build_fw.py` → `ARM Thumb-2 clean_src compile OK (4 module(s) -> firmware\build\clean_obj)`
(`-Wall -Wextra -Werror`, без ошибок и предупреждений). `.c` подключает `yglc_light_mixer_clean.h` и
вызывает микшер через его типизированную vtable (`yglc_mixer_t`), пишет duty через `yglc_pwm_group_t`, а таймер
переходов указывает на `yglc_light_controller_continue_transition`. В заголовке unit эти типы только
объявлены вперёд (те же теги структур), поэтому он собирается и отдельно, и вместе с заголовком микшера.

Всё восстановлено по дизассемблеру сырого образа (capstone, Thumb-2), а не по Ghidra. В диапазоне
0x100c06a0..0x100c26c8 Ghidra **не видит 15 функций** (в `functions.csv` их нет), склеивает хвостовые
вызовы с вызывающими (`FUN_100c1ad0` содержит тело `0x100be6bc`, `FUN_100c1de6` и `FUN_100c1dfa`
пересекаются, «`yglc_light_effect_sync_update_to_trans`» на деле обёртка над `0x100c0760`) и путает
число аргументов. Псевдокод из `yglc_core.c` для этого диапазона использовать нельзя.

Пометки: **BUG** — ошибка самой прошивки, **ПРЕДПОЛОЖЕНИЕ** — не проверено на устройстве или по коду
за пределами диапазона.

## 1. Карта функций

`*` — функции нет в `functions.csv`.

### Trans manager (переходы ШИМ)

| Адрес | Чистое имя | Назначение |
|---|---|---|
| 0x100c06a0 | `yglc_trans_tuple_steps` | dur ограничивается [300, 3600000] мс, шаг ≥ 10 мс; возвращает ceil(dur/шаг) |
| 0x100c06d4* | `yglc_trans_manager_retarget` | новая цель для идущего перехода (keep_from: пересчитать и начало), пропорциональное масштабирование шагов, остаток не меньше 200 мс |
| 0x100c07d4 | `yglc_trans_manager_start_transition` | старт перехода: mixer +0x34/+0x2c/+0x50/+0x30, таймер → 0x100c02bc |
| 0x100c0dcc | `yglc_new_trans_manager` | calloc 0xe0 |

### Эффекты (стек эффектов канала)

| Адрес | Чистое имя | Назначение |
|---|---|---|
| 0x100c0760* | `yglc_effect_manager_retarget` | кортеж текущего эффекта → retarget |
| 0x100c0910 | `yglc_light_effect_next_trans` | следующий переход эффекта или вызов on_finish |
| 0x100c0a04 | `yglc_effect_on_trans_done` | done-callback trans manager'а → next_trans(m, 0) |
| 0x100c0a14 | `yglc_effect_flow_chain_add` | добавить узел потока (0x24 Б) |
| 0x100c0a80 | `yglc_effect_flow_chain_free` | |
| 0x100c0abc | `yglc_effect_flow_add_duration` | +delta к длительностям, округление до 10, ограничение 1..10 с |
| 0x100c0af8 | `yglc_effect_flow_set_bright` | одна яркость на все узлы |
| 0x100c0b14 | `yglc_light_effect_create_new` | calloc 0x3c |
| 0x100c0b8c | `yglc_light_effect_free` | |
| 0x100c0bfc | `yglc_light_effect_parse_flow_string` | разбор "count,action,dur,mode,value,bright,…" |
| 0x100c0e50 | `yglc_new_effect_manager` | calloc 0x20 |
| 0x100c0eb4 | `yglc_light_effect_sync_update_to_trans` | яркость → все узлы потока, retarget(keep_from=1) |
| 0x100c0ecc | `yglc_effect_flow_adjust_speed` | |
| 0x100c0eec | `yglc_effect_manager_stop` | `yglc_timer_del(trans->timer)` |
| 0x100c0ef4 | `yglc_light_effect_push` | |
| 0x100c0f24 | `yglc_light_effect_resume` | перезапуск верхнего эффекта (с новой dur для типов 1 и 8) |
| 0x100c0f4c | `yglc_light_effect_pop` | |
| 0x100c0fcc | `yglc_light_effect_pop_types` | снимать, пока `type & mask` |
| 0x100c0fee | `yglc_light_effect_pop_finite_flows` | снимает только конечные потоки (count ≠ 0) |
| 0x100c1014 | `yglc_light_effect_start` | вытеснение по типу → push → cbs → resume |

### Канал

| Адрес | Чистое имя | Назначение |
|---|---|---|
| 0x100c10b4 | `yglc_new_light_channel` | calloc 0x1b4 |
| 0x100c1120 | `yglc_channel_fill_flow_tuple` | «относительные» узлы потока → абсолютные по состоянию канала |
| 0x100c1204 | `yglc_channel_before_trans` | cb +0x2c (заполнить узел перед переходом) |
| 0x100c1224 | `yglc_light_channel_update_flow_string` | ch->flow_str из идущего бесконечного потока |
| 0x100c12f8 | `yglc_light_channel_commit_tuple` | кортеж → ct / bright / off канала |
| 0x100c136c | `yglc_light_channel_on_effect_start` | cb +0x28 |
| 0x100c1418* | `yglc_light_channel_flow_adjust_speed` | |
| **0x100c1434** | **`yglc_light_channel_apply`** | **единственная воронка всех статических изменений цвета/яркости канала** |
| 0x100c1520* | `yglc_light_channel_set_raw` | mixer +0x5c, прямая запись в ШИМ без перехода |
| 0x100c15d4* | `yglc_light_channel_set_rgb` | |
| 0x100c1640* | `yglc_light_channel_set_nl_rgb` | {7, 0xff9800, bright 1} (nl_type 1) |
| 0x100c1664* | `yglc_light_channel_set_nl_ct` | {7, min_ct, bright 1} (nl_type 2) |
| 0x100c1682* | `yglc_light_channel_set_hsv` | |
| 0x100c16bc* | `yglc_light_channel_set_ct` | ct ограничивается [min_ct, max_ct], 0 → текущий |
| 0x100c16f4 | `yglc_light_channel_set_bright` | поток: sync_update; иначе apply{2, ct, bright} |
| 0x100c1780 | `yglc_light_channel_restore` | заново применить сохранённые bright/ct, затем поток или снова выключение |
| 0x100c17b8 | `yglc_light_channel_start_flow` | |
| 0x100c1854 | `yglc_light_channel_exit_nl` | is_nl = 0, restore_saved, restore |
| 0x100c1868 | `yglc_light_channel_power_off` | эффект OFF (bright 0, dur) |
| 0x100c18d0 | `yglc_light_channel_power_on` | снять OFF/8 и продолжить эффект под ними (или restore) |

### Unit

| Адрес | Чистое имя | Назначение |
|---|---|---|
| 0x100c1920 | `yglc_new_light_unit` | calloc 0x164 |
| 0x100c1978 | `yglc_light_unit_find_channel` | активный, если поддерживает режим; иначе первый подходящий |
| 0x100c19bc | `yglc_light_unit_on_flow_step` | cb канала +0x17c: перенос потока на другой канал |
| 0x100c1ad0 | `yglc_light_unit_on_channel_state` | cb канала +0x184: nl_active / active_mode / active_bright / off |
| 0x100c1b94 | `yglc_light_unit_restore_all` | |
| 0x100c1bc6 | `yglc_light_unit_on_flow_end` | cb канала +0x180: действие по окончании потока |
| 0x100c1cb0 | `yglc_light_unit_attach_channel` | |
| 0x100c1cdc | `yglc_light_unit_activate` | active = idx, этот канал включить, остальные погасить |
| 0x100c1d12 | `yglc_light_unit_set_power` | |
| 0x100c1d4a | `yglc_light_unit_set_bright` | → set_bright активного канала |
| 0x100c1d64 | `yglc_light_unit_stop_flow` | stop_cf |
| 0x100c1d90 | `yglc_light_unit_ct_in_range` | |
| **0x100c1db8** | **`yglc_light_unit_select_channel`** | **переключатель dl↔nl по режиму** |
| 0x100c1de6 | `yglc_light_unit_set_raw` | |
| 0x100c1dfa | `yglc_light_unit_set_rgb` | |
| 0x100c1e2e | `yglc_light_unit_set_hsv` | |
| 0x100c1e62* | `yglc_light_unit_set_ct_keep_bright` | |
| 0x100c1e92 | `yglc_light_unit_set_ct` | |
| 0x100c1ec8 | `yglc_light_unit_adjust_ct` | |
| 0x100c2008 | `yglc_light_unit_nl_set_bright` | nl_br; switch: active = 1 + гасит dl |
| 0x100c2050 | `yglc_light_unit_enable_nl` | |
| 0x100c20b4 | `yglc_light_unit_disable_nl` | |
| 0x100c2164 / 0x100c2248 | `yglc_light_unit_adjust_rgb_new` / `_adjust_rgb` | на ceiling3 таблиц нет → ошибка 1 |
| 0x100c232c* | `yglc_light_unit_start_flow` | |
| 0x100c2366 | `yglc_light_unit_flow_adjust_speed` | |
| 0x100c237c | `yglc_light_unit_arm_night_autooff` | req 0x15 (cron «выключить через N мин») |
| **0x100c23b0** | **`yglc_light_unit_power_on`** | **включение из выключенного: день или ночник** |
| 0x100c243c | `yglc_light_unit_set_power_on_mode` | set_power on, режимы 0..5 |
| 0x100c24a8 | `yglc_light_unit_adjust_bright` | |

### API по маске unit (0 → 1; существует только бит 0 = settings->unit)

`0x100c2484` power_off, `0x100c2574*` power_on(mask, mode, dur) (вызывает `FUN_100bdcfa` = req 1),
`0x100c2594` set_bright, `0x100c25b4*` set_raw, `0x100c25d4*` set_rgb, `0x100c25fc` set_hsv,
`0x100c2628` ct_in_range, `0x100c2650*` set_ct, `0x100c2670` adjust_ct, `0x100c269c` adjust_rgb,
`0x100c26c8` adjust_bright. Функция `0x100c26e8` (команды "power"/"wrgb" через mixer +0x60,
заводской тест) не реконструирована.

### Вне диапазона, но тоже восстановлены

`0x100c021c` flow_step_check (cb эффекта +0x34), `0x100c0238` rgb_distance, `0x100c0578` table_step,
`0x100c05e4` effect_current_tuple, `0x100c05f4` on_effect_finish (cb +0x30), `0x100be560` **is_night_time**,
`0x100be6bc` unit_state_notify (хвост `on_channel_state`).

## 2. Структуры (все смещения закреплены `_Static_assert`)

Граф объектов (`yglc_setting_cache_alloc` @0x100bad18, строки 757-830 `yglc_core.c`):

```
settings(+0x58) ─► unit "main" (0x164): off=1, count=2, active=0, nl_type=0, nl_status=0
                     channel[0] "dl" (0x1b4) mask 0x04 (CT), ct 4000, 2700..6500, bright 100
                        └ effect_mgr "dl" (0x20) ─► trans "dl" (0xe0): mixer CT, pwm group 0,
                                                     curve 2/2, step 10 мс, min_bright 15/15
                     channel[1] "nl" (0x1b4) mask 0x80 (mode 7), 1700..6500, bright (nl_br) 5
                        └ effect_mgr "nl" ─► trans "nl": mixer single, pwm group 1,
                                                     curve 1/0, step 10 мс, min_bright 0/0
```

Аргументы `yglc_new_trans_manager(name, mixer, pwm, curve_new(+0xcd), curve_same(+0xcc), step(+0xc0),
min_bright(+0xd4), min_bright_nl(+0xd0))`. Для dl: `("dl", m, g, 2, 2, 10, 15, 15)`, для nl: `(.., 1, 0, 10, 0, 0)`.

**Кортеж перехода** `yglc_trans_tuple_t` (0x18): `+0 u16 ct`, `+4 u8 bright` (0 → цель = 0), `+5 has_ct`,
`+8 float scale` (= 1.0), `+0xc duration_ms`, `+0x10 step_ms`, `+0x14 is_nl` (выбирает min_bright_nl).

**Эффект** `yglc_light_effect_t` (0x3c): `+0 type` (1 STATIC, 2 FLOW, 4 OFF, 8 STATIC_8), `+4` объединение
{кортеж | поток: count, iter, nodes, cur, action, allow_switch}, `+0x20` звено стека, `+0x28` callbacks
{on_start, before_trans, on_finish, flow_step_check, ctx = канал}. Узел потока (0x24): `{mode, tuple, link}`.

**Effect manager** (0x20): `+0 top`, `+4` стек (LIFO), `+0xc trans`, `+0x10 name`.

**Trans manager** (0xe0): `+0x10` таймер (0x28), `+0x38/+0x3c` total/cur step, `+0x40` step_ms,
`+0x44` {done, step, ctx}, `+0x58 prev` кортеж, `+0x70 cur` кортеж, `+0x88 from[4]`, `+0x98 out[4]`,
`+0xa8 target[4]`, `+0xb8 pwm`, `+0xbc mixer`, `+0xc0` шаг по умолчанию, `+0xc4/+0xc8` committed/pending bright
(выбор кривой: та же яркость → +0xcc, новая → +0xcd), `+0xd0/+0xd4` min_bright, `+0xd8 retarget`,
`+0x50..0x57` и `+0xdc` не используются.

**Канал** (0x1b4): `+0 ct`, `+4 max_ct`, `+8 min_ct`, `+0x14 rgb`, `+0x18/+0x1c hue/sat`, `+0x20 color_mode`,
`+0x24 bright`, `+0x2c flow_active`, `+0x30 flow_str[256]`, `+0x130 effects`, `+0x134 off`,
`+0x138..+0x167` таблицы adjust (bright, ct, rgb, aux_a, aux_b, rgb_new + длины), `+0x168 name`, `+0x178 mode_mask`,
`+0x17c/+0x180/+0x184` callbacks unit'а, `+0x188 owner`, `+0x18c enabled`, `+0x190 u8 is_nl`, `+0x194 restore_saved`,
`+0x198/+0x19c` последний режим и ct потока, `+0x1a4 last_bright`, `+0x1b0 skip_resume_once` (кто пишет, в
`yglc_core.c` не найдено).

**Unit** (0x164): `+0 off`, `+4 count`, `+8 channel[2]`, `+0x10 active`, `+0x14 name`, `+0x24 flowing`,
`+0x28 flow_params[256]`, `+0x128 unit_index`, `+0x12c nl_status` (свойство, сохраняется), `+0x130 nl_active`,
`+0x134 nl_br_effective`, `+0x138 u8 nl_type`, `+0x13c active_mode`, `+0x140 active_bright`,
`+0x144 saved_req{mode,value,aux,bright}` (последний не-ночной запрос), `+0x154 aux_power` (ИЛИ в settings->power,
кто пишет — не найдено), `+0x158 flow_switched`, `+0x15c/+0x160` сохранённые active и nl_status.

Используемые поля settings (0x118): `+0x54 power_off`, `+0x58 unit`, `+0x60 save_state`, `+0x6c night_autooff_armed`,
`+0xbc report_mute`, `+0xc4/+0xc8` начало и конец ночи (секунды суток), `+0xcc nighttime_enabled`,
`+0xcd` строка "nighttime", `+0xf4 night_autooff_min`, `+0xf8 is_night_time` (кэш), `+0x100 init_power_opt`,
`+0x110` длительность перехода по умолчанию.

## 3. Ответы

### В1. Структуры
См. раздел 2 и заголовок (`_Static_assert` на каждое смещение, которое читается кодом).

### В2. Переключение dl ↔ nl

**Кто меняет `unit->active` (+0x10):**

| Где | Когда | Что делает |
|---|---|---|
| `select_channel` 0x100c1db8 | любая команда с режимом (set_ct → CT → dl; enable_nl для nl_type 1/2) | `idx = find_channel(mode)`; если индекс другой: `active = idx`, **старый канал → power_off(dur)**. Новый канал не трогает: его зажигает следующий `apply` вызывающего |
| `nl_set_bright(sw=1)` 0x100c2008 | enable_nl (nl_type 0): set_power mode 5, nl "on", ночное включение | `active = 1` **до** гашения dl, `power_off(dl, dur)`, затем `set_bright(nl, b)` |
| `activate` 0x100c1cdc | disable_nl (nl_type 0) | `active = 0`; dl → power_on (если unit не выключен), nl → power_off |
| `power_on` 0x100c23b0 | дневной путь | `active = 0`; `set_power(u, 0)`: dl → power_on, nl → power_off |
| `on_flow_step` / `on_flow_end` | поток с узлом другого режима (mode 8 → nl) | перенос эффекта потока на другой канал или возврат |

**Питание и яркость каналов.** У каждого канала свой стек эффектов, свой trans manager, свой mixer и своя
группа ШИМ. Погасить канал — положить на стек эффект OFF (переход к 0 за `dur`), предыдущий эффект остаётся
под ним. Включить — снять OFF/8 и продолжить эффект под ними (`resume(dur)`); если стек пуст или у dl стоит
`skip_resume_once`, выполняется `restore` (заново применить `ch->bright` и `ch->ct`). Поэтому переключение —
это **кроссфейд двух независимых переходов**: старый канал гаснет за `dur`, новый разгорается за `dur`
(в путях nl_type 1/2 старый канал гаснет за фиксированные 300 мс). Яркости каналов не связаны: `dl->bright`
(свойство "bright") и `nl->bright` (свойство "nl_br") при переключении не копируются и не меняются; ночник
включается на сохранённой nl_br (0 → 5).

**Отчётные поля** пересчитывает `on_channel_state` 0x100c1ad0 при каждом commit или старте эффекта
любого канала:
- nl_type 0, `active == 1`: `nl_active = 1`, `active_mode = 1`, `nl_br_effective = nl->bright`;
- nl_type 0, `active == 0`: `nl_active = 0`, `active_mode = 0`, `nl_br_effective = 0` **и `nl_status = 0`**;
- всегда: `active_bright = bright активного канала`; если сработал активный канал — `unit->off = ch->off`,
  `flowing`, `flow_params`;
- хвост `0x100be6bc`: `settings->power_off = unit->off || unit->aux_power`, отмена cron-заданий нужного типа
  (ПРЕДПОЛОЖЕНИЕ о назначении `FUN_100c0158`), сброс ночного автовыключения при выходе из ночника, отчёт свойств.

`nl_status = 1` ставит только `enable_nl(..., set_status=1)`: set_power mode 5 и команда nl "on"
(`FUN_100be44c` → `FUN_100be430(br, 1)`). Сбрасывают его `disable_nl(clear=1)` и любой показ дневного
света (`on_channel_state`). Выключение и повторное включение ночника его не сбрасывают: при выключении
`active` остаётся 1.

**Переход.** `steps = ceil(clamp(dur, 300, 3600000) / 10)`, период таймера `step_ms/10` = 1 тик (10 мс).
Значит 300 мс — это 30 шагов, 500 мс — 50. Минимальная длительность 300 мс, даже если просили 0.

### В3. Как bright доходит до микшера, и где вставлять «гибрид»

```
set_bright (req 4) → api_set_bright 0x100c2594 → unit_set_bright 0x100c1d4a
  → channel_set_bright 0x100c16f4          [поток: sync_update → retarget 0x100c06d4, см. ниже]
  → channel_apply 0x100c1434               эффект STATIC: tuple{ct = (u16)req.value, bright, dur, is_nl}
  → effect_start 0x100c1014 → push → resume 0x100c0f24
  → next_trans 0x100c0910                  tuple.step = 10; before_trans
  → BL @0x100c09ce → start_transition 0x100c07d4:
        mixer+0x34 set_trans_curve(cc | cd)
        from = out
        mixer+0x2c set_min_bright(is_nl ? +0xd0 : +0xd4)      dl: 15, nl: 0
        bright == 0 ? target = {0} : mixer+0x50 compute_target(mixer, ct, bright, 1.0f, tm+0xa8)
        is_nl → target[3] = 0
        mixer+0x30 set_trans_steps(steps)
        таймер → 0x100c02bc (каждые 10 мс: mixer+0x54 interpolate(from, target, step) → duty → 0x100c0272)
```

Тем же путём идут set_ct, restore (power on), шаги потоков и OFF (bright 0). Второй и последний вызов
`+0x50` — `retarget` 0x100c06d4 (смена яркости во время потока; единственный вызывающий — хвостовой B.W
@0x100c07ba). У `start_transition` 0x100c07d4 **ровно один вызывающий — BL @0x100c09ce** (проверено
сканированием всего образа).

**Патч 1 (рекомендую): перехват BL @0x100c09ce.** Заменить `bl 0x100c07d4` на `bl yglc_hybrid_start_transition`
(код в EXT_FLASH 0x10150650+, до него ~0x90000, в пределах дальности BL). Реализация лежит в конце
`yglc_light_unit_clean.c` (помечена PROPOSAL). Логика:
- `active != 0` (настоящий ночник): ничего не трогаем;
- переход dl с `1 ≤ bright < K`: вызвать оригинал, затем обнулить `dl_tm->target` (dl гаснет за те же шаги)
  и запустить переход `nl_tm` к `f(bright)` с той же длительностью; callbacks nl сохраняются;
- переход dl с `bright ≥ K` или 0: если гибрид был включён, nl плавно уходит в 0;
- переход nl с bright 0 (OFF, который unit кладёт на nl, пока активен dl: `set_power` сначала включает dl,
  потом гасит nl) при включённом гибриде заменяется на уровень гибрида, иначе ночник погас бы сразу.

Без BSS: уровень гибрида хранится в неиспользуемом `dl_tm+0xdc` (в 0x100c02bc..0x100c2800 к нему
не обращаются). Размер −Oz: **268 байт** плюс 4 байта патча BL.

**K и f(b) по замеру (LIGHT_MIXER.md).** Драйвер основных светодиодов гаснет ниже ~5.5% скважности и
светит одинаково-минимально до ~10.5–11% (≈ nl_br 100). Ночник линейный: nl_br N = N% скважности.
- С патчами микшера #9a (min_bright 0) и #9b (пол 6.5): dl bright N ≈ 6.5 + 0.935·N %. Яркости 1..4
  (7.4..10.2%) — одно и то же плато, 5 (11.2%) — первая реальная ступень затемнения. Отсюда
  **K = 5** (по умолчанию, `YGLC_HYBRID_K`).
- f — квадратичная, f(K−1) = 100 (уровень плато), чтобы на стыке с dl@K не было провала:
  `f(b) = ceil(100·b² / (K−1)²)`, при K = 5 это nl_br 7 / 25 / 57 / 100 для bright 1..4.
- Со **стоковым** микшером (min_bright 15, пол 8) dl bright 1 = 21.8% скважности, то есть плато вообще
  не используется. Гибрид тогда даёт скачок на K, поэтому его нужно ставить вместе с #9a.
- Чтобы отдать ночнику больше ступеней, придётся сдвинуть шкалу dl в микшере (b ≥ K → начало сразу
  над плато). Это задача микшера, здесь не делалось.
- Что видит приложение: `active_mode = 0`, `bright = b`, ct не меняется. Сохранение настроек не затронуто
  (сохраняется dl@b), поэтому после выключения и включения гибрид восстанавливается сам.
- Риски: (а) не покрыт retarget (смена яркости внутри потока): при необходимости так же перехватить B.W
  @0x100c07ba; (б) пока гибрид включён, nl-группа светит при «выключенном» nl-канале: команды ночника
  работают как раньше (они ставят `active = 1` до гашения dl, hook сразу отпускает nl), но это стоит
  проверить на устройстве; (в) ПРЕДПОЛОЖЕНИЕ: таймерные callback'и выполняются в той же задаче yglc, что и
  `next_trans`, поэтому обнуление target сразу после старта не гоняется с шагом таймера; (г) ПРЕДПОЛОЖЕНИЕ:
  оба драйвера светодиодов выдерживают одновременную работу (штатный кроссфейд dl↔nl делает так 300+ мс).

**Патч 2 (альтернатива, хуже): на уровне unit.** В `unit_set_bright` 0x100c1d4a, `set_ct` 0x100c1e62/0x100c1e92
и `power_on` при `active == 0 && b < K` вызывать `nl_set_bright(u, f(b), dur, 1)`. Всё делается штатными
средствами (кроссфейд, отчёт), около 80 байт, но приложение увидит `active_mode = 1`, `active_bright` и
`nl_br = f(b)`, а `dl->bright` останется старым. Следующий set_bright из приложения пойдёт в nl-канал;
`nl_status` и сохранённое состояние поменяют смысл. Нужна обратная логика выхода (b ≥ K → `disable_nl`).
Объём и риски для пользовательского поведения заметно выше.

Не подходит: объединить каналы в одну группу ШИМ. Группа рассчитана на 2 вывода (0x44 Б), а запись duty в
`continue_transition` для каналов 2 и 3 попадает на смещения со сдвигом 4 байта относительно nl-группы.

### В4. Включение (power on)

1. **Загрузка.** `yglc_init_async`: `init_power_opt` (+0x100) = 1 или `FUN_100bebb8() != 0` → `power_off = 0`;
   2 → `power_off = 1`; иначе остаётся восстановленное значение. Затем продуктовый init `FUN_100bac8c`: если
   `power_off == 0` — `power_off = 1`, постановка **req 1** {mask, dur 500 (300, если `FUN_100c323c() == 2`),
   mode 0}, и `unit->off = 1`, чтобы mode 0 действительно отработал.
2. **req 1** → `FUN_100bdcfa` → `api_power_on(mask, mode, dur)` 0x100c2574 → `set_power_on_mode` 0x100c243c:
   - 0 normal: если `unit->off`, то `unit_power_on`; если уже включено, ничего;
   - 1 CT: `set_ct_keep_bright(ct=0 → текущая)`;
   - 4 flow: `start_flow("")` → последний сохранённый бесконечный поток канала;
   - 5 NL: `enable_nl(br=0 → nl_br или 5, dur, set_status=1)` → `nl_status = 1`;
   - 2, 3: ошибка 1.
3. **`unit_power_on` 0x100c23b0:**
   - **день**, если `nl_type == 3` или (`!is_night_time()` и `nl_status != 1`): для nl_type 1/2 выход из
     режима 7 с восстановлением `saved_req`; затем `active = 0` и `set_power(u, 0, dur)`: dl продолжает
     эффект под OFF (тот же цвет и яркость, длительность dur), nl гаснет;
   - **ночь** (или `nl_status == 1`): если `is_night_time()` и `night_autooff_min` (4-е поле "nighttime"),
     то `arm_night_autooff` (req 0x15, cron «off через N мин», `settings+0x6c = 1`); затем
     `enable_nl(u, 0, dur, set_status=0)` → `active = 1`, dl гаснет, nl на сохранённой `nl_br`
     (`nl_status` при этом не меняется).
4. **`is_night_time` 0x100be560:** 0, если `nighttime_enabled` (+0xcc) = 0. Без NTP возвращает прошлый
   кэш +0xf8. Иначе локальное время = UTC + tz из miio (если tz вне ±12 ч, берётся UTC+8),
   секунды суток сравниваются с [`+0xc4`, `+0xc8`] (с переходом через полночь; по умолчанию 22:00..06:00);
   результат кэшируется в +0xf8.
5. **`save_state`** (+0x60) в этом диапазоне не читается. Он влияет на `set_default` (req 0x14,
   `FUN_100be058`): при `save_state == 0` и включённом свете `nl_status = nl_active` и сохранение. Значит,
   «по умолчанию включаться ночником» можно закрепить и через set_default.
   Восстановление состояния после перезагрузки находится в `yglc_reboot_restore` (вне диапазона,
   не разбиралось).

## 4. Найденные ошибки прошивки (BUG)

- `yglc_new_trans_manager`: после неудачного calloc пишет по NULL.
- Разбор потока: `strtol(NULL)` для mode 3/7/8 без значения и для последнего узла режимов 4..7 без яркости.
  Временный узел не очищается между кортежами, поэтому mode 3/8 наследуют ct предыдущего узла.
  Mode 1 (RGB), 4 и 6 в строке потока отвергаются, хотя `fill_flow_tuple` их обрабатывает.
- `on_flow_end` (action 0 после переноса потока на другой канал): эффект потока отвязывается, но **не
  освобождается** (утечка 0x3c Б + узлы), а длительность гашения читается с **адреса 0x10**
  (`movs r3,#0; ldr r1,[r3,#0x10]`); значение затем ограничивается 300 мс..1 ч.
- `on_flow_step` / `on_flow_end`: после отвязки `top = stack.next − 0x20` без проверки на пустой стек.
- `effect_current_tuple`, `before_trans`, `effect_manager_retarget`: нет проверки `flow.cur == NULL`.
- `channel_apply`: `ct = (u16)req.value` — RGB и 0xff9800 (nl_type 1) обрезаются до 16 бит; `saved_req`
  всегда пишется в unit settings->unit, а не во владельца канала; `LU_LAST_IS_NL` общий на все каналы.
- `yglc_new_light_unit` печатает сообщение про light channel (копипаста).

## 5. Что не проверено / предположения

- Назначение `FUN_100c0158` (отмена cron-заданий по типу) и `FUN_100be34c` (хук смены питания) —
  ПРЕДПОЛОЖЕНИЕ по структуре кода.
- Кто пишет `dl->skip_resume_once` (+0x1b0) и `unit->aux_power` (+0x154): в `yglc_core.c` записей не найдено.
- Назначение таблиц `aux_table_a/b` (+0x150/+0x158) — в этом диапазоне их никто не читает.
- Для параметра set_trans_curve оригинал передаёт в r2 `committed_bright`; mixer его игнорирует
  (по реконструкции mixer'а).
- Hook гибрида проверен только компиляцией; поведение на лампе не проверялось.
