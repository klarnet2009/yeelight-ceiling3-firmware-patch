# Yeelight ceiling3 firmware: reverse engineering and patching

Reverse engineering notes, C reconstructions derived from the decompiled firmware and a binary patcher for the stock firmware of the
Yeelight ceiling light `yeelink.light.ceiling3` (YLXD17YL, MediaTek MT7697N), firmware version `2.0.2_0048`.

The full documentation is in Russian: [README_RU.md](README_RU.md). This page is a short English summary.

> **Warning.** The resulting firmware is unofficial. A mistake or an incompatible board revision can leave the lamp
> unbootable and may require recovery over UART. Use it at your own risk. This project is not affiliated with
> Yeelight, Xiaomi or MediaTek.

## What the `--hybrid` image does

`python patch_ota.py --hybrid` builds `yeelink.light.ceiling3-hybrid-2.0.2_0048.bin` from the stock OTA file:

- **Wi-Fi fixes.** Recovery patches for the stock Wi-Fi/network code (reconnect, offline timeout, local `miIO.ota`
  and others); see [decomp/WIFI_RECOVERY.md](decomp/WIFI_RECOVERY.md) and README_RU.md for the list.
- **"Hybrid" brightness.** `bright 1..9` drives the moon LEDs (`nl_br` 1/5/11/20/31/44/60/79/100) with the main LEDs
  off; `bright 10..100` drives the main LEDs from the driver minimum up to 100 %. The app still sees the ordinary day
  mode, and the real moon mode works as before. After a crash the hook passes through to the plain `patched` behaviour.
- **Network monitor v3.** Checks the lwIP lock, probes the gateway with ARP every 60 s, and reboots the lamp when a
  problem lasts longer than 5 min, then 30 min, then 6 h (from the third consecutive reboot). It does not act during
  setup or firmware update.
- **Event log `bblog`.** A compact boot/network/reboot log kept in RAM and in flash (`psm`), read with
  `get_prop ["bblog"]` (up to 240 characters) and decoded by `python tools/read_bblog.py`. Request it separately
  from other properties: the stock `get_prop` has a 512-byte reply buffer and returns a truncated or garbled reply
  when the total is longer.
- **Crash path.** The reboot is moved right after the register dump, and PC, LR and `SCB->CFSR` are saved before the
  reboot; if `arch_crash_reboot()` returns, a WDT reset is issued as a fallback.

## Main findings

- **LED driver limit.** The main LEDs go dark below about 5.5 % duty and hold a single minimum level up to about
  10.5-11 %. The firmware path is linear (verified by a differential test: 63,600 cases, 0 mismatches), so the
  limit is in the driver hardware. See [decomp/LIGHT_MIXER.md](decomp/LIGHT_MIXER.md).
- **No watchdog.** The stock firmware has no task watchdog, so hangs without a crash (deadlock, endless loop) are
  not recovered by anything.
- **Crash behaviour.** After a crash the stock firmware prints the registers, dumps about 320 KB of memory as text to
  the UART, and only then reboots. It does not hang forever (an earlier version of these notes claimed it did).
- **RAM block `0x2003C000`.** It does not survive a reboot (checked on the lamp), so crash address and previous
  uptime do not reach the log; the flash copy of the log does.

Light subsystem details: [decomp/LIGHT_MIXER.md](decomp/LIGHT_MIXER.md), [decomp/LIGHT_UNIT.md](decomp/LIGHT_UNIT.md),
[decomp/LIGHT_CMD.md](decomp/LIGHT_CMD.md).

## Status

Checked on the lamp: the `bblog` property can be read, its time stamps match the PC clock, and the flash copy
survives a `miIO.reboot`. Not checked on hardware: whether the reset in `arch_crash_reboot` works on this board, and
whether gateway ARP replies reach lwIP (if not, false reboots with reason `A` are possible when the internet is
unavailable).

## Build

The stock firmware is not included. Obtain the original OTA file
`yeelink.light.ceiling3-stock-2.0.2_0048.bin` yourself (MD5 `13b7471a0f84e3cfbac78c116fae7a1c`), put it in the
repository root and unpack it:

```
python unpack.py                  # verifies size/MD5/SHA-1, writes extracted/
python tools/build_fw.py          # compiles the hook code (clang/LLVM)
python patch_ota.py --hybrid      # writes the hybrid OTA image
```

Requirements: Python 3, clang/LLVM, liblzma (`LIBLZMA` environment variable can point to it); for the tests also
`unicorn` and `capstone`, and `gcc` for the host-side tests. Tool paths in `tools/build_fw.py` assume
`C:\Program Files\LLVM\bin`. `patch_ota.py` refuses to pack a binary older than its sources and headers.

## Tests

The emulator tests run the real segment 0 from `extracted/` in a Cortex-M4 emulator:

```
python tools/emu_hybrid_test.py   # 65 checks: brightness hook, PWM output, crash latch
python tools/emu_bb_test.py       # 46 checks: network monitor v3 and bblog
python tools/diff_light_mixer.py  # clean mixer vs. original, 63,600 cases
```

## Repository layout

- `patch_ota.py`, `unpack.py`, `miio_ota.py`, `monitor_lamp.py`, `xiaomi_qr_token.py`: OTA unpack/patch/flash tools.
- `firmware/src`, `firmware/include`, `firmware/ld`: code injected into the image and its linker scripts.
- `firmware/clean_src`: C11 reconstruction of the light and Wi-Fi code; `firmware/tests`: host tests.
- `tools/`: build, emulator tests, log reader, Ghidra scripts. `decomp/`: analysis notes and the function table.
- `docs/`: early working notes in Russian (README_RU.md is the current description).

Not included: any Yeelight/Xiaomi/MediaTek firmware, extracted segments, Ghidra projects and raw decompiler output.
Some notes refer to such files; they are not needed to read the code.

## License

MIT, see [LICENSE](LICENSE). The license covers only this project's own code and notes. The Yeelight, Xiaomi and
MediaTek firmware is not part of this repository and is not covered by it. `firmware/clean_src` contains
reconstructions of the vendor firmware's behaviour, published for interoperability research; the original firmware
remains the property of its owners.
