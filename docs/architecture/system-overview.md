# Обзор системы (System Overview)

## 1. Назначение системы

Репозиторий предназначен для статического реверс-инжиниринга, воспроизводимого бинарного патчинга, верификации OTA-контейнеров, сетевого мониторинга и прошивки потолочного светильника **Yeelight Ceiling Light (`YLXD17YL`)**, аппаратная платформа **MediaTek MT7697N**, модель устройства в экосистеме Xiaomi — `yeelink.light.ceiling3`, базовая версия прошивки — `2.0.2_0048`.

Ключевая инженерная цель проекта — устранить зависания Wi-Fi и потерю доступности лампы в сети (включая сценарии с несколькими точками доступа / MESH и включённым светом), а также обеспечить надёжную доставку патченых образов через локальный и облачный OTA-механизм без аппаратного программатора.

## 2. Высокоуровневая архитектура

Система состоит из двух взаимосвязанных контуров:

1. **Хостовый инструментарий (Windows / Python + Ghidra)** в корне репозитория:
   - Загрузка/верификация стокового образа `yeelink.light.ceiling3-stock-2.0.2_0048.bin` и распаковка контейнера `MMM` (`unpack.py`).
   - Статический анализ и декомпиляция сегмента 0 в Ghidra 12.2 (`tools/ghidra/YlSetup.java`, `YlStrings.java`, `YlEnrich.java`, `YlExport.java`) + постобработка (`tools/clean_decomp.py`) с генерацией чистого модульного C-кода в `decomp/`.
   - Наложение верифицированных ARM Thumb-2 патчей и переупаковка валидного `MMM`-контейнера (`patch_ota.py` + `liblzma-5.dll`).
   - Получение токена устройства через QR-авторизацию Xiaomi Cloud (`xiaomi_qr_token.py`), оркестрация прошивки (`miio_ota.py`) и многослойная диагностика состояния лампы (`monitor_lamp.py`).

2. **Целевая встроенная прошивка MT7697N (ARM Cortex-M4F + подсистема N9)**:
   - Исполняется поверх **FreeRTOS** и стека **lwIP** (`SDK_V4.8.0_wifi`), использует `miio` SDK (локальный UDP и облачный TLS), `mible` (BLE Gateway / Beacon / RPC) и проприетарную логику освещения Yeelight (`yglc` + `lan_ctrl_task`).
   - Контейнер `MMM` разделяет прошивку на 3 сегмента; основная логика приложения расположена в сегменте 0 (`segment-0-flash-00071000.bin`, адрес выполнения XIP `0x10071000`).

## 3. Основные подсистемы и зоны ответственности

