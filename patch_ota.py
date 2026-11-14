"""Build a patched ceiling3 OTA (MMM container) from the verified stock 2.0.2_0048.

Run: python patch_ota.py            -> writes yeelink.light.ceiling3-patched-2.0.2_0048.bin
     python patch_ota.py --stock    -> repacks WITHOUT patches (pipeline self-test image)
     python patch_ota.py --measure  -> patched, but #9b replaced by MEASURE_PATCHES (dl duty = 0.1*bright %)

Patches (ARM Thumb-2, segment 0 base 0x10071000):
  1v2. 0x100bd918..0x100bd9f8 (yglc_network_monitor, 224-byte slot): replaced in-place with
     firmware/build/yglc_network_monitor_inplace.bin (built by tools/build_fw.py).
     Removes light-off gate, fixes 5-min offline reboot, and adds staged Wi-Fi recovery.
  2. 0x100af298 (miio_wifi_try_reconnect): bls -> b. Stock firmware skips miio_wifi_stop() and
     lwip_net_stop() when >= 2 APs are seen in scan, leaving a stale association/IP state.
     Unconditional branch forces full Wi-Fi + lwIP teardown/restart on reconnect regardless of AP count.
  3. 0x100f831c (do_ota): bmi -> b. Stock firmware checks *(int *)(param_1 + 8) << 0x1e
     (bit 1 = cloud ots transport vs local UDP otu) and drops local miIO.ota with -30020
     ("ota is from local, droped."). Unconditional branch enables direct local miIO.ota.
  4. 0x100af66e, 0x100af670, 0x100af700 (wifi_cb_event_handler case 8 CONNECT_FAILED):
     detour via case 8 middle dead-log slot to call log + post MIIO_WIFI_EVENT_STA_DISCONNECTED (5)
     via bl #0x100f78b0; movs r0, #5; bl #0x100af2e0; b.w #0x100af704.
  5. 0x100b04a4, 0x100b0c80, 0x100b0cae (sta_offline_timeout_s): 900s -> 180s (mov.w r2/r3, #0xb4);
     0x100b0cbe (5b, sta_monitor_offline_countout): (param_1[0x5c] / 22) + 2 so threshold >= 2 for any ap_count.
  6. 0x100e88e8 (wifi_config_reload_setting): b #0x100e8910; nop — fix semaphore leak on error.
  7. 0x100e6bf4 (wifi_config_set_bssid): b #0x100e6c04 — skip broken %s raw binary MAC log.
  8. 0x100ce49e (wpa_driver_inband_set_ps_mode): movs r4, #0; nop — force PSMode = 0.

Container integrity the firmware checks (all recomputed here):
  header SHA-1 over bytes[0:136], SHA-1 of each packed segment, and a trailing CRC32
  (table 0xEDB88320, init 0, no final xor) over everything before it (arch_ota_check_crc).
Segments are LZMA-alone with known size and NO end marker (LZMA SDK style). Python's lzma
always writes an end marker, so segment 0 is re-encoded via liblzma's LZMA1EXT filter.
"""
import ctypes
import hashlib
import lzma
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, 'yeelink.light.ceiling3-stock-2.0.2_0048.bin')
MON_BIN = os.path.join(HERE, 'firmware', 'build', 'yglc_network_monitor_inplace.bin')
BASE = 0x10071000  # execution address of segment 0
MON_SLOT_ADDR = 0x100bd918
MON_SLOT_SIZE = 224  # [0x100bd918..0x100bd9f8)
MON_SLOT_SHA256 = '3b65c043d967a5f6213606d3163867e81af7990e7f358caa652cf7e98415e5c1'
PATCHES = [  # (address, expected stock bytes, new bytes, why)
    (0x100af298, bytes.fromhex('0bd9'), bytes.fromhex('0be0'),
     'bls -> b (miio_wifi_try_reconnect): always call miio_wifi_stop() + lwip_net_stop() even when >= 2 APs found'),
    (0x100f831c, bytes.fromhex('13d4'), bytes.fromhex('13e0'),
     'bmi -> b (do_ota): bypass local-vs-cloud RPC flag check (-30020) so local miIO.ota works directly'),
    (0x100af66e, bytes.fromhex('2748'), bytes.fromhex('1de0'),
     'wifi_cb_event_handler case 8 middle log skip -> b #0x100af6ac (reclaim dead-log slot for case 8 detour)'),
    (0x100af670, bytes.fromhex('f8f746fd9df8261000919df82710'), bytes.fromhex('48f01ef90520fff733fe00f043b8'),
     'wifi_cb_event_handler case 8 detour: bl #0x100f78b0; movs r0, #5; bl #0x100af2e0; b.w #0x100af704'),
    (0x100af700, bytes.fromhex('48f0d6f8'), bytes.fromhex('fff7b6bf'),
     'wifi_cb_event_handler case 8 CONNECT_FAILED -> b.w #0x100af670 to post MIIO_WIFI_EVENT_STA_DISCONNECTED (5)'),
    (0x100b04a4, bytes.fromhex('4ff46172'), bytes.fromhex('4ff0b402'),
     'sta_offline_timeout_s 900s -> 180s (mov.w r2, #0xb4)'),
    (0x100b0c80, bytes.fromhex('4ff46173'), bytes.fromhex('4ff0b403'),
     'sta_offline_timeout_s 900s -> 180s (mov.w r3, #0xb4)'),
    (0x100b0cae, bytes.fromhex('4ff46172'), bytes.fromhex('4ff0b402'),
     'sta_offline_timeout_s 900s -> 180s (mov.w r2, #0xb4)'),
    (0x100b0cbe, bytes.fromhex('53431a4ab3fbf2f3'), bytes.fromhex('1622b3fbf2f30233'),
     'sta_monitor_offline_countout threshold: (param_1[0x5c] / 22) + 2 so >= 2 for any ap_count (10 for 1 AP, 6 for 2 APs, 4 for 3 APs)'),
    (0x100e88e8, bytes.fromhex('2868b8b9'), bytes.fromhex('12e000bf'),
     'wifi_config_reload_setting: b #0x100e8910; nop (fix semaphore leak on error)'),
    (0x100e6bf4, bytes.fromhex('144a'), bytes.fromhex('06e0'),
     'wifi_config_set_bssid: b #0x100e6c04 (skip broken %s raw binary MAC log)'),
    (0x100ce49e, bytes.fromhex('93f8ff40'), bytes.fromhex('002400bf'),
     'wpa_driver_inband_set_ps_mode: movs r4, #0; nop (force PSMode = 0)'),
    (0x100ebffe, bytes.fromhex('642928bf6421'), bytes.fromhex('002100bf00bf'),
     'mixer set_min_bright_pct: movs r1, #0; nop; nop (remove dl 15% min_bright floor so bright=1% scales from 1%)'),
    (0x1014067c, bytes.fromhex('0000004100000041'), bytes.fromhex('0000d0400000d040'),
     'dl CT calib table +0x154/+0x158 (8.0f, 8.0f -> 6.5f, 6.5f): hardware-minimum per-channel PWM floor (260/4000, above ~5% driver cutoff)'),
]
# --measure: temporary build to find the LED driver's lowest stable PWM duty in daylight ("dl").
# Replaces #9b and scales the whole dl output by 0.1, so at 2700K / 6500K (single-channel table rows)
# bright N -> exactly 0.1*N % duty (4*N of 4000 counts); max brightness is 10%. Moonlight ("nl") untouched.
MEASURE_PATCHES = [
    (0x1014067c, bytes.fromhex('0000004100000041'), bytes.fromhex('0000000000000000'),
     'MEASURE dl CT table +0x154/+0x158 floor 8.0f -> 0.0f (FUN_100ec654 floor remap and CT cutoff off)'),
    (0x100ec93c, bytes.fromhex('0000c842'), bytes.fromhex('00007a44'),
     'MEASURE FUN_100ec71c share divisor and full-share value 100.0f -> 1000.0f (dl output x0.1)'),
]
# --hybrid (the image the lamp runs), on top of the Wi-Fi patches #2..#8 and #9a:
#   * daylight bright 1..K-1 on the moonlight LEDs, K..100 rescaled onto the main LEDs from the
#     driver's knee (firmware/src/hybrid_bright.c, in-place in two dead-code slots);
#   * network monitor v3 + "black box" event log (firmware/src/blackbox_monitor.c, appended
#     after the stock image; replaces the in-place monitor v2 of the plain "patched" image);
#   * immediate reboot after a fault + crash PC/LR recording.
# Everything is built by tools/build_fw.py.
HYBRID_SLOTS = [  # (address, size, stock sha256, built binary)
    (0x100ecac4, 212, '4197a009d20ac9461a820419d4524e180f722b794ebae41f8e9c7bf421bac3fe',
     os.path.join(HERE, 'firmware', 'build', 'hybrid_slot_a.bin')),
    (0x100ec204, 236, '2a7a4f91d3753d9457fc9ae2e80f5a54bdf99ef6a6cfef7a4f0826704e805297',
     os.path.join(HERE, 'firmware', 'build', 'hybrid_slot_b.bin')),
]
SEG0_END = 0x1015064c                  # end of the stock segment-0 image
HYBRID_BL_SITE = 0x100c09ce            # next_trans: the only call of start_transition 0x100c07d4
HYBRID_BL_OLD = bytes.fromhex('fff701ff')
HYBRID_PATCHES = [
    (0x1014067c, bytes.fromhex('0000004100000041'), bytes.fromhex('0000204100002041'),
     'HYBRID dl CT table +0x154/+0x158 floor 8.0f -> 10.0f (LED driver knee: dl bright K = minimum level)'),
    (0x100ca2c4, bytes.fromhex('efbeadde'), bytes.fromhex('aaaaadde'),
     'CRASH register-dump function 0x100c9f38 (called by every fault handler): reboot magic '
     '0xDEADBEEF -> 0xDEADAAAA (= the default flag @0x20000d1c), so the reboot happens right after '
     'the register dump instead of after the stock dump of ~320 KB of RAM to the UART (minutes)'),
]


