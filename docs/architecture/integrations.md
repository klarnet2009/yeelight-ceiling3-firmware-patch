# Внешние интеграции (`yeelink.light.ceiling3` / `YLXD17YL`)

Документ описывает внешние протоколы, облачные API и инструментальные интеграции проекта для потолочного светильника Yeelight `YLXD17YL` (`yeelink.light.ceiling3`, MediaTek `MT7697N`, базовая прошивка `2.0.2_0048`). Все адреса даны в отображении flash сегмента `0` (`BASE = 0x10071000`).

---

## 1. Локальный UDP-протокол MiIO

- **Назначение:** локальный RPC-обмен с лампой для диагностики, чтения метаданных и запуска OTA-обновления.
- **Транспорт и адресация:** UDP порт `54321` (`0xd431` в `otu_init` @ `0x100ff664`, `decomp/miio/otu.c`; `PORT = 54321` в `miio_ota.py` и `monitor_lamp.py`).
- **Формат пакета:**
  - Заголовок `32` байта (big-endian, `>HHIII` + 16-байтный дайджест):
    - `bytes[0:2]` — magic `0x2131`;
    - `bytes[2:4]` — полная длина пакета (`32 + len(payload)`);
    - `bytes[4:8]` — зарезервировано (`0`);
    - `bytes[8:12]` — идентификатор устройства `did`;
    - `bytes[12:16]` — монотонная метка времени `stamp`;
    - `bytes[16:32]` — контрольный дайджест `MD5(head[:16] + token + payload)`.
  - Рукопожатие `hello`: 32-байтный пакет `0x21310020` + 28 байт `0xff` (`Miio.hello()` в `miio_ota.py`, `miio()` в `monitor_lamp.py`); в ответе лампа возвращает `did` и `stamp`.
- **Криптография (`Miio._crypt` в `miio_ota.py`; `FUN_100ff830` @ `0x100ff830` и `FUN_100ff906` @ `0x100ff906` в `decomp/miio/miio_core.c`):**
  - Общий секрет: 16-байтный `token` (`32` hex-символа).
  - Ключ и вектор инициализации: `key = MD5(token)`, `iv = MD5(key + token)`.
  - Шифр: `AES-128-CBC` с дополнением `PKCS#7` (размер блока 128 бит; при разборе ответа `miio_ota.py` также выполняет `.rstrip(b'\0')`).
- **Хранение токена:**
  - На хосте: в файле `token.txt` рядом со скриптами `miio_ota.py` и `xiaomi_qr_token.py` (в аргументы командной строки не передаётся).
  - В прошивке: в NVDM (группа `ot_config`, ключ `token`, 32 байта / `0x20`), чтение через `thunk_FUN_10104aa0("ot_config", "token", ...)` (`FUN_100aadc4` в `decomp/hal/arch_platform.c`), запись при обновлении через `update_token` @ `0x100aae04` (`decomp/miio/miio_ciphers.c`) -> `thunk_FUN_10104b14`.
- **Используемые RPC-методы:**
  - `miIO.info` — проверка токена, получение `model`, `fw_ver`, `life` (время работы); используется в `miio_ota.py info` и до/после перезагрузки при `flash`.
  - `miIO.get_ota_state` и `miIO.get_ota_progress` — опрос состояния и процента загрузки/установки OTA (`miio_ota.py state`, цикл ожидания в `flash()`).
  - `miIO.ota` — запуск прошивки с параметрами `{'mode': 'normal', 'install': '1', 'app_url': url, 'file_md5': md5, 'proc': 'dnld install'}`.
- **Обработка ошибок и ограничение стока:**
  - В `miio_ota.py`: таймаут сокета `5` с, до `3` попыток с пересинхронизацией `hello()`; несовпадение MD5-суммы ответа вызывает `ValueError`.
  - В стоковой прошивке `do_ota` @ `0x100f8304` (`decomp/miio/miio_ota.c`) проверяет флаг происхождения RPC (`*(int *)(param_1 + 8) << 0x1e`, бит 1): локальные запросы `miIO.ota` по UDP `54321` отбрасываются с логом `"ota is from local, droped"` и кодом `-30020`. Патч `0x100f831c` (`13d4 -> 13e0`, `bmi -> b`) в `patch_ota.py` снимает эту проверку для последующих локальных прошивок.

---

## 2. Облако Xiaomi (`--cloud` / `xiaomi_qr_token.py` / клиент `ots`)

### 2.1. Хостовая интеграция (`xiaomi_qr_token.py` и `miio_ota.py --cloud=de`)

