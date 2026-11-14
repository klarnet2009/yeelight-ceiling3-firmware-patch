# Инфраструктура и среда выполнения (`infrastructure.md`)

Документ описывает целевую аппаратную платформу, карту памяти (Flash/RAM/NVDM), сетевое окружение LAN и инструментарий хоста Windows, используемые для анализа, сборки и доставки прошивки `yeelink.light.ceiling3` (`2.0.2_0048`).

---

## 1. Целевое аппаратное обеспечение и карта памяти

- **Устройство**: потолочный светильник Yeelight `YLXD17YL` (идентификатор модели `yeelink.light.ceiling3`).
- **SoC**: **MediaTek MT7697N** — основное ядро приложения **ARM Cortex-M4F** (набор инструкций Thumb-2 little-endian) и сетевой Wi-Fi/BLE-сопроцессор **N9**.
- **Операционная система и стек**: `SDK_V4.8.0_wifi`, `FreeRTOS`, `lwIP`, `mbedTLS`, Xiaomi `miio` SDK, контроллер Yeelight `yglc`.

### 1.1. Сегменты OTA-контейнера (`MMM`) и отображение XIP Flash

Данные подтверждены разбором стокового образа [structure.json](../../structure.json) и [YlSetup.java](../../tools/ghidra/YlSetup.java):

| Сегмент | Файл в `extracted/` | Смещение во Flash | Адрес XIP / исполнения | Сжатый размер | Распакованный размер | Размер раздела | Назначение |
|---|---|---:|---:|---:|---:|---:|---|
| 0 | `segment-0-flash-00071000.bin` | `0x00071000` | `0x10071000` | 509 288 Б | 915 020 Б | 1 245 184 Б (`0x130000`) | Приложение Cortex-M4F (RTOS, Wi-Fi/lwIP, miio, yglc) |
| 1 | `segment-1-flash-00008000.bin` | `0x00008000` | — | 217 384 Б | 299 242 Б | 430 080 Б (`0x69000`) | Образ подсистемы N9 |
| 2 | `segment-2-flash-0020a000.bin` | `0x0020a000` | — | 86 Б | 64 Б | 1 245 184 Б (`0x130000`) | Трамплин перехода на `0x10071001` |

### 1.2. Карта RAM (реконструкция по коду старта `0x10071000`)

Согласно [disassembly-start.txt](../../disassembly-start.txt) и [YlSetup.java](../../tools/ghidra/YlSetup.java), код `reset_startup` при старте инициализирует следующие области:

| Блок | Диапазон адресов | Источник во Flash | Права | Назначение |
|---|---|---|---|---|
| `flash_app` | `0x10071000..0x1015064C` | — | R-X | XIP-код и константы сегмента 0 |
| `tcm_text_data` | `0x00100000..0x00102C18` | `0x1014DA2C` (`0x2C18` Б) | RWX | Быстрый код (`ramfuncs`) и данные в TCM |
| `tcm_bss` | `0x00102C18..0x0010F4B8` | обнуление `[0x00102C20, 0x0010F4B5)` | RW- | `.bss` в TCM |
| `sysram_data` | `0x20000000..0x20001548` | `0x1014C4E4` (`0x1548` Б) | RW- | Инициализированные данные в SYSRAM |
| `sysram_bss` | `0x20001548..0x2003AA44` | обнуление (`0x394FC` Б) | RW- | `.bss` и куча/глобальные структуры в SYSRAM |
| `sysram_stack` | `0x2003AA44..0x20040000` | — | RW- | Системный стек (`SP = 0x20040000`) |

### 1.3. Служебные области Flash, NVDM и WDT

- **OTA staging для SoC (`0x20a000`)**: выбирается в `arch_ota_select_flash` (`0x100b1d0c`, arch_ota.c) по константе `DAT_100a8ca8 = 0x20a000`, окно `0x130000` байт (допустимый размер образа `0x2800..0x130000`).
- **OTA staging для MCU (`0x33a000`)**: выбирается в `arch_ota_select_mcu_flash` (`0x100a8cc0`, arch_ota.c) по константе `DAT_100a8cfc = 0x33a000`, окно `0x80000` байт.
- **Persistent Storage Manager (`miio_psm`)**: инициализируется в `arch_psm_init` (`0x100a8f50`, arch_psm.c) по адресу `0x1fc000`, размер `0xe000`.
- **Кэш тёплой перезагрузки `yl_cache` (`0x3ba000`)**: сектор flash, в который `yglc_reboot_save` (`0x100bed64`, yglc_core.c) записывает структуру с магией `0xdeadbeef` и `type = 1`, а `FUN_100c32fc` (yl_cache.c) читает и очищает её при загрузке, восстанавливая состояние света без инкремента `fast_reboot_count`.
- **NVDM `STA/PSMode`**: читается при инициализации Wi-Fi в `wifi_config_init` (`0x100ba70c`, yglc_core.c). Согласно [WIFI_RECOVERY.md](../../decomp/WIFI_RECOVERY.md), таблица значений по умолчанию SDK (`0x10129c50`) содержит `PSMode = "0"` (энергосбережение STA выключено), `BeaconLostTime = "5"`, `ListenInterval = "1"`; в runtime приложение не вызывает `wifi_config_set_power_save_mode` (`0x100eafe4`, доступна только через CLI `wifi_config_set_ps_mode` @ `0x100a2988`).
- **Аппаратный WDT**: в стоковой прошивке не активен как периодический сторожевой таймер задач; `hal_wdt_init` (`0x10072094`, mt7687_hal.c) и `hal_wdt_software_reset` (`0x10072140`, mt7687_hal.c) с записью `0x1209` (`WDT_SWRST_KEY`) в регистр сброса `WDT_SWRST` используются только для немедленного программного сброса.

