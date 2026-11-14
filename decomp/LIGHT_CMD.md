# Командный слой управления светом — yeelink.light.ceiling3 2.0.2_0048

Чистая реконструкция: `firmware/include/yglc_light_cmd_clean.h`, `firmware/clean_src/yglc_light_cmd_clean.c`
(собирается `python tools/build_fw.py` → `ARM Thumb-2 clean_src compile OK (3 module(s) ...)`).

Все смещения и ветвления проверены по дизассемблированному образу (Thumb-2, capstone), а не только по
выводу Ghidra: Ghidra ошибается в нескольких обработчиках (например, `set_power`, `FUN_100c243c`,
`yglc_proc_request` case 0x10, а у `FUN_100bdcfa` не видит хвостовой вызов `0x100c2574`).
Пометки: **BUG** — ошибка самой прошивки, **ПРЕДПОЛОЖЕНИЕ** — вывод без проверки на устройстве.

## 1. Общая схема

```
cloud (miIO) / LAN TCP 55443 (handle_lan_command 0x100c3ee0 -> call_method 0x100c3814,
                               белый список 16 методов @0x20000a34)
   -> реестр miIO "user.<m>" @0x20001328 (34 x 16 Б), у всех handler = 0x100bc7d1
   -> yglc_rpc_dispatch 0x100bc7d0 (таблица методов @0x20000aec, 32 x {flag,name,fn})
   -> обработчик метода (разбор params, проверки) -> json_rpc_to_yglc 0x100bbd44
   -> yglc_post_request_async 0x100bdbc0 (10 слотов x 0x238 @0x20006918, xQueue(10,4))
   -> yglc_task 0x100bff30 -> yglc_proc_request 0x100bfad4 (таблица запретов по power)
   -> листовые обработчики 0x100bdcfa..0x100be058 -> API light unit 0x100c1cb0..0x100c26e8
   -> колбэк 0x100bc44c отвечает клиенту {"result":["ok"]} или {"code":-5000-N}
   -> каждые 200 мс и после каждого запроса FUN_100c30b8(1): отчёт props + psm + nv-кэш
```

Коды ответа (`0x100bc33c`, code = −5000−N): 1 invalid params, 2 hardware fault, 3 timed out,
4 general error, 5 memory exhausted, 6 illegal request, иначе internal error.

## 2. Карта функций

