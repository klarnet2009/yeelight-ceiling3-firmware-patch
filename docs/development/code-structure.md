# Структура кода и навигация по репозиторию

Репозиторий содержит инструментарий хоста (Python), извлечённые сегменты прошивки, проект Ghidra и декомпилированный псевдо-C код для потолочного светильника Yeelight (`YLXD17YL`, модель `yeelink.light.ceiling3`, SoC MediaTek MT7697N, стоковая прошивка `2.0.2_0048`).

---

## 1. Карта каталога

```text
yeelight/
├── yeelink.light.ceiling3-stock-2.0.2_0048.bin           # Оригинальный стоковый OTA-контейнер MMM (726 978 Б)
├── yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin  # Переупакованный сток без патчей (--stock, 727 122 Б)
├── yeelink.light.ceiling3-patched-2.0.2_0048.bin         # Патченый OTA-образ с патчами #1v2–#8 (727 130 Б, MD5 db3ce52b85a80e27225fcca2b302abc9)
│
├── unpack.py                 # Распаковка стокового OTA в extracted/ (только stdlib Python)
├── patch_ota.py              # Патчинг segment 0, пересжатие LZMA1EXT (без EOS) и сборка контейнера MMM
├── miio_ota.py               # Клиент miIO (info/state/flash), локальный HTTP-сервер на порту 80 и --cloud relay
├── monitor_lamp.py           # Долговременный 4-уровневый мониторинг доступности лампы (ARP/ICMP/miIO/LAN)
├── xiaomi_qr_token.py        # Получение token.txt через QR-авторизацию в облаке Xiaomi
│
├── token.txt                 # 32 hex-символа miIO-токена устройства (локальный секрет, не публиковать)
├── xiaomi_login_qr.png       # Временный QR-код входа Mi Home (генерируется xiaomi_qr_token.py)
│
├── catalog-entry.json        # Метаданные каталога syssi для модели yeelink.light.ceiling3
├── changelog.json            # Описание стокового релиза 2.0.2_0048
├── provenance.json           # Происхождение стокового OTA (endpoint, URL-метаданные, хеши, заголовки)
├── structure.json            # Детальная разметка контейнера MMM (смещения, SHA-1, размеры, trailing CRC)
├── SHA256SUMS.txt            # Контрольные суммы SHA-256 исходных артефактов
├── disassembly-start.txt     # Стартовый ARM Thumb дизассемблинг с адреса 0x10071000 (Capstone)
├── README_RU.md              # Отчёт о первичном получении прошивки и текущем статусе разработки
│
├── flash.log                 # Лог запусков miio_ota.py (flash/HTTP-передача/перезагрузка)
├── flash-step1.log           # Лог первой попытки раздачи repacked-stock на нестандартном порту :8321
├── monitor_lamp.log          # Лог смены состояний и heartbeat из monitor_lamp.py
│
├── extracted\                # Распакованные бинарные сегменты OTA и дампы ASCII-строк
│   ├── segment-0-flash-00071000.bin               # Приложение Cortex-M4 (915 020 Б, база исполнения 0x10071000)
│   ├── segment-0-flash-00071000.bin.strings.txt
│   ├── segment-1-flash-00008000.bin               # Образ подсистемы N9 (299 242 Б, flash 0x8000)
│   ├── segment-1-flash-00008000.bin.strings.txt
│   ├── segment-2-flash-0020a000.bin               # Трамплин запуска (64 Б, flash 0x20a000)
│   └── segment-2-flash-0020a000.bin.strings.txt
│
├── tools\
│   ├── clean_decomp.py       # Очистка псевдо-C (inline-строки, битовые слайсы, uint32_t, раскладка по подсистемам)
│   ├── prep_decomp_build.py  # Генерация decomp_symbols.h и Thumb-таблицы линковщика ceiling3_symbols.ld
│   ├── build_fw.py           # Сборка C11-модулей под CM4F (clang/ld.lld), host-тестов (gcc) и проверка decomp/
│   └── ghidra\               # Headless-скрипты Ghidra 12.2 для анализа и экспорта segment 0
│       ├── YlSetup.java      # Карта памяти (SYSRAM/TCM), Thumb TMode=1, сидинг функций по указателям
│       ├── YlStrings.java    # Типизация указателей literal pool и разметка ASCII-строк
│       ├── YlEnrich.java     # Восстановление имён RTOS/HAL/libc/arch/jsmi/yglc и глобальных символов
│       └── YlExport.java     # Голосование имён по логам (DOTALL + assert/func) и экспорт в staging/
│
├── firmware\                 # Готовые C11-исходники расширений прошивки CM4F и хостовые ABI/unit-тесты
│   ├── include\              # Заголовки SDK прошивки (`ceiling3_sdk.h`, `miio_wifi_clean.h`) с проверками _Static_assert
│   ├── ld\                   # Скрипты линковки `ceiling3_inplace_monitor.ld`, `ceiling3_ext.ld` и таблица ROM-символов `ceiling3_symbols.ld`
│   ├── src\                  # Реализация модулей оверлея на C11 (`wifi_recovery_guard.c`, `ota_hooks.c`)
│   ├── clean_src\            # Чистый компилируемый исходник подсистем (`miio_wifi_clean.c` — Wi-Fi/miio_net)
│   ├── tests\                # Хостовые тесты ABI и конечного автомата (`test_ceiling3_firmware.c`)
│   └── build\                # Артефакты сборки (`yglc_network_monitor_inplace.{elf,bin}`, `ceiling3_ext.{elf,bin}`, `clean_obj\`, `test_*.exe`, `decomp_obj\`)
│
├── ghidra-proj\              # Рабочий проект Ghidra (`ceiling3.gpr`, `ceiling3.rep\`, логи `analyze*.log`, `script*.log`)
│
├── decomp\                   # Структурированный C-декомпилят (4210 функций, 489 именованных, 91 файл)
│   ├── functions.csv         # Индекс всех функций: entry, name, module, size, file
│   ├── WIFI_RECOVERY.md      # Разбор механизмов восстановления Wi-Fi, WDT и применённых патчей
│   ├── include\              # Типы, глобальные адреса и compat-заголовки (`fw_types.h`, `fw_globals.h`, `ghidra_compat.h`, `decomp_symbols.h`)
│   ├── app\                  # Прикладная логика светильника (`yglc.c`, `yglc_core.c`, `yglc_miIO.c`, `yl_cache.c`, `lan_ctrl_cli.c`)
│   ├── miio\                 # Стек miIO (`miio_core.c`, `miio_net.c`, `miio_ota.c`, `ota_app.c`, `httpc.c`, `jsmi.c`, ...)
│   ├── ble\                  # BLE-стек MTK и Xiaomi MIBLE (`mible_*.c`, `bt_*.c`, `mtk_bt_stack.c`)
│   ├── lwip\                 # Сетевой стек lwIP, DHCP/ARP/TCP/IP и HTTPClient/FOTA (`lwip_core.c`, `net_apps.c`, `sys_arch.c`)
│   ├── rtos\                 # Ядро FreeRTOS (`tasks.c`, `queue.c`, `timers.c`, `heap_4.c`, `port.c`, `syslog.c`, ...)
│   ├── hal\                  # Драйверы MT7687/7697 HAL, NVDM и обёртки `arch_*` (`arch_os.c`, `arch_ota.c`, `arch_flash.c`, ...)
│   ├── wifi\                 # Драйвер Wi-Fi, wpa_supplicant, SmartConfig и inband-очереди
│   ├── crypto\               # Библиотека TLS/криптографии (`mbedtls.c`)
│   ├── libc\                 # Функции newlib/libgcc (`memcpy`, `memset`, `snprintf`, ...)
│   └── misc\                 # Внешние ссылки и служебные области (`external_refs.c`)
│
└── docs\
    ├── architecture\         # Архитектурная документация системы
    └── development\          # Руководства разработчика (code-structure, extension-guide, deployment, coding-standards)
```

