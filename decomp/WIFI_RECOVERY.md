# Wi‑Fi recovery in ceiling3 2.0.2_0048 (from decompiled segment 0)

Addresses are in the app's flash mapping (base 0x10071000). State names come from the firmware's tables.

**Network states** (`miio_net_state_change`, table `MIIO_NET_*`): 0 UNPROV, 1 UAP, 2 DISCONNECTED (no IPv4 on `st2`), 3 LOCAL (has IP, no cloud), 4 CLOUD, 5 UPDATING.

**Wi‑Fi events** (`FUN_100af2e0` = post event, table `MIIO_WIFI_EVENT_*`): 0 CONFIG_ROUTER, 1 SCAN_DONE, 2 STA_START, 3 STA_STOP, 4 STA_CONNECTED, 5 STA_DISCONNECTED, 6 STA_GOT_IP, 7 STA_LOST_IP, 8 AP_START, 9 AP_STOP.

## Layer 1: Xiaomi miio SDK monitor (`task_monitor`, inside the mis-bounded `miio_wifi_scan_finish` ~0x100b1170..0x100b13ba)

Settings in `miio_net_init` (miio_net.c, about line 1026): `+0x170 = 900` s, `+0x1a4 = 10`.

| Condition | Code | Action |
|---|---|---|
| State DISCONNECTED, counter `+0x1b4` reaches 15 ticks | `FUN_100ae84c` | arm timer `0x100aedd9` with 7000 ms (rescan or reconnect) |
| No IP for 900 s in total (`+0x16c` ≥ `+0x170`) | `sta_monitor_offline_timeout` | `miio_wifi_try_reconnect`; if it does not return 0, `wifi stop` (0x100b0610) followed by `wifi start(5000)` (0x100af868) |
| ≥ 10 disconnects within 900 s (10 buckets, `+0x17c..`) | `sta_monitor_offline_countout` | same Wi‑Fi stop/start |

`miio_wifi_try_reconnect` returns -8 (forcing the stop/start) when fewer than 2 entries in the scan list are flagged as matching.

This layer only restarts the Wi‑Fi interface; it **does not** reset the SoC or the N9 core.

## Layer 2: Yeelight `yglc_network_monitor` (0x100bd918, timer every 500 ms, registered ~0x100bef16)

- State ≠ DISCONNECTED: the timestamp is reset.
- State DISCONNECTED for more than 300 000 ms **and the light is off** (`[*0x20006908+0x54] == 1`): `cmd.reboot`, a full SoC reboot that also reloads N9.
- With the light on the timestamp is reset every tick, so the lamp never reboots. **The patch removes this condition.**

## Gaps (not covered by either layer)

1. **State LOCAL/CLOUD but no traffic** (IP still assigned, link dead or N9 stuck while associated): stock firmware does not detect this. **Patch #1v2** (`ceiling3_yglc_network_monitor_inplace()`) addresses `LOCAL` hangs by probing the default gateway every 60 s via `etharp_request` under `lock_tcpip_core` and rebooting if gateway ARP stays missing/stale (`>= 90 s` `ts` age) for `> 300 000 ms`.
2. N9 hung while the light is on: the Wi‑Fi stop/start in layer 1 may never complete. Only the layer 2 reboot recovers it, and that requires the patch.

## Power save

`wifi_config_set_power_save_mode` (0x100eafe4) is called only from the CLI command `wifi_config_set_ps_mode` (0x100a2988); the app never changes PS at runtime. The startup value is `STA/PSMode` from NVDM, read in `FUN_100ba70c` (wifi_config_init). The SDK default table (0x10129c50) has `PSMode = "0"` (PS off), `BeaconLostTime = "5"` and `ListenInterval = "1"`. The value actually stored on the lamp is unknown without a flash dump.

## What decides the next fix

When the lamp drops off next time, before power-cycling it:
- `ping 192.168.0.20`
- does the router still list it (DHCP lease, ARP, Wi‑Fi clients)?

**Not associated or no IP** → the current patch covers it.

