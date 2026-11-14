# Документация сервисов и подсистем (Services)

Документация разделена на два контура: хостовые CLI-сервисы и утилиты в корне репозитория и встроенные службы/RTOS-задачи прошивки `yeelink.light.ceiling3` версии `2.0.2_0048` (сегмент 0, XIP-база `0x10071000`).

---

## Часть A. Хостовые сервисы и утилиты (корень репозитория)

### 1. `unpack.py` — верифицированный распаковщик контейнера `MMM`
- **Файл**: [unpack.py](../../unpack.py)
- **Назначение**: Воспроизводимая проверка и распаковка стокового OTA-образа `yeelink.light.ceiling3-stock-2.0.2_0048.bin` без модификации исходного файла.
- **Ответственность**:
  - Проверка длины (`726 978` байт) и MD5 (`13b7471a0f84e3cfbac78c116fae7a1c`).
  - Проверка magic `0x004D4D4D`, числа сегментов (`3`), SHA-1 заголовка (`bytes[0:136]` vs `bytes[136:156]`) и SHA-1 каждого сжатого сегмента.
  - Декомпрессия каждого сегмента стандартным `lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)` с контролем `eof` и отсутствия `unused_data`.
  - Запись сегментов в `extracted/segment-{0,1,2}-flash-{addr}.bin`.
- **Зависимости**: Только стандартная библиотека Python (`hashlib`, `json`, `lzma`, `pathlib`, `struct`, `sys`).
- **Интерфейс (CLI)**: `python unpack.py [path-to-original.bin]`; печатает JSON-строки с именем, размером и SHA-256 каждого сегмента.

### 2. `patch_ota.py` — бинарный патчер и репакер `MMM`
- **Файл**: [patch_ota.py](../../patch_ota.py)
- **Назначение**: Наложение верифицированных Thumb-2 патчей на сегмент 0 и сборка нового валидного `MMM`-образа, совместимого с загрузчиком и `arch_ota_check_crc`.
- **Ответственность**:
  - Валидация контейнера в `parse(data)` (magic, SHA-1 заголовка и сегментов, потоки LZMA, завершающий CRC32 `fw_crc()` с полиномом `0xEDB88320`, init `0`, без финального XOR).
  - Self-test немодифицированной пересборки `build(stock, parse(stock)[0][1]) == stock`.
  - Наложение 3 патчей (при отсутствии `--stock`):
    1. `0x100bd942` (`yglc_network_monitor`): `efd1` (`bne`) -> `00bf` (`nop`).
    2. `0x100af298` (`miio_wifi_try_reconnect`): `0bd9` (`bls`) -> `0be0` (`b`).
    3. `0x100f831c` (`do_ota`): `13d4` (`bmi`) -> `13e0` (`b`).
  - Перекодирование сегмента 0 функцией `lzma_alone_no_eos()` через `liblzma-5.dll` (`LZMA1EXT`, `LZMA_MODE_FAST`, `LZMA_MF_HC4`, `ext_flags = 0`) без EOS-маркера и с подбором `nice_len` (`32..273`) для кратности длины 8 байтам.
  - Полная повторная проверка выходного файла через `parse(out)` и проверка отсутствия EOS-маркера при неизвестном размере потока (`-1`).
- **Зависимости**: `liblzma-5.dll` >= `5.4` (по умолчанию `C:\Program Files\Git\mingw64\bin\liblzma-5.dll`, переопределяется `LIBLZMA`), Python stdlib, опционально `capstone` для вывода дизассемблера.
- **Интерфейс (CLI / Python API)**:
  - `python patch_ota.py` -> создаёт `yeelink.light.ceiling3-patched-2.0.2_0048.bin` (`727 042` байт).
  - `python patch_ota.py --stock` -> создаёт `yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin` (`727 122` байт).
  - Экспортирует `parse()` и `fw_crc()`, которые переиспользуются в `miio_ota.py`.

