# MT7621 QEMU model and simulated boot (Xiaomi Mi Router 4 / R4)

A QEMU machine model for the MT7621 that runs this board's real U-Boot
against a real NAND dump, so the boot chain can be exercised without touching
the router. The router has no soldered serial header, so a failed flash is
unrecoverable; this is how the image is checked before it ever goes near flash.

## What exists here

| path | what it is |
|---|---|
| `include/hw/misc/mt7621-nfc.h`, `hw/misc/mt7621-nfc.c` | the NAND flash controller: NFI register block and ECC engine |
| `hw/mips/mt7621.c` | the MT7621 SoC, and the `mi-router-4` machine |
| `scripts/build_qemu.sh` | clones QEMU, installs the model, wires Kconfig/meson, builds `mipsel-softmmu` |
| `scripts/compose_flash.py` | builds the 128MiB flash image the machine boots from |
| `scripts/run_boot.sh` | runs the boot and grades how far it got |
| `../.github/workflows/qemu-mt7621-boot.yml` | builds and runs it on every change |

## Why this had to be written

There is no MT7621 machine in upstream QEMU. The closest existing work is
Lu Hui's `newluhux/qemu-hui` MT7628 model, which is the structural template
here (SoC container plus an eval-board machine, `unimplemented` devices for
everything unmodelled), but MT7628 is a different SoC: different memory map,
no NAND, and its flash is SPI **NOR**. Mi Router 4 has serial **NAND** behind
the MT7621 NFC controller, and that controller is the thing the boot path
actually talks to. The MT7603 (2.4G) and MT7612 (5G) radios are PCIe devices
with no public model and no emulation anywhere.

## The NFC model

Register offsets and semantics come from this repo's own driver,
`drivers/mtd/nand/mt7621_nand.c`, and are not guessed. The behaviours that
matter:

- **Address phase.** `nfc_cmd_ctrl()` writes *each* address byte through
  `NFI_COLADDR` (row is written as 0, `ADDRNOB` column count is 1). The wire
  order the driver produces is `col_lo, col_hi, row0, row1, row2`, so the page
  number is `row0 | row1 << 8 | row2 << 16`.
- **Read sequence.** `NFI_CMD = 0x00` (READ0), then the address bytes, then
  `NFI_CMD = 0x0f` (READSTART). At that point the page is fetched into the
  controller's stream buffer.
- **Data phase.** The driver sets `READ_MODE` (plus `AUTO_FMT_EN` and
  `HW_ECC_EN` for the hardware-ECC path) in `CNFG`, `NFI_BRD | sectors` in
  `CON`, and then reads 4 bytes per access from `NFI_DATAR` after polling
  `NFI_PIO_DIRDY` bit 0. Words are little-endian.
- **Two stream shapes.** With `AUTO_FMT_EN` the stream is 2048B of page data
  and the per-sector FDM comes from the `FDML(n)` / `FDMM(n)` registers.
  Without it — the raw path UBI uses — the stream is 2048B data plus 64B of
  OOB, 2112B total, in the raw interleave `DAT0|FDM0|ECC0|DAT1|...`.
- **ECC.** Must read idle in `ENCIDLE`/`DECIDLE`, report zero corrected
  errors in `DECENUM`, and set every sector-done bit in `DECDONE`. The model
  reports a good image; the real engine is not reimplemented.

Geometry: 2048B pages, 64B OOB, four 512B ECC steps, 16B spare per sector
(8B FDM + 8B ECC) — which is what the driver derives for this board
(`spare_per_sector = 16`, strength 4).

## The backing file is the logical partition image

`compose_flash.py` produces a 128MiB file laid out as the partition map, with
this build's `u-boot_*.img` at offset 0 and everything else copied verbatim
from the stock dump. The controller therefore addresses it as
`page * 2048` — page data only, **no OOB interleaved**. That is deliberate: a
`full.bin` partition dump is a logical MTD view and is exactly 128MiB, while
a physical page layout would be 124MiB of usable data.

The consequence is that OOB is synthesized as `0xff`. That is enough for the
SPL, the U-Boot banner, the MTD partition probe and raw reads, and it is **not**
enough for UBI, which keeps its EC headers and VID headers in OOB. Expect
`ubi_attach` to find no volumes, and expect the boot to stop there rather than
handing control to a kernel. Recovering that would need a physical NAND dump
with real OOB, which is not something a logical partition dump can provide.

## Booting

```sh
bash scripts/build_qemu.sh                 # ~15-30 min, builds mipsel-softmmu only
python3 scripts/compose_flash.py \
    -o flash.bin --uboot u-boot-mt7621.bin --dump full.bin
bash scripts/run_boot.sh \
    /tmp/qemu-mt7621-build/build/qemu-system-mipsel flash.bin 90 boot.log 3
```

or just run the workflow, which does all of it. The image path reaches the
machine as `-M mi-router-4,flash=<path>`; `MI_ROUTER4_FLASH` is accepted as a
fallback.

## Which release asset actually boots

The release carries two images and only one of them is a flash image:

| asset | what it is |
|---|---|
| `u-boot-mt7621_*.bin` | **the flash image.** uImage at `0x0` (`MT7621 NAND`, 72192B, load/entry `0x80100000`) = SPL, plus the LZMA U-Boot uImage at `0x20000` (176483B, load/entry `0x80200000`). This matches `CONFIG_SPL_PAYLOAD="u-boot-lzma.img"` and it is what the hardware runs. |
| `u-boot_*.img` | just `mkimage(u-boot.bin)` of the uncompressed main U-Boot (520353B payload). No first stage. BootROM would jump straight to main U-Boot. |