| Адрес | Чистое имя | Назначение |
|---|---|---|
| 0x100bbd44 | `json_rpc_to_yglc` | постановка запроса с колбэком ответа; 0 / 5 (нет слота) / 7 (нет cb) |
| 0x100bbd98 | `yglc_m_stop_cf` | stop_cf → req 0x0b |
| 0x100bc33c | `yglc_rpc_reply_status` | ok / ошибка −5000−N |
| 0x100bc384 | `yglc_rpc_parser_free` | |
| 0x100bc398 | `yglc_m_set_default` | → req 0x14 |
| 0x100bc3c8 | `yglc_m_toggle` | → req 0x03 |
| 0x100bc44c | `yglc_rpc_on_request_done` | колбэк завершения запроса (ответ + free) |
| 0x100bc64c | `yglc_json_parser_init` | контекст разбора 0x68 Б |
| 0x100bc7d0 | `yglc_rpc_dispatch` | диспетчер методов (общий для облака и LAN) |
| 0x100bc864 | `yglc_rpc_pop_int` | params[i] → uint32 |
| 0x100bc8ac | `yglc_m_cron_add` | → req 0x15 |
| 0x100bcaa0 | `rpc_parse_adjust_pct` | [pct −100..100 ≠0, dur ≤ 60000, opt] |
| 0x100bcb00 / 0x100bcb58 / 0x100bcbb0 | `yglc_m_adjust_bright/ct/color` | → req 0x1b prop 4/5/6 |
| 0x100bcc00 | `yglc_rpc_pop_str` | params[i] → строка |
| 0x100bcc40 | get_prop | синхронно, читает `g_yl_cache_list` |
| 0x100bcd14 | `yglc_m_set_adjust` | → req 0x1b |
| 0x100bce38 | `yglc_m_set_ps` | → req 0x19 |
| 0x100bd074 | `yglc_rpc_parse_effect_duration` | ["effect", dur] |
| 0x100bd0f0 | `yglc_m_set_power` | → req 1/2 |
| 0x100bd160 | `rpc_parse_duration_effect_any` | effect/dur в любом порядке |
| 0x100bd234 | `rpc_pop_values_effect` | v1,(v2),effect,dur |
| 0x100bd2cc | `yglc_m_set_bright` | → req 4 |
| 0x100bd4ac | `yglc_m_set_ct_abx` | → req 5 |
| 0x100bd50c | `rpc_pop_values_effect_any` | |
| 0x100bd560 | `rpc_parse_cf` | "count,action,flow" |
| 0x100bd610 | `yglc_m_start_cf` | → req 0x0a |
| 0x100bd668 | `yglc_m_set_scene` | → req 0x10 |
| 0x100bdad8 / 0x100bdb1c | `req_slot_alloc/free` | пул 10 слотов под BASEPRI |
| 0x100bdb3c / 0x100bdba4 | `req_queue_send/recv` | |
| 0x100bdbc0 | `yglc_post_request_async` | |
| 0x100bdc40 | startup propsync | ждёт облако, затем req 0x1f |
| 0x100bdcb0 | `yglc_reset_bright_to_max` | |
| 0x100bdcfa | `yglc_req_power_on` | → 0x100c2574 → `unit_power_on_mode` 0x100c243c |
| 0x100bdd0e | `yglc_req_power_off` | → 0x100c2484 → 0x100c1d12(unit,1,dur) |
| 0x100bdd24 | `yglc_req_toggle` | по unit->off |
| 0x100bdd7c / 0x100bddc4 | `yglc_req_set_bright(2)` | → 0x100c2594 → 0x100c1d4a (АКТИВНЫЙ канал) |
| 0x100bde0c / 0x100bde20 | `req_set_ct` | → 0x100c2650 → 0x100c1e92 (переключает на DL!) |
| 0x100bde46 / 0x100bde52 / 0x100bde66 / 0x100bde76 / 0x100bde9c | wrgb / rgb / scene color / hsv / scene hsv | |
| 0x100bdebc | `yglc_req_adjust` | req 0x1b |
| 0x100bdfd8 | `req_start_cf` | |
| 0x100be058 | `yglc_req_set_default` | |
| 0x100be088 / 0x100be0d0 | set_ps cfg_lan_ctrl / miband_sleep | |
| 0x100be150 | `yglc_ps_nighttime` | |
| 0x100be34c | `settings->power = x` | |
| 0x100be364 | `yglc_set_music` | req 0x1d |
| 0x100be414 / 0x100be430 | NL off / NL on (обёртки) | |
| 0x100be44c | `yglc_ps_nightlight` | set_ps "nightlight" |
| 0x100be520 / 0x100be560 | nighttime enabled / `yglc_is_night_time` | |
| 0x100be728..0x100be944 | действия кнопок BLE | см. §5 |
| 0x100be9a0 | `ble_blink_cf` | мигание через сцену CF |
| 0x100bea68 | `req_alert` | req 0x1c |
| 0x100beb4c | `ble_long_off` | |
| 0x100beca4 / 0x100bed54 | factory passed / factory mode | |
| 0x100bebb8 | `yglc_fast_reboot_count_load` | psm app_data/fast_reboot_count |
| 0x100bebf0 | `yglc_check_fast_reboot` | 5 быстрых включений → сброс Wi-Fi |
| 0x100becd8 | `fast_reboot_count_zero_arm` | таймер 5 с → счётчик "0" |
| 0x100bedb4 | `yglc_init_async` | (в т.ч. `yglc_boot_decide_power`) |
| 0x100bac8c | `yglc_product_power_on_at_boot` | включение при холодном старте |
| 0x100bf1e0 | `yglc_post_nightlight_ps` | постит set_ps("nightlight", on/off/set) |
| 0x100bf248 / 0x100bf280 | `ble_moon` / `ble_moon_long` | |
| 0x100bf29c | `yglc_seesaw_event` | ручка-«качель» |
| 0x100bf5a8 | `yglc_handle_ble_event` | |
| 0x100bf740 | `yglc_ps_pseudo_beacon` | виртуальная кнопка через set_ps |
| 0x100bf77c | `yglc_set_ps` | (в Ghidra «yglc_set_trans_default») |
| 0x100bfa60 | `yglc_scene_apply` | |
| 0x100bfad4 | `yglc_proc_request` | |
| 0x100bff30 | `yglc_task` | |

