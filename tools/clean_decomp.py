#!/usr/bin/env python3
"""Post-process raw Ghidra decompilation into a clean, modular C tree under decomp/."""

import csv
import os
import re
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_PATH = os.path.join(ROOT, "extracted", "segment-0-flash-00071000.bin")
FLASH_BASE = 0x10071000

RAM_GLOBALS = {
    0x20004a2c: "g_log_level",
    0x20004b24: "g_miio_net",
    0x20006908: "g_yglc_settings_ptr",
    0x20008030: "g_offline_since_ms",
    0x2000805c: "g_yl_cache_list",
    0x2000806c: "g_yl_cache_schema_crc",
    0x20008074: "g_warm_boot_marker",
    0x20008084: "g_warm_boot_type",
    0x83080030: "WDT_SWRST",
}

# Verified contiguous linker boundaries inside segment-0-flash-00071000.bin
LINKER_RANGES = [
    (0x10071000, 0x10074183, "hal/mt7687_hal.c"),
    (0x10074184, 0x10075397, "wifi/smt_smartconfig.c"),
    (0x10075398, 0x10083c53, "lwip/lwip_core.c"),
    (0x10083c54, 0x10088fff, "lwip/net_apps.c"),
    (0x10089000, 0x1009bb2f, "crypto/mbedtls.c"),
    (0x1009bb30, 0x100a726b, "wifi/wifi_api_service.c"),
    (0x100a726c, 0x100ab8ff, "hal/arch_platform.c"),
    (0x100ab900, 0x100b21ff, "miio/miio_net_ota.c"),
    (0x100b2200, 0x100ba027, "ble/mible_host.c"),
    (0x100ba028, 0x100c296b, "app/yglc_core.c"),
    (0x100c296c, 0x100c3647, "app/yl_cache.c"),
    (0x100c3648, 0x100c5283, "app/lan_ctrl_cli.c"),
    (0x100c5284, 0x100c6fff, "hal/nvdm_storage.c"),
    (0x100c7000, 0x100ca52f, "rtos/freertos_kernel.c"),
    (0x100ca530, 0x100ee68f, "wifi/wpa_supplicant_wifi.c"),
    (0x100ee690, 0x100f6fff, "ble/mtk_bt_stack.c"),
    (0x100f7000, 0x10105fff, "miio/miio_core.c"),
    (0x10106000, 0x1010b9c6, "libc/newlib_libgcc.c"),
    (0x1010b9c7, 0x1014da2b, "misc/rodata_thunks.c"),
    (0x1014da2c, 0x1015064c, "hal/tcm_flash_sfc.c"),
]