CI boots the first and decides pass/fail on it, then boots the second as a
comparison: if the comparison reaches a banner while the real chain does not,
the SPL stage is the problem, not U-Boot.

## What is in the stock dump at offset 0

Not stock U-Boot. `full.bin` starts with a **Breed** bootloader uImage
(payload 105404B, load/entry `0xa0201000`, the kseg1 uncached mapping), which
is the third-party recovery bootloader flashed over the original one - the
right choice for a board with no serial header. Leftovers of the original
`Ralink U-Boot` (`DRAM:` at `0x23958`, `Ralink` at `0x23de8`) sit past the
end of that payload, so the first 512K of the dump is not one coherent
image. `compose_flash.py` therefore overwrites `0x0..0x80000` wholesale with
the build under test and copies everything from `0x80000` on verbatim, which
keeps the real `u-boot-env`, `bdata` and `factory` calibration.

## QEMU 9.2 API traps hit on the way

Every one of these cost a CI cycle before it was found:

- `#include "hw/mips/cpudevs.h"` no longer exists. Use `target/mips/cpu.h`,
  which is what `hw/mips/malta.c` uses for the same calls.
- `mips_cpu_create_with_clock()` takes **three** arguments now:
  `(type, refclk, TARGET_BIG_ENDIAN)`, and it realizes the CPU internally.
  For `mipsel` `TARGET_BIG_ENDIAN` is 0, which is correct for this board.
- `MIPS_CPU_TYPE_NAME()` moved to `target/mips/cpu-qom.h`.
- `blk_pread()` is gone from 9.2 and the surviving replacements want a
  `BdrvChild` reachable only through private block-layer structures. This
  model therefore never touches the block layer: the machine reads the image
  with plain stdio into `nfc->data`. 128MiB of host RAM is the trade.
- `serial_hd()` has no declaration in any 9.2 header. The equivalent is
  `qemu_chr_find()` from `chardev/char.h`, and it resolves the same object
  because `-serial` labels its chardev `serialN` (`system/vl.c`) and chardevs
  are registered as children of the chardevs root **by label**
  (`chardev/char.c`).
- A `MemoryRegion` belongs to **exactly one** container. Mapping the UART's
  region a second time to expose the `0xbe000c00` console address fails with
  `system/memory.c: memory_region_add_subregion_common: Assertion
  `!subregion->container' failed` and aborts QEMU before any guest code runs.
  The second mapping is also pointless: MIPS kseg0/kseg1 translate to
  `phys = addr & 0x1fffffff`, so `0xbe000c00` *is* physical `0x1e000c00`
  where UART0 already sits, and QEMU's system_memory is indexed physically.
  Confirmed from the firmware itself - the SPL contains exactly one
  `0xbe000c00` constant (its `CONFIG_SYS_NS16550_COM1`).
- `serial-mm`'s region is `8 << regshift` bytes, so `0x20` with `regshift=2`.
  An `unimplemented` placeholder sized `0x200` at `0x1e000b00` therefore
  covered `0x1e000c00..0x1e000d00` and shadowed UART0 and UART1. Sizes in the
  `unimp[]` table must not overlap the devices that are really mapped.
- **The `flash` machine property:** registering it with the generic
  `object_class_property_add(oc, "flash", "string", get, set, ...)` while
  passing `char *(*)(Object *, void *)` callbacks compiles with a warning and
  then fails at runtime with `Parameter 'flash' is unexpected`. The generic
  API wants `ObjectPropertyAccessor` callbacks that take a `Visitor`, so the
  key is never consumed by a visitor and `qobject_input_check_struct()`
  rejects it. The right API is `object_class_property_add_str()` with
  `char *(*)(Object *, Error **)` and `void (*)(Object *, const char *, Error **)`.
- The `flash` symbol must be appended to
  `configs/devices/mipsel-softmmu/default.mak`, not `mips-softmmu/` (they are
  different directories), and the meson `when:` symbol must match the Kconfig
  name exactly. A mismatch drops the file from the build with **no** warning
  and links a complete QEMU with no model in it - hence the object-file check
  at the end of `build_qemu.sh`.

The machine emulates the boot ROM's one important act: it reads the NAND
uImage at offset 0, copies the payload to the address in the header
(`0x80200000`, the kseg0 alias of physical `0x00200000`) and enters the SPL
there. From that point the code running is the real U-Boot.

## Boot levels

`run_boot.sh` grades the serial log against what a real boot produces:

| level | meaning |
|---|---|
| L1 | QEMU starts and accepts the machine |
| L2 | the SPL runs and prints something |
| L3 | the main U-Boot banner, plus NAND/MTD probe output |
| L4 | a Linux `Linux version` / `Booting Linux` line |

## What is not modelled, and why it is fine

- **MT7603 / MT7612 radios.** Private PCIe devices, no model exists. The PCIe
  window is an `unimplemented` device, so wifi probes fail, which is what would
  happen on a machine without the radios.
- **GIC.** `0x1fbc0000` is unimplemented, so there are no external interrupts.
  The SPL and U-Boot run off the CP0 timer, so this does not stop them, but
  anything that needs a real interrupt will not fire.
- **DDR calibration.** `DRAMC` at `0x1e005000` is unimplemented. QEMU hands
  over RAM directly, so the DRAM init the SPL performs has nothing to do. Its
  register writes land on a harmless sink.
- **GPIO, LED, reset button.** Unimplemented; the failsafe button path cannot
  be exercised here. The host-side logic for it is covered by
  `tests/test_boot_path.py` and the C unit tests instead.
- **Writes.** Program and erase are accepted and kept in memory only; the
  backing dump is never modified by a simulated boot.
