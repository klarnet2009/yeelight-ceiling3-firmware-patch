# Модули и компоненты (`modules.md`)

Репозиторий состоит из трёх слоёв модулей:
1. Хостовые Python-скрипты (корень репозитория и `tools/clean_decomp.py`) для распаковки, патчинга, прошивки, мониторинга и очистки декомпиляции.
2. Java-скрипты Ghidra (`tools/ghidra/`) для воспроизводимой реконструкции карты памяти, обогащения символов и экспорта псевдо-C.
3. Структурированный C-декомпилят прошивки сегмента 0 (`decomp/<subsystem>/`) и заголовки типов (fw_types.h, fw_globals.h).

---

## 1. Хостовые Python-модули

| Файл | Назначение | Ключевые классы и функции |
|---|---|---|
| [patch_ota.py](../../patch_ota.py) | Верификация, бинарное патчирование и переупаковка `MMM`-контейнера | `LzmaOptions`, `LzmaFilter`, `lzma_alone_no_eos()`, `fw_crc()`, `parse()`, `build()`, `main()` |
| [miio_ota.py](../../miio_ota.py) | Клиент `miio` (UDP `54321`), локальный HTTP-сервер (`80`) и сценарий прошивки | `Miio` (`__init__`, `hello`, `_crypt`, `call`), `token()`, `local_ip_for()`, `say()`, `serve()`, `H`, `flash()`, `main()` |
| [monitor_lamp.py](../../monitor_lamp.py) | Непрерывный многоуровневый опрос доступности лампы | `ping()`, `arp()`, `miio()`, `lan()`, `probe()`, `verdict()`, `log()`, `main()` |
| [unpack.py](../../unpack.py) | Чистая распаковка стокового OTA в `extracted/` средствами stdlib | Топ-уровневый скрипт (проверка размера/MD5/SHA-1, `lzma.LZMADecompressor`) |
| [xiaomi_qr_token.py](../../xiaomi_qr_token.py) | Получение 32-hex токена лампы через облачный QR-логин Xiaomi | `js()`, `rc4()`, `login()`, `api()`, `main()` |

### Детали ключевых Python-модулей

- **[patch_ota.py](../../patch_ota.py)**:
  - Хранит базу `BASE = 0x10071000`, заменяет слот `yglc_network_monitor` (`0x100bd918..0x100bd9f8`, 224 Б) бинарником yglc_network_monitor_inplace.bin (патч `#1v2`, 212/224 Б) и применяет таблицу патчей `PATCHES` (`#2` `0x100af298`, `#3` `0x100f831c`, `#4` `0x100af66e`/`0x100af670`/`0x100af700` — пропуск среднего лога внутри case 8 + `miio_net_post_event(5)`, `#5` `0x100b04a4`/`0x100b0c80`/`0x100b0cae`, `#5b` `0x100b0cbe` — порог `countout = (param_1[0x5c] / 22) + 2 >= 2`, `#6` `0x100e88e8`, `#7` `0x100e6bf4`, `#8` `0x100ce49e`, `#9a` `0x100ebffe` — `set_min_bright_pct = 0` вместо `15%`, `#9b` `0x1014067c` — аппаратный порог `6.5f` вместо `8.0f` в калибровочной таблице CT `+0x154/+0x158`), собирая yeelink.light.ceiling3-patched-2.0.2_0048.bin (`727 130` Б, MD5 `41fa639b254a13116d5de9521e892d63`, CRC32 `0xfbacd319`, `267` изменённых байт в segment 0) с использованием нативной библиотеки `LIBLZMA`.
  - `LzmaOptions` и `LzmaFilter` описывают C-структуры `liblzma >= 5.4` (`LZMA_FILTER_LZMA1EXT = 0x4000000000000002`).
  - `lzma_alone_no_eos()` подбирает `nice_len` (`32..273`) в режиме `LZMA_MODE_FAST` / `LZMA_MF_HC4` без маркера конца потока (`ext_flags = 0`) так, чтобы размер упакованного сегмента 0 был кратен 8 байтам.
  - `fw_crc()` воспроизводит алгоритм `arch_ota_check_crc` (полином `0xEDB88320`, `init = 0`, без финального XOR).
  - При наличии `capstone` выводит Thumb-2 дизассемблирование патчей (иначе `'capstone n/a'`). Поддерживает `--stock` для контрольной пересборки без патчей.