HYBRID_ELF = os.path.join(HERE, 'firmware', 'build', 'hybrid_bright.elf')


def hybrid_symbol(name, elf=None):
    """Address of a function in a linked ELF (32-bit LE symbol table, Thumb bit cleared)."""
    elf = elf or HYBRID_ELF
    d = open(elf, 'rb').read()
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', d, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] == 2:  # SHT_SYMTAB
            strtab = secs[s[6]]
            for o in range(s[4], s[4] + s[5], 16):
                st_name, st_value = struct.unpack_from('<II', d, o)
                end = d.index(b'\0', strtab[4] + st_name)
                if d[strtab[4] + st_name:end].decode() == name:
                    return st_value & ~1
    raise SystemExit(f'symbol {name} not found in {elf}')


BB_ELF = os.path.join(HERE, 'firmware', 'build', 'bb_ext.elf')
BB_BIN = os.path.join(HERE, 'firmware', 'build', 'bb_ext.bin')
BB_EXT_ADDR = 0x10150650               # firmware/ld/ceiling3_ext_bb.ld
BB_EXT_MAX = 0x1000
MON_TRAMPOLINE_SITE = 0x100bd918       # yglc_network_monitor -> B.W bb_monitor_tick
MON_ENTRY_OLD = bytes.fromhex('73b5f2f7')  # stock: push {r0, r1, r4, r5, r6, lr}; bl ...
CRASH_BL_SITE = 0x100ca1da             # register-dump function: bl arch_crash_reboot
CRASH_BL_OLD = bytes.fromhex('ddf7e5fe')