Вызываемые функции light-unit (не реконструированы, только контракт):

| Адрес | Вызов | Смысл |
|---|---|---|
| 0x100c243c | `(unit, mode, dur)` | включение по режиму (см. §3.1) |
| 0x100c23b0 | `(unit, dur)` | выбор DL/NL при обычном включении |
| 0x100c1d12 | `(unit, off, dur)` | unit->off=off; активный канал вкл/выкл, остальные выкл |
| 0x100c1d4a | `(unit, bright, dur, v2)` | `yglc_light_channel_set_bright` АКТИВНОГО канала |
| 0x100c1db8 | `(unit, color_mode, dur)` | переключиться на канал с битом color_mode в маске (+0x178): CT=2 → DL, NL=7 → NL |
| 0x100c2050 | `(unit, br, dur, save)` | войти в NL (unit->active=1, DL гаснет), br=0 → сохранённый nl_br (или 5); save → nl_status_cfg=1 |
| 0x100c2008 | `(unit, br, dur, activate)` | яркость NL; применяется только если NL активен, иначе запоминается |
| 0x100c20b4 | `(unit, dur, save, x)` | выйти из NL (active=0, DL загорается); save → nl_status_cfg=0 |
| 0x100c1e92 | `(unit, ct, bright, dur)` | CT: сначала 0x100c1db8(unit, 2) → из NL переходит в DL |
| 0x100c24a8 | `(unit, action, dur, value)` | adjust_bright по активному каналу |
| 0x100c1ad0 | колбэк канала | пересчитывает nl_active/active_mode/active_bright/nl_br_effective, settings->power, вызывает FUN_100c30b8(0) → немедленный отчёт props |

## 3. Методы JSON-RPC

LAN (белый список, остальное «Reject call on unexposed method»): `get_prop set_default set_power toggle
set_bright set_scene cron_add cron_get cron_del start_cf stop_cf set_ct_abx set_name set_adjust adjust_bright adjust_ct`.
Облако — все 32 метода таблицы `0x20000aec`.

| Метод | Обработчик | Параметры / проверки | Запрос |
|---|---|---|---|
| set_power | 0x100bd0f0 | `"on"` → req 1, **любая другая строка** → req 2; `[effect="smooth", dur=500]`, 4-й `mode` без проверки диапазона | 1 / 2 |
| toggle | 0x100bc3c8 | — | 3 |
| set_bright | 0x100bd2cc | `[bright, effect, dur]`; bright 1..100 в proc_request | 4 |
| set_ct_abx | 0x100bd4ac | `[ct, effect, dur]`; ct в [min_ct,max_ct] канала CT (2700..6500) | 5 |
| set_rgb / set_hsv / set_wrgb / set_xy | 0x100bd32c / 0x100bd38c / 0x100bd3ec / 0x100bd44c | на ceiling3 бесполезны; set_xy → req 8 → «general error» | 6 / 7 / 9 / 8 |
| set_scene | 0x100bd668 | см. ниже | 0x10 |
| start_cf / stop_cf | 0x100bd610 / 0x100bbd98 | `[count, action, "flow"]` | 0x0a / 0x0b |
| set_default | 0x100bc398 | только если ВКЛ и save_state==0, иначе 6 | 0x14 |
| cron_add | 0x100bc8ac | `[type 0..4, minutes>0, (extra для 2..4)]`; выполняется только type 0 (delay-off, нужно ВКЛ) и type 1 (нужно ВЫКЛ, без действия) | 0x15 |
| cron_del / cron_get | 0x100bc930 / 0x100bc994 | список типов (≤4) | 0x16 / 0x17 |
| set_adjust | 0x100bcd14 | `[increase|decrease|circle|auto, bright|ct|color, (opt<3)]`, dur=0, value=0 | 0x1b |
| adjust_bright / adjust_ct / adjust_color | 0x100bcb00 / 0x100bcb58 / 0x100bcbb0 | `[pct −100..100 ≠0, dur ≤ 60000, (opt)]` | 0x1b |
| set_ps | 0x100bce38 | `["key", "value"|int]` (только облако) | 0x19 |
| set_music / set_name / set_test | 0x100bcf1c / 0x100bcf98 / 0x100bcff4 | | 0x1d / 0x26×2 / 0x21 |
| get_prop | 0x100bcc40 | синхронно из кэша настроек | — |