# Map Ghidra-discovered module tags/source basenames -> clean subsystem folders
MOD_SUBDIR_EXACT = {
    "yglc": "app",
    "yglc_miIO": "app",
    "yl_cache": "app",
    "lan_ctrl": "app",
    "arch_chip": "hal",
    "arch_crash": "hal",
    "arch_flash": "hal",
    "arch_os": "hal",
    "arch_ota": "hal",
    "arch_psm": "hal",
    "dma": "hal",
    "gpt": "hal",
    "flash_sfc": "hal",
    "sfc": "hal",
    "spi_flash": "hal",
    "hal_cache": "hal",
    "nvdm_port": "hal",
    "nvdm_data": "hal",
    "nvdm_main": "hal",
    "tasks": "rtos",
    "queue": "rtos",
    "timers": "rtos",
    "heap_4": "rtos",
    "port": "rtos",
    "syslog": "rtos",
    "os_port_callback": "rtos",
    "bt_os_layer_api": "rtos",
    "dns": "lwip",
    "mem": "lwip",
    "memp": "lwip",
    "netif": "lwip",
    "pbuf": "lwip",
    "raw": "lwip",
    "tcp_in": "lwip",
    "tcp_out": "lwip",
    "timeouts": "lwip",
    "dhcp": "lwip",
    "etharp": "lwip",
    "icmp": "lwip",
    "igmp": "lwip",
    "ip4": "lwip",
    "ip4_addr": "lwip",
    "ip4_frag": "lwip",
    "api_lib": "lwip",
    "api_msg": "lwip",
    "netbuf": "lwip",
    "sockets": "lwip",
    "tcpip": "lwip",
    "ethernet": "lwip",
    "ethernetif": "lwip",
    "sys_arch": "lwip",
    "ping": "lwip",
    "smt_api": "wifi",
    "wifi_os_api": "wifi",
    "net_task": "wifi",
    "inband_queue": "wifi",
    "wifi_event": "wifi",
    "bt_callback_manager": "ble",
    "bt_gatts": "ble",
    "bt_gap_le": "ble",
    "bt_gap_le_adv": "ble",
    "bt_smp": "ble",
    "bt_memory": "ble",
    "bt_uuid": "ble",
    "bt_hci": "ble",
    "bt_l2cap_le": "ble",
    "bt_gap_init": "ble",
    "bt_sm": "ble",
    "bt_driver": "ble",
    "miio_instance": "miio",
    "miio_ciphers": "miio",
    "mcmd": "miio",
    "miio_net": "miio",
    "miio_info": "miio",
    "miio_hooks": "miio",
    "miio": "miio",
    "miio_ota": "miio",
    "miio_ot": "miio",
    "ota_app": "miio",
    "jsmi": "miio",
    "xmodem": "miio",
    "httpdns": "miio",
    "httpc": "miio",
    "dlog": "miio",
    "ot": "miio",
    "ots": "miio",
    "otu": "miio",
    "udp": "miio",
    "tcp": "miio",
    "tls": "miio",
    "iperf_client": "miio",
    "hello": "miio",
    "Addon": "miio",
    "D0": "miio",
}


def c_escape(s: str) -> str:
    out = ['"']
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) == 0x1B:
            out.append("\\x1b")
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(f"\\x{ord(ch):02x}")
    out.append('"')
    return "".join(out)


class BinaryOracle:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()
        self.end = FLASH_BASE + len(self.data)

    def u32(self, va: int):
        off = va - FLASH_BASE
        if 0 <= off <= len(self.data) - 4:
            return struct.unpack_from("<I", self.data, off)[0]
        return None

    def cstr(self, va: int, max_len: int = 240):
        off = va - FLASH_BASE
        if not (0 <= off < len(self.data)):
            return None
        end = self.data.find(b"\x00", off, min(len(self.data), off + max_len))
        if end <= off:
            return None
        raw = self.data[off:end]
        for b in raw:
            if b not in (9, 10, 13, 0x1B) and not (32 <= b < 127):
                return None
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError:
            return None


def target_file(va: int, module: str) -> str:
    if module:
        # Handle "tcp" ambiguity (lwIP core vs miio helper) by VA
        if module == "tcp" and va < 0x10085000:
            return "lwip/tcp.c"
        if module in MOD_SUBDIR_EXACT:
            return f"{MOD_SUBDIR_EXACT[module]}/{module}.c"
        if module.startswith("mible"):
            return f"ble/{module}.c"
        if module.startswith(("yglc", "yl_", "lan_")):
            return f"app/{module}.c"
        if module.startswith(("miio", "ot")):
            return f"miio/{module}.c"
        if module.startswith(("hal_", "arch_")):
            return f"hal/{module}.c"
        if module.startswith("bt_"):
            return f"ble/{module}.c"
        if module.startswith("wifi_"):
            return f"wifi/{module}.c"
    for lo, hi, path in LINKER_RANGES:
        if lo <= va <= hi:
            return path
    return "misc/external_refs.c"


TYPE_RE = re.compile(
    r"\b(undefined8|undefined7|undefined6|undefined5|undefined4|undefined3|undefined2|undefined1|undefined|ulonglong|longlong|uint7|uint6|uint5|uint3|int7|int6|int5|int3|ushort|uint|byte)\b"
)
TYPE_MAP = {
    "undefined8": "uint64_t",
    "undefined7": "uint64_t",
    "undefined6": "uint64_t",
    "undefined5": "uint64_t",
    "undefined4": "uint32_t",
    "undefined3": "uint32_t",
    "undefined2": "uint16_t",
    "undefined1": "uint8_t",
    "undefined": "uint8_t",
    "ulonglong": "uint64_t",
    "longlong": "int64_t",
    "uint7": "uint64_t",
    "uint6": "uint64_t",
    "uint5": "uint64_t",
    "uint3": "uint32_t",
    "int7": "int64_t",
    "int6": "int64_t",
    "int5": "int64_t",
    "int3": "int32_t",
    "ushort": "uint16_t",
    "uint": "uint32_t",
    "byte": "uint8_t",
}

