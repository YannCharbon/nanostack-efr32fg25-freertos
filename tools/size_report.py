#!/usr/bin/env python3
"""Size report and size gate for a firmware ELF (GENERICITY_PLAN.md §5.9).

Flash = everything loaded in FLASH (vectors, text, rodata, init data copy).
RAM   = data + bss + stack/heap sections reserved by the linker script.
The FreeRTOS heap (ucHeap, configTOTAL_HEAP_SIZE) is shown apart, because it
is a fixed reservation the stack's malloc() draws from, not static data.

Gate: fails (exit 1) if flash > FLASH_GATE or static RAM without the FreeRTOS
heap and the linker's leftover newlib heap > RAM_GATE, or if anything does not
fit the device (1920 kB flash / 512 kB RAM, EFR32FG25B222F1920IM56).
"""
import argparse
import subprocess
import sys

DEVICE_FLASH = 1920 * 1024
DEVICE_RAM = 512 * 1024
FLASH_GATE = 1024 * 1024
RAM_GATE = 64 * 1024

FLASH_BASE, FLASH_END = 0x08000000, 0x08000000 + DEVICE_FLASH
RAM_BASE, RAM_END = 0x20000000, 0x20000000 + DEVICE_RAM


def sections(size_tool, elf):
    out = subprocess.run([size_tool, "-A", "-d", elf], check=True, capture_output=True, text=True).stdout
    res = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
            res.append((parts[0], int(parts[1]), int(parts[2])))
    return res


def symbol_size(nm_tool, elf, name):
    out = subprocess.run([nm_tool, "-S", elf], check=True, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] == name:
            return int(parts[1], 16)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--size", default="arm-none-eabi-size")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    a = ap.parse_args()

    flash = ram = newlib_heap = 0
    for name, size, addr in sections(a.size, a.elf):
        if name.startswith(".debug") or name in (".comment", ".ARM.attributes"):
            continue
        if FLASH_BASE <= addr < FLASH_END:
            flash += size
        elif RAM_BASE <= addr < RAM_END:
            ram += size
            if name == ".data":
                flash += size          # initialiser copy in flash
            if name == ".heap":
                newlib_heap = size
    rtos_heap = symbol_size(a.nm, a.elf, "ucHeap")
    static_ram = ram - newlib_heap
    core_ram = static_ram - rtos_heap

    name = a.elf.rsplit("/", 1)[-1]
    print(f"== size report {name} (EFR32FG25B222F1920IM56: 1920 kB flash / 512 kB RAM)")
    print(f"   flash          {flash / 1024:8.1f} kB  ({100.0 * flash / DEVICE_FLASH:4.1f} % of device, gate {FLASH_GATE // 1024} kB)")
    print(f"   RAM static     {static_ram / 1024:8.1f} kB  ({100.0 * static_ram / DEVICE_RAM:4.1f} % of device)")
    print(f"     FreeRTOS heap (ucHeap)   {rtos_heap / 1024:8.1f} kB  (stack malloc() pool)")
    print(f"     everything else          {core_ram / 1024:8.1f} kB  (gate {RAM_GATE // 1024} kB)")
    print(f"   RAM left to newlib heap / unused {newlib_heap / 1024:8.1f} kB")

    ok = flash <= FLASH_GATE and core_ram <= RAM_GATE and flash <= DEVICE_FLASH and static_ram <= DEVICE_RAM
    if not ok:
        print("   SIZE GATE FAILED", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