- **[miio_ota.py](../../miio_ota.py)**:
  - `Miio`: реализует протокол miio over UDP `54321` — `hello()`, шифрование `AES-128-CBC` + PKCS7 (`_crypt()`), синхронизацию `stamp` и MD5-подпись пакетов (`call()`).
  - `serve()`: запускает `http.server.ThreadingHTTPServer(('0.0.0.0', port), H)` (`HTTP/1.1`, поддерживает `200 OK` и `206 Partial Content` по `Range`, по умолчанию порт `80`) с `Connection: close` / полузакрытием `SHUT_WR` и ожиданием закрытия сокета клиентом (`recv(4096)`).
  - `flash()`: валидирует образ через `patch_ota.parse()`, проверяет `model == 'yeelink.light.ceiling3'`, отправляет `miIO.ota` локально или через облако (`--cloud=<server>`, используя `xiaomi_qr_token.login()` и `xiaomi_qr_token.api()`), затем опрашивает `miIO.get_ota_state`, `miIO.get_ota_progress` и `miIO.info` в `flash.log`.
  - CLI-команды `main()`: `info`, `state`, `flash <image.bin> [--yes] [--ip=...] [--cloud=de] [--port=80]`.

- **[monitor_lamp.py](../../monitor_lamp.py)**:
  - Опрашивает `ping()` (2 попытки `ping -n 1 -w 1500`), `miio()` (UDP `54321` hello), `lan()` (TCP `55443` `get_prop`), `arp()` (`netsh interface ipv4 show neighbors` → `Reachable`).
  - `verdict()` различает `OK`, `OFFLINE` (нет ассоциации/IP), два вида `ZOMBIE` и `PARTIAL`. Поддерживает `--once`, пишет переходы и 30-минутный heartbeat в `monitor_lamp.log`.

- **[unpack.py](../../unpack.py)** и **[xiaomi_qr_token.py](../../xiaomi_qr_token.py)**:
  - `unpack.py` сверяет точный размер (`726978`) и MD5 (`13b7471a0f84e3cfbac78c116fae7a1c`) стокового контейнера, проверяет `SHA-1` заголовка и всех 3 сегментов и распаковывает их в `extracted/segment-{0,1,2}-flash-{addr}.bin`.
  - `xiaomi_qr_token.py` генерирует `xiaomi_login_qr.png`, проходит long-poll авторизацию `account.xiaomi.com`, опрашивает регионы (`cn`, `de`, `us`, `ru`, `tw`, `sg`, `in`, `i2`) по RC4-шифрованному `/home/device_list` (с отбрасыванием первых 1024 байт keystream в `rc4()`) и сохраняет токен в `token.txt`.

---

## 2. Ghidra Java-скрипты (`tools/ghidra/`)

Все скрипты наследуют `ghidra.app.script.GhidraScript` (`//@category Yeelight`) и предназначены для анализа `segment-0-flash-00071000.bin` (загруженного как Raw Binary по базе `0x10071000` с языком `ARM:LE:32:Cortex:default`).

- **[YlSetup.java](../../tools/ghidra/YlSetup.java)** (pre-analysis):
  - Методы: `a()`, `copyBlock()`, `bssBlock()`, `run()`.
  - Воспроизводит startup-разметку памяти: переименовывает базовый блок в `flash_app`, копирует `sysram_data` (`0x20000000` ← `0x1014C4E4`, длина `0x1548`) и исполняемый `tcm_text_data` (`0x00100000` ← `0x1014DA2C`, длина `0x2C18`), создаёт неинициализированные `sysram_bss` (`0x20001548..0x2003AA44`), `sysram_stack` (`0x2003AA44..0x20040000`) и `tcm_bss` (`0x00102C18..0x0010F4B8`).
  - Устанавливает `TMode = 1` для flash и TCM, создаёт функции `reset_startup` (`0x10071000`), `startup_init` (`0x100BAC68`), `startup_main` (`0x100A7694`) и засеивает функции по нечётным 32-битным указателям, ведущим на Thumb-прологи `PUSH` (`0xB4xx`/`0xB5xx`/`0xE92D`) — по analyze.log засеяно `816` функций.