PTR_STR_RE = re.compile(r'(?<![&*])\bPTR_s_[^\s(),;\[\]"]+?([0-9a-f]{8})\b')
PTR_DAT_RE = re.compile(r"(?<![&*])\bPTR_DAT_([0-9a-f]{8})\b")
PTR_LBL_RE = re.compile(r"(?<![&*])\bPTR_(g_[A-Za-z0-9_]+|WDT_SWRST)_[0-9a-f]{8}\b")
PTR_FUN_RE = re.compile(r"\bPTR_(?:FUN|LAB)_([0-9a-f]{8})(?:_[0-9a-f]+)+\b")
SLICE_LVAL3_RE = re.compile(r"^([ \t]*)([A-Za-z_][A-Za-z0-9_]*)\._(\d+)_3_\s*=\s*(.+?);$", re.M)
SLICE_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\._(\d+)_([1234])_")
LOCAL_ALIAS_RE = re.compile(r"\b_local_([0-9a-f]+)\b")


def clean_body(body: str, oracle: BinaryOracle, func_by_va: dict) -> str:
    body = TYPE_RE.sub(lambda m: TYPE_MAP[m.group(1)], body)

    def sub_ptr_str(m):
        pool_va = int(m.group(1), 16)
        tgt = oracle.u32(pool_va)
        if tgt is not None:
            s = oracle.cstr(tgt)
            if s is not None:
                return c_escape(s)
        return m.group(0)

    def sub_ptr_dat(m):
        pool_va = int(m.group(1), 16)
        tgt = oracle.u32(pool_va)
        if tgt is None:
            return m.group(0)
        if tgt in RAM_GLOBALS:
            return f"(&{RAM_GLOBALS[tgt]})"
        s = oracle.cstr(tgt)
        if s is not None:
            return c_escape(s)
        return m.group(0)

    def sub_ptr_fun(m):
        tgt = int(m.group(1), 16)
        # Thumb bit 0 might be set in literal pool, mask off bit 0
        name = func_by_va.get(tgt) or func_by_va.get(tgt & ~1)
        if name and not name.startswith(("FUN_", "thunk_FUN_")):
            return name
        return m.group(0)

    def sub_slice(m):
        var, off, size = m.group(1), int(m.group(2)), int(m.group(3))
        if size == 1:
            return f"(*((uint8_t *)&({var}) + {off}))"
        if size == 2:
            return f"(*(uint16_t *)((uint8_t *)&({var}) + {off}))"
        if size == 4:
            return f"(*(uint32_t *)((uint8_t *)&({var}) + {off}))"
        return f"((*(uint32_t *)((uint8_t *)&({var}) + {off})) & 0xffffffu)"

    body = PTR_STR_RE.sub(sub_ptr_str, body)
    body = PTR_DAT_RE.sub(sub_ptr_dat, body)
    body = PTR_LBL_RE.sub(r"(&\1)", body)
    body = PTR_FUN_RE.sub(sub_ptr_fun, body)
    body = SLICE_LVAL3_RE.sub(
        r"\1do { uint32_t _v3 = (uint32_t)(\4); memcpy((uint8_t *)&(\2) + \3, &_v3, 3); } while (0);",
        body,
    )
    body = SLICE_RE.sub(sub_slice, body)
    body = LOCAL_ALIAS_RE.sub(r"(*(uint32_t *)&local_\1)", body)
    return body