| Подсистема | Расположение | Основные артефакты / символы | Ответственность |
|---|---|---|---|
| **Контейнер и патчинг OTA** | Хост | [unpack.py](../../unpack.py), [patch_ota.py](../../patch_ota.py), [structure.json](../../structure.json) | Проверка целостности `MMM`, распаковка LZMA-потоков, наложение Thumb-2 патчей, репак без EOS-маркера с пересчётом SHA-1 и CRC32. |
| **Конвейер декомпиляции** | Хост | [YlSetup.java](../../tools/ghidra/YlSetup.java), [YlStrings.java](../../tools/ghidra/YlStrings.java), [YlEnrich.java](../../tools/ghidra/YlEnrich.java), [YlExport.java](../../tools/ghidra/YlExport.java), [clean_decomp.py](../../tools/clean_decomp.py), [prep_decomp_build.py](../../tools/prep_decomp_build.py), [functions.csv](../../decomp/functions.csv) | Восстановление карты памяти (Flash XIP, SYSRAM, TCM), типизация строк/литералов, разметка подтверждённых символов, именование функций и сборка модульного дерева `decomp/<подсистема>/*.c` + таблиц компиляции. |
| **C11 SDK и сборка под CM4F** | Хост / CM4F | [ceiling3_sdk.h](../../firmware/include/ceiling3_sdk.h), [wifi_recovery_guard.c](../../firmware/src/wifi_recovery_guard.c), [ota_hooks.c](../../firmware/src/ota_hooks.c), [ceiling3_ext.ld](../../firmware/ld/ceiling3_ext.ld), [build_fw.py](../../tools/build_fw.py) | Компиляция модулей C11 (`clang --target=thumbv7em-none-eabi` + `ld.lld` -> `ceiling3_ext.elf`/`.bin`) с линковкой на Thumb-символы стоковой прошивки и хостовыми unit-тестами (`gcc`). |
| **Доставка OTA и авторизация** | Хост | [xiaomi_qr_token.py](../../xiaomi_qr_token.py), [miio_ota.py](../../miio_ota.py), provenance.json | QR-авторизация Xiaomi Cloud (RC4 API), запись `token.txt`, встроенный HTTP-сервер на порту `80`, отправка `miIO.ota` напрямую или через `/home/rpc/<did>`, контроль прогресса и перезагрузки. |
| **Сетевой мониторинг** | Хост | [monitor_lamp.py](../../monitor_lamp.py), monitor_lamp.log | Периодический опрос лампы по 4 уровням (`ping`, `miio`, `lan`, `arp`) и классификация сбоев (`OK`, `OFFLINE`, `ZOMBIE`, `PARTIAL`). |
| **Ядро освещения и LAN** | Прошивка (CM4F) | yglc_core.c (`yglc_task`, `yglc_network_monitor` @ `0x100bd918`), lan_ctrl_cli.c (`FUN_100c3648` @ `0x100c3648`) | Управление светом, таймеры/кроны, локальный TCP API (`55443`), SSDP (`239.255.255.250:1982`), loopback-управление (`127.0.0.1:43210`), 5-минутный авторебут при потере сети. |
| **Сетевой стек и транспорт `miio`** | Прошивка (CM4F) | miio_net.c, otu.c, ots.c, miio_ot.c | Менеджер Wi-Fi STA (`netMonitorTask` / `miio_wifi_scan_finish` + `miio_wifi_try_reconnect`), локальный UDP `54321` (`otu`), облачный TLS-клиент (`ots`), маршрутизация JSON-RPC. |
| **Подсистема OTA устройства** | Прошивка (CM4F) | miio_ota.c, ota_app.c, arch_ota.c, httpc.c | Обработка `miIO.ota` (`do_ota` @ `0x100f8304`), HTTP-загрузка (`httpc_req` @ `0x10100750`, порт `80`), запись во flash, проверка подписи и `arch_ota_check_crc` (`0x100a8e78`). |
| **Сброс, WDT и тёплый кэш** | Прошивка (CM4F) | `miio_reboot` (`0x100f81e4`), `hal_sys_reboot` (`0x10071e98`), `hal_wdt_init` (`0x10072094`), `hal_wdt_software_reset` (`0x10072140`), `yglc_reboot_save` (`0x100bed64`), `FUN_100c32fc` (`0x100c32fc`) | Сохранение состояния света в `yl_cache` (`0x3ba000`, маркер `0xdeadbeef`), программный сброс WDT_SWRST (`0x1209`), исключение ложного инкремента `fast_reboot_count`. |
| **BLE-шлюз (`mible`)** | Прошивка (CM4F) | mible_gateway.c, mible_rpc.c, mible_host.c, yglc_miIO.c | Задача `ble_gateway` (`FUN_100b9834` @ `0x100b9834`), обработка BLE-событий и маячков, синхронизация правил через `miio`. |

## 4. Структура контейнера `MMM` и карта памяти сегмента 0

Исходный образ `yeelink.light.ceiling3-stock-2.0.2_0048.bin` (726 978 байт, MD5 `13b7471a0f84e3cfbac78c116fae7a1c`, SHA-256 `24282ae0ce1bd94134c1266a26e4057dc9a2c173dc8c2f3bdb4fc5d925debdb8`) имеет заголовок 156 байт (magic `0x004D4D4D`, число сегментов `3`, 3 записи по 32 байта, SHA-1 над байтами `[0:136]`), 3 LZMA-сегмента с 20-байтными SHA-1 после каждого сжатого блока и 4-байтный завершающий CRC32 (`0xEDB88320`, init `0`, без финального XOR).

| Сегмент | Смещение во flash | Сжатый размер (сток) | Распакованный размер | Файл в `extracted/` | Роль |
|---:|---:|---:|---:|---|---|
| 0 | `0x71000` (XIP `0x10071000`) | 509 288 Б | 915 020 Б | segment-0-flash-00071000.bin | Основное приложение Cortex-M4F (Thumb-2) |
| 1 | `0x8000` | 217 384 Б | 299 242 Б | segment-1-flash-00008000.bin | Предположительно образ подсистемы N9 (внутренний формат не исследован) |
| 2 | `0x20a000` | 86 Б | 64 Б | segment-2-flash-0020a000.bin | Thumb-переходник к точке входа `0x10071001` |

При старте код инициализации сегмента 0 (`reset_startup` @ `0x10071000`, детали в [disassembly-start.txt](../../disassembly-start.txt) и [YlSetup.java](../../tools/ghidra/YlSetup.java)) формирует карту ОЗУ:
- `SYSRAM .data`: копирование `0x1548` байт из `0x1014C4E4` в `0x20000000`;
- `SYSRAM .bss` и стек: обнуление `[0x20001548, 0x2003AA44)`, начальный `SP = 0x20040000`;
- `TCM text/data`: копирование `0x2C18` байт из `0x1014DA2C` в `0x00100000` (ramfuncs + данные);
- `TCM .bss`: обнуление `[0x00102C18, 0x0010F4B8)`.