---

## 2. Точки входа (Entry Points)

### 2.1. Хостовые Python-скрипты

| Скрипт | Назначение | Основные команды / аргументы |
|---|---|---|
| [unpack.py](../../unpack.py) | Проверка и распаковка стокового OTA в `extracted/` | `python unpack.py [path-to-original.bin]` |
| [patch_ota.py](../../patch_ota.py) | Сборка и валидация переупакованного или патченого OTA | `python patch_ota.py` / `python patch_ota.py --stock` |
| [miio_ota.py](../../miio_ota.py) | Взаимодействие по протоколу miIO (AES-128-CBC) и OTA-прошивка | `python miio_ota.py info`, `state`, `flash <image.bin> [--ip=...] [--port=80] [--cloud=de] [--yes]` |
| [monitor_lamp.py](../../monitor_lamp.py) | Непрерывный опрос состояния сети и сервисов лампы | `python monitor_lamp.py [ip] [interval_s] [--once]` |
| [xiaomi_qr_token.py](../../xiaomi_qr_token.py) | Авторизация в облаке Xiaomi по QR и запись токена | `python xiaomi_qr_token.py [server]` (по умолчанию `de`) |

### 2.2. Скрипты Ghidra (`tools\ghidra\`) и постобработка

Запускаются через `analyzeHeadless.bat` из состава Ghidra (`12.2_PUBLIC`, требуется JDK 25+) для бинарника `extracted/segment-0-flash-00071000.bin` с базовым адресом `0x10071000` и языком `ARM:LE:32:Cortex:default`:
1. [YlSetup.java](../../tools/ghidra/YlSetup.java) — `preScript` при первичном импорте; создаёт блоки памяти `flash_app`, `sysram_data`, `sysram_bss`, `sysram_stack`, `tcm_text_data`, `tcm_bss`, включает `TMode=1` и засевает функции.
2. [YlStrings.java](../../tools/ghidra/YlStrings.java) — `postScript` для разметки указателей в literal pool (`ldr rX, [pc, #imm]`) и ASCII-строк.
3. [YlEnrich.java](../../tools/ghidra/YlEnrich.java) — `postScript` перед экспортом; восстанавливает имена верифицированных функций RTOS/HAL/libc/`arch_*`/`jsmi_*`/`yglc_*`, размечает глобальные переменные (`g_miio_net`, `g_yglc_settings_ptr`, `g_offline_since_ms`, `WDT_SWRST`, ...) и транки.
4. [YlExport.java](../../tools/ghidra/YlExport.java) — `postScript <outdir>`; именует функции по формату логов `(%s,%d)` (включая многострочные `(?s)`) и `func:%s`, определяет модули по assert-вызовам `__FILE__` и экспортирует сырой результат во временный каталог `decomp_stage/`.
5. [clean_decomp.py](../../tools/clean_decomp.py) — постобработка: разрешает `PTR_s_*` / `PTR_DAT_*` в реальные строковые литералы и адреса через `segment-0-flash-00071000.bin`, заменяет `undefined*` на `<stdint.h>`, подключает fw_globals.h и раскладывает код по подсистемам в `decomp/`.