### set_scene

| Класс | Параметры | type | Обработка |
|---|---|---|---|
| `color` | rgb, bright | 1 | 0x100c25d4 |
| `ct` | ct, bright | 2 | 0x100c2650 (переход в DL) |
| `hsv` | hue, sat, bright | 3 | 0x100c25fc |
| `cf` | count, action, "flow" | 4 | старт потока |
| `auto_delay_off` | bright (−1..100), minutes ≠0 | 6 | set_bright + cron type 0 |
| `nightlight` | bright 1..100 | 7 | set_ps("nightlight", "on|NNN") → NL с яркостью NNN, nl_status_cfg=1 |
| `bright` | bright 0..100 | 8 | ВКЛ: 0 → выкл и все яркости = 100, иначе set_bright; ВЫКЛ: 0 → ничего, иначе set_bright + включение mode 0 |
| `off` / `keep` / `on` | — | 10 / 11 / 12 | выкл / ничего / вкл mode 0 |

Все типы, уходящие в `yglc_scene_apply` (0x100bfa60), сначала ставят `settings->power = 0`.

### 3.1. Режим (4-й параметр) set_power

Цепочка: req 1 → 0x100bdcfa (dur := 0 если effect «sudden») → 0x100c2574 → `0x100c243c(unit, mode, dur)`:

| mode | Действие |
|---|---|
| 0 | Только если свет ВЫКЛ: `0x100c23b0` → **NL**, если `yglc_is_night_time()` или `nl_status_cfg (unit+0x12c) == 1`; иначе **DL**. Если свет уже ВКЛ — ничего (в т.ч. не выходит из NL). При ночном времени и `night_delayoff_min ≠ 0` ещё ставит delay-off |
| 1 | CT/дневной: `0x100c1e62` → канал CT (DL), текущая ct/яркость. Из NL переводит в DL (проверено по LAN) |
| 2, 3 | RGB/HSV — ничего, но ответ «ok» |
| 4 | поток с ПУСТЫМ выражением (`0x101171a0` = "") |
| 5 | лунный: `0x100c2050(unit, 0, dur, save=1)` — NL с сохранённой nl_br, nl_status_cfg=1 |
| >5 | ничего, «ok» |

## 4. Внутренние запросы (id → смысл)

Таблица запретов `@0x20007fc4+8 [power][id]`: при ВЫКЛ (power=1) запросы **2, 4, 5, 6, 7, 0x1c молча
отбрасываются со статусом 0** (клиент получает «ok»).

| id | payload | Обработчик → unit |
|---|---|---|
| 0x00 | — | копия настроек 0x118 |
| 0x01 | power {flags, effect, dur, mode} | 0x100bdcfa → 0x100c243c |
| 0x02 | power | 0x100bdd0e → 0x100c1d12(unit,1,dur) |
| 0x03 | — | 0x100bdd24 toggle |
| 0x04 | bright {flags, bright, effect, dur} | 1..100 → 0x100bdd7c → 0x100c1d4a (активный канал: в NL меняет яркость NL!) |
| 0x05 | ct {flags, ct, effect, dur} | диапазон → 0x100bde20 → 0x100c1e92 (переход в DL) |
| 0x06 / 0x07 / 0x09 | rgb / hsv / wrgb | 0x100c25d4 / 0x100c25fc / 0x100c25b4 |
| 0x08 | set_xy | не поддержан → 4 |
| 0x0a / 0x0b | cf 0x114 | старт / стоп потока |
| 0x10 | scene 0x224 | §3 |
| 0x14 | — | set_default |
| 0x15 / 0x16 / 0x17 | cron | add / del / get |
| 0x19 | set_ps {key[0x20], value[0x200]} | 0x100bf77c |
| 0x1a | — | get_ps (заглушка) |
| 0x1b | adjust {flags, action, prop, dur, value, opt} | 0x100bdebc |
| 0x1c | {idx} | мигание индикатора, если бит в cfg_alert_ctrl |
| 0x1d | music | |
| 0x1e | — | обновление строки потока |
| 0x1f | — | полный отчёт props (после подключения к облаку) |
| 0x21 | test | сырое управление каналом |
| 0x26 | — | no-op (после set_name) |
| 0x27 | bright | как 0x04 с флагом 1 (производитель не найден) |
| 0x28 | — | lan_ctrl_init (после получения IP) |
| 0x0c–0x0f, 0x11–0x13, 0x18 | — | заглушки (0) |