def parse_stage_functions(stage_dir: str):
    csv_path = os.path.join(stage_dir, "functions.csv")
    with open(csv_path, encoding="utf-8") as f:
        rows = list(csv.DictReader(f))

    func_by_va = {}
    file_cache = {}
    bodies = {}  # entry_hex -> body text

    for r in rows:
        va = int(r["entry"], 16)
        func_by_va[va] = r["name"]

    rel_files = {r["file"] for r in rows}
    hdr_re = re.compile(r"^// ---- (\S+) @ ([0-9a-f]+) \(size (\d+)\) ----\s*$")
    for rel in sorted(rel_files):
        fp = os.path.join(stage_dir, rel)
        with open(fp, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines(keepends=True)
        cur_va = None
        cur_buf = []
        for line in lines:
            m = hdr_re.match(line)
            if m:
                if cur_va is not None:
                    bodies[cur_va] = "".join(cur_buf)
                cur_va = m.group(2).lower().lstrip("0") or "0"
                # Normalize 8-hex key using exact CSV entry formatting
                cur_va = f"{int(m.group(2), 16):08x}"
                cur_buf = [line]
            elif cur_va is not None:
                cur_buf.append(line)
        if cur_va is not None:
            bodies[cur_va] = "".join(cur_buf)

    return rows, func_by_va, bodies


def main():
    stage_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "decomp_stage")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "decomp")

    oracle = BinaryOracle(BIN_PATH)
    rows, func_by_va, bodies = parse_stage_functions(stage_dir)

    # Preserve non-generated documentation inside decomp/ (e.g. WIFI_RECOVERY.md) and include/
    tmp_out = os.path.join(ROOT, "decomp_clean_tmp")
    if os.path.exists(tmp_out):
        shutil.rmtree(tmp_out)
    os.makedirs(tmp_out, exist_ok=True)

    # Copy existing include/ and WIFI_RECOVERY.md if present
    inc_src = os.path.join(out_dir, "include")
    if os.path.isdir(inc_src):
        shutil.copytree(inc_src, os.path.join(tmp_out, "include"))
    wifi_md = os.path.join(out_dir, "WIFI_RECOVERY.md")
    if os.path.isfile(wifi_md):
        shutil.copy2(wifi_md, os.path.join(tmp_out, "WIFI_RECOVERY.md"))

    grouped = {}  # target rel file -> list of (row, cleaned_body)
    new_csv = []
    missing = 0
    for r in rows:
        va = int(r["entry"], 16)
        va_key = f"{va:08x}"
        raw_body = bodies.get(va_key)
        if raw_body is None:
            missing += 1
            continue
        dst = target_file(va, r["module"])
        cleaned = clean_body(raw_body, oracle, func_by_va)
        grouped.setdefault(dst, []).append(cleaned)
        new_csv.append({
            "entry": r["entry"],
            "name": r["name"],
            "module": r["module"],
            "size": r["size"],
            "file": dst,
        })

    if missing:
        raise RuntimeError(f"Missing {missing} function bodies while reorganizing")

    for dst, chunks in sorted(grouped.items()):
        fp = os.path.join(tmp_out, dst)
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "w", encoding="utf-8", newline="\n") as f:
            f.write(
                "// Clean Ghidra 12.2 decompilation of yeelink.light.ceiling3 2.0.2_0048 (segment 0).\n"
                "// Reconstructed types/names/strings from binary oracle; not original vendor source.\n"
                '#include "../include/fw_globals.h"\n\n'
            )
            for chunk in chunks:
                f.write(chunk.rstrip() + "\n\n")

    with open(os.path.join(tmp_out, "functions.csv"), "w", encoding="utf-8", newline="\n") as f:
        w = csv.DictWriter(f, fieldnames=["entry", "name", "module", "size", "file"])
        w.writeheader()
        w.writerows(new_csv)

    # Atomic swap into decomp/
    backup_out = os.path.join(ROOT, "decomp_old_backup")
    if os.path.exists(backup_out):
        shutil.rmtree(backup_out)
    if os.path.exists(out_dir):
        os.rename(out_dir, backup_out)
    os.rename(tmp_out, out_dir)
    if os.path.exists(backup_out):
        shutil.rmtree(backup_out)

    named = sum(1 for r in new_csv if not r["name"].startswith(("FUN_", "thunk_FUN_")))
    print(f"clean_decomp: {len(new_csv)} functions ({named} named) -> {len(grouped)} files in {out_dir}")


if __name__ == "__main__":
    main()