### 3. `xiaomi_qr_token.py` — клиент QR-авторизации и API Xiaomi Cloud
- **Файл**: [xiaomi_qr_token.py](../../xiaomi_qr_token.py)
- **Назначение**: Получение 32-символьного hex-токена устройства из облака Xiaomi через сканирование QR-кода в приложении Mi Home и предоставление RC4-подписанного клиента облачного API.
- **Ответственность**:
  - `login(session)`: запрос `https://account.xiaomi.com/longPolling/loginUrl`, сохранение QR-кода в `xiaomi_login_qr.png`, ожидание сканирования, получение `userId`, `ssecurity` и cookie `serviceToken`.
  - `api(session, user_id, ssecurity, service_token, path, params, server)`: формирование nonce, вычисление SHA-256 ключа сессии, подпись `rc4_hash__` / `signature`, шифрование параметров и расшифровка ответа алгоритмом RC4 с отбрасыванием первых 1024 байт keystream.
  - Поиск устройства `yeelink.light.ceiling3` по списку серверов (`de`, `cn`, `us`, `ru`, `tw`, `sg`, `in`, `i2`) и запись токена в `token.txt`.
- **Зависимости**: `requests`, `qrcode`.
- **Интерфейс**: `python xiaomi_qr_token.py [server]`; функции `login()` и `api()` импортируются `miio_ota.py` при использовании `--cloud`.

### 4. `miio_ota.py` — оркестратор локальной и облачной OTA-прошивки
- **Файл**: [miio_ota.py](../../miio_ota.py)
- **Назначение**: Взаимодействие с лампой по протоколу `miio` (UDP `54321`) и доставка локально собранных образов прошивки по HTTP.
- **Ответственность**:
  - Класс `Miio`: рукопожатие `hello()` (`21310020` + 28 байт `0xff`), вычисление ключа/IV (`MD5(token)`, `MD5(key + token)`), шифрование/расшифровка JSON-RPC в AES-128-CBC (PKCS7) с MD5-контролем пакета.
  - Предварительная валидация образа через `patch_ota.parse()` и проверка модели `yeelink.light.ceiling3` через `miIO.info`.
  - Встроенный `ThreadingHTTPServer` (`serve()`) на порту `80` (вынужденное ограничение из-за `httpc_req` @ `0x10100750`), с ожиданием закрытия сокета клиентом (`shutdown(SHUT_WR)` + цикл `recv`).
  - Отправка `miIO.ota` (`mode=normal`, `install=1`, `proc="dnld install"`) либо напрямую по UDP (`54321`), либо через Xiaomi Cloud `/home/rpc/<did>` (`--cloud=de`).
  - Мониторинг состояния (`miIO.get_ota_state`, `miIO.get_ota_progress`, `miIO.info`) до перезагрузки и возврата в сеть с логированием в `flash.log`.
- **Зависимости**: `cryptography`, `patch_ota.py`, `token.txt`, опционально `xiaomi_qr_token.py` + `requests`.
- **Интерфейс (CLI)**:
  - `python miio_ota.py info [--ip=...]`
  - `python miio_ota.py state [--ip=...]`
  - `python miio_ota.py flash <image.bin> [--ip=...] [--cloud=de] [--yes]`

### 5. `monitor_lamp.py` — многоуровневый зонд доступности лампы
- **Файл**: [monitor_lamp.py](../../monitor_lamp.py)
- **Назначение**: Непрерывная диагностика сценариев отвала лампы с разделением проблем ассоциации/IP и зависания стека при живом ARP/ICMP.
- **Ответственность**:
  - Четыре read-only пробы (в порядке `ping()` -> `miio()` -> `lan()` -> `arp()`):
    - `ping`: ICMP echo через системный `ping -n 1 -w 1500` (2 попытки).
    - `miio`: UDP `54321` `hello`-пакет (`len == 32`, заголовок `0x2131`).
    - `lan`: TCP `55443` запрос `{"id":1,"method":"get_prop","params":["power","bright"]}\r\n`.
    - `arp`: проверка состояния соседа `Reachable` в `netsh interface ipv4 show neighbors`.
  - Вынесение вердикта (`OK`, `OFFLINE`, `ZOMBIE`, `PARTIAL`) и запись смен состояния + heartbeat каждые 30 минут в `monitor_lamp.log`.
- **Зависимости**: Windows `ping.exe`, `netsh.exe`, Python stdlib.
- **Интерфейс (CLI)**: `python monitor_lamp.py [ip] [interval_s] [--once]`.