def thumb_bw(src, dst):
    """Encode Thumb-2 B.W (T4) at src to dst."""
    off = dst - (src + 4)
    _require(-(1 << 24) <= off < (1 << 24) and off % 2 == 0, f'B.W out of range {src:#x}->{dst:#x}')
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    hi = 0xf000 | (s << 10) | ((off >> 12) & 0x3ff)
    lo = 0x9000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff)
    return struct.pack('<HH', hi, lo)


def thumb_bl(src, dst):
    """Encode Thumb-2 BL at src to dst."""
    off = dst - (src + 4)
    _require(-(1 << 24) <= off < (1 << 24) and off % 2 == 0, f'BL out of range {src:#x}->{dst:#x}')
    s = (off >> 24) & 1
    i1, i2 = (off >> 23) & 1, (off >> 22) & 1
    j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
    hi = 0xf000 | (s << 10) | ((off >> 12) & 0x3ff)
    lo = 0xd000 | (j1 << 13) | (j2 << 11) | ((off >> 1) & 0x7ff)
    return struct.pack('<HH', hi, lo)


LIBLZMA = os.environ.get('LIBLZMA', r'C:\Program Files\Git\mingw64\bin\liblzma-5.dll')  # Git for Windows ships 5.8


class LzmaOptions(ctypes.Structure):  # lzma_options_lzma, liblzma >= 5.4
    _fields_ = [('dict_size', ctypes.c_uint32), ('preset_dict', ctypes.c_void_p),
                ('preset_dict_size', ctypes.c_uint32), ('lc', ctypes.c_uint32),
                ('lp', ctypes.c_uint32), ('pb', ctypes.c_uint32), ('mode', ctypes.c_int),
                ('nice_len', ctypes.c_uint32), ('mf', ctypes.c_int), ('depth', ctypes.c_uint32),
                ('ext_flags', ctypes.c_uint32), ('ext_size_low', ctypes.c_uint32),
                ('ext_size_high', ctypes.c_uint32), ('reserved_int4', ctypes.c_uint32),
                ('reserved_int5', ctypes.c_uint32), ('reserved_int6', ctypes.c_uint32),
                ('reserved_int7', ctypes.c_uint32), ('reserved_int8', ctypes.c_uint32),
                ('reserved_enum1', ctypes.c_int), ('reserved_enum2', ctypes.c_int),
                ('reserved_enum3', ctypes.c_int), ('reserved_enum4', ctypes.c_int),
                ('reserved_ptr1', ctypes.c_void_p), ('reserved_ptr2', ctypes.c_void_p)]