### 2.3. Точка входа прошивки Cortex-M4 (`segment-0-flash-00071000.bin`)

Согласно [disassembly-start.txt](../../disassembly-start.txt) и [YlSetup.java](../../tools/ghidra/YlSetup.java#L34-L55):
- `0x10071000` (`reset_startup`): устанавливает `sp = 0x20040000`, отключает прерывания (`cpsid i`), копирует `.data` из `0x1014C4E4` в `0x20000000` (`0x1548` байт) и `ramfuncs`/TCM-данные из `0x1014DA2C` в `0x00100000` (`0x2C18` байт), обнуляет `.bss` в SYSRAM (`0x20001548..0x2003AA44`) и TCM (`0x00102C18..0x0010F4B8`).
- Далее вызывает `startup_init` (`0x100BAC68`, Thumb-адрес `0x100BAC69`) и переходит на `startup_main` (`0x100A7694`, Thumb-адрес `0x100A7695`).

---

## 3. Конфигурация, переменные окружения и локальные файлы

| Параметр / переменная | Где используется | Значение по умолчанию / формат |
|---|---|---|
| `LIBLZMA` (env var) | [patch_ota.py](../../patch_ota.py#L40) | `C:\Program Files\Git\mingw64\bin\liblzma-5.dll` (требуется `liblzma >= 5.4`, конкретно проверка версии `>= 50040002`, для `LZMA1EXT` без end-marker). |
| `token.txt` | [miio_ota.py](../../miio_ota.py#L82-L86), [xiaomi_qr_token.py](../../xiaomi_qr_token.py#L111-L112) | Ровно 32 hex-символа (16 байт AES-токена miIO). На командную строку не передаётся. |
| IP лампы (`--ip=...` / позиционный arg) | `miio_ota.py`, `monitor_lamp.py`, `xiaomi_qr_token.py` | По умолчанию `192.168.0.20` (`PORT = 54321` в `miio_ota.py`). |
| `--cloud=<region>` | `miio_ota.py` | Регион облака Xiaomi (например `de`) для ретрансляции `miIO.ota` через `/home/rpc/<did>`. По умолчанию при `--cloud` без `=` берётся `'de'`. |
| `--port=<num>` | `miio_ota.py` | HTTP-порт раздачи OTA (по умолчанию `80`). Использование иного порта выдаёт предупреждение: `httpc_req` лампы не парсит `:port` из URL. |
| `--yes` | `miio_ota.py` | Пропускает интерактивное подтверждение `input(...)` перед отправкой `miIO.ota`. |
| `--stock` | `patch_ota.py` | Собирает `yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin` без применения `PATCHES`. |
| `--once` / `interval_s` | `monitor_lamp.py` | `--once` выполняет один цикл зондов; числовой аргумент задаёт период опроса в секундах (по умолчанию `30`). |

---

## 4. Где живёт ключевая логика

### 4.1. Хостовые инструменты

- **Разбор и верификация MMM-контейнера**: `parse()`, `build()`, `fw_crc()` в [patch_ota.py](../../patch_ota.py#L98-L138). Здесь реализованы проверки magic `0x004d4d4d`, количества сегментов (`3`), SHA-1 заголовка (`bytes[0:136]`), SHA-1 каждого сжатого сегмента, LZMA-распаковки и завершающего CRC32 (`0xEDB88320`, init `0`, без финального xor — соответствует `arch_ota_check_crc`).
- **Сжатие без маркера конца потока**: `lzma_alone_no_eos()` в [patch_ota.py](../../patch_ota.py#L61-L95) перебирает `nice_len` (`32..273`) в режиме `LZMA_MODE_FAST` + `LZMA_MF_HC4` через `liblzma-5.dll`, добиваясь кратности размера `8` байтам.
- **Протокол miIO и доставка OTA**: класс `Miio`, функция `serve()`, `flash()` в [miio_ota.py](../../miio_ota.py#L31-L255).
- **Диагностика сети лампы**: `ping()`, `arp()`, `miio()`, `lan()`, `verdict()` в [monitor_lamp.py](../../monitor_lamp.py#L31-L91).

### 4.2. Декомпилированная прошивка (`decomp\`)

Для поиска функций и адресов используйте [functions.csv](../../decomp/functions.csv), заголовки типов fw_types.h / fw_globals.h и [WIFI_RECOVERY.md](../../decomp/WIFI_RECOVERY.md):

- **Wi-Fi state machine и переподключение**: decomp/miio/miio_net.c — `miio_net_state_change`, `miio_wifi_try_reconnect` (`0x100af244`, патч #2 @ `0x100af298`), `wifi_cb_event_handler` (`0x100af370`, патч #4 @ `0x100af66e`/`0x100af670`/`0x100af700` для пропуска среднего лога внутри case 8 `CONNECT_FAILED -> miio_net_post_event(5)`), `miio_wifi_scan_finish` (`task_monitor`, `sta_offline_timeout_s` `900s -> 180s` патч #5 @ `0x100b04a4`/`0x100b0c80`/`0x100b0cae` и патч #5b @ `0x100b0cbe` для порога `sta_monitor_offline_countout`).
- **Монитор сети Yeelight и автоперезагрузка**: decomp/app/yglc_core.c — `yglc_network_monitor` (`0x100bd918..0x100bd9f8`, 224-байтный слот, 500 мс таймер). Патч **#1v2** заменяет его in-place бинарником yglc_network_monitor_inplace.bin (212/224 Б, функция `ceiling3_yglc_network_monitor_inplace()` в [wifi_recovery_guard.c](../../firmware/src/wifi_recovery_guard.c#L122-L167), линкер [ceiling3_inplace_monitor.ld](../../firmware/ld/ceiling3_inplace_monitor.ld)): в `LOCAL` каждые 60 с отправляет `etharp_request` шлюзу, проверяет `ts` в окне 90 с под `lock_tcpip_core` и выполняет `cmd.reboot` через 300 с при отсутствии ответа или в `DISCONNECTED`.
- **Память оверлея (`EXT_SRAM` vs `sysram_bss`)**: в [ceiling3_ext.ld](../../firmware/ld/ceiling3_ext.ld) секция `EXT_SRAM` (`0x20039800..0x2003AA00`) пересекается с `sysram_bss` (`0x20001548..0x2003AA44`) и близка к `ucHeap` (`0x20008f98..0x20038798`), поэтому продакшен-фикс использует zero-BSS in-place замену в слоте `0x100bd918..0x100bd9f8` с существующим 8-байтным состоянием `0x20008030..0x20008037`.
- **Драйвер Wi-Fi и wpa_supplicant**: decomp/wifi/wpa_supplicant_wifi.c — `wifi_config_reload_setting` (`0x100e88e8`, патч #6: утечка семафора при ошибке), `wifi_config_set_bssid` (`0x100e6bf4`, патч #7: пропуск битого `%s` лога сырого MAC), а также `wpa_driver_inband_set_ps_mode` (`0x100ce49e`, патч #8: принудительный `PSMode = 0`).
- **Обработка `miIO.ota` и локальная блокировка**: decomp/miio/miio_ota.c — `do_ota` (`0x100f8304`, патч #3 @ `0x100f831c`).
- **Запись OTA во flash и проверка CRC**: decomp/hal/arch_ota.c (`arch_ota_check_crc` @ `0x100a8e78`, `arch_ota_select_flash` @ `0x100b1d0c`) и decomp/miio/ota_app.c (`app_fw_init`, `app_fw_write`, `app_fw_close`).
- **Тёплый ребут и сохранение состояния света (`yl_cache` @ `0x3ba000`)**: `miio_reboot` (`0x100f81e4` в decomp/miio/miio_core.c), `yglc_reboot_save` (`0x100bed64` в decomp/app/yglc_core.c) и `FUN_100c32fc` (decomp/app/yl_cache.c).
- **LAN Control и CLI**: decomp/app/lan_ctrl_cli.c (`lan_open_mchannel` @ `0x100c3e84`, `lan_ctrl_init` @ `0x100c41e4`).
- **HTTP-клиент OTA**: `httpc_req` (`0x10100750`), а также модули decomp/miio/httpc.c, decomp/miio/httpdns.c, decomp/miio/tcp.c, decomp/miio/udp.c.