- **[YlStrings.java](../../tools/ghidra/YlStrings.java)** (post-analysis):
  - Метод `run()`: сканирует инструкции `ldr ... [pc, #imm]`, типизирует 4-байтные слова пула литералов в `flash_app` как `PointerDataType` и создаёт ASCII-строки по их адресам назначения (по analyze2.log: типизировано `13734` указателей, определено `164` строки).

- **[YlEnrich.java](../../tools/ghidra/YlEnrich.java)** (post-analysis):
  - Переименовывает `142` верифицированные функции RTOS/HAL/libc/`arch_*`/`jsmi_*`/`yglc_*`/`psm_*` (только если текущее имя начинается с `FUN_`), размечает `7` глобальных RAM/MMIO-переменных (`g_miio_net`, `g_yglc_settings_ptr`, `g_offline_since_ms`, `g_yl_cache_*`, `WDT_SWRST`) и транки (`thunk_*`).

- **[YlExport.java](../../tools/ghidra/YlExport.java)** (post-analysis):
  - Методы: `cstr()`, `literal()`, `reg()`, `run()`, `handleCall()`, `best()`.
  - Прослеживает аргументы вызовов логгера (`r0..r3`, `[sp+...]`) по форматам `(?s)...(%s,%d)`, `func:%s` и assert `__FILE__`, извлекает имя функции (`__func__`) и модуля (`[L] %s: ...` или имя `.c`-файла), переименовывает ещё `54` функции `FUN_*` и декомпилирует все `4210` функций (`0` ошибок).

- **[clean_decomp.py](../../tools/clean_decomp.py)** (постобработка):
  - Подставляет строковые литералы и адреса вместо `PTR_s_*` / `PTR_DAT_*` через segment-0-flash-00071000.bin, переводит типы `undefined*` в `<stdint.h>`, раскрывает битовые слайсы и алиасы стека, подключает `#include "../include/fw_globals.h"`, раскладывает код по подсистемам в `decomp/` (`91` `.c`-файл, `489` именованных функций) и генерирует [functions.csv](../../decomp/functions.csv).
- **[prep_decomp_build.py](../../tools/prep_decomp_build.py)** и **[build_fw.py](../../tools/build_fw.py)** (сборка C11 / проверка декомпилята):
  - `prep_decomp_build.py` генерирует decomp_symbols.h и Thumb-таблицу [ceiling3_symbols.ld](../../firmware/ld/ceiling3_symbols.ld).
  - `build_fw.py` запускает host-тесты ([test_ceiling3_firmware.c](../../firmware/tests/test_ceiling3_firmware.c)), компилирует C11-модули из `firmware/src/` через LLVM `clang` + `ld.lld` в ceiling3_ext.elf / ceiling3_ext.bin, а также собирает с `-Oz` zero-BSS in-place монитор yglc_network_monitor_inplace.elf / yglc_network_monitor_inplace.bin (212/224 Б по [ceiling3_inplace_monitor.ld](../../firmware/ld/ceiling3_inplace_monitor.ld)), компилирует чистый C11-исходник Wi-Fi подсистемы из `firmware/clean_src/` в `firmware/build/clean_obj/` и опционально (`--with-decomp`) собирает `.o`-файлы подсистемы `decomp/`.

---

## 3. Декомпилированные подсистемы (`decomp/`) и C11 SDK (`firmware/`)

### 3.1. Заголовки типов (decomp/include/)

