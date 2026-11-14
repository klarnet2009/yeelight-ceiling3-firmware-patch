#!/usr/bin/env python3
"""Build C11 Cortex-M4F firmware sources, host ABI unit tests, and optional decomp audit."""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLVM_BIN = r"C:\Program Files\LLVM\bin"
CLANG = os.path.join(LLVM_BIN, "clang.exe")
LLD = os.path.join(LLVM_BIN, "ld.lld.exe")
OBJCOPY = os.path.join(LLVM_BIN, "llvm-objcopy.exe")
OBJDUMP = os.path.join(LLVM_BIN, "llvm-objdump.exe")
SIZE_TOOL = os.path.join(LLVM_BIN, "llvm-size.exe")

GCC = shutil.which("gcc")

FW_DIR = os.path.join(ROOT, "firmware")
BUILD_DIR = os.path.join(FW_DIR, "build")
DECOMP_DIR = os.path.join(ROOT, "decomp")

ARM_CFLAGS = [
    "--target=thumbv7em-none-eabi",
    "-mcpu=cortex-m4",
    "-mthumb",
    "-std=c11",
    "-Os",
    "-ffreestanding",
    "-nostdlib",
    "-ffunction-sections",
    "-fdata-sections",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-c",
]


def run(cmd, cwd=ROOT, check=True):
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if check and res.returncode != 0:
        if res.stdout:
            print(res.stdout)
        if res.stderr:
            print(res.stderr, file=sys.stderr)
        raise SystemExit(res.returncode)
    return res


