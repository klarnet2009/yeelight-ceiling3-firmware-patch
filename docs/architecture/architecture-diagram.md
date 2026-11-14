# Архитектурные диаграммы (Architecture Diagrams)

## 1. Общая архитектура системы и контуры взаимодействия

Система разделена на хостовый контур анализа/доставки (корень репозитория), внешний контур Xiaomi Cloud и встроенную систему на кристалле MediaTek MT7697N (`yeelink.light.ceiling3`).

```mermaid
flowchart TD
    subgraph Host["Хост (Windows / Python + Ghidra)"]
        UNP["unpack.py (проверка и распаковка MMM)"]
        GH["Ghidra 12.2 + tools/ghidra (YlSetup, YlStrings, YlEnrich, YlExport)"]
        PAT["patch_ota.py + liblzma-5.dll (Thumb-2 патчи и репак MMM)"]
        QRT["xiaomi_qr_token.py (QR-логин и token.txt)"]
        OTA["miio_ota.py (HTTP :80 + miIO.ota)"]
        MON["monitor_lamp.py (4-слойный зонд доступности)"]
    end

    subgraph Cloud["Облако Xiaomi"]
        XAUTH["account.xiaomi.com (QR long-polling)"]
        XAPI["de.api.io.mi.com/app (/home/device_list, /home/rpc/did)"]
        XOTS["OTS Cloud Endpoint (TLS)"]
    end

    subgraph Device["Светильник YLXD17YL (MT7697N: Cortex-M4F + N9)"]
        N9["Подсистема N9 (Wi-Fi / BLE Radio, segment-1)"]
        LWIP["FreeRTOS + lwIP TCP/IP"]
        MIIO["miio SDK: otu (UDP 54321), ots (TLS), miio_net"]
        OTASVC["miio_ota + ota_app + arch_ota + httpc"]
        YGLC["yglc_task + yglc_network_monitor (500 мс)"]
        LAN["lan_ctrl_task (TCP 55443, SSDP 1982, UDP 127.0.0.1:43210)"]
        BLE["mible_gateway (ble_gateway task)"]
        FLASH["Flash (MMM segments + yl_cache @ 0x3ba000) + WDT SWRST"]
    end

    UNP --> GH
    GH --> PAT
    PAT --> OTA
    QRT -->|"HTTPS QR + RC4 API"| XAUTH
    QRT -->|"token.txt"| OTA
    OTA -->|"HTTPS /home/rpc/did (--cloud)"| XAPI
    XAPI -->|"TLS relay (miIO.ota)"| XOTS
    XOTS --> MIIO
    OTA -->|"UDP 54321 (miIO.info / state / ota)"| MIIO
    OTASVC -->|"HTTP GET :80/*.bin"| OTA
    MON -->|"ICMP / ARP / UDP 54321 / TCP 55443"| LWIP

    N9 <--> LWIP
    N9 <--> BLE
    LWIP <--> MIIO
    LWIP <--> LAN
    LWIP <--> OTASVC
    MIIO <--> YGLC
    MIIO <--> OTASVC
    YGLC <--> LAN
    YGLC <--> BLE
    OTASVC --> FLASH
    YGLC --> FLASH
```

## 2. Иерархия компонентов: контейнер `MMM`, карта памяти CM4F и конвейер сборки

Ниже показаны границы данных внутри OTA-контейнера `MMM`, отображение сегмента 0 в адресное пространство Cortex-M4F и поток преобразования бинарных артефактов.