- **Назначение:** получение токена устройства и облачная ретрансляция `miIO.ota` через `/home/rpc/<did>` для обхода запрета локального OTA на стоковой прошивке.
- **Авторизация (QR-вход через Mi Home):**
  1. `GET https://account.xiaomi.com/longPolling/loginUrl` (`sid=xiaomiio`, `callback=https://sts.api.io.mi.com/sts`) -> JSON с префиксом `&&&START&&&`, из которого извлекаются `loginUrl` и `lp` (long-poll URL, TTL ~`300` с).
  2. `loginUrl` сохраняется в `xiaomi_login_qr.png` (`qrcode`).
  3. Long-poll по `d['lp']` возвращает `userId`, `ssecurity` и `location`; запрос по `location` выдаёт cookie `serviceToken`. Сессия хранится только в оперативной памяти процесса.
- **Протокол клиентского облачного API (`xiaomi_qr_token.py:api()`):**
  - Базовый URL: `https://{server}.api.io.mi.com/app{path}` (для `cn` — `https://api.io.mi.com/app{path}`; поддерживаются регионы `cn`, `de`, `us`, `ru`, `tw`, `sg`, `in`, `i2`, по умолчанию `de`).
  - Заголовки: `User-Agent` приложения Mi Home (`AGENT`), `x-xiaomi-protocal-flag-cli: PROTOCAL-HTTP2`, `MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4`, cookies `userId`, `serviceToken`, `yetAnotherServiceToken`.
  - Шифрование и подпись:
    - Генерируется `_nonce = base64(os.urandom(8) + (ts_ms // 60000).to_bytes(4, 'big'))`.
    - Сеансовый ключ RC4: `key = SHA256(base64_decode(ssecurity) + base64_decode(_nonce))`.
    - Параметры подписываются `SHA-1` (`rc4_hash__` по незашифрованным полям, затем `signature` по зашифрованным) и шифруются алгоритмом `RC4` с отбрасыванием первых `1024` байт потока ключа (drop-1024 RC4). Ответ сервера расшифровывается тем же ключом.
- **Вызываемые эндпоинты:**
  - `POST /home/device_list` (`{"getVirtualModel":false,"getHuamiDevices":0}`) — поиск устройства с `model == 'yeelink.light.ceiling3'` и `localip == IP`, получение `token` (в `xiaomi_qr_token.py` -> запись в `token.txt`) и `did` (в `miio_ota.py`).
  - `POST /home/rpc/{did}` (`{'method': 'miIO.ota', 'params': params}`) — отправка команды `miIO.ota` через облако при передаче `--cloud=<server>` (например, `--cloud=de`) в `miio_ota.py`. При этом саму прошивку лампа по-прежнему скачивает с локального HTTP-сервера на ПК.
  - `POST /home/latest_version` (`{"model":"yeelink.light.ceiling3"}`) — использовался при первичном получении метаданных и подписанной HTTPS-ссылки на сток `2.0.2_0048` с CDN `fk-res-abroad-cdn.home.mi.com` (`provenance.json`).

### 2.2. Облачный клиент `ots` в прошивке

- **Назначение:** постоянное двустороннее облачное соединение для телеметрии и приёма облачных RPC (включая `miIO.ota` от Mi Home / `/home/rpc/<did>`).
- **Реализация (`decomp/miio/ots.c`, `decomp/miio/tls.c`, `decomp/miio/miio_ot.c`):**
  - Инициализация в `ots_init` @ `0x100fde24`, рабочий поток `ots_task` (`ots_login_verify` @ `0x100fcd10`).
  - Адрес сервера: `ots.io.mi.com` (или `<region>.ots.io.mi.com`), порт `443` (`0x1bb`). Разрешение адреса выполняется через `httpdns` (`FUN_100fab52`) с резервным переходом на стандартный DNS (`FUN_100a841c`).
  - Транспорт: `d0_tls_open` @ `0x101024e4` (`decomp/miio/tls.c`) поверх `mbedTLS` (проверка X.509 / `Mijia Cloud`, конфигурация PSK `mbedtls_ssl_conf_psk` по `DID`, `TCP_NODELAY`, таймаут рукопожатия до `10000` мс).
  - Поддерживается аутентификация (`ots_login_ack`), keepalive-таймеры (`ots_timer_kplv_timeout`, начальный интервал `45000` мс) и переподключение (`ots_reconnect`). Входящие облачные RPC (`ots_down_rpc_delegate_ack`) передаются в диспетчер с выставленным облачным флагом, благодаря чему проходят проверку `do_ota`.