**Associated with an IP but silent** → gap 1. That needs new code: a liveness check in `yglc_network_monitor`, for example "LOCAL with no cloud for N minutes" or an ARP/ICMP probe to the gateway, placed in a code cave.


---

## Обновление (аудит RTOS и бинарные патчи)

### Полная причина зависания Wi-Fi и подтверждённые выводы
1. **`yglc_network_monitor` (`0x100bd918`)**: таймер раз в 500 мс отслеживает состояние `miio_net_get_state()` (`2 = MIIO_NET_DISCONNECTED`, нет IP на `st2`). Пока сеть не отключена (`state != 2`), таймштамп `0x20008030` постоянно обновляется; при переходе в `state == 2` и истечении `0x493e0` мс (5 минут) вызывается `cmd.reboot` (`0x100f81e4`), но в стоковой прошивке при `0x100bd940: cmp r3, #1` / `0x100bd942: bne 0x100bd924` перезагрузка выполняется только если свет выключен (`power == 1` — OFF, `power == 0` — ON).
2. **`miio_wifi_try_reconnect` (`miio_net.c`, `0x100af296..0x100af298`)**: при сканировании, если найдено `>= 2` подходящих AP/BSSID (типично для MESH или 2.4/5 ГГц с одинаковым/скрытым профилем), ветвление `bls 0x100af2b2` пропускается и не вызываются `miio_wifi_stop()` + `lwip_net_stop()`, из-за чего Wi-Fi/lwIP может застрять в полуотвалившемся состоянии.
3. **WDT и тёплая перезагрузка (`yl_cache` @ `0x3ba000`)**:
   - Аппаратный WDT (`hal_wdt_init` @ `0x10072094` / `hal_wdt_software_reset` @ `0x10072140` в `decomp/hal/mt7687_hal.c`) в стоке **не включён** как периодический сторожевой таймер задач — используется только `hal_wdt_software_reset` (запись `0x1209` в `WDT_SWRST`) для мгновенного программного сброса.
   - `miio_reboot` (`0x100f81e4` -> `hal_sys_reboot` @ `0x10071e98`) вызывает `yglc_reboot_save(0)` (`0x100bed64`), сохраняя состояние света и маркер `0xdeadbeef` (`type = 1`) в `yl_cache` (`0x3ba000`), после чего при загрузке `FUN_100c32fc()` (`decomp/app/yl_cache.c`) восстанавливает яркость/цветовую температуру/режим и не инкрементирует `fast_reboot_count` (нет ложного сброса в сопряжение).
4. **`PSMode` и `do_ota` (`0x100f8304`)**:
   - `PSMode` по умолчанию `"0"` (`decomp/app/yglc_core.c`, `FUN_100ba70c`), но при смене профиля или NVDM следует учитывать энергосбережение STA.
   - В `do_ota` инструкция `0x100f831a: lsls r3, r3, #0x1e` + `0x100f831c: bmi 0x100f8346` проверяет бит 1 флагов транспорта RPC `*(uint32_t *)(param_1 + 8)` (`2` = облачный `ots`, `1` = локальный UDP `otu`) и в стоке отклоняет локальный `miIO.ota` с ошибкой `-30020` (`"ota is from local, droped"`).

### Применённые бинарные патчи (`patch_ota.py`, база сегмента 0 = `0x10071000`)