### adjust (0x1b), action: 0 increase, 1 decrease (по таблице шагов), 2/3 ±pct, 4 circle, 5 auto
- свет ВЫКЛ и action=auto → включение в режиме свойства (bright→0, ct→1, color→2);
- bright → `0x100c26c8` → `yglc_light_unit_adjust_bright`: работает по **активному** каналу; уменьшение pct до ≤0 → выключение;
- ct/color при opt=1 и ВЫКЛ → включение в соответствующем режиме.

Таблицы шагов (+/- пульта): DL `[1,10,40,100]` (@0x20000adc), NL `[1,2,5,10,40,100]` (@0x20000ac4), CT «солнце» `[2700,4000,5200,6500]`; ручка: яркость 15 шагов `[1,5,10..100]`, CT 16 шагов 2700..6500.

## 5. BLE-пульт (`yglc_handle_ble_event` 0x100bf5a8)

Код события 0x1001: `code = edata[2]<<16 | edata[1]<<8 | edata[0]`, edata[0] — кнопка, edata[2] — тип (0 клик, 2 долгое, 3/4 — «качель»).
Таблица @0x200008a4 (15 записей × 0xc: code, fn, arg). Раскладка кнопок — **ПРЕДПОЛОЖЕНИЕ** по пульту YLYK01YL (0 вкл, 1 выкл, 2 солнце, 3 +, 4 луна, 5 −).

| code | Функция | Действие | DL | NL |
|---|---|---|---|---|
| 0x00000 | 0x100be728 | req 1 mode 0, smooth 500 | вкл, если выкл (DL/NL по nl_status_cfg/ночи) | если уже вкл — ничего |
| 0x00001 | 0x100be752 | req 2 | выкл | выкл |
| 0x00002 | 0x100be850 | CT «пинг-понг» по `[2700,4000,5200,6500]` (req 0x1b prop ct) | да | **игнор** (nl_active) |
| 0x00003 / 0x00005 | 0x100be870 / 0x100be8b0 | ± шаг яркости активного канала (req 0x1b) | DL-таблица | NL-таблица |
| 0x00004 | 0x100bf248 | переключить NL: nl_active ? set_ps nightlight "off" : "on|000" (сохраняет nl_status_cfg) | → NL | → DL |
| 0x00008 / 0x00009 | 0x100be85a / 0x100be864 | CT +/− (ПРЕДПОЛОЖЕНИЕ: другой пульт) | да | игнор |
| 0x20001 (долго выкл) | 0x100beb4c | мигнуть 2 раза + delay-off через `delayoff_info` минут (10 по умолч.) | | |
| 0x20003 / 0x20005 (долго ±) | 0x100be944 / 0x100be8f4 | set_bright = последний / первый элемент таблицы активного канала | 100 / 1 | 100 / 1 (NL) |
| 0x20004 (долго луна) | 0x100bf280 | set_ps nightlight "on|001" — NL на минимуме | → NL | NL=1 |
| 6 (синтет.) | 0x100beaa0 | выход из заводского теста (последовательность выкл,+,−,+,−,выкл до прохождения factory) | | |
| 7 | 0x100be77c | toggle по settings->power | | |
| 1000 (синтет.) | 0x100be752 | выкл, когда Mi Band «заснул» (eid 0x2001, miband_sleep=1, ПРЕДПОЛОЖЕНИЕ о значениях 2→1) | | |

Все клавиши, кроме ±/луны/солнца, работают и при ВЫКЛ; ± и долгие ± при ВЫКЛ игнорируются.
Ночью (nighttime включён и время в окне) при ВЫКЛ любая кнопка, кроме «выкл», превращается в «вкл» (→ NL через 0x100c23b0).