---

## 3. Протокол локального управления Yeelight LAN Control

- **Назначение:** локальное управление светильником без облака и без токена MiIO, а также обнаружение устройства в локальной сети. В проекте используется функцией `lan()` в `monitor_lamp.py` как проба работоспособности прикладной задачи Yeelight.
- **Реализация в прошивке (`decomp/app/lan_ctrl_cli.c`):**
  - Инициализация: `lan_ctrl_init` @ `0x100c41e4` (запускает поток `lan_ctrl_task` @ `FUN_100c3648`), запуск TCP-сервера в `lan_open_mchannel` / `ctrl_sock_recv` @ `0x100c3e84`, приём клиентов в `FUN_100c39ec` (`server_sock_recv`), инициализация клиента в `FUN_100c365c` (`lan_clnt_new`), разбор команд в `FUN_100c3ee0` (`lan_clnt_recv`).
- **TCP-сервер команд:**
  - Порт: `55443` (`0xd893`), `listen(..., 2)`.
  - Формат: JSON-строки с разделителем `\r\n` (размер входного буфера клиента `0x100` = `256` байт). Пример запроса в `monitor_lamp.py`: `{"id":1,"method":"get_prop","params":["power","bright"]}\r\n`.
  - Ограничение подключений: **максимум `4` одновременных клиента** (проверки `(byte)PTR_DAT_100c3a7c[0x10] < 4` в `server_sock_recv` и `3 < (byte)puVar5[0x10]` при открытии music-канала `set_music`). При превышении лимита сокет нового клиента сразу закрывается.
  - Параметры `SO_KEEPALIVE` (устанавливаются в `FUN_100c365c` через `FUN_10082584` в `decomp/lwip/lwip_core.c`):
    - `SO_KEEPALIVE = 1` (`level=0xfff, opt=8`);
    - `keep_idle = 10` с (`level=6, opt=3`, внутри хранится как `10000` мс);
    - `keep_cnt = 3` (`level=6, opt=5`);
    - `keep_intvl = 2` с (`level=6, opt=4`, внутри хранится как `2000` мс).
- **Обнаружение SSDP (UDP multicast):**
  - Инициализация в `FUN_100c4ca4` (`lan_discover_init`), обработчик `FUN_100c4b24` (`mcast_sock_recv`).
  - Группа `239.255.255.250`, UDP-порт `1982` (`0x7be`).
  - Реагирует на `M-SEARCH * HTTP/1.1` с `"ssdp:discover"` и `wifi_bulb`, отправляя unicast-ответ `HTTP/1.1 200 OK` с `Cache-Control: max-age=3600` (`0xe10`) и `Location: yeelight://<ip>:55443`.
  - Периодически рассылает multicast-анонсы `NOTIFY * HTTP/1.1` на `239.255.255.250:1982`.

---

## 4. Загрузка OTA по HTTP (`httpc_req` @ `0x10100750`)

- **Назначение:** скачивание OTA-образа лампой по URL из поля `app_url` команды `miIO.ota`.
- **Поведение и ограничение порта `80` в прошивке:**
  - Задача OTA вызывает `FUN_100f8250` (`decomp/miio/miio_core.c#L805`), которая передаёт структуру запроса в `httpc_req` @ `0x10100750` (`decomp/miio/httpc.c#L695`).
  - Поле явного порта `param_2[2]` (`local_1c`) всегда остаётся `0`, а `param_2[5]` (корневой сертификат) — `0`.
  - Для `http://` при `param_2[2] == 0` функция `httpc_req` жёстко подставляет порт `80` (`0x50`). Для `https://` подставляется `443` (`0x1bb`), но при `param_2[5] == 0` запрос отклоняется с ошибкой `"https need root cert"`.
  - При разборе `http://` функция ищет первый символ `'/'` (`FUN_10108134(iVar3, 0x2f)`), **не извлекая `:port` из URL**. Если указать `http://192.168.0.100:8321/...`, вся подстрока `192.168.0.100:8321` передаётся в DNS-резолвер как имя хоста, резолв падает и TCP-соединение не открывается (зафиксировано в `flash-step1.log`).
  - HTTP-клиент (`httpc_resp_body_recving` @ `0x100ffbc4`) отправляет `GET <path> HTTP/1.1`, `Host: <host>`, `User-Agent: MIoT` и при необходимости заголовок `Range: bytes=...`.