---

## 2. Сетевое окружение (LAN и внешние сервисы)

- **Адрес лампы по умолчанию**: `192.168.0.20` (задан в [miio_ota.py](../../miio_ota.py), [monitor_lamp.py](../../monitor_lamp.py), [xiaomi_qr_token.py](../../xiaomi_qr_token.py); в первых двух переопределяется параметрами).

| Протокол / Порт | Направление | Назначение и требования |
|---|---|---|
| **UDP `54321`** | Хост ↔ Лампа | Протокол `miio` (hello + команды `AES-128-CBC` / PKCS7 + MD5-подпись; токен берётся из `token.txt`) |
| **TCP `55443`** (`0xd893`) | Хост → Лампа | Yeelight LAN Control (`lan_ctrl_task`, до 4 клиентов, keepalive `10s / 3 / 2s`, `\r\n`-JSON RPC) |
| **UDP `127.0.0.1:43210`** (`0xa8ca`) | Внутри лампы | Внутренний loopback-канал IPC между `yglc` и `lan_ctrl_task` |
| **UDP `239.255.255.250:1982`** (`0x7be`) | Лампа ↔ LAN (Multicast) | SSDP-обнаружение (`NOTIFY * HTTP/1.1` и `M-SEARCH` для `wifi_bulb`) |
| **TCP `80`** (`0x50`) | Лампа → Хост (HTTP/1.1) | **Обязателен** для локального OTA-сервера в `miio_ota.py`: `FUN_100f8250` оставляет поле порта `0`, а `httpc_req` (`0x10100750`) не парсит `:port` из URL и принудительно использует `80` для `http://`. Требует свободного порта `80` и правила входящего трафика в брандмауэре Windows для LAN |
| **HTTPS `443`** (`<server>.api.io.mi.com`) | Хост → Xiaomi Cloud | Используется `xiaomi_qr_token.py` и `miio_ota.py --cloud=de` (QR-логин `account.xiaomi.com`, RC4 API `/home/device_list`, `/home/rpc/<did>`) |

---

## 3. Инструментарий хоста (Windows)

### 3.1. Python и системные зависимости

Проверено в рабочей среде:
- **ОС**: Windows (используются системные утилиты `ping -n 1 -w 1500` и `netsh interface ipv4 show neighbors`, флаг `subprocess.CREATE_NO_WINDOW`).
- **Python**: `3.14.0`.
- **Установленные пакеты**:
  - `cryptography` (`50.0.2`) — обязателен для `miio_ota.py` (`AES-128-CBC`, PKCS7).
  - `requests` (`2.32.5`) и `qrcode` — обязательны для `xiaomi_qr_token.py` и облачного режима `miio_ota.py --cloud=...`.
  - `capstone` (`5.0.7`) — опционален для `patch_ota.py` (вывод Thumb-дизассемблирования вносимых патчей; при отсутствии скрипт продолжает работу).
- **Нативная библиотека сжатия**: `liblzma-5.dll` из состава Git for Windows (`C:\Program Files\Git\mingw64\bin\liblzma-5.dll`, проверенная версия `5.8.3`). Путь можно переопределить через переменную окружения `LIBLZMA`; минимально требуемая версия — `>= 5.4` (`50040002`) для поддержки `LZMA_FILTER_LZMA1EXT` и `lzma_raw_buffer_encode`.

### 3.2. Статический анализ и декомпиляция (Ghidra)

- **Платформа**: **Ghidra `12.2 PUBLIC`** (`<GHIDRA_HOME>`).
- **Проект**: ghidra-proj/ceiling3.gpr и хранилище `ghidra-proj/ceiling3.rep/`.
- **Параметры импорта сегмента 0**: формат `Raw Binary`, процессор/компилятор `ARM:LE:32:Cortex:default`, базовый адрес `-loader-baseAddr 0x10071000`.
- **Headless / GUI скрипты** (`-scriptPath tools\ghidra`):
  1. [YlSetup.java](../../tools/ghidra/YlSetup.java) — выполняется до автоанализа (`-preScript`), строит сегменты RAM/TCM, задаёт `TMode=1` и засеивает `816` функций по указателям.
  2. [YlStrings.java](../../tools/ghidra/YlStrings.java) — типизирует литеральные указатели (`13734`) и ASCII-строки (`164`).
  3. [YlEnrich.java](../../tools/ghidra/YlEnrich.java) и [YlExport.java](../../tools/ghidra/YlExport.java) — восстанавливают имена функций (`142` + `54`), декомпилируют все `4210` функций (`0` ошибок), а [clean_decomp.py](../../tools/clean_decomp.py) собирает модульный каталог `decomp/<subsystem>/` (`91` `.c`-файл + `include/`) и индекс [decomp/functions.csv](../../decomp/functions.csv).