```mermaid
flowchart LR
    subgraph MMM["Контейнер MMM (yeelink.light.ceiling3-*.bin)"]
        HDR["Заголовок 156 Б (0x004D4D4D, count=3, 3x32 Б, SHA-1[0:136])"]
        S0["Сегмент 0: LZMA без EOS -> flash 0x71000 + SHA-1 (20 Б)"]
        S1["Сегмент 1: LZMA без EOS -> flash 0x8000 + SHA-1 (20 Б)"]
        S2["Сегмент 2: LZMA без EOS -> flash 0x20a000 + SHA-1 (20 Б)"]
        CRC["Хвостовой CRC32 (4 Б, 0xEDB88320, init 0, no final XOR)"]
        HDR --- S0 --- S1 --- S2 --- CRC
    end

    subgraph CM4Map["Карта памяти Cortex-M4F (после reset_startup)"]
        XIP["Flash XIP: 0x10071000.. (915 020 Б, TMode=1)"]
        SYS["SYSRAM: 0x20000000 .data (0x1548) + .bss + stack (SP=0x20040000)"]
        TCM["TCM: 0x00100000 ramfuncs/.data (0x2C18) + .bss"]
        CACHE["Flash NVDM / yl_cache: 0x3ba000 (magic 0xdeadbeef)"]
        XIP -->|"копирование .data / ramfuncs"| SYS
        XIP -->|"копирование .data / ramfuncs"| TCM
    end

    subgraph Pipeline["Конвейер анализа и патчинга"]
        STOCK["stock-2.0.2_0048.bin"] -->|"unpack.py"| EXT["extracted/segment-0..2.bin"]
        EXT -->|"Ghidra + clean_decomp.py"| DEC["decomp/<subsystem>/*.c + include/*.h + functions.csv"]
        STOCK -->|"patch_ota.py (3 Thumb-2 патча + liblzma-5.dll)"| PATCHED["patched-2.0.2_0048.bin"]
        DEC -.->|"обоснование адресов 0x100bd942, 0x100af298, 0x100f831c"| PATCHED
    end
```

## 3. Последовательность OTA-прошивки (`miio_ota.py flash`)

При использовании флага `--cloud=de` команда `miIO.ota` маршрутизируется через облачный RPC Xiaomi (`/home/rpc/<did>`), чтобы обойти проверку источника команды в `do_ota` (на стоке локальный вызов отклоняется с ошибкой `-30020`; при установленном патче `0x100f831c` проверка флага обходится). Саму загрузку образа лампа всегда выполняет по HTTP на порт `80` ПК, так как `httpc_req` (`0x10100750`) не извлекает `:port` из URL.

```mermaid
sequenceDiagram
    participant User as "Оператор / miio_ota.py"
    participant Parse as "patch_ota.parse()"
    participant HTTP as "ThreadingHTTPServer (:80)"
    participant Cloud as "Xiaomi Cloud (/home/rpc/did)"
    participant Lamp as "Лампа (otu / do_ota / ota_app)"
    participant Flash as "arch_ota + yl_cache + WDT"

    User->>Parse: "Проверка MMM (SHA-1, LZMA, CRC32)"
    Parse-->>User: "OK (размер, MD5)"
    User->>Lamp: "UDP 54321: miIO.info (модель, fw_ver, life)"
    Lamp-->>User: "model=yeelink.light.ceiling3"
    User->>HTTP: "Запуск HTTP/1.1 сервера (/<md5>.bin)"

    alt "Режим --cloud=de (через xiaomi_qr_token.py)"
        User->>Cloud: "POST /home/rpc/did (miIO.ota, app_url=http://host/md5.bin)"
        Cloud->>Lamp: "Пересылка miIO.ota через ots (TLS)"
    else "Локальный режим (UDP 54321)"
        User->>Lamp: "AES-CBC UDP: miIO.ota (app_url=http://host/md5.bin)"
    end

    Lamp->>Lamp: "do_ota (0x100f8304) -> app_fw_init (0x100b1d74)"
    Lamp-->>User: "result: ['ok']"
    Lamp->>HTTP: "httpc_req (0x10100750) GET /<md5>.bin (порт 80)"
    HTTP-->>Lamp: "200 OK (поток байт + half-close shutdown(SHUT_WR))"
    Lamp->>Flash: "app_fw_write (0x100b1c94) -> app_fw_close (0x100b1ee8)"
    Flash->>Flash: "arch_ota_check_crc (0x100a8e78)"
    Flash->>Flash: "miio_reboot -> yglc_reboot_save(0) (yl_cache 0xdeadbeef)"
    Flash->>Lamp: "FUN_10072140 (WDT_SWRST = 0x1209)"

    loop "Опрос каждые 3..5 с (до 10 мин + 5 мин)"
        User->>Lamp: "miIO.get_ota_state / get_ota_progress / miIO.info"
        Lamp-->>User: "TimeoutError -> возврат с новым life"
    end
```

