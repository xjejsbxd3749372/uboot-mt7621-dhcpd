#!/usr/bin/env python3
"""Compose the 128MiB logical NAND image the MT7621 QEMU model boots from.

Layout follows CONFIG_MTDPARTS_DEFAULT of the U-Boot build under test:
  mtdparts=nand0:512k(u-boot),256k(u-boot-env),256k(bdata),256k(factory),
            256k(crash),256k(crash_syslog),256k(reserved0),4m(kernel_stock),-(firmware)

  0x000000  u-boot        512K  <- the build under test (SPL @0, payload @0x20000)
  0x080000  u-boot-env    256K
  0x0c0000  bdata         256K
  0x100000  factory       256K  (MT7603 + MT7612 calibration)
  0x140000  crash         256K
  0x180000  crash_syslog  256K
  0x1c0000  reserved0     256K
  0x200000  kernel_stock  4M
  0x600000  firmware/ubi  rest  (CONFIG_DEFAULT_NAND_KERNEL_OFFSET=0x600000)

Everything from 0x80000 on is copied verbatim from the stock dump, so env,
bdata and the factory calibration are the real ones.

The rest of the flash is copied verbatim from the stock dump, so only the
bootloader region is replaced by the build under test.

Usage:
  compose_flash.py -o flash.bin --uboot u-boot.img --dump full.bin
"""
import argparse
import os
import sys

NAND_SIZE = 128 * 1024 * 1024
PAYLOAD_OFF = 0x20000       # where the SPL payload uImage sits
IH_MAGIC = b"\x27\x05\x19\x56"
BOOT_SIZE = 0x80000          # bootloader partition
STOCK_FROM = 0x80000         # everything past the bootloader


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--uboot", required=True,
                    help="combined U-Boot image (SPL + main) to flash at 0x0")
    ap.add_argument("--dump", required=True,
                    help="stock full.bin used for the remaining partitions")
    args = ap.parse_args()

    uboot = open(args.uboot, "rb").read()
    if uboot[:4] != b"\x27\x05\x19\x56":
        sys.exit(f"error: {args.uboot} is not a uImage (magic {uboot[:4].hex()})")
    if len(uboot) > BOOT_SIZE:
        sys.exit(f"error: {args.uboot} is {len(uboot)}B, larger than the "
                 f"{BOOT_SIZE}B bootloader partition")

    # The SPL finds the payload with a grid derived from __rom_cfg, and the
    # layout that ships puts it at PAYLOAD_OFF. An SPL-only image composes
    # perfectly well and then fails much later as an unrelated
    # "Failed to load U-Boot image!", so reject it here where the message can
    # name the actual problem.
    if len(uboot) < PAYLOAD_OFF + 64 or uboot[PAYLOAD_OFF:PAYLOAD_OFF + 4] != IH_MAGIC:
        sys.exit(f"error: {args.uboot} has no payload uImage at 0x{PAYLOAD_OFF:x} "
                 f"(file is {len(uboot)}B); expected the two-stage flash image "
                 f"u-boot-mt7621_*.bin, not a u-boot_*.img")

    stock = open(args.dump, "rb").read()
    if len(stock) != NAND_SIZE:
        sys.exit(f"error: {args.dump} is {len(stock)}B, expected {NAND_SIZE}B")

    with open(args.output, "wb") as out:
        out.write(uboot)
        out.write(b"\xff" * (STOCK_FROM - len(uboot)))
        out.write(stock[STOCK_FROM:])

    # The stock dump comes from a release asset that may not exist. When it is
    # missing the workflow synthesises an all-erased device, which is fine for
    # the boot test but not the flash the docstring describes - so say so.
    if set(stock[STOCK_FROM:STOCK_FROM + 0x10000]) == {0xff}:
        print(f"  WARNING: everything at/after 0x{STOCK_FROM:x} in "
              f"{os.path.basename(args.dump)} is erased - u-boot-env, bdata and "
              f"the factory calibration are blank (stock dump release asset "
              f"'mi-router-4-full.bin' is missing)")

    print(f"composed {args.output} ({os.path.getsize(args.output)}B)")
    print(f"  bootloader 0x0        : {len(uboot)}B (build under test)")
    print(f"  0x{STOCK_FROM:x}..0x{NAND_SIZE:x}: {NAND_SIZE - STOCK_FROM}B "
          f"from {os.path.basename(args.dump)}")


if __name__ == "__main__":
    main()