def build_host_tests():
    if not GCC:
        raise SystemExit("gcc not found in PATH (needed to build host tests)")
    os.makedirs(BUILD_DIR, exist_ok=True)
    exe_path = os.path.join(BUILD_DIR, "test_ceiling3_firmware.exe")
    srcs = [
        os.path.join(FW_DIR, "src", "wifi_recovery_guard.c"),
        os.path.join(FW_DIR, "src", "ota_hooks.c"),
        os.path.join(FW_DIR, "tests", "test_ceiling3_firmware.c"),
    ]
    run([GCC, "-DHOST_TEST=1", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", *srcs, "-o", exe_path])
    res = run([exe_path])
    print(res.stdout.strip())


def build_firmware_arm():
    obj_dir = os.path.join(BUILD_DIR, "obj")
    os.makedirs(obj_dir, exist_ok=True)
    srcs = sorted(
        os.path.join(FW_DIR, "src", f)
        for f in os.listdir(os.path.join(FW_DIR, "src"))
        if f.endswith(".c")
    )
    objs = []
    for src in srcs:
        obj = os.path.join(obj_dir, os.path.basename(src)[:-2] + ".o")
        run([CLANG, *ARM_CFLAGS, src, "-o", obj])
        objs.append(obj)

    elf_path = os.path.join(BUILD_DIR, "ceiling3_ext.elf")
    bin_path = os.path.join(BUILD_DIR, "ceiling3_ext.bin")
    ld_script = os.path.join(FW_DIR, "ld", "ceiling3_ext.ld")

    run([
        LLD,
        "-T", ld_script,
        "-L", os.path.join(FW_DIR, "ld"),
        "--gc-sections",
        *objs,
        "-o", elf_path,
    ])
    run([OBJCOPY, "-O", "binary", elf_path, bin_path])

    size_res = run([SIZE_TOOL, elf_path])
    print(size_res.stdout.strip())
    bin_size = os.path.getsize(bin_path)
    print(f"ARM Thumb-2 build OK -> {os.path.relpath(elf_path, ROOT)} & {os.path.relpath(bin_path, ROOT)} ({bin_size} bytes)")

    # Build zero-BSS in-place replacement for yglc_network_monitor (@ 0x100bd918, max 224 bytes)
    oz_flags = ["-Oz" if f == "-Os" else f for f in ARM_CFLAGS]
    guard_src = os.path.join(FW_DIR, "src", "wifi_recovery_guard.c")
    guard_oz_obj = os.path.join(obj_dir, "wifi_recovery_guard_oz.o")
    inplace_elf = os.path.join(BUILD_DIR, "yglc_network_monitor_inplace.elf")
    inplace_bin = os.path.join(BUILD_DIR, "yglc_network_monitor_inplace.bin")
    inplace_ld = os.path.join(FW_DIR, "ld", "ceiling3_inplace_monitor.ld")

    run([CLANG, *oz_flags, guard_src, "-o", guard_oz_obj])
    run([
        LLD,
        "-T", inplace_ld,
        "-L", os.path.join(FW_DIR, "ld"),
        "--gc-sections",
        guard_oz_obj,
        "-o", inplace_elf,
    ])
    run([OBJCOPY, "-O", "binary", inplace_elf, inplace_bin])

    inplace_size = os.path.getsize(inplace_bin)
    if inplace_size == 0 or inplace_size > 224:
        raise SystemExit(f"yglc_network_monitor_inplace.bin size {inplace_size} outside 1..224 byte budget")
    print(f"ARM Thumb-2 inplace monitor OK -> {os.path.relpath(inplace_bin, ROOT)} ({inplace_size}/224 bytes)")

    # Hybrid daylight brightness hook, in-place in two dead-code slots (patch_ota.py --hybrid)
    hyb_src = os.path.join(FW_DIR, "src", "hybrid_bright.c")
    hyb_obj = os.path.join(obj_dir, "hybrid_bright_oz.o")
    hyb_elf = os.path.join(BUILD_DIR, "hybrid_bright.elf")
    run([CLANG, *oz_flags, hyb_src, "-o", hyb_obj])
    run([LLD, "-T", os.path.join(FW_DIR, "ld", "ceiling3_hybrid.ld"), "-L", os.path.join(FW_DIR, "ld"),
         "--gc-sections", hyb_obj, "-o", hyb_elf])
    for slot, budget in (("a", 212), ("b", 236)):
        out = os.path.join(BUILD_DIR, f"hybrid_slot_{slot}.bin")
        run([OBJCOPY, "-O", "binary", f"--only-section=.slot_{slot}", hyb_elf, out])
        size = os.path.getsize(out)
        if size == 0 or size > budget:
            raise SystemExit(f"hybrid_slot_{slot}.bin size {size} outside 1..{budget}")
        print(f"ARM Thumb-2 hybrid slot {slot.upper()} OK -> {os.path.relpath(out, ROOT)} ({size}/{budget} bytes)")

    # Network monitor v3 + black box, appended after segment 0 @ 0x10150650 (patch_ota.py --hybrid)
    bb_src = os.path.join(FW_DIR, "src", "blackbox_monitor.c")
    bb_obj = os.path.join(obj_dir, "blackbox_monitor_oz.o")
    bb_elf = os.path.join(BUILD_DIR, "bb_ext.elf")
    bb_bin = os.path.join(BUILD_DIR, "bb_ext.bin")
    run([CLANG, *oz_flags, bb_src, "-o", bb_obj])
    run([LLD, "-T", os.path.join(FW_DIR, "ld", "ceiling3_ext_bb.ld"), "-L", os.path.join(FW_DIR, "ld"),
         "--gc-sections", bb_obj, "-o", bb_elf])
    run([OBJCOPY, "-O", "binary", bb_elf, bb_bin])
    bb_size = os.path.getsize(bb_bin)
    if bb_size == 0 or bb_size > 0x1000:
        raise SystemExit(f"bb_ext.bin size {bb_size} outside 1..4096")
    print(f"ARM Thumb-2 monitor v3 + black box OK -> {os.path.relpath(bb_bin, ROOT)} ({bb_size} bytes @ 0x10150650)")


def build_clean_sources():
    clean_dir = os.path.join(FW_DIR, "clean_src")
    if not os.path.isdir(clean_dir):
        return
    obj_dir = os.path.join(BUILD_DIR, "clean_obj")
    os.makedirs(obj_dir, exist_ok=True)
    srcs = sorted(
        os.path.join(clean_dir, f)
        for f in os.listdir(clean_dir)
        if f.endswith(".c")
    )
    for src in srcs:
        obj = os.path.join(obj_dir, os.path.basename(src)[:-2] + ".o")
        run([CLANG, *ARM_CFLAGS, src, "-o", obj])
    print(f"ARM Thumb-2 clean_src compile OK ({len(srcs)} module(s) -> {os.path.relpath(obj_dir, ROOT)})")


def compile_decomp_tree():
    decomp_obj_dir = os.path.join(BUILD_DIR, "decomp_obj")
    os.makedirs(decomp_obj_dir, exist_ok=True)
    c_files = []
    for dp, _, fns in os.walk(DECOMP_DIR):
        for fn in sorted(fns):
            if fn.endswith(".c"):
                c_files.append(os.path.join(dp, fn))

    ok = 0
    failed = []
    cflags = [
        "--target=thumbv7em-none-eabi",
        "-mcpu=cortex-m4",
        "-mthumb",
        "-std=gnu17",
        "-ffreestanding",
        "-fno-builtin",
        "-Os",
        "-DDECOMP_BUILD",
        "-Wno-everything",
        "-c",
    ]
    for cf in c_files:
        rel = os.path.relpath(cf, DECOMP_DIR)
        out_o = os.path.join(decomp_obj_dir, rel[:-2] + ".o")
        os.makedirs(os.path.dirname(out_o), exist_ok=True)
        res = run([CLANG, *cflags, cf, "-o", out_o], check=False)
        if res.returncode == 0:
            ok += 1
        else:
            first_err = next((l for l in res.stderr.splitlines() if "error:" in l), "compile error")
            failed.append((rel.replace("\\", "/"), first_err))

    print(f"Decomp ARM object build: {ok}/{len(c_files)} compiled cleanly (.o), {len(failed)} failed (Ghidra arity/void mismatches)")
    for rel, msg in failed[:15]:
        print(f"  - {rel}: {msg}")
    if len(failed) > 15:
        print(f"  ... ({len(failed) - 15} more)")
    return ok, failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--with-decomp", action="store_true", help="Also compile all decomp/*.c modules to ELF .o")
    args = ap.parse_args()

    run([sys.executable, os.path.join(ROOT, "tools", "prep_decomp_build.py")])
    build_host_tests()
    build_firmware_arm()
    build_clean_sources()
    if args.with_decomp:
        compile_decomp_tree()


if __name__ == "__main__":
    main()