- **fw_types.h**: восстановленные C-структуры `miio_rpc_delegate_arg_t`, `arch_ota_part_t`, `ota_app_ctx_t`, `miio_net_t`, `yglc_settings_t`, `jsmi_tok_path_t` и типы RTOS-дескрипторов.
- **fw_globals.h**: верифицированные адреса глобальных переменных (`g_log_level`, `g_miio_net`, `g_yglc_settings_ptr`, `g_offline_since_ms`, `g_yl_cache_list`, `g_yl_cache_schema_crc`, `g_warm_boot_marker`, `g_warm_boot_type`, `WDT_SWRST`).
- **ghidra_compat.h** и **decomp_symbols.h**: совместимые макросы Ghidra (`CONCAT*`, `CARRY4`, `SBORROW4`, `SUB4*`, `code`) и декларации внешних/внутренних символов для объектной компиляции с `-DDECOMP_BUILD`.
- **[firmware/](../../firmware/include/ceiling3_sdk.h)**: готовые компилируемые C11-модули расширения прошивки ([wifi_recovery_guard.c](../../firmware/src/wifi_recovery_guard.c), [ota_hooks.c](../../firmware/src/ota_hooks.c)), linker-скрипты [ceiling3_inplace_monitor.ld](../../firmware/ld/ceiling3_inplace_monitor.ld) (zero-BSS in-place слот `0x100bd918..0x100bd9f8`, 224 Б, с использованием существующего 8-байтного слота `0x20008030..0x20008037`) и [ceiling3_ext.ld](../../firmware/ld/ceiling3_ext.ld) (примечание: `EXT_SRAM` `0x20039800..0x2003AA00` пересекается с `sysram_bss` `0x20001548..0x2003AA44` / `ucHeap` `0x20008f98..0x20038798`, поэтому для продакшен-оверлея без релокации SRAM используется zero-BSS in-place хук `ceiling3_yglc_network_monitor_inplace()`), хостовые unit-тесты [test_ceiling3_firmware.c](../../firmware/tests/test_ceiling3_firmware.c) (включая `test_inplace_monitor_local_arp_and_disconnected()` с проверкой 60s `etharp_request`, 90s окна `ts` под `lock_tcpip_core`, 300s ребута в `LOCAL`/`DISCONNECTED` и `test_daylight_brightness_curve()` для кривой яркости `"dl"` `1..100%`), а также чистая строго типизированная реконструкция Wi-Fi подсистемы: [miio_wifi_clean.h](../../firmware/include/miio_wifi_clean.h) (`miio_net_ctx_t` ровно `0x1d4` байт, `_Static_assert` на все ключевые смещения) и [miio_wifi_clean.c](../../firmware/clean_src/miio_wifi_clean.c).

### 3.2. Прикладные и сетевые модули (`app/`, `miio/`, `hal/`)