### 6. Конвейер декомпиляции (`tools/ghidra/*.java` и `tools/clean_decomp.py`)
- **Файлы**: [YlSetup.java](../../tools/ghidra/YlSetup.java), [YlStrings.java](../../tools/ghidra/YlStrings.java), [YlEnrich.java](../../tools/ghidra/YlEnrich.java), [YlExport.java](../../tools/ghidra/YlExport.java), [clean_decomp.py](../../tools/clean_decomp.py).
- **Назначение**: Автоматизированная подготовка бинарника сегмента 0 к анализу в Ghidra и экспорт читаемого модульного псевдо-C в `decomp/`.
- **Внутренняя логика**:
  - `YlSetup.java`: переименовывает `flash_app`, создаёт блоки `sysram_data`, `sysram_bss`, `sysram_stack`, `tcm_text_data`, `tcm_bss`, включает `TMode=1`, создаёт `reset_startup` (`0x10071000`), `startup_init` (`0x100BAC68`), `startup_main` (`0x100A7694`) и засеивает функции по нечётным Thumb-указателям на прологи `PUSH` (`0xB4xx`/`0xB5xx`/`0xE92D`).
  - `YlStrings.java`: типизирует слова пула литералов `ldr rX, [pc, #imm]` как указатели и размечает ASCII C-строки по их адресам.
  - `YlEnrich.java`: переименовывает подтверждённые функции HAL/RTOS/libc/`arch_*`/`jsmi_*`/`yglc_*`/`psm_*` (`142` функции) и глобальные метки (`g_miio_net`, `WDT_SWRST` и др.).
  - `YlExport.java`: анализирует вызовы логгера формата `...(%s,%d)` перед точками вызова, восстанавливает имена функций, пути исходников (`__FILE__`) и модули (`[L] <module>: ...`), распространяет имена на thunk-обёртки и выгружает функции + `decomp/functions.csv`.
  - `clean_decomp.py`: разрешает `PTR_s_*`/`PTR_DAT_*`, нормализует `<stdint.h>` через `fw_types.h` и `fw_globals.h` и раскладывает код по подсистемам `decomp/<subsystem>/*.c` (`4210` функций, `489` именованных -> `91` файл).

---

## Часть B. Встроенные сервисы прошивки (Cortex-M4F, база `0x10071000`)

### 1. `yglc` и `yglc_network_monitor`
- **Расположение**: yglc_core.c (`yglc_network_monitor` @ `0x100bd918`, `yglc_init_async` @ `0x100bedb4`, `yglc_task` @ `0x100bff30`), yglc_miIO.c.
- **Назначение**: Основная бизнес-логика светильника (управление светом, режимами, таймерами, состоянием питания) и сторожевой контроль потери сетевого подключения.
- **Ответственность и логика**:
  - Создаёт RTOS-задачи `yglc_task` (стек `0x1000`) и `app_test` (`0x100bc254`, стек `0x800`), а также периодические таймеры, включая `yglc_network_monitor` (`0x100bd918`, период 500 мс).
  - `yglc_network_monitor` опрашивает `miio_net_get_state()` (`FUN_100afc14`): пока состояние `!= 2` (`MIIO_NET_DISCONNECTED`), обновляет 64-битный таймштамп в `g_offline_since_ms` (`0x20008030`).
  - Если состояние держится равным `2` дольше `0x493e0` мс (300 000 мс = 5 минут), в стоке проверяется `*(int *)(*(int *)g_yglc_settings_ptr + 0x54) == 1` (выключенный свет); при выполнении условия вызывается `miio_reboot(..., "cmd_reboot")`.
  - **Патч `0x100bd942` (`efd1` -> `00bf`)** убирает ветвление по состоянию света, обеспечивая 5-минутный тёплый авторебут в любом режиме питания.

### 2. `lan_ctrl_task` — служба локального управления Yeelight LAN
- **Расположение**: lan_ctrl_cli.c (`lan_ctrl_init` @ `0x100c41e4`, `FUN_100c3648` @ `0x100c3648`), yglc_core.c (`FUN_100bba24` @ `0x100bba24`).
- **Назначение**: Локальный открытый протокол управления Yeelight без участия облака и обнаружение светильника в локальной сети.
- **Зависимости**: lwIP сокеты, `yglc` (чтение/изменение параметров света).
- **Экспортируемые интерфейсы**:
  - **TCP `55443`** (`0xd893`): приём JSON-команд (`get_prop`, управление состоянием) и рассылка уведомлений подключённым клиентам.
  - **UDP multicast `239.255.255.250:1982`** (`0x7be`): ответы и `NOTIFY` по SSDP с объявлением сервиса `Location: yeelight://<ip>:55443`.
  - **UDP loopback `127.0.0.1:43210`** (`0xa8ca`): внутренний управляющий сокет для пробуждения/координации цикла `select()`.