#### Патч #1v2: In-place замена `yglc_network_monitor` (`0x100bd918..0x100bd9f8`)
- Заменяет старый патч #1 (`0x100bd942: efd1 -> 00bf`).
- Исходник: `ceiling3_yglc_network_monitor_inplace()` в [wifi_recovery_guard.c](../firmware/src/wifi_recovery_guard.c#L122-L167), SDK-заголовок [ceiling3_sdk.h](../firmware/include/ceiling3_sdk.h), линкер-скрипт [ceiling3_inplace_monitor.ld](../firmware/ld/ceiling3_inplace_monitor.ld), сборка `-Oz` через [build_fw.py](../tools/build_fw.py) -> yglc_network_monitor_inplace.bin (**212 / 224 байт**).
- **Zero-BSS и память (`EXT_SRAM` vs `sysram_bss`)**: в [ceiling3_ext.ld](../firmware/ld/ceiling3_ext.ld) секция `EXT_SRAM` (`0x20039800..0x2003AA00`) пересекается с `sysram_bss` (`0x20001548..0x2003AA44`), а `heap_4` `ucHeap` занимает `0x20008f98..0x20038798`. Поэтому `ceiling3_inplace_monitor.ld` размещает код непосредственно в 224-байтном слоте `0x100bd918..0x100bd9f8` без `.data`/`.bss` и использует существующий 8-байтный слот `0x20008030..0x20008037` (`[0] = dead_since`, `[1] = last_probe`).
- **Логика ARP-зонда и перезагрузки**:
  - `MIIO_NET_LOCAL (3)`: через `FUN_10083558(2)` (`st2`) берёт IPv4 шлюза (`nif + 0x0c`), под `lock_tcpip_core` (`FUN_10083808` / `FUN_1008381c` @ `0x20002c40`) проверяет 10 слотов ARP-таблицы (`0x2000264c`, шаг `0x1c`, `state >= 2`, `ipaddr == gw`, окно свежести `ts`: `(now_s - e->ts) < 90`), и раз в `60 000 ms` отправляет `etharp_request` (`FUN_1007d658`). При отсутствии свежего ARP (`!alive`) ведёт `dead_since` и через `> 300 000 ms` вызывает `miio_reboot(..., "cmd.reboot")`.
  - `MIIO_NET_DISCONNECTED (2)`: ведёт `dead_since` и через `> 300 000 ms` вызывает `miio_reboot(..., "cmd.reboot")` независимо от состояния света (`power`).
  - Прочие состояния (`UNPROV`/`UAP`/`CLOUD`/`UPDATING`): сбрасывает оба слова `st[0] = 0`, `st[1] = 0`.
- Покрыто хостовым unit-тестом `test_inplace_monitor_local_arp_and_disconnected()` в [test_ceiling3_firmware.c](../firmware/tests/test_ceiling3_firmware.c#L182-L260).

#### Патчи #2–#8 (Thumb-2 бинарные патчи поверх сегмента 0)

| # | Адрес | Функция | Стоковые байты | Патч / Thumb-2 | Назначение |
|---|---|---|---|---|---|
| #1v2 | `0x100bd918..0x100bd9f8` | `yglc_network_monitor` | 224 Б (слот `0x100bd942: efd1`) | `yglc_network_monitor_inplace.bin` (212/224 Б) | Zero-BSS in-place монитор: 60s `etharp_request` + 90s `ts` окно в `LOCAL`, 300s `cmd.reboot` в `LOCAL`/`DISCONNECTED` |
| #2 | `0x100af298` | `miio_wifi_try_reconnect` | `0bd9` (`bls #0x100af2b2`) | `0be0` (`b #0x100af2b2`) | Безусловный вызов `miio_wifi_stop()` + `lwip_net_stop()` при переподключении, даже если найдено `>= 2` AP |
| #3 | `0x100f831c` | `do_ota` | `13d4` (`bmi #0x100f8346`) | `13e0` (`b #0x100f8346`) | Обход проверки канала доставки RPC (`*(int *)(param_1 + 8) << 0x1e`, `-30020`), чтобы локальный `miIO.ota` работал напрямую |
| #4 | `0x100af66e` / `0x100af670` / `0x100af700` | `wifi_cb_event_handler` (case `8` `CONNECT_FAILED`) | `2748` / `f8f746fd9df8261000919df82710` / `48f0d6f8` | `1de0` / `48f01ef90520fff733fe00f043b8` / `fff7b6bf` | Пропускает средний dead-log блок внутри case `8` (`0x100af66e: b #0x100af6ac`, не case `13`) и постит `MIIO_WIFI_EVENT_STA_DISCONNECTED (5)` в `miio_net_post_event(5)` при ошибке ассоциации/PSK |
| #5 | `0x100b04a4` / `0x100b0c80` / `0x100b0cae` | `sta_offline_timeout_s` (`miio_net`) | `4ff46172` / `4ff46173` / `4ff46172` (`900 s`) | `4ff0b402` / `4ff0b403` / `4ff0b402` (`180 s`) | Уменьшает таймаут офлайна STA с `900 s` до `180 s` (`0xb4`) |
| #5b | `0x100b0cbe` | `miio_wifi_scan_finish` (`sta_monitor_offline_countout`) | `53431a4ab3fbf2f3` (`* 1000 / 90000`) | `1622b3fbf2f30233` (`movs r2, #0x16; udiv r3, r3, r2; adds r3, #2`) | Пересчитывает порог `param_1[0x69] = (param_1[0x5c] / 22) + 2` (`10` для 1 AP, `6` для 2 AP, `4` для 3 AP, всегда `>= 2`), устраняя `param_1[0x69] == 0` и бесконечный цикл реконнекта при `ap_count >= 3` |
| #6 | `0x100e88e8` | `wifi_config_reload_setting` | `2868b8b9` | `12e000bf` (`b #0x100e8910; nop`) | Устраняет утечку RTOS-семафора в `heap_4` на ветке ошибки |
| #7 | `0x100e6bf4` | `wifi_config_set_bssid` | `144a` | `06e0` (`b #0x100e6c04`) | Пропускает битый `%s`-лог сырого 6-байтного MAC-адреса |
| #8 | `0x100ce49e` | `wpa_driver_inband_set_ps_mode` | `93f8ff40` | `002400bf` (`movs r4, #0; nop`) | Принудительно задаёт `PSMode = 0` (отключает power save) |

#### Собранный патченый OTA-образ (`python patch_ota.py`)
- Файл: yeelink.light.ceiling3-patched-2.0.2_0048.bin
- Размер: `727 130` байт (`packed` segment 0: `509 440` байт, `nice_len=35`)
- MD5: `db3ce52b85a80e27225fcca2b302abc9`
- CRC32 (`arch_ota_check_crc`): `0xa6095d26`
- Изменено байт в segment 0 относительно стока: `258`
- Контрольная пересборка стока (`python patch_ota.py --stock`): yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin (`727 122` байт, MD5 `0a4389bbd63496f582ebf33558212ac6`, `0` изменённых байт в segment 0)

---

## Дополнительные найденные дефекты Wi-Fi (`miio_net` + `wpa_supplicant` + `lwIP`)

Полная чистая C11-реконструкция вынесена в [miio_wifi_clean.h](../firmware/include/miio_wifi_clean.h) и [miio_wifi_clean.c](../firmware/clean_src/miio_wifi_clean.c):
1. **`wifi_cb_event_handler` (`0x100af370`, case `8` = `WIFI_EVT_IOT_CONNECT_FAILED`)**: переводит состояние в `MIIO_NET_DISCONNECTED`, сбрасывает режим и возвращает `0`, **не отправляя** `MIIO_WIFI_EVENT_STA_DISCONNECTED (5)` в `monitor_queue`, из-за чего восстановление после ошибки ассоциации/PSK зависает до срабатывания пассивного таймера (`900 с` для 1 AP).
2. **`miio_wifi_connect` (`0x100af028`)**: жёстко фиксирует и BSSID, и канал перед `wifi_config_reload_setting()` (`0x100e881c`), ломая быстрый роуминг при смене канала роутером (Auto/DFS) или mesh-переключении.
3. **`wifi_cb_event_handler` (case `2` = `DISCONNECTED`)**: немедленно обнуляет IPv4 на `st2`, помечает текущий AP `active = 0` и удаляет IP до попытки быстрого переподключения.
4. **`wifi_config_reload_setting` (`0x100e881c`, `wpa_supplicant_wifi.c`)**: при ошибке inband-команды `0x66` перепрыгивает к выходу мимо `FUN_1009f20c` (`vSemaphoreDelete`), вызывая утечку RTOS-семафора в `heap_4`.
5. **`wifi_config_set_bssid` (`0x100e6bc0`)**: передаёт сырой 6-байтный MAC-адрес как `%s` в `log_printf`.
