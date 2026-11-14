# Руководство по расширению и модификации

Документ описывает безопасный рабочий процесс добавления новых бинарных патчей прошивки в [patch_ota.py](../../patch_ota.py), расширения зондов в [monitor_lamp.py](../../monitor_lamp.py) и повторного экспорта декомпиляции через [YlExport.java](../../tools/ghidra/YlExport.java).

---

## 1. Добавление нового бинарного патча в `patch_ota.py`

### 1.1. Исследование и выбор адреса

1. Найдите целевую функцию в [decomp\functions.csv](../../decomp/functions.csv) или в модулях подсистем `decomp\<subsystem>\*.c` (`app/`, `miio/`, `hal/`, `rtos/`, `lwip/`, `wifi/`, `ble/`, `crypto/`, `libc/`).
2. Сверьте машинный код и точные адреса инструкций в проекте Ghidra (`ghidra-proj\ceiling3.gpr`), так как псевдо-C не всегда отражает границы отдельных 16/32-битных Thumb-2 инструкций.
3. Убедитесь, что адрес лежит в пределах образа `segment-0-flash-00071000.bin` (`915 020` байт, диапазон адресов `0x10071000 .. 0x1015064B`).
   - Если код модифицируемой функции копируется при старте в TCM (`0x00100000..0x00102C17`), источник во flash находится по смещению `0x1014DA2C + (tcm_addr - 0x00100000)` (см. [YlSetup.java](../../tools/ghidra/YlSetup.java#L44)).

### 1.2. Особенности ARM Thumb-2: endianness, ширина и выравнивание

При составлении патча для Cortex-M4 (`ARM:LE:32:Cortex`) учитывайте следующие ограничения:
- **Базовый адрес и смещение в файле**: `BASE = 0x10071000`. Смещение в распакованном `segment 0` всегда вычисляется как `o = addr - BASE` ([patch_ota.py](../../patch_ota.py#L157)).
- **Сброс бита Thumb (`LSB`)**: в указателях кода и в `disassembly-start.txt` младший бит установлен в `1` (например, `0x100BAC69`), но адрес самой инструкции в памяти **всегда чётный** (`addr & ~1`, например `0x100bd942`).
- **Порядок байт (Little-Endian halfwords)**:
  - 16-битная Thumb-инструкция хранится как 2 байта в little-endian: например, `NOP` (`0xBF00`) записывается в файл как `00 bf` (`bytes.fromhex('00bf')`); `BNE` (`0xD1EF`) — как `ef d1`.
  - 32-битная Thumb-2 инструкция (например, `BL`, `B.W`, `LDR.W`) состоит из **двух 16-битных полуслов**, каждое из которых хранится в little-endian в порядке `[hw1_lo, hw1_hi, hw2_lo, hw2_hi]`. Не перепутайте это с 32-битным little-endian словом!
- **Запрет сдвига инструкций**: заменяемая последовательность `new` должна иметь **ровно ту же длину**, что и `old` (in-place замена). Вставка или удаление байт сдвинет PC-relative смещения (`ldr [pc, #imm]`, ветвления и таблицы переходов).
- **Условные/безусловные переходы (`B<cc>` -> `B` / `NOP`)**: проверьте по таблице кодирования Thumb, что целевое смещение помещается в диапазон новой инструкции, и не перезаписывайте инструкции, на которые завязаны флаги состояния (`CPSR` `N/Z/C/V`) для последующего кода (если только это не преднамеренно).
- **Ограничение раздела**: итоговый сжатый размер сегмента 0 не должен превышать `partition_length` (`1 245 184` байт в `structure.json`, проверка `assert len(packed) <= ent[3]` в `patch_ota.py`), а размер упакованного сегмента 0 подбирается `lzma_alone_no_eos()` кратным 8 байтам.

### 1.3. Добавление записи в `PATCHES`

В [patch_ota.py](../../patch_ota.py#L32-L39) список `PATCHES` состоит из кортежей `(addr, old, new, why)`:

```python
PATCHES = [  # (address, expected stock bytes, new bytes, why)
    (0x100bd942, bytes.fromhex('efd1'), bytes.fromhex('00bf'),
     'bne -> nop (yglc_network_monitor): reboot after 5 min without IP regardless of light power state'),
    ...
]
```

Правила добавления:
1. `addr` — чётный виртуальный адрес в `flash_app` (`0x10071000 + offset`).
2. `old` — точные ожидаемые стоковые байты; `patch_ota.py` перед записью проверяет `assert raw[o:o + len(old)] == old`, что защищает от патча не по тому смещению.
3. `new` — новые байты той же длины (`len(new) == len(old)`).
4. `why` — краткое инженерное описание (что изменено и зачем).

### 1.4. Сборка и верификация диффа

1. Убедитесь, что `liblzma-5.dll` версии `>= 5.4` доступна по пути по умолчанию (`C:\Program Files\Git\mingw64\bin\liblzma-5.dll`) или через переменную окружения `LIBLZMA`. Опционально установите `capstone` для вывода дизассемблинга патча в консоль.
2. Сначала прогоните сборку без патчей (self-test конвейера упаковки):
   ```powershell
   python patch_ota.py --stock
   ```
   Скрипт проверит MD5 стокового образа (`13b7471a0f84e3cfbac78c116fae7a1c`), соберёт `yeelink.light.ceiling3-repacked-stock-2.0.2_0048.bin` и убедится, что `segment-0 bytes changed vs stock: 0`.
3. Соберите патченый образ:
   ```powershell
   python patch_ota.py
   ```
   Проверьте в выводе строки вида `patched 0x...: <old> (...) -> <new> (...)`, итоговое число изменённых байт (`segment-0 bytes changed vs stock: N`, где `N` равно сумме реально изменившихся байт в патчах) и `all container checks OK`.
4. Сверьте побайтовый diff между распакованным стоковым сегментом 0 (`extracted\segment-0-flash-00071000.bin`) и сегментом 0 внутри собранного `yeelink.light.ceiling3-patched-2.0.2_0048.bin`:
   ```powershell
   python -c "import patch_ota; s=open('extracted/segment-0-flash-00071000.bin','rb').read(); p=patch_ota.parse(open('yeelink.light.ceiling3-patched-2.0.2_0048.bin','rb').read())[0][2]; diffs=[(hex(patch_ota.BASE+i), hex(a), hex(b)) for i,(a,b) in enumerate(zip(s,p)) if a!=b]; print('count:', len(diffs)); print('\n'.join(str(x) for x in diffs))"
   ```
   Убедитесь, что изменились **только** ожидаемые адреса `PATCHES`, а сегменты `1` и `2` остались побайтово идентичны стоку (это также проверяет сам `patch_ota.py`).

---

## 2. Расширение `monitor_lamp.py`

[monitor_lamp.py](../../monitor_lamp.py) опрашивает лампу с интервалом `INTERVAL` (по умолчанию `30` с) и пишет в `monitor_lamp.log` только при смене вердикта `verdict(st)` и раз в 30 минут (`heartbeat`).

### Архитектура зондов

В функции `probe()` ([monitor_lamp.py](../../monitor_lamp.py#L74-L79)) выполняются 4 read-only проверки:
- `ping()` — до 2 попыток `ping -n 1 -w 1500 <IP>`, проверяет `'TTL='` в `stdout`.
- `miio()` — отправляет UDP-пакет `hello` (`21310020` + `'ff' * 28`) на порт `54321`, ждёт 32-байтный ответ с заголовком `b'\x21\x31'`.
- `lan()` — TCP-подключение на порт `55443`, команда `{"id":1,"method":"get_prop","params":["power","bright"]}\r\n`, возвращает `(ok, props)`.
- `arp()` — вызывается **последним**, чтобы ОС успела обновить neighbor cache после `ping`; парсит `netsh interface ipv4 show neighbors` и проверяет статус `Reachable`.

### Как добавить новый зонд или состояние

1. Реализуйте функцию зонда (например, `http_check()` или чтение доп. свойств в `lan()` через список `params`) с коротким таймаутом (`2–3` с) и перехватом `OSError` / `ValueError`, чтобы скрипт не падал при зависании лампы.
2. Добавьте ключ в словарь, возвращаемый `probe()`.
3. Обновите `verdict(st)` ([monitor_lamp.py](../../monitor_lamp.py#L82-L91)), соблюдая приоритет от полного отказа (`not any(st.values())`) к частичным (`ZOMBIE`, `PARTIAL`).
4. Проверьте работу без запуска цикла флагом `--once`:
   ```powershell
   python monitor_lamp.py --once
   ```

> [!NOTE]
> Избегайте вызовов, меняющих состояние лампы (`set_power`, `toggle`), внутри `monitor_lamp.py` — скрипт предназначен исключительно для пассивного телеметрического наблюдения.

---

## 3. Повторный экспорт декомпиляции Ghidra (`YlEnrich.java` + `YlExport.java` + `clean_decomp.py`)

Если в проекте `ghidra-proj\ceiling3.gpr` были вручную переименованы функции, добавлены верифицированные символы в [YlEnrich.java](../../tools/ghidra/YlEnrich.java) или скорректированы структуры в fw_types.h, обновите каталог `decomp\` с помощью конвейера экспорта и [clean_decomp.py](../../tools/clean_decomp.py).

### Как работает конвейер экспорта

- [YlEnrich.java](../../tools/ghidra/YlEnrich.java) восстанавливает верифицированные имена RTOS/HAL/libc/`arch_*`/`jsmi_*`/`yglc_*`, размечает глобальные переменные и транки (`thunk_*`).
- [YlExport.java](../../tools/ghidra/YlExport.java) сканирует вызовы с форматными строками `(?s)...(%s,%d)`, `func:%s` и assert `__FILE__`, заменяет **только** имена `FUN_*` и экспортирует сырой результат во временный каталог.
- [clean_decomp.py](../../tools/clean_decomp.py) разрешает указатели `PTR_s_*`/`PTR_DAT_*` через бинарный образ `segment-0-flash-00071000.bin`, заменяет типы на `<stdint.h>`, подключает `fw_globals.h`, раскладывает 4 210 функций по подсистемам в `decomp/` и пересобирает `decomp/functions.csv`.

### Команда headless-экспорта

Для обновления `decomp\` на базе уже проанализированного проекта `ghidra-proj\ceiling3` (требуется JDK 25+):

```powershell
$env:JAVA_HOME = '<JDK_HOME>'
& "<GHIDRA_HOME>\support\analyzeHeadless.bat" `
  "ghidra-proj" "ceiling3" `
  -process "segment-0-flash-00071000.bin" `
  -noanalysis `
  -scriptPath "tools\ghidra" `
  -postScript YlEnrich.java `
  -postScript YlExport.java "decomp_stage"
python tools/clean_decomp.py "decomp_stage" "decomp"
Remove-Item -Recurse -Force "decomp_stage"
```

Если нужно пересоздать проект с нуля из `extracted\segment-0-flash-00071000.bin` (как в analyze.log):
- Используйте `-import "extracted\segment-0-flash-00071000.bin"`, `-loader BinaryLoader -loader-baseAddr 0x10071000`, `-processor ARM:LE:32:Cortex -cspec default`, `-preScript YlSetup.java`, `-postScript YlStrings.java`, `-postScript YlEnrich.java`, `-postScript YlExport.java "decomp_stage"`, затем `python tools/clean_decomp.py`.
- После экспорта убедитесь по логу, что `failed 0`, и при изменении структуры/символов синхронизируйте [WIFI_RECOVERY.md](../../decomp/WIFI_RECOVERY.md) и документы в `docs\`.

---

## 4. Разработка и компиляция модулей C11 под Cortex-M4F (`firmware/`)

Новые C-модули прошивки размещаются в `firmware/src/` и используют верифицированный ABI из [ceiling3_sdk.h](../../firmware/include/ceiling3_sdk.h) (fw_types.h + fw_globals.h) с линковкой по Thumb-адресам стоковой прошивки через `firmware/ld/ceiling3_symbols.ld`.

1. Добавьте модуль в `firmware/src/*.c` и соответствующие unit-тесты с `-DHOST_TEST=1` в [test_ceiling3_firmware.c](../../firmware/tests/test_ceiling3_firmware.c).
2. Запустите единый сборочный скрипт [build_fw.py](../../tools/build_fw.py):
   ```powershell
   python tools/build_fw.py
   python tools/build_fw.py --with-decomp
   ```
   - `build_fw.py` перегенерирует таблицы символов ([prep_decomp_build.py](../../tools/prep_decomp_build.py)), собирает и запускает host-тесты через `gcc`, компилирует `firmware/src/*.c` через LLVM `clang` (`--target=thumbv7em-none-eabi -mcpu=cortex-m4 -mthumb -Os -ffreestanding -nostdlib`) и линкует `ld.lld` в ceiling3_ext.elf и ceiling3_ext.bin.
   - С флагом `--with-decomp` дополнительно выполняется объектная компиляция (`clang -DDECOMP_BUILD -c`) модулей `decomp/` в `firmware/build/decomp_obj/`.