- **Внутренняя логика**: `lan_ctrl_init` создаёт таймер, структуру мультиплексирования ввода-вывода, loopback-сокет `127.0.0.1:43210` и поток `lan_ctrl_task` (`0x100c3648`, стек `0x1000`), который выполняет бесконечный цикл `FUN_100bba24()` поверх lwIP `select()` (`FUN_10081f38`) и диспатчит read/write/error коллбэки зарегистрированных дескрипторов.

### 3. `miio_net` / `netMonitorTask` — диспетчер Wi-Fi STA
- **Расположение**: miio_net.c (`miio_net_init` @ `0x100b0074`, `miio_net_state_change` @ `0x100ae8ec`, `miio_wifi_try_reconnect` @ `0x100af244`, `miio_wifi_scan_finish` @ `0x100b0a08`).
- **Назначение**: Управление Wi-Fi-подключением станции, хранение/смена сетевых состояний `MIIO_NET_*` и восстановление связи на уровне Wi-Fi/lwIP.
- **Состояния и события**:
  - Состояния `MIIO_NET_*`: `0 UNPROV`, `1 UAP`, `2 DISCONNECTED`, `3 LOCAL`, `4 CLOUD`, `5 UPDATING`.
  - События `MIIO_WIFI_EVENT_*` (`FUN_100af2e0` в miio_net_ota.c): `0 CONFIG_ROUTER`, `1 SCAN_DONE`, `2 STA_START`, `3 STA_STOP`, `4 STA_CONNECTED`, `5 STA_DISCONNECTED`, `6 STA_GOT_IP`, `7 STA_LOST_IP`, `8 AP_START`, `9 AP_STOP`.
- **Внутренняя логика**:
  - `miio_net_init` задаёт пороги `900` с (`+0x170`) и `10` отключений (`+0x1a4`), создаёт очередь и задачу `netMonitorTask` (стек `0x800`, точка входа `miio_wifi_scan_finish`, внутри которой расположен цикл `task_monitor` ~`0x100b1170..0x100b13ba`).
  - При достижении 15 тиков в `DISCONNECTED` взводится таймер 7000 мс на пересканирование/переподключение; при превышении `sta_monitor_offline_timeout` (900 с без IP) или `sta_monitor_offline_countout` (>= 10 отключений за 900 с) вызывается `miio_wifi_try_reconnect`.
  - В стоке `miio_wifi_try_reconnect` возвращает `-8` (запускающий `miio_wifi_stop` + `lwip_net_stop` + `wifi start(5000)`) только если найдено `< 2` подходящих AP. При `>= 2` AP возвращается `0` без сброса интерфейса.
  - **Патч `0x100af298` (`0bd9` -> `0be0`)** заменяет `bls` на безусловный `b`, принудительно возвращая `-8` и гарантируя полный перезапуск Wi-Fi/lwIP даже при наличии нескольких AP/MESH.

### 4. `miio` ядро, `otu` (локальный UDP) и `ots` (облачный TLS)
- **Расположение**: miio.c, miio_core.c, miio_instance.c, miio_ot.c, otu.c, ots.c, miio_ciphers.c.
- **Назначение**: Единая шина JSON-RPC протокола `miio` для локального (`otu`) и облачного (`ots`) управления, NTP-синхронизации и системных методов.
- **Ответственность и API**:
  - `miio_instance_create` (`0x100aa9bc`) читает конфигурацию из NVDM/PSM (`psm_model`, `uid`, `country_domain`, `gmtoffset`), вызывает `miio_create` (`0x100f7fd0`, буфер `0x1fa0` байт), инициализирует `httpc`, `ot`, `ota`, `miio_ota_app_init`, `miio_net_init` и `mible_net_register`.
  - `otu` обрабатывает зашифрованный UDP-трафик на порту `54321` (`otu_packet_handle`, `otu_packet_parser_decrypt`), поддерживая handshake (`hello`), keepalive и локальные RPC.
  - `ots` поддерживает постоянную TLS-сессию с облаком Xiaomi (`ots_login_verify`, `ots_reconnect`, `ots_packet_send`, `ots_timer_kplv_timeout`), транслируя облачные RPC-вызовы и отчёты свойств.
  - Системные RPC включают `miIO.info`, `miIO.ota`, `miIO.get_ota_state`, `miIO.get_ota_progress`, `miIO.reboot` (`FUN_100f8234`), `miIO.ble_gateway_enable` и др.