«Качель» (0x100bf29c): тип 3 — клик: 1 раз = toggle, 2 раза = DL↔NL (из NL: set_ps off; иначе req 1 mode 5);
тип 4 — вращение: в NL → сцена bright (шаги по таблице ручки), в DL → сцена ct+bright, 400 мс.

`set_ps ["pseudo_beacon","4097|<code>"]` (0x100bf740) генерирует виртуальное событие кнопки — удобно для тестов (только облако).

## 6. Включение питания выключателем (холодный старт)

1. `yglc_setting_cache_alloc`: по умолчанию power=ON(0), unit->off=1, DL bright 100, NL bright 5, init_power_opt=1, save_state=1, nighttime «disable|220000|060000|00».
2. `yglc_reboot_restore` (0x100bed94): маркер тёплой перезагрузки 0xdeadbeef (тип 1/2 ставит `yglc_reboot_save` перед программной перезагрузкой). После выключателя — холодный старт.
3. `FUN_100c32d0`: загрузка psm кат. 4; если save_state=0 — загружаются «по умолчанию» (сохранённые set_default) кат. 1|2; если save_state=1 — восстанавливается nv-кэш последнего состояния (bright, ct, color_mode, nl_status_cfg, nl_br, flow), power — только при init_power_opt=0, is_night_time — только при тёплом старте.
4. `yglc_init_async` (только холодный старт): `init_power_opt==1 || fast_reboot_count!=0` → ВКЛ; `==2` → ВЫКЛ; `0` → восстановленное.
5. `0x100bac8c`: если ВКЛ — req 1 mode 0 (smooth 500 мс; 300 мс при тёплом типе 2) → `0x100c23b0`: **NL, если nl_status_cfg==1** (свет выключили в лунном режиме), иначе DL. `is_night_time()` при холодном старте всегда 0 (нет NTP, кэш +0xf8 не восстанавливается) — nighttime на холодный старт **не влияет**; влияет только на последующие включения (BLE/команды) после синхронизации времени.
6. `miio_instance_create`: для устройства, прошедшего factory, и при холодном старте — таймер 5 с обнуления счётчика и `yglc_check_fast_reboot(5)`.

**Логики «быстрое выкл-вкл = лунный режим» в 2.0.2_0048 нет.** Счётчик `fast_reboot_count` (psm app_data) используется только так:
- любое включение в течение 5 с после предыдущего → свет принудительно ВКЛ (даже при init_power_opt=2);
- 5-е такое включение подряд → мигание 99 раз и через 5 с сброс Wi-Fi (`cmd.restore`, 0x100bd8c0).

## 7. Что видит приложение при смене режима

Колбэк канала `0x100c1ad0` пересчитывает поля unit и вызывает `FUN_100c30b8(0)` → `FUN_100c27b8(0)`:
изменившиеся props с флагом отчёта уходят в облако (`FUN_100f7508`, «props») и LAN-клиентам (notify «props»).

| prop (имя в отчёте) | Источник | DL→NL |
|---|---|---|
| power | settings+0x54 (0 → "on") | без изменений |
| bright | DL-канал +0x24 | **не меняется** (яркость дневного канала) |
| ct, color_mode | DL-канал | без изменений |
| active_mode | unit+0x13c | 0 → **1** |
| nl_br | unit+0x134 (nl_br_effective) | 0 → яркость NL |
| active_bright | unit+0x140 | → яркость NL |
| flowing, flow_params, delayoff, nighttime, save_state, name, lan_ctrl, miband_sleep, init_power_opt, sub_model | | |

Не отчитываются: is_night_time, nl_status (cfg), nl_br (cfg), max/min_ct, delayoff_info, model, fw_ver, rt_temp, fw_fab, trans_interval_dflt, power_on_effect.
Если прошивка сама переключит DL→NL, Mi Home получит `active_mode=1`, `nl_br=X`, `active_bright=X` при прежнем `bright`
и, по-видимому, покажет лунный режим (ПРЕДПОЛОЖЕНИЕ о поведении плагина).

## 8. Найденные ошибки (BUG)