## 5. Технологический стек

- **Целевой SoC и ОС**: MediaTek MT7697N (ARM Cortex-M4F, Thumb-2, little-endian), FreeRTOS, lwIP, MediaTek `SDK_V4.8.0_wifi`, `miio` SDK, `mible` SDK.
- **Компиляция C11 и тестирование**: LLVM `clang` (`--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb`), линкер `ld.lld`, `llvm-objcopy` / `llvm-size`, `gcc` (хостовые ABI и state-machine тесты).
- **Статический анализ**: Ghidra 12.2 (язык `ARM:LE:32:Cortex:default`), Python + `capstone` (опционально для дизассемблирования патчей в `patch_ota.py`).
- **Инструментарий хоста**: Python 3 (`hashlib`, `lzma`, `zlib`, `struct`, `http.server`, `socket`, `ctypes`, `subprocess`), внешние пакеты `cryptography` (AES-128-CBC для протокола `miio`), `requests` и `qrcode` (облачный клиент), нативная библиотека `liblzma-5.dll` >= `5.4` (фильтр `LZMA1EXT` для сжатия без EOS-маркера).

## 6. Архитектурная философия и верифицированные изменения

1. **Строгая совместимость с загрузчиком**: стандартный `lzma` в Python всегда записывает EOS-маркер, поэтому сегмент 0 пережимается через `liblzma-5.dll` (`LZMA_MODE_FAST`, `LZMA_MF_HC4`, `ext_flags = 0`) с подбором `nice_len` для кратности размера 8 байтам. Патченные образы проходят полную обратную валидацию `patch_ota.parse()`.
2. **Точечные патчи и in-place замена в [patch_ota.py](../../patch_ota.py)** (итого `267` изменённых байт в `segment 0`):
   - `#1v2` `0x100bd918..0x100bd9f8` (`yglc_network_monitor`): in-place замена (212/224 Б) из `yglc_network_monitor_inplace.bin` — 5-минутный авторебут без IP (независимо от питания света) + активный ARP-зонд шлюза в режиме `LOCAL` раз в 60с;
   - `#2` `0x100af298` (`miio_wifi_try_reconnect`): `0bd9` (`bls`) -> `0be0` (`b`) — принудительный `miio_wifi_stop()` + `lwip_net_stop()` при переподключении, даже если найдено `>= 2` AP;
   - `#3` `0x100f831c` (`do_ota`): `13d4` (`bmi`) -> `13e0` (`b`) — разрешает прямой локальный `miIO.ota` без ошибки `-30020`;
   - `#4`–`#8` (`0x100af66e..0x100ce49e`): `MIIO_WIFI_EVENT_STA_DISCONNECTED (5)` при `CONNECT_FAILED`, `sta_offline_timeout_s = 180s`, фикс порога `sta_monitor_offline_countout >= 2`, фикс утечки семафора в `wifi_config_reload_setting`, пропуск битого `%s` лога MAC в `wifi_config_set_bssid`, отключение `PSMode = 0`;
   - `#9a` `0x100ebffe` (`FUN_100ebffe`, `mixer set_min_bright_pct`): `642928bf6421` -> `002100bf00bf` (`movs r1, #0; nop; nop`) — убирает 15%-й нижний порог яркости в дневном канале `"dl"`, позволяя масштабировать `bright = 1..100%` от `1%`;
   - `#9b` `0x1014067c..0x10140683` (`dl` CT calibration table `+0x154/+0x158`): `8.0f, 8.0f` (`00000041`) -> `6.5f, 6.5f` (`0000d040`) — снижает минимальный PWM-порог на канал с `8.0%` (`320/4000`) до `6.5%` (`260/4000`), чуть выше аппаратного отключения драйвера (`~5%` PWM).
3. **Известные ограничения** (см. [WIFI_RECOVERY.md](../../decomp/WIFI_RECOVERY.md)): мониторинг ARP в патче `#1v2` также покрывает тихие зависания сетевого пути в состоянии `LOCAL` (таймаут 300с при отсутствии ответа от шлюза), а аппаратные сбои уровня N9/RF требуют мониторинга через [monitor_lamp.py](../../monitor_lamp.py).
4. **Выходные образы**:
   - `yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin` (`727 122` байт) — тестовая перепаковка без патчей (`python patch_ota.py --stock`).
   - `yeelink.light.ceiling3-patched-2.0.2_0048.bin` (`727 130` байт, MD5 `41fa639b254a13116d5de9521e892d63`) — рабочий патченый образ (`python patch_ota.py`).