- **decomp/miio/miio_net.c** (`18` функций): сетевой менеджер miio SDK (Wi-Fi STA/UAP, события, lwIP, мониторинг).
  - Ключевые функции: `miio_net_state_change` (`0x100ae8ec`), `sta_monitor_offline_timeout` (`0x100aea08`), `miio_wifi_scan_start` (`0x100aedf0`), `miio_wifi_connect` (`0x100af028`), `miio_wifi_try_reconnect` (`0x100af244` — место **патча #2** @ `0x100af298`), `miio_wifi_scan_timeout` (`0x100af334`), `wifi_cb_event_handler` (`0x100af370` — место **патча #4** @ `0x100af66e`/`0x100af670`/`0x100af700`, пропуск среднего лога внутри case 8 `CONNECT_FAILED -> miio_net_post_event(5)`), `ip_ready_callback` (`0x100af73c`), `miio_wifi_scan_finish` (`0x100b0a08` — включает тело задачи `netMonitorTask` / `task_monitor`, `sta_offline_timeout_s` **патч #5** @ `0x100b04a4`/`0x100b0c80`/`0x100b0cae` и **патч #5b** @ `0x100b0cbe` для порога `sta_monitor_offline_countout`), `do_restore` (`0x100b1c0c`).

- **decomp/miio/miio_ota.c** (`2` функции): обработка RPC `miIO.ota` и ошибок загрузки.
  - `do_ota` (`0x100f8304`): проверка флага локального источника (место **патча #3** @ `0x100f831c`), разбор `params` через `jsmi_parse_start`, запуск зарегистрированных OTA-задач (`FUN_100f8298`).
  - `ota_virtual_error` (`0x100f871c`): инкрементирует счётчик ошибок (до 3 повторов), при исчерпании переводит задачу в статус `6` (`down_error`).

- **decomp/hal/arch_ota.c** (`3` функции): платформенная привязка OTA-разделов и проверка целостности.
  - `arch_ota_select_mcu_flash` (`0x100a8cc0`): staging MCU по адресу `0x33a000`, лимит `0x80000`.
  - `arch_ota_check_crc` (`0x100a8e78`): чтение flash по 128 байт (`arch_flash_read`) и проверка 4-байтового CRC32 (`FUN_100a7720`).
  - `arch_ota_select_flash` (`0x100b1d0c`): staging основного образа по адресу `0x20a000`, окно `0x130000`.

- **decomp/app/yglc_miIO.c** (`3` функции): привязка BLE-правил Yeelight к RPC miio (`yglc_proc_ble_event`, `do_bleEvtRuleDel`, `do_bleEvtRuleAdd`).
- **decomp/app/yglc_core.c**: основная логика светильника Yeelight — `startup_init` (`0x100bac68`), `yglc_network_monitor` (`0x100bd918..0x100bd9f8` — место **патча #1v2**, in-place замена `yglc_network_monitor_inplace.bin`), `yglc_check_fast_reboot` (`0x100bebf0`), `yglc_reboot_save` (`0x100bed64`), `yglc_init_async` (`0x100bedb4`).
- **decomp/app/yl_cache.c**: работа с сектором `yl_cache` (`0x3ba000`) и таймерами `yglc_timer_*` (`FUN_100c32fc` восстановления тёплой перезагрузки).
- **decomp/app/lan_ctrl_cli.c**: LAN Control TCP `55443` и SSDP `239.255.255.250:1982` (`lan_ctrl_init` @ `0x100c41e4`, `lan_open_mchannel` @ `0x100c3e84`), UART CLI.

### 3.3. Сводка по каталогам подсистем `decomp/`

| Подсистема | Каталог | Файлов | Функций (именованных) | Основное содержимое |
|---|---|---:|---:|---|
| Приложение Yeelight | `decomp/app/` | 5 | 348 (65) | `yglc.c`, `yglc_core.c`, `yglc_miIO.c`, `yl_cache.c`, `lan_ctrl_cli.c` |
| Стек MiIO | `decomp/miio/` | 27 | 526 (145) | `miio_core.c` (`miio_reboot`), `miio_net.c`, `miio_ota.c`, `ota_app.c`, `httpc.c`, `jsmi.c`, `ots.c`, `otu.c` |
| BLE / MIBLE | `decomp/ble/` | 20 | 394 (111) | `bt_*` драйвер/стек MTK и `mible_*` (beacon, fastpair, gateway, rpc) |
| HAL / Platform | `decomp/hal/` | 18 | 480 (98) | `mt7687_hal.c` (`hal_wdt_*`, `hal_flash_*`), `arch_*.c`, `arch_platform.c`, `nvdm_*`, `tcm_flash_sfc.c` |
| FreeRTOS / Kernel | `decomp/rtos/` | 8 | 126 (36) | `tasks.c`, `queue.c`, `timers.c`, `heap_4.c`, `port.c`, `syslog.c`, `freertos_kernel.c` |
| Wi-Fi / Supplicant | `decomp/wifi/` | 7 | 1243 (11) | `smt_smartconfig.c`, `wifi_api_service.c`, `wpa_supplicant_wifi.c` (`wifi_config_reload_setting` патч #6 @ `0x100e88e8`, `wifi_config_set_bssid` патч #7 @ `0x100e6bf4`, `wpa_driver_inband_set_ps_mode` патч #8 @ `0x100ce49e`), `net_task.c`, `inband_queue.c` |
| Сетевой стек lwIP | `decomp/lwip/` | 3 | 423 (2) | `lwip_core.c` (TCP/UDP/IP/DHCP/DNS/sockets), `net_apps.c` (`fota_trigger_update`, HTTPClient), `sys_arch.c` |
| Криптография | `decomp/crypto/` | 1 | 433 (0) | `mbedtls.c` (PEM/RSA/EC/TLS handshake) |
| Стандартная библиотека | `decomp/libc/` | 1 | 161 (21) | `newlib_libgcc.c` (`memcpy`, `memset`, `snprintf`, `strcmp`, `strtol`, ...) |
| Прочее | `decomp/misc/` | 1 | 76 (0) | `external_refs.c` |