## 4. Двухуровневое восстановление Wi-Fi (с патчами)

В прошивке работают два независимых контура восстановления сети: SDK-уровень (`miio_net` в задаче `netMonitorTask`) и прикладной уровень (`yglc_network_monitor` по 500-миллисекундному таймеру). Патчи в `patch_ota.py` устраняют два блокирующих условия: пропуск перезапуска Wi-Fi/lwIP при `>= 2` AP и отмену тёплого авторебута при включённом свете.

```mermaid
sequenceDiagram
    participant NetMon as "netMonitorTask (miio_wifi_scan_finish)"
    participant Reconn as "miio_wifi_try_reconnect (0x100af244)"
    participant YglcMon as "yglc_network_monitor (0x100bd918, 500 мс)"
    participant Cache as "yl_cache (0x3ba000)"
    participant WDT as "FUN_10072140 (WDT_SWRST)"

    Note over NetMon,YglcMon: "Состояние сети: MIIO_NET_DISCONNECTED (state == 2, нет IPv4)"

    loop "Слой 1: miio SDK (таймаут 900 с или >= 10 отключений)"
        NetMon->>Reconn: "Попытка переподключения"
        Note right of Reconn: "Патч 0x100af298 (bls -> b): всегда идти на полный перезапуск"
        Reconn-->>NetMon: "Возврат -8 (независимо от числа найденных AP)"
        NetMon->>NetMon: "miio_wifi_stop (0x100b0610) + lwip_net_stop + wifi start(5000)"
    end

    loop "Слой 2: Yeelight watchdog (каждые 500 мс)"
        YglcMon->>YglcMon: "Проверка miio_net_get_state() == 2"
        Note right of YglcMon: "Патч 0x100bd942 (bne -> nop): ветка power==1 игнорируется"
        alt "Нет IP > 300 000 мс (0x493e0)"
            YglcMon->>Cache: "miio_reboot -> hal_sys_reboot -> yglc_reboot_save(0)"
            Cache->>Cache: "Запись маркера 0xdeadbeef и состояния света"
            YglcMon->>WDT: "FUN_10072094 + FUN_10072140 (запись 0x1209)"
            Note over WDT: "Полный тёплый сброс SoC (CM4F + N9) с восстановлением света"
        end
    end
```

## 5. Границы сервисов и коммуникационные паттерны

- **Граница ПК <-> Лампа в локальной сети**:
  - Управляющий обмен `miio` работает поверх **UDP `54321`** с шифрованием AES-128-CBC (ключ/IV выводятся из 16-байтового `token.txt` через MD5) и контролем целостности MD5.
  - Протокол Yeelight LAN Control работает по **TCP `55443`** (строковый JSON, разделённый `\r\n`) и **UDP multicast `239.255.255.250:1982`** (SSDP discovery/notify).
  - Передача OTA-образа выполняется по **HTTP/1.1 на TCP-порту `80`** (`miio_ota.py` -> `httpc_req`, поддерживаются `200 OK` и `206 Partial Content` по `Range`).
- **Внутрипроцессное взаимодействие в RTOS**:
  - Передача команд управления внутри `lan_ctrl_task` использует локальный loopback-сокет **`127.0.0.1:43210`** (`0xa8ca` в `lan_ctrl_init` @ `0x100c41e4`) и собственный цикл `select()` в `FUN_100bba24`.
  - Конвейер `ota_app` изолирует скачивание и запись во flash в отдельную задачу `ota_app` (`miio_ota_app_init` @ `0x100b203c`), а финальная перезагрузка откладывается на 4000 мс через таймер в `miio_reboot` (`0x100f81e4`) для отправки ответа/события `miio_reboot`.
- **Известный пробел границ**: если N9 или lwIP зависает с уже назначенным IP (`state == 3 LOCAL` или `4 CLOUD`), ни `netMonitorTask`, ни `yglc_network_monitor` не срабатывают, так как нет активного liveness-зонда трафика (ICMP/ARP/шлюз). На хосте этот режим детектируется как `ZOMBIE` скриптом [monitor_lamp.py](../../monitor_lamp.py).