class LzmaFilter(ctypes.Structure):
    _fields_ = [('id', ctypes.c_uint64), ('options', ctypes.c_void_p)]


def lzma_alone_no_eos(raw, props_byte, dict_size):
    """LZMA-alone stream with known size and no end marker, same lc/lp/pb/dict as stock.

    Fast mode + HC4 like the stock encoder: the stock streams contain no short-rep symbols,
    while liblzma's normal mode emits ~25k of them; we stay on symbol kinds the bootloader's
    decoder has provably handled. nice_len is chosen so the packed size is a multiple of 8,
    keeping later segment offsets 8-byte aligned as in stock.
    """
    lib = ctypes.CDLL(LIBLZMA)
    lib.lzma_version_string.restype = ctypes.c_char_p
    ver = lib.lzma_version_number()
    if ver < 50040002:
        raise SystemExit(f'liblzma {lib.lzma_version_string().decode()} is too old (need >= 5.4)')
    for nice_len in range(32, 274):
        opt = LzmaOptions()
        if lib.lzma_lzma_preset(ctypes.byref(opt), ctypes.c_uint32(0)):
            raise SystemExit('lzma_lzma_preset failed')
        opt.dict_size = dict_size
        opt.lc, opt.lp, opt.pb = props_byte % 9, (props_byte // 9) % 5, props_byte // 45
        opt.mode, opt.mf, opt.depth, opt.nice_len = 1, 0x04, 0, nice_len  # LZMA_MODE_FAST, LZMA_MF_HC4
        opt.ext_flags = 0  # no LZMA_LZMA1EXT_ALLOW_EOPM -> encoder omits the end marker
        opt.ext_size_low, opt.ext_size_high = len(raw) & 0xffffffff, len(raw) >> 32
        filters = (LzmaFilter * 2)(LzmaFilter(0x4000000000000002, ctypes.cast(ctypes.byref(opt), ctypes.c_void_p)),
                                   LzmaFilter(0xffffffffffffffff, None))
        out = ctypes.create_string_buffer(len(raw) + len(raw) // 2 + 65536)
        pos = ctypes.c_size_t(0)
        rc = lib.lzma_raw_buffer_encode(filters, None, raw, ctypes.c_size_t(len(raw)),
                                        out, ctypes.byref(pos), ctypes.c_size_t(len(out)))
        if rc != 0:
            raise SystemExit(f'lzma_raw_buffer_encode failed: {rc}')
        packed = bytes([props_byte]) + struct.pack('<IQ', dict_size, len(raw)) + out.raw[:pos.value]
        if len(packed) % 8 == 0:
            print(f'segment 0: fast/HC4 nice_len={nice_len}, packed {len(packed)} bytes')
            return packed
    raise SystemExit('no nice_len gives an 8-byte aligned packed size')


def _require(cond, msg):
    """Raise ValueError unconditionally even under python -O."""
    if not cond:
        raise ValueError(msg)


def fw_crc(data):
    """CRC used by arch_ota_check_crc: reflected 0xEDB88320, init 0, no xor in/out."""
    return zlib.crc32(data, 0xffffffff) ^ 0xffffffff  # zlib pre/post-inverts; undo both


def parse(data, expect_count=3):
    magic, count = struct.unpack_from('<II', data)
    _require(magic == 0x004d4d4d and count == expect_count, 'bad container magic/count')
    _require(hashlib.sha1(data[:136]).digest() == data[136:156], 'header SHA-1 mismatch')
    _require(struct.unpack('<I', data[-4:])[0] == fw_crc(data[:-4]), 'trailing CRC mismatch')
    segs = []
    for i in range(count):
        ent = list(struct.unpack_from('<8I', data, 8 + i * 32))
        off, _addr, size, _part, doff, dlen, comp, _res = ent
        _require(off + size == doff and dlen == 20 and comp == 1, f'segment {i} bounds')
        packed = data[off:off + size]
        _require(hashlib.sha1(packed).digest() == data[doff:doff + 20], f'segment {i} SHA-1')
        dec = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        raw = dec.decompress(packed)
        _require(dec.eof and not dec.unused_data, f'segment {i} LZMA stream')
        _require(raw is not None and len(raw) == struct.unpack_from('<Q', packed, 5)[0], f'segment {i} size')
        segs.append((ent, packed, raw))
    return segs


def build(stock, new_seg0_packed):
    segs = parse(stock)
    hdr = bytearray(stock[:136])
    body = bytearray()
    off = 156
    for i, (ent, packed, _raw) in enumerate(segs):
        if i == 0:
            packed = new_seg0_packed
        ent[0], ent[2], ent[4] = off, len(packed), off + len(packed)
        _require(len(packed) <= ent[3], f'segment {i} exceeds its partition')
        struct.pack_into('<8I', hdr, 8 + i * 32, *ent)
        body += packed + hashlib.sha1(packed).digest()
        off += len(packed) + 20
    out = bytes(hdr) + hashlib.sha1(hdr).digest() + bytes(body)
    return out + struct.pack('<I', fw_crc(out))


def require_fresh(built, *sources):
    """Refuse to package a binary that is older than its sources (build_fw.py failed or not run)."""
    inc = os.path.join(HERE, 'firmware', 'include')
    headers = [os.path.join(inc, f) for f in os.listdir(inc) if f.endswith('.h')]
    for src in list(sources) + headers:
        _require(os.path.getmtime(built) >= os.path.getmtime(src),
                 f'{os.path.relpath(built, HERE)} is older than {os.path.relpath(src, HERE)}: '
                 f'run python tools/build_fw.py first')


def apply_monitor_v2(raw):
    """In-place network monitor v2 (plain "patched" image only; --hybrid uses monitor v3)."""
    mon = open(MON_BIN, 'rb').read()
    _require(1 <= len(mon) <= MON_SLOT_SIZE,
             f'yglc_network_monitor_inplace.bin size {len(mon)} out of range 1..{MON_SLOT_SIZE}')
    slot_off = MON_SLOT_ADDR - BASE
    raw[slot_off:slot_off + len(mon)] = mon
    print(f'patched {MON_SLOT_ADDR:#x}: replaced yglc_network_monitor ({len(mon)}/{MON_SLOT_SIZE} bytes) '
          f'from {os.path.relpath(MON_BIN, HERE)}')


def apply_patches(raw, patches, disasm):
    """Byte patches; every one checks the stock bytes it replaces."""
    for addr, old, new, why in patches:
        o = addr - BASE
        _require(raw[o:o + len(old)] == old, f'unexpected bytes at {addr:#x}')
        raw[o:o + len(new)] = new
        print(f'patched {addr:#x}: {old.hex()} ({disasm(addr, old)}) -> {new.hex()} ({disasm(addr, new)})  [{why}]')


def apply_hybrid_slots(raw, raw0):
    """hybrid_bright.c into the two dead-code slots of the stock image."""
    for addr, size, sha, path in HYBRID_SLOTS:
        require_fresh(path, os.path.join(HERE, 'firmware', 'src', 'hybrid_bright.c'),
                      os.path.join(HERE, 'firmware', 'ld', 'ceiling3_hybrid.ld'))
        o = addr - BASE
        _require(hashlib.sha256(raw0[o:o + size]).hexdigest() == sha, f'unexpected stock slot {addr:#x}')
        code = open(path, 'rb').read()
        _require(0 < len(code) <= size, f'{os.path.basename(path)} size {len(code)} > slot {size}')
        raw[o:o + len(code)] = code
        print(f'patched {addr:#x}: dead-code slot <- {os.path.relpath(path, HERE)} ({len(code)}/{size} bytes)')
    _require(BASE + len(raw) == SEG0_END, f'segment 0 does not end at {SEG0_END:#x}')


def apply_blackbox(raw, partition_len, disasm):
    """blackbox_monitor.c appended after the stock image + its two entry points."""
    require_fresh(BB_BIN, os.path.join(HERE, 'firmware', 'src', 'blackbox_monitor.c'),
                  os.path.join(HERE, 'firmware', 'ld', 'ceiling3_ext_bb.ld'))
    bb = open(BB_BIN, 'rb').read()
    _require(BASE + len(raw) == SEG0_END, f'segment 0 does not end at {SEG0_END:#x}')
    _require(0 < len(bb) <= BB_EXT_MAX, f'bb_ext.bin size {len(bb)}')
    raw += bytes(BB_EXT_ADDR - SEG0_END) + bb
    _require(len(raw) <= partition_len, 'segment 0 exceeds its partition')
    print(f'appended {os.path.relpath(BB_BIN, HERE)} ({len(bb)} bytes) @ {BB_EXT_ADDR:#x}, '
          f'segment 0 now {len(raw)} bytes (partition {partition_len})')
    tick = hybrid_symbol('bb_monitor_tick', BB_ELF)
    crash = hybrid_symbol('bb_on_crash', BB_ELF)
    _require(BB_EXT_ADDR <= tick < BB_EXT_ADDR + len(bb) and BB_EXT_ADDR <= crash < BB_EXT_ADDR + len(bb),
             'black box entry points outside the appended code')
    for site, old, new, what in (
            (MON_TRAMPOLINE_SITE, MON_ENTRY_OLD, thumb_bw(MON_TRAMPOLINE_SITE, tick),
             f'yglc_network_monitor -> b.w bb_monitor_tick @{tick:#x} (rest of the stock function is dead)'),
            (CRASH_BL_SITE, CRASH_BL_OLD, thumb_bl(CRASH_BL_SITE, crash),
             f'register-dump function: bl arch_crash_reboot -> bl bb_on_crash @{crash:#x}')):
        o = site - BASE
        _require(raw[o:o + 4] == old, f'unexpected bytes at {site:#x}')
        raw[o:o + 4] = new
        print(f'patched {site:#x}: {old.hex()} ({disasm(site, old)}) -> {new.hex()} ({disasm(site, new)})  [{what}]')


def main():
    stock = open(STOCK, 'rb').read()
    if hashlib.md5(stock).hexdigest() != '13b7471a0f84e3cfbac78c116fae7a1c':
        raise SystemExit('stock OTA MD5 mismatch')
    _require(build(stock, parse(stock)[0][1]) == stock, 'rebuild of untouched container differs')
    ent0, packed0, raw0 = parse(stock)[0]
    raw = bytearray(raw0)
    stock_only = '--stock' in sys.argv
    try:
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
        def disasm(addr, code):
            return '; '.join(f'{i.mnemonic} {i.op_str}'.strip() for i in md.disasm(code, addr)) or '?'
    except ImportError:
        def disasm(addr, code):
            return 'capstone n/a'
    measure = '--measure' in sys.argv and not stock_only
    hybrid = '--hybrid' in sys.argv and not stock_only and not measure
    if not stock_only:
        _require(hashlib.sha256(raw[MON_SLOT_ADDR - BASE:MON_SLOT_ADDR - BASE + MON_SLOT_SIZE]).hexdigest()
                 == MON_SLOT_SHA256, f'unexpected stock slot SHA-256 at {MON_SLOT_ADDR:#x}')
        if not hybrid:
            apply_monitor_v2(raw)
        patches = PATCHES
        if measure:
            patches = [p for p in PATCHES if p[0] != 0x1014067c] + MEASURE_PATCHES
        if hybrid:
            entry = hybrid_symbol('hybrid_start_transition')
            patches = [p for p in PATCHES if p[0] != 0x1014067c] + HYBRID_PATCHES + [
                (HYBRID_BL_SITE, HYBRID_BL_OLD, thumb_bl(HYBRID_BL_SITE, entry),
                 f'next_trans: bl start_transition -> bl hybrid_start_transition @{entry:#x}')]
        apply_patches(raw, patches, disasm)
        if hybrid:
            apply_hybrid_slots(raw, raw0)
            apply_blackbox(raw, ent0[3], disasm)
    packed = lzma_alone_no_eos(bytes(raw), packed0[0], struct.unpack_from('<I', packed0, 1)[0])
    out = build(stock, packed)
    segs = parse(out)  # full re-validation of what we wrote
    _require(segs[0][2] == bytes(raw), 'segment 0 round-trip mismatch')
    for i in (1, 2):
        _require(segs[i][1] == parse(stock)[i][1], f'segment {i} changed')
    try:  # same end-marker check as the stock stream: unknown size must NOT reach EOF
        unk = packed[:5] + struct.pack('<q', -1) + packed[13:]
        d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
        d.decompress(unk)
        _require(not d.eof, 'stream unexpectedly contains an end marker')
    except lzma.LZMAError:
        pass
    diff = sum(a != b for a, b in zip(raw0, segs[0][2]))
    name = 'yeelink.light.ceiling3-%s-2.0.2_0048.bin' % (
        'repacked-stock' if stock_only else 'measure' if measure else 'hybrid' if hybrid else 'patched')
    path = os.path.join(HERE, name)
    open(path, 'wb').write(out)
    print(f'wrote {name}: {len(out)} bytes, md5 {hashlib.md5(out).hexdigest()}, '
          f'segment-0 bytes changed vs stock: {diff}, all container checks OK')


if __name__ == '__main__':
    main()
