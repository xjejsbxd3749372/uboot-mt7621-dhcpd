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

## The stock dump release

`compose_flash.py` needs a 128MiB `--dump`; without one it composes a flash
whose partitions past `0x80000` are all `0xFF`, so `env`/`bdata`/`factory` are
blank and L4 is unreachable. That dump now exists:

    https://github.com/xjejsbxd3749372/uboot-mt7621-dhcpd/releases/tag/nand-dump

| asset | what it is |
|---|---|
| `mi-router-4-full.bin` | the stock the compose script wants: Breed at `0x0`, MiWiFi-R4-2.26.175 / 2.26.145 kernels (Linux 3.10.14) at `0x200000` / `0x600000`, squashfs 4.0/XZ rootfs inside UBI from `0xa41000`. |
| `mi-router-4-current.bin` | a later dump, now running third-party OpenWrt r11208 (Linux 4.14.195) with this project's U-Boot. Its kernel is **truncated** - 6142484B declared, 4194240B that fit in the 4MiB `kernel_stock` partition - so the LZMA stream ends at 81%. Kept as a record, not as a working stock. |

Both are sanitised in place at identical length (serial number, MAC, SSIDs,
admin password hash, `DEVICE_ID`, `CHANNEL_SECRET` and their copies inside the
rootfs), with the CRC32 of the `0x80000` config block and of `bdata`
recomputed and re-verified - both are stored little-endian. Equal-length edits
inside the rootfs invalidate its node checksums, so the published files must
not be flashed back to a device.

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

## Where the simulated boot gets to

Graded by `scripts/run_boot.sh`, run 36776959760:

```
U-Boot SPL 2018.09 (Sep 30 2026)
Trying to boot from NAND
nfc[1/40]: wr off=0x08 val=0x3        NFI_CON
nfc[6/40]: wr off=0x00 val=0x40       NFI_CNFG
nfc[8/40]: wr off=0x0c val=0x30c77fff NFI_ACCON
```

| level | status |
|---|---|
| L1 console output | **PASS** |
| L2 SPL stage + NAND/DRAM init | **PASS** |
| L3 main U-Boot banner | **PASS** (run 36828431431) |
| L4 Linux `Linux version` | FAIL - needs a stock image that actually boots |

The chain that got there, each step found by instrumentation rather than guesswork:

1. **CPU model** - `24KEc` reads `mfc0 $2,2` (TCBind, gated on `Config3.MT`) as
   unimplemented, returns `~0`, and the SPL takes a wrong branch into FPU
   code: `coprocessor unusable`. `34Kf` has MT and FPU.
2. **`0x1fc00000` flash-mmap** - this is both the MIPS BEV=1 vector base
   (`MT7621_FLASH_MMAP_BASE`) and where the vectors land. Unmapped, one
   exception stranded the CPU retrying the same rejected read forever; the
   serial log reached 4.2GB.
3. **CDMM size** - the SPL's `lw t1, 0x2028(t0)` with `t0 = CKSEG1ADDR(0x1fbf8000)`
   fell 0x28 past a 0x2000 placeholder. Now 0x8000, ending exactly at flash-mmap.
4. **The SPL stack lives in the FE window** - `CONFIG_SYS_INIT_SP_ADDR
   0xbe10d000` is kseg1 of 0x1e10d000, inside `MT7621_FE_BASE+MT7621_FE_SIZE`.
   A write-dropping placeholder made every saved register read back 0, so
   `jr $ra` went to 0x0. Placeholders are now memory-backed.
5. **DDR calibration blob** - `mt7621_stage_sram_noprint.bin` is exactly 13928
   bytes, the count in the memcpy the sampler catches, and it is copied to the
   FE SRAM at `0xbe108800` and executed. It cannot complete against a
   placeholder DRAMC, so the model stubs that entry with `jr ra` - QEMU already
   provides working RAM.
6. **`get_ram_size(KSEG1, SZ_512M)`** probes past 128MB; the unmapped range
   caused `Invalid read at addr 0x10000000` -> bus error -> vector at
   `0xbfc00380` -> executing flash bytes. The holes above 128MB are now mapped.
7. **`join_coherent_domain()`** (`launch_ll.S`) spins on `GCR_CO_COHERENCE`
   (CDMM+0x4008) until non-zero, and nothing ever writes that offset. Reads are
   mirrored from `GCR_Cx_COHERENCE`, which the caller does write.
8. **`SYSCTL+0x44` (`CUR_CLK_STS`)** - a hardware status register, never
   written by the firmware. `ext t1, t0, 8, 4; teq t1, $0` needs
   `CUR_CPU_FDIV` non-zero, otherwise `trap`. Now supplied.
9. **Current blocker** - `teq a1, $0` at `0x80105844`, in a 64-bit division
   helper called from `0x80105d08` with `a1 = lo(1000 * arg0)` where `arg0`
   came back as 0. Same shape as the previous two: a value the firmware reads
   from hardware that the model still reports as zero.

Diagnosis was driven by three pieces of instrumentation added to the model,
all bounded so they cannot flood the log:

- `mt7621-sample` - PC/sp/ra/t9/EPC/status every 20ms
- `mt7621-region` - first entry into each 4KB page, up to 500 lines
- `scratch[...]: dominant=<addr>` - one line per window per second naming the
  address that dominates its traffic (Boyer-Moore, so a polling loop wins even
  with interleaved stack traffic)
- `nfc[n/40]` - the first 40 NFI register accesses

### NAND, and where the boot now stops

The SPL runs to completion, the controller is driven end to end, and the boot
fails in the payload search:

```
U-Boot SPL 2018.09 (Oct 01 2026)
Trying to boot from NAND
nfc: read id ef f1 00 95
wr 0x44 = 0xf1            <- NFI_CNRN: nand_scan_ident() matched, chip registered
nfc: load page=35  col=512 addr=0x11a00 word0=0x8010aca8 raw=0:2:35:0:0 n=4
nfc: load page=99  col=512 addr=0x31a00 word0=0x9c67e72a raw=0:2:99:0:0 n=4
nfc: load page=163 col=512 addr=0x51a00 word0=0xffffffff raw=0:2:163:0:0 n=4
nfc: load page=64  col=512 addr=0x20200 word0=0x659c8319 raw=0:2:64:0:0 n=4
Trying to boot from UART
Failed to load U-Boot image!
```

Four more model bugs were found by reading the driver rather than the log:

1. **`NFI_STA.STA_CMD` was stuck high.** `nfc_wait_status_ready()` in
   `drivers/mtd/nand/mt7621_nand.c` polls `!(val & STA_CMD)`; the model set the
   bit when a command was written and never cleared it, so `nand_init()` spun
   for the whole run. The model finishes a command immediately, so the bit must
   read clear.
2. **`NAND_CMD_READSTART` was defined as `0x0f`.** `include/linux/mtd/rawnand.h`
   says `0x30`. The model therefore never matched a command the driver sends.
3. **Pages were only armed on `READSTART`.** `nfc_read_page_hwecc()` issues no
   command at all - the core sends `NAND_CMD_READ0` and the function starts the
   transfer with `NFI_CON.NFI_BRD`. Arming now happens on the first
   `NFI_DATAR` read, because the address cycle has not happened yet when the
   command is decoded.
4. **`READID` was unimplemented**, so `id[]` came back all `0xff`,
   `nand_scan_ident()` found no match, `nfc_probe()` returned before
   `nand_register()`, and there was no device at all. The id now served is
   `ef f1 00 95 00` - the `W29N01HVSINA 1G` entry of `nand_ids.c`, whose
   `SZ_2K / SZ_128 / SZ_128K / oob 64 / NAND_ECC_INFO(4, SZ_512)` matches
   `MT7621_NFC_PAGE_SIZE / BLOCK_SIZE / OOB_SIZE / ECC_STEPS` exactly.

Two follow-on faults in the same area:

- **The PIO port stepped four bytes per access regardless of `CNFG.BYTE_RW`.**
  `nfc_probe()` sets `BYTE_RW` before the id read, so `id[1]` was being taken
  from byte 5 instead of byte 1. The step is now 1 or 4 from the register.
- **`NAND_FSM` stayed in `FSM_CUSTOM_DATA`.** `nfc_pio_read()` only reprograms
  `CNFG` - and thereby picks byte versus word mode - when it sees a state other
  than `FSM_CUSTOM_DATA`, so a parked FSM froze the access width. The FSM now
  reads `FSM_IDLE`, matching the hardware behaviour the driver documents.

### How L3 was reached (the bad-block false positive)

`__rom_cfg` sits at flash 0x80 and reads `magic=0x31323637 size=0x11a20
align=0x20000`, so `get_mtk_image_search_start()` returns `size + 64 =
0x11A60`, `ALIGN` to `0x20000`, and the payload is found at `0x20000`. The
payload was always healthy: `ih_hcrc` recomputes to `0xb3e30a32` and `ih_dcrc`
to `0xb0186cca`, both matching the header.

What stopped it was `nfc_block_bad()`, which reads the bad-block marker at
column `ecc.size + badblockpos = 512`. `mt7621_nand.c` documents two page
layouts - raw `DAT0|FDM0|ECC0|...` and formatted `DAT0..3|FDM0..3|ECC0..3` -
and the model only implemented formatted, so column 512 came back as *data*
bytes (`0x19` at `0x20200`, `0xa8` at `0x11a00`, `0x67` at `0x40200`) instead
of `0xFF`. Every block therefore looked bad and `nand_spl_load_image()` skipped
`0x20000` entirely; the SPL only ever saw the `0xFF` that follows it. Twelve of
the thirteen observed `page=` traces were those probes, which is also why the
`0x11A00` address looked like a search start - it is `page*2048 + 512`, a
synthesised value, not any offset the SPL computes. The `0x60` gap is
`(0x11A60 & 0x7FF) - 0x200`.

With the raw layout implemented, `nfc_block_bad()` sees `0xFFFFFFFF`, blocks
probe good, and the copy runs page 64 through 150 with no gap - 174KiB against
`ih_size` 176521B. `Clocks: CPU: 880MHz, DDR: 1200MHz, Bus: 220MHz,
XTAL: 40MHz`, `DRAM: 128 MiB`, `NAND: 128 MiB` follow, all matching
`--cpufreq 880 --ramfreq 1200`.

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