### 5. `miio_ota`, `ota_app`, `arch_ota` и `httpc`
- **Расположение**: miio_ota.c, ota_app.c, arch_ota.c, httpc.c.
- **Назначение**: Валидация, загрузка по HTTP и запись обновлений прошивки во flash.
- **Внутренняя логика**:
  - `do_ota` (`0x100f8304`) проверяет флаг источника команды в `*(int *)(param_1 + 8) << 0x1e`: в стоке при сброшенном бите логирует `"ota is from local, droped"` и отклоняет запрос (`-30020` / `service not available`). **Патч `0x100f831c` (`13d4` -> `13e0`)** обходит эту проверку.
  - `app_fw_init` (`0x100b1d74`) парсит параметры через `jsmi_parse_start`, извлекает `app_url` и опциональные поля подписи (`original_length`).
  - `httpc_req` (`0x10100750`) ставит HTTP-запрос в очередь; порт берётся из отдельного поля структуры (`0` -> `80`), а разбор URL ищет только первый `/`, поэтому указание `:port` в URL ломает имя хоста.
  - `app_fw_write` (`0x100b1c94`) потоково пишет образ во flash (`FUN_100a8d14`) и при наличии подписи хэширует тело (`FUN_100ab7b4`).
  - `app_fw_close` (`0x100b1ee8`) проверяет сертификат/подпись (`miio_signature_verify`, если включена), вызывает `arch_ota_check_crc` (`0x100a8e78`, CRC32 `0xEDB88320` над телом до последних 4 байт) и завершает установку с последующей перезагрузкой через `miio_reboot(..., "miIO_ota_installed")`.

### 6. Подсистема сброса, WDT и тёплого кэша (`yl_cache`)
- **Расположение**: miio_core.c (`miio_reboot` @ `0x100f81e4`), mt7687_hal.c (`hal_sys_reboot`, `hal_wdt_init`, `hal_wdt_software_reset`), yglc_core.c (`yglc_reboot_save` @ `0x100bed64`), yl_cache.c (`FUN_100c32fc` @ `0x100c32fc`).
- **Назначение**: Безопасный тёплый перезапуск SoC (CM4F + N9) с сохранением состояния светильника.
- **Внутренняя логика**:
  - `miio_reboot` формирует событие `miio_reboot` и планирует таймер на 4000 мс (`FUN_100f7ed0` -> `hal_sys_reboot`).
  - `hal_sys_reboot` вызывает `yglc_reboot_save(0)`, который записывает во flash-область `yl_cache` (`0x3ba000`) 16-байтную структуру с маркером `0xdeadbeef`, типом `1` и текущими параметрами света.
  - Затем `hal_wdt_init` и `hal_wdt_software_reset` записывают `0x1209` (`WDT_SWRST_KEY`) в регистр программного сброса WDT (`WDT_SWRST`), переводя MCU в мгновенный аппаратный рестарт (как периодический сторожевой таймер задач WDT в стоке не активирован).
  - При загрузке `FUN_100c32fc` считывает и стирает запись `0xdeadbeef` из `0x3ba000`, восстанавливая яркость/цветовую температуру/питание и предотвращая инкремент счётчика сброса в режим сопряжения (`fast_reboot_count`).

### 7. `mible` — BLE Gateway и обработка BLE-событий
- **Расположение**: mible_gateway.c, mible_rpc.c, mible_net.c, mible_beacon.c, yglc_miIO.c, mible_host.c (`FUN_100b9834` @ `0x100b9834`).
- **Назначение**: Работа в роли Mi BLE Gateway, приём/дешифровка BLE-маячков (пульты, датчики) и выполнение локальных/облачных правил.
- **Внутренняя логика**: `mible_gateway_init` запускает задачу `ble_gateway` (стек `0x1000`), `mible_net_register` связывает BLE-шлюз с `miio`, а `do_bleEvtRuleAdd` / `do_bleEvtRuleDel` и `yglc_proc_ble_event` (`yglc_miIO.c`) управляют таблицей привязанных BLE-устройств (`mac`, `pid`, `eid`, `beacon_key`).