1. `set_power` (0x100bd0f0): если params[0] не строка — id запроса не инициализирован (r6); ошибки разбора effect/dur не возвращаются; mode не проверяется; любая строка кроме "on" = выключить.
2. `start_cf` и `set_scene "cf"`: `yglc_json_parse_cf` при ошибке сам отвечает и освобождает парсер, после чего вызывающий отвечает ещё раз и/или передаёт освобождённый парсер в очередь (double free / use-after-free).
3. `set_scene`: при ошибке разбора/неизвестном классе запрос всё равно уходит (type 0 → `settings->power=0` и ответ 1).
4. `cron_add` с type>4 отвечает «ok» без действия.
5. `set_alert`, `set_indicator` возвращают 0 без ответа — клиент не получает ответа, парсер течёт; неизвестный метод в `yglc_rpc_dispatch` тоже течёт.
6. Таблица запретов отвечает «ok» на set_bright/set_ct при выключенном свете.
7. `req_queue_send` из ISR: успех → −1, неудача → 0 (перевёрнуто).
8. BLE: индекс заводской последовательности после 6-го совпадения не сбрасывается — следующий код сравнивается с элементом за пределами массива из 6.
9. `yglc_task`: после переполнения `now+200` периодический сброс состояния идёт только по запросам.
10. `nighttime`: допускает минуты/секунды = 60; `memcpy` без проверки длины.

## 9. Где делать «гибридную» яркость

Цель: одна шкала bright 1..100; ниже порога K — лунные светодиоды с яркостью f(bright), выше — дневные.

1. **Основная точка — `0x100bdd7c` (`yglc_req_set_bright`, 56 Б, листовой обработчик req 4).** Через неё проходят:
   set_bright (облако/LAN), долгие ± пульта (req 4), сцена `bright` (inline в proc_request 0x100bfd7e/0x100bfd96 и `0x100bfa60` case 8),
   `auto_delay_off` (0x100bfcfe), вращение ручки в NL (сцена bright). Вход: r0 → `{flags, bright, effect, dur}`; effect==0 ⇒ dur=0.
   Замена тела на переход во внешний код, вызывающий логику `yglc_hybrid_set_bright()` из реконструкции:
   bright<K: если `unit->active!=1` → `0x100c2050(unit, f(b), dur, save=0)`, иначе `0x100c2008(unit, f(b), dur, 0)`; записать `unit->ch[0]->bright=b`;
   bright≥K: если NL активен → `ch[0]->bright=b` и `0x100c20b4(unit, dur, 0, 0)`, иначе `0x100c1d4a(unit, b, dur, 0)`.
2. **Относительные изменения** (± клик пульта, set_adjust, adjust_bright): req 0x1b → `0x100bdebc`, ветка prop==4 — `bl 0x100c26c8` по адресу **0x100bdf36**.
   Её нужно перенаправить в гибридный adjust (шаг по DL-таблице `[1,10,40,100]` или ±pct от «виртуальной» DL-яркости `ch[0]->bright`), иначе в NL ± будет ходить по NL-таблице.
3. **Включение** (mode 0): решение DL/NL в `0x100c23b0` (слой light-unit). В командном слое — `0x100bdcfa` (18 Б, req 1):
   при mode==0, unit->off и `ch[0]->bright<K` подставить включение NL с f(bright). Это покрывает set_power, toggle, кнопку «вкл», сцены on/bright и холодный старт (`0x100bac8c`).
4. **Холодный старт / «выкл-вкл выключателем = NL»** (если нужно): в `0x100bac8c` перед `bl 0x100bdbc0` (0x100bacd0) записать mode=5 в `[sp+0x18]`, когда `g_fast_reboot_count (0x20000c80) == 1`.
   Значение счётчика на этот момент — от предыдущей загрузки (инкремент делается позже, в `yglc_check_fast_reboot`). Учтите: 5 быстрых циклов подряд сбрасывают Wi-Fi.
5. Что учесть:
   - set_bright при ВЫКЛ отбрасывается таблицей запретов (как в стоке);
   - `0x100c1ad0` при выходе из NL обнуляет nl_status_cfg; с save=0 гибридный NL не запоминается как «лунный» и при включении 0x100c23b0 вернёт DL с яркостью <K — поэтому нужен пункт 3;
   - Mi Home увидит `active_mode=1` (лунный режим) в зоне <K; `bright` останется равным запрошенному значению только при записи `ch[0]->bright` (см. п.1);
   - `set_ct_abx` и сцена `ct` переводят в DL (0x100c1e92) — в зоне <K это «выбьет» гибридный NL.