- **Локальный HTTP-сервер в `miio_ota.py:serve()`:**
  - Запускает `http.server.ThreadingHTTPServer(('0.0.0.0', port), H)` с `protocol_version = 'HTTP/1.1'` на порту `80` (поддерживает `200 OK` и `206 Partial Content` по `Range`; при `--port != 80` выводится предупреждение).
  - Автоматически определяет исходящий IP ПК в подсети лампы через `local_ip_for(IP)` и публикует образ по пути `/<md5>.bin`.
  - Для корректного завершения передачи на медленном встроенном TCP-стеке после `wfile.write(data)` и `flush()` вызывает `self.connection.shutdown(socket.SHUT_WR)` и ожидает закрытия соединения со стороны лампы (`recv(4096)` с таймаутом `180` с), логируя результат в `flash.log`.

---

## 5. Инструменты реверс-инжиниринга, сжатия и верификации

### 5.1. Ghidra `12.2_PUBLIC`, скрипты `tools/ghidra/` и `tools/clean_decomp.py`

- **Проект:** `ghidra-proj/ceiling3.gpr`, анализируемый бинарник — `extracted/segment-0-flash-00071000.bin` (`ARM:LE:32:Cortex:default`, база `0x10071000`, `TMode=1`).
- **Скрипты (`tools/ghidra/` и `tools/clean_decomp.py`):**
  - `YlSetup.java` — воссоздаёт карту памяти из литералов стартапа (`0x10071000..` XIP, `sysram`, `tcm`), выставляет `TMode=1`, задаёт `reset_startup`, `startup_init`, `startup_main` и засевает функции по указателям на Thumb-код.
  - `YlStrings.java` — типизирует слова пулов литералов (`ldr rX, [pc, #imm]`) как указатели и определяет ASCII-строки.
  - `YlEnrich.java` — применяет подтверждённые символы HAL/RTOS/libc/`arch_*`/`jsmi_*`/`yglc_*`/`psm_*` (`142` функции, `7` глобальных меток `g_*` / `WDT_SWRST`).
  - `YlExport.java` — восстанавливает имена `FUN_*`, пути исходников и модули по форматным логам и assert-ам, распространяет имена на thunk-обёртки и экспортирует псевдо-C в промежуточный дамп (`4210` функций, `0` ошибок).
  - `clean_decomp.py` — разрешает `PTR_s_*`/`PTR_DAT_*`, нормализует типы `<stdint.h>` через `decomp/include/fw_types.h` и `fw_globals.h` и формирует чистое модульное дерево `decomp/<subsystem>/*.c` (`4210` функций, `489` именованных -> `91` файл + `decomp/functions.csv`).

### 5.2. `liblzma-5.dll` (Git for Windows `5.8`, кодировщик `LZMA1EXT`)

- **Проблема стандартной библиотеки:** сегменты в MMM-контейнере упакованы в формате `LZMA-alone` с известным распакованным размером в заголовке и **без маркера конца потока (EOS / EOPM)**. Модуль `lzma` из стандартной библиотеки Python (`lzma.FORMAT_ALONE`) всегда записывает EOS-маркер.
- **Интеграция в `patch_ota.py:lzma_alone_no_eos()`:**
  - Через `ctypes.CDLL` загружается `liblzma-5.dll` (по умолчанию `C:\Program Files\Git\mingw64\bin\liblzma-5.dll`, переопределяется переменной окружения `LIBLZMA`; требуется версия `>= 5.4` / `50040002`, Git for Windows поставляет `5.8`).
  - Используется фильтр `LZMA1EXT` (`0x4000000000000002`) и структура `LzmaOptions` (`lzma_options_lzma`) с вызовом `lzma_raw_buffer_encode`.
  - Режим сжатия: `mode = 1` (`LZMA_MODE_FAST`), `mf = 0x04` (`LZMA_MF_HC4`), `ext_flags = 0` (без `LZMA_LZMA1EXT_ALLOW_EOPM` — отключает EOS), `lc`/`lp`/`pb`/`dict_size` наследуются от стокового сегмента `0`.
  - Подбор `nice_len` (`32..273`) обеспечивает кратность размера упакованного сегмента `0` (`13 + len(stream)`) восьми байтам для сохранения 8-байтового выравнивания следующих сегментов.

### 5.3. `capstone` (верификация ARM Thumb-2)

- **Назначение:** независимая проверка инструкций ARM Thumb-2 по целевым адресам патчей.
- **Интеграция:**
  - В `patch_ota.py` (опциональная зависимость): `capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)` дизассемблирует старые и новые байты каждого патча при сборке образа.
  - Ранее использовался для верификации начального кода стартапа `0x10071000` (`disassembly-start.txt`).
