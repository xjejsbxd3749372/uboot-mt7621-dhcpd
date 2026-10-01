/*
 * MediaTek MT7621 SoC emulation and Xiaomi Mi Router 4 (R4) machine.
 *
 * The machine reproduces the parts of the MT7621 boot ROM that matter for a
 * simulated boot: it loads the NAND uImage payload into DRAM at the address
 * the uImage header asks for and enters the SPL there. From that point on
 * everything is the real U-Boot running against the emulated SoC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <stdio.h>
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "hw/char/serial.h"
#include "hw/char/serial-mm.h"
#include "chardev/char.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/mips/mips.h"
#include "hw/misc/unimp.h"
#include "hw/misc/mt7621-nfc.h"
#include "hw/qdev-properties.h"
#include "exec/memory.h"
#include "sysemu/reset.h"
#include "qom/object.h"
/* MIPSCPU, MIPS_CPU_TYPE_NAME, cpu_mips_*_init() live in the target headers,
 * which are on the include path for sources in mips_ss (see malta.c). */
#include "target/mips/cpu.h"

#define TYPE_MT7621_SOC "mt7621-soc"
#define TYPE_MI_ROUTER4 "mi-router-4"
OBJECT_DECLARE_SIMPLE_TYPE(MT7621SoCState, MT7621_SOC)

/*
 * Memory-backed placeholder for an address range the SoC has but the model
 * does not emulate. Unlike create_unimplemented_device(), which drops writes
 * and always reads back 0, this keeps whatever was stored - which matters
 * because the SPL uses part of the FE window as its initial stack:
 *
 *   arch/mips/mach-mt7621/spl/start.S:
 *       #define CONFIG_SYS_INIT_SP_ADDR 0xbe10d000
 *       lui  t1, 0xbe10 ; ori t1, 0xd000 ; and t9, t1, t0
 *
 * kseg1 0xbe10d000 is physical 0x1e10d000, inside MT7621_FE_BASE+MT7621_FE_SIZE
 * (0x1e100000 + 0xe000). With a write-dropping placeholder the saved registers
 * at sp-0x10 read back 0, so `jr $ra` lands at 0x0 and the SPL dies on a
 * reserved-instruction exception at PC 0x0000002c before it ever reaches the
 * NAND driver.
 */
typedef struct MT7621Scratch {
    uint8_t *buf;
    hwaddr size;
    const char *name;
    /* Boyer-Moore majority address: survives interleaved traffic from the
     * stack living in the same window, while a polling loop - one register
     * read over and over - stays dominant. */
    hwaddr dom_addr;
    uint64_t dom_count;
    uint64_t n;
    int64_t next_report;
    /*
     * Mirror rule: reads of mirror_dst answer with the word stored at
     * mirror_src. Only the CM window uses it, and mirror_dst == 0 disables it.
     */
    hwaddr mirror_src;
    hwaddr mirror_dst;
    /*
     * A hardware status register: the firmware reads it but nothing writes it,
     * so a plain read-back placeholder leaves it at 0 and the code asserts.
     * const_val is returned while the guest has not stored anything itself.
     *
     * arch/mips/mach-mt7621/clocks.c: mt7621_get_clocks() computes
     *   cpu_clk = cpu_clk / REG_GET_VAL(CUR_CPU_FDIV, cur_clk)
     *                * REG_GET_VAL(CUR_CPU_FFRAC, cur_clk);
     * so both halves of MT7621_SYS_CUR_CLK_STS_REG (0x44) must be non-zero.
     * FFRAC occupies bits 4:0; with it clear the product collapses to 0,
     * gd->cpu_clk stays 0, get_tbclk() returns 0 and tick_to_time()'s
     * do_div(tick, div) is a divide by zero - the teq at 0x80105844.
     */
    hwaddr const_reg;
    uint32_t const_val;
} MT7621Scratch;

/*
 * Gaps that must be readable/writable for the SPL to run. Every entry is
 * deliberately disjoint from the real devices (NAND controller at
 * 0x1e003000..0x1e004000, UARTs at 0x1e000c00/d00/e00 + 0x20 each) and from
 * the flash-mmap window at 0x1fc00000, so nothing overlaps and the question
 * of which region wins never arises.
 */
#define MT7621_N_SCRATCH 8
static const struct {
    hwaddr addr;
    hwaddr size;
    const char *name;
    hwaddr mirror_src;
    hwaddr mirror_dst;
    hwaddr const_reg;
    uint32_t const_val;
} mt7621_scratch_map[MT7621_N_SCRATCH] = {
    { 0x1e000000, 0x000c00, "mt7621-sysc-wdt-gpio", 0, 0,
    0x44, 0x00110101 },   /* CUR_CLK_STS: FDIV=1, FFRAC=1, OCP=1, SAME_FREQ */  /* up to UART0 */
    { 0x1e000c20, 0x0000e0, "mt7621-uart0-tail", 0, 0, 0, 0 },     /* ends 0xc20 */
    { 0x1e000d20, 0x0000e0, "mt7621-uart1-tail", 0, 0, 0, 0 },
    /*
     * Every window below is chosen so it cannot overlap a real device: the
     * NAND controller sits at 0x1e003000..0x1e004000, and letting a placeholder
     * cover it too put two same-priority regions over one address, which
     * corrupts the flatview section table - QEMU died in the NAND driver with
     * physmem.c: iotlb_to_section: Assertion `section_index < d->map.sections_nb'.
     */
    { 0x1e000e20, 0x0021e0, "mt7621-gdma-gap", 0, 0, 0, 0 },       /* ends at NFI */
    /*
     * DRAMC gets its own window on purpose. The boot chain runs the legacy
     * DDR calibration blob - mt7621_stage_sram_noprint.bin, exactly 13928
     * bytes, which is the count in the memcpy the PC sampler catches - from
     * the FE SRAM, and it sweeps these registers looking for timing/status
     * bits that a placeholder can never assert. Splitting it out means its
     * dominant-address report names the register it waits on instead of being
     * drowned out by instruction fetches from the blob itself.
     */
    { 0x1e004000, 0x001000, "mt7621-crypto-gap", 0, 0, 0, 0 },     /* after NFI ECC */
    { 0x1e005000, 0x001000, "mt7621-dramc", 0, 0, 0, 0 },
    { 0x1e006000, 0x01fa000, "mt7621-fe-and-friends", 0, 0, 0, 0 }, /* to 0x1e200000 */
    { 0x1fbc0000, 0x040000, "mt7621-cm", 0x3a008, 0x3c008, 0, 0 },
    /*
     * MIPS CM: join_coherent_domain() in arch/mips/mach-mt7621/launch_ll.S
     * writes the whole core mask to GCR_Cx_COHERENCE (CDMM+0x2008 = window
     * offset 0x3a008), then for each core selects it through GCR_CL_OTHER
     * (CDMM+0x2018) and spins on GCR_CO_COHERENCE (CDMM+0x4008 = window
     * offset 0x3c008) until it reads back non-zero. GCR_CO_* is the view of
     * the core selected by GCR_CL_OTHER, and nothing else in the window is
     * ever written at 0x3c008, so without this mirror the read stays 0 and
     * the SPL spins there forever - which is exactly where the PC sampler
     * found it, at 0x80100c94.
     */
};



/* MT7621 memory map (target/linux/ramips/dts/mt7621.dtsi) */
enum {
    DEV_SDRAM = 0,
    DEV_SYSC,        /* 0x1e000000 */
    DEV_WDT,         /* 0x1e000100 */
    DEV_GPIO,        /* 0x1e000600 */
    DEV_I2C,         /* 0x1e000900 */
    DEV_SPI,         /* 0x1e000b00 */
    DEV_UART1,       /* 0x1e000c00 */
    DEV_UART2,       /* 0x1e000d00 */
    DEV_UART3,       /* 0x1e000e00 */
    DEV_GDMA,        /* 0x1e002800 */
    DEV_NFI,         /* 0x1e003000 */
    DEV_NFI_ECC,     /* 0x1e003800 */
    DEV_CRYPTO,      /* 0x1e004000 */
    DEV_DRAMC,       /* 0x1e005000 */
    DEV_ETH,         /* 0x1e100000 */
    DEV_SDHCI,       /* 0x1e130000 */
    DEV_PCIE,        /* 0x1e140000 */
    DEV_XHCI,        /* 0x1e1c0000 */
    DEV_USB_PHY,     /* 0x1e1d0000 */
    DEV_GIC,         /* 0x1fbc0000 */
    DEV_CPC,         /* 0x1fbf0000 */
    DEV_CDMM,        /* 0x1fbf8000 */
    DEV_DBG_UART,    /* 0xbe000c00, SPL debug console alias */
};

static const hwaddr mt7621_memmap[] = {
    [DEV_SDRAM]    = 0x00000000,
    [DEV_SYSC]     = 0x1e000000,
    [DEV_WDT]      = 0x1e000100,
    [DEV_GPIO]     = 0x1e000600,
    [DEV_I2C]      = 0x1e000900,
    [DEV_SPI]      = 0x1e000b00,
    [DEV_UART1]    = 0x1e000c00,
    [DEV_UART2]    = 0x1e000d00,
    [DEV_UART3]    = 0x1e000e00,
    [DEV_GDMA]     = 0x1e002800,
    [DEV_NFI]      = 0x1e003000,
    [DEV_NFI_ECC]  = 0x1e003800,
    [DEV_CRYPTO]   = 0x1e004000,
    [DEV_DRAMC]    = 0x1e005000,
    [DEV_ETH]      = 0x1e100000,
    [DEV_SDHCI]    = 0x1e130000,
    [DEV_PCIE]     = 0x1e140000,
    [DEV_XHCI]     = 0x1e1c0000,
    [DEV_USB_PHY]  = 0x1e1d0000,
    [DEV_GIC]      = 0x1fbc0000,
    [DEV_CPC]      = 0x1fbf0000,
    [DEV_CDMM]     = 0x1fbf8000,
    [DEV_DBG_UART] = 0xbe000c00,
};

struct MT7621SoCState {
    DeviceState parent_obj;
    MIPSCPU *cpu;
    /*
     * SerialMM is only forward-declared by hw/char/serial-mm.h in 9.2
     * (the struct definition lives in that header, but the device is
     * always realized through qdev), so hold pointers rather than values.
     */
    SerialMM *uart[3];
    SysBusDevice *nfc;
    QEMUTimer *pc_sample;
    MemoryRegion dramc_stub;
    /* region-transition trace: which 4KB page the CPU is running in, in order */
    hwaddr last_region;
    int n_regions;
    MemoryRegion flash_mmap_mr;
    MT7621Scratch scratch[MT7621_N_SCRATCH];
    MemoryRegion scratch_mr[MT7621_N_SCRATCH];
    hwaddr memmap[sizeof(mt7621_memmap) / sizeof(hwaddr)];
};

/*
 * MT7621_FLASH_MMAP_BASE - the SoC maps flash at physical 0x1fc00000, which is
 * also where MIPS parks the BEV=1 exception vectors (general exception at
 * 0xBFC00380 = kseg1 of 0x1fc00380). This board's own SPL relies on it:
 * arch/mips/mach-mt7621/spl/start.S lays its ROM exception vectors out with
 * `.org 0x380` and comments "we need spaces for storing stage1 header required
 * by BootROM". Without a region here a single exception strands the CPU
 * spinning on an unmapped fetch - that produced a 4.2GB serial log and no
 * boot - because QEMU retries the rejected read forever instead of advancing.
 *
 * The window is filled from the NAND image, which is what the hardware does.
 */
#define MT7621_FLASH_MMAP_BASE 0x1fc00000
#define MT7621_FLASH_MMAP_SIZE 0x400000   /* up to the top of kseg1 (0x1ffffff0) */

static uint64_t mt7621_flash_mmap_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621SoCState *soc = opaque;
    mt7621NfcState *nfc = MT7621_NFC(soc->nfc);
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint64_t b = 0xff;             /* erased flash outside the image */

        if (nfc->data && addr + i < nfc->size) {
            b = nfc->data[addr + i];
        }
        v |= b << (8 * i);             /* DEVICE_LITTLE_ENDIAN */
    }
    return v;
}

static void mt7621_flash_mmap_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    /* The real controller only updates the window base here. Dropping writes
     * is enough, and avoids a log line per store from a spinning guest. */
}

static const MemoryRegionOps mt7621_flash_mmap_ops = {
    .read = mt7621_flash_mmap_read,
    .write = mt7621_flash_mmap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * One report per region per second naming the address that dominates the
 * traffic. This exists because the run before it was a complete blank: 90s of
 * guest execution with no exception and no console output. Knowing the
 * dominant address turns "it is spinning on something" into "it is spinning
 * on this register".
 */
static void mt7621_scratch_touch(MT7621Scratch *sc, hwaddr addr)
{
    int64_t now_ms;

    sc->n++;
    if (addr == sc->dom_addr) {
        sc->dom_count++;
    } else if (sc->dom_count > 0) {
        sc->dom_count--;
    } else {
        sc->dom_addr = addr;
        sc->dom_count = 1;
    }

    now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now_ms >= sc->next_report) {
        sc->next_report = now_ms + 1000;
        fprintf(stderr,
                "scratch[%s]: accesses=%llu dominant=0x%llx count=%llu "
                "last=0x%llx\n",
                sc->name, (unsigned long long)sc->n,
                (unsigned long long)sc->dom_addr,
                (unsigned long long)sc->dom_count,
                (unsigned long long)addr);
    }
}

static uint32_t stored_word(MT7621Scratch *sc, hwaddr addr)
{
    uint32_t w = 0;
    unsigned i;

    for (i = 0; i < 4 && addr + i < sc->size; i++) {
        w |= (uint32_t)sc->buf[addr + i] << (8 * i);
    }
    return w;
}

static uint64_t mt7621_scratch_read(void *opaque, hwaddr addr, unsigned size)
{
    MT7621Scratch *sc = opaque;
    uint64_t v = 0;
    unsigned i;

    mt7621_scratch_touch(sc, addr);
    if (sc->mirror_dst && addr == sc->mirror_dst) {
        addr = sc->mirror_src;
    } else if (sc->const_reg && addr == sc->const_reg && !stored_word(sc, addr)) {
        /* hardware status the firmware reads but nothing writes */
        for (i = 0; i < size; i++) {
            v |= (uint64_t)((sc->const_val >> (8 * i)) & 0xff) << (8 * i);
        }
        return v;
    }
    for (i = 0; i < size; i++) {
        v |= (addr + i < sc->size) ? (uint64_t)sc->buf[addr + i] << (8 * i)
                                   : (uint64_t)0xff << (8 * i);
    }
    return v;
}

static void mt7621_scratch_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    MT7621Scratch *sc = opaque;
    unsigned i;

    mt7621_scratch_touch(sc, addr);
    for (i = 0; i < size && addr + i < sc->size; i++) {
        sc->buf[addr + i] = val >> (8 * i);
    }
}

static const MemoryRegionOps mt7621_scratch_ops = {
    .read = mt7621_scratch_read,
    .write = mt7621_scratch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};



/*
 * Execution sampler.
 *
 * The runs that mattered were completely silent: 90s with no exception, no
 * unmapped access and no console output. Nothing in the model's event log
 * says where the guest actually is, and "it produced no lines" is not a
 * diagnosis. Sampling the PC every 100ms costs ~900 short lines for the whole
 * window and turns that into a precise answer - fixed PC means a spin loop
 * (which address, so it can be mapped back to the SPL), moving PC means it is
 * progressing quietly, PC in RAM in the LZMA block means it is decompressing.
 *
 * QEMU_CLOCK_REALTIME rather than VIRTUAL so the samples keep coming even if
 * the guest wedges in a halt.
 */
static void mt7621_pc_sample(void *opaque)
{
    MT7621SoCState *s = opaque;
    const CPUMIPSState *env = &s->cpu->env;

    {
        hwaddr region = ((target_ulong)env->active_tc.PC) >> 12;

        if (region != s->last_region) {
            s->last_region = region;
            if (s->n_regions < 500) {
                s->n_regions++;
                fprintf(stderr, "mt7621-region[%d]: pc=" TARGET_FMT_lx
                        " sp=" TARGET_FMT_lx " t9=" TARGET_FMT_lx "\n",
                        s->n_regions, (target_ulong)env->active_tc.PC,
                        (target_ulong)env->active_tc.gpr[29],
                        (target_ulong)env->active_tc.gpr[25]);
            }
        }
    }

    /*
     * sp/ra/t9 answer the question the PC alone cannot: whether the code
     * running in the FE SRAM window (0x1e108000..0x1e10c000, right under the
     * stack top 0x1e10d000) is a routine the SPL deliberately relocated there
     * - sp inside 0xbe10xxxx and ra pointing back into the SPL image - or the
     * CPU executing its own stack as instructions, which means a bad jump.
     */
    fprintf(stderr,
            "mt7621-sample: pc=" TARGET_FMT_lx " sp=" TARGET_FMT_lx
            " ra=" TARGET_FMT_lx " t9=" TARGET_FMT_lx
            " epc=" TARGET_FMT_lx " status=" TARGET_FMT_lx "\n",
            (target_ulong)env->active_tc.PC,
            (target_ulong)env->active_tc.gpr[29],
            (target_ulong)env->active_tc.gpr[31],
            (target_ulong)env->active_tc.gpr[25],
            (target_ulong)env->CP0_EPC,
            (target_ulong)env->CP0_Status);
    timer_mod(s->pc_sample,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + 20 * 1000000ULL);
}

/*
 * DDR calibration stub.
 *
 * With CONFIG_MT7621_LEGACY_DRAMC_BIN the SPL copies
 * arch/mips/mach-mt7621/dramc-legacy/mt7621_stage_sram_noprint.bin - exactly
 * 13928 bytes, which is the literal `addiu t3, $0, 13928' in its memcpy - into
 * the FE SRAM at 0xbe108800 and jumps there to calibrate the memory
 * controller over 0x1e005000. That blob sweeps timing registers and waits on
 * status bits a placeholder can never assert, so it never completes: the PC
 * sampler showed the same three-step cycle - copy, run SRAM, come back - from
 * the first sample to the last, and preloader_console_init() sits after DRAM
 * init, which is why nothing was ever printed to the console.
 *
 * QEMU hands the guest working RAM without calibration, so the honest thing
 * for this model is to make the blob a no-op that returns to its caller. Only
 * the first two instructions are intercepted; the rest of the SRAM window
 * still behaves normally.
 *
 *   0x03e00008   jr ra
 *   0x00000000   nop
 */
static uint64_t mt7621_dramc_stub_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    static const uint8_t stub[8] = { 0x08, 0x00, 0xe0, 0x03,
                                     0x00, 0x00, 0x00, 0x00 };
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < sizeof(stub); i++) {
        v |= (uint64_t)stub[addr + i] << (8 * i);
    }
    return v;
}

/*
 * Writes are accepted and dropped. A NULL write handler would make QEMU
 * report the copy of the blob into this address as a rejected write and raise
 * a bus error, and the read handler always answers with the two stub
 * instructions regardless of what was stored, so the stub survives the copy
 * that overwrites everything around it.
 */
static void mt7621_dramc_stub_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
}

static const MemoryRegionOps mt7621_dramc_stub_ops = {
    .read = mt7621_dramc_stub_read,
    .write = mt7621_dramc_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt7621_soc_init(Object *obj)
{
    MT7621SoCState *s = MT7621_SOC(obj);

    for (unsigned i = 0; i < sizeof(s->memmap) / sizeof(s->memmap[0]); i++) {
        s->memmap[i] = mt7621_memmap[i];
    }
    for (int i = 0; i < 3; i++) {
        s->uart[i] = SERIAL_MM(qdev_new(TYPE_SERIAL_MM));
        object_property_add_child(obj, "uart[*]", OBJECT(s->uart[i]));
    }
    s->nfc = SYS_BUS_DEVICE(qdev_new(TYPE_MT7621_NFC));
    object_property_add_child(obj, "nfc", OBJECT(s->nfc));
}

static void mt7621_soc_realize(DeviceState *dev, Error **errp)
{
    MT7621SoCState *s = MT7621_SOC(dev);
    Clock *cpuclk;

    cpuclk = clock_new(OBJECT(dev), "cpu-refclk");
    /* MT7621 runs at 1GHz nominal; the exact rate only affects timeouts. */
    clock_set_hz(cpuclk, 1000000000);

    /*
     * 34Kf, not 24KEc - and that matters. The SPL's startup code in
     * arch/mips/mach-mt7621/spl/start.S does:
     *
     *     mfc0 t0, $2, 2     # QEMU maps this to TCBind, gated on Config3.MT
     *     andi t0, t0, 0xf
     *     bne  t0, $0, <shadow/secondary path>
     *
     * On 24KEc (which has no Config3.MT and no FPU - QEMU's def comment is
     * literally "we have a DSP, but no FPU") disas_mt_available() is false,
     * so the read lands in gen_mfc0's cp0_unimplemented branch and returns
     * ~0. That makes the branch taken, the SPL runs a secondary-thread path
     * it should not, and execution ends up executing COP1 with no FPU:
     *
     *   do_raise_exception_err: 19 (coprocessor unusable) 1
     *   mips_cpu_do_interrupt: PC bfc00380 EPC a0000f1c cause 11
     *
     * 34Kf sets CP0C3_MT and CP0C1_FP, so TCBind reads back 0 for TC0, the
     * branch is not taken, and a COP1 instruction is legal. The real core is
     * MT-capable - this tree includes asm/mipsmtregs.h, cps.c and launch.c
     * for routing BootROM to the second core.
     *
     * QEMU 9.2 also added the endianness argument here.
     */
    s->cpu = mips_cpu_create_with_clock(MIPS_CPU_TYPE_NAME("34Kf"), cpuclk,
                                        TARGET_BIG_ENDIAN);
    cpu_mips_irq_init_cpu(s->cpu);
    cpu_mips_clock_init(s->cpu);

    /* NAND flash controller */
    sysbus_realize(s->nfc, errp);
    sysbus_mmio_map(s->nfc, 0, s->memmap[DEV_NFI]);
    sysbus_mmio_map(s->nfc, 1, s->memmap[DEV_NFI_ECC]);

    /* three 16550 UARTs with a 2-bit register shift */
    for (int i = 0; i < 3; i++) {
        /*
         * serial_hd() has no declaration in the 9.2 headers; qemu_chr_find()
         * from chardev/char.h is the documented lookup and resolves the same
         * object, because -serial labels its chardev "serialN" and chardevs
         * are registered as children of the chardevs root by label.
         */
        Chardev *chr = qemu_chr_find(i == 0 ? "serial0" :
                                     i == 1 ? "serial1" : "serial2");
        if (chr) {
            qdev_prop_set_chr(DEVICE(s->uart[i]), "chardev", chr);
        } else {
            warn_report("mt7621: no chardev serial%d, UART%d will print "
                        "nothing to the console", i, i + 1);
        }
        qdev_prop_set_uint8(DEVICE(s->uart[i]), "regshift", 2);
        qdev_prop_set_uint8(DEVICE(s->uart[i]), "endianness",
                            TARGET_BIG_ENDIAN ? DEVICE_BIG_ENDIAN
                                              : DEVICE_LITTLE_ENDIAN);
        sysbus_realize(SYS_BUS_DEVICE(s->uart[i]), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->uart[i]), 0,
                        s->memmap[DEV_UART1 + i]);
    }

    /*
     * UART0 at 0x1e000c00 serves both console addresses: MIPS kseg1
     * 0xbe000c00 translates to physical 0x1e000c00 (kseg0 and kseg1 both mask
     * to phys = addr & 0x1fffffff) and QEMU's system_memory is indexed by
     * physical address. So there is deliberately no region at physical
     * 0xbe000c00, and no second mapping of the UART region either - a
     * MemoryRegion has exactly one container, and mapping it twice trips
     * memory_region_add_subregion_common()'s `!subregion->container' assert.
     */

    /* flash memory-mapped window, also the MIPS BEV=1 exception vectors */
    memory_region_init_io(&s->flash_mmap_mr, OBJECT(s),
                          &mt7621_flash_mmap_ops, s, "mt7621-flash-mmap",
                          MT7621_FLASH_MMAP_SIZE);
    memory_region_add_subregion(get_system_memory(), MT7621_FLASH_MMAP_BASE,
                                &s->flash_mmap_mr);

    /*
     * Memory-backed gaps, see MT7621Scratch above: unlike
     * create_unimplemented_device() these keep what was written, which the
     * SPL depends on because its initial stack (CONFIG_SYS_INIT_SP_ADDR
     * 0xbe10d000 = kseg1 of 0x1e10d000) sits inside the FE window, and
     * because it probes the CM registers in CDMM at offset 0x2028.
     */
    /*
     * Installed before the generic windows and at priority 1, so it hides
     * them where it overlaps (memory_region_add_subregion_overlap resolves
     * "higher priority hides lower priority").
     */
    memory_region_init_io(&s->dramc_stub, OBJECT(s),
                          &mt7621_dramc_stub_ops, s, "mt7621-dramc-stub", 8);
    memory_region_add_subregion_overlap(get_system_memory(), 0x1e108800,
                                        &s->dramc_stub, 1);

    /*
     * Fill the two holes between RAM and the peripheral windows.
     *
     * post_lowlevel_init() does `gd->ram_size = get_ram_size((void *)KSEG1,
     * SZ_512M)', which walks memory up to 512MB looking for the end of RAM.
     * This board has 128MB, and with the range above it simply unmapped, the
     * first probe past 128MB produced
     *
     *   Invalid read at addr 0x10000000, size 4, region '(null)', rejected
     *     -> data bus error (cause 7)
     *     -> vector at 0xbfc00380, which is our flash-mmap window full of
     *        image bytes, which then executed garbage and raised a trap
     *        (cause 13), looping forever before the console was ever inited.
     *
     * create_unimplemented_device() is exactly right here: reads return 0 so
     * the pattern test fails and get_ram_size() correctly reports 128MB, and
     * writes are dropped, so nothing faults. These two regions plus RAM, the
     * peripheral window, the GIC/CPC/CDMM window and flash-mmap leave no hole
     * anywhere in the first 512MB.
     */
    create_unimplemented_device("mt7621-dram-hole", 0x08000000,
                                0x1e000000 - 0x08000000);
    create_unimplemented_device("mt7621-apu-hole", 0x1e200000,
                                0x1fbc0000 - 0x1e200000);

    for (unsigned i = 0; i < MT7621_N_SCRATCH; i++) {
        s->scratch[i].size = mt7621_scratch_map[i].size;
        s->scratch[i].name = mt7621_scratch_map[i].name;
        s->scratch[i].mirror_src = mt7621_scratch_map[i].mirror_src;
        s->scratch[i].mirror_dst = mt7621_scratch_map[i].mirror_dst;
        s->scratch[i].const_reg = mt7621_scratch_map[i].const_reg;
        s->scratch[i].const_val = mt7621_scratch_map[i].const_val;
        s->scratch[i].buf = g_malloc0(s->scratch[i].size);
        memory_region_init_io(&s->scratch_mr[i], OBJECT(s),
                              &mt7621_scratch_ops, &s->scratch[i],
                              mt7621_scratch_map[i].name, s->scratch[i].size);
        memory_region_add_subregion(get_system_memory(),
                                    mt7621_scratch_map[i].addr,
                                    &s->scratch_mr[i]);
    }

    /* start the PC sampler: one sample every 20ms of wall time */
    s->pc_sample = timer_new(QEMU_CLOCK_REALTIME, SCALE_NS,
                             mt7621_pc_sample, s);
    timer_mod(s->pc_sample, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                            20 * 1000000ULL);

}

static void mt7621_soc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt7621_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo mt7621_soc_info = {
    .name = TYPE_MT7621_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(MT7621SoCState),
    .instance_init = mt7621_soc_init,
    .class_init = mt7621_soc_class_init,
};

static void mt7621_soc_register_types(void)
{
    type_register_static(&mt7621_soc_info);
}
type_init(mt7621_soc_register_types)

/* ------------------------------------------------------------------ */
/* Xiaomi Mi Router 4 (R4) machine                                    */
/* ------------------------------------------------------------------ */

/* the NAND image path, from -mt7621-flash <path> */
static char *mi_router4_flash_file;

typedef struct MiRouter4Reset {
    MT7621SoCState *soc;
    uint64_t vector;
} MiRouter4Reset;

static void mi_router4_cpu_reset(void *opaque)
{
    MiRouter4Reset *r = opaque;
    CPUMIPSState *env = &r->soc->cpu->env;

    cpu_reset(CPU(r->soc->cpu));
    env->active_tc.PC = r->vector & ~(target_ulong)1;
}

/*
 * Big-endian uImage, as produced by mkimage for this board.
 *   0x00 magic, 0x0c size, 0x10 load, 0x14 entry, 0x40 data
 */
#define UIMAGE_MAGIC 0x27051956u

static bool load_spl_from_nand(MachineState *machine, MT7621SoCState *soc,
                               hwaddr *entry_out)
{
    mt7621NfcState *nfc = MT7621_NFC(soc->nfc);
    uint8_t hdr[0x40];
    uint32_t magic, size, load, entry;

    if (!nfc->data) {
        error_report("mi-router-4: -mt7621-flash <image> is required");
        return false;
    }
    if (nfc->size < sizeof(hdr)) {
        error_report("mi-router-4: flash image is only %" PRIu64 "B",
                     nfc->size);
        return false;
    }

    memcpy(hdr, nfc->data, sizeof(hdr));

    magic = ldl_be_p(hdr);
    if (magic != UIMAGE_MAGIC) {
        error_report("mi-router-4: no uImage at NAND offset 0 (magic 0x%08x)",
                     magic);
        return false;
    }

    size = ldl_be_p(hdr + 0x0c);
    load = ldl_be_p(hdr + 0x10);
    entry = ldl_be_p(hdr + 0x14);

    /*
     * The boot ROM copies the image payload to the address in the header.
     * U-Boot for this board is linked at 0x80200000, which is the kseg0 alias
     * of physical 0x00200000.
     */
    hwaddr phys = load & 0x1fffffff;
    uint8_t *buf;

    if ((uint64_t)size + 0x40 > nfc->size) {
        error_report("mi-router-4: SPL payload of %uB runs past the flash image",
                     size);
        return false;
    }

    buf = g_malloc(size);
    memcpy(buf, nfc->data + 0x40, size);

    if (phys + (hwaddr)size > machine->ram_size) {
        error_report("mi-router-4: SPL load 0x%" HWADDR_PRIx " + %u exceeds RAM",
                     phys, size);
        g_free(buf);
        return false;
    }

    cpu_physical_memory_write(phys, buf, size);
    g_free(buf);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "mi-router-4: loaded SPL %u bytes to phys 0x%" HWADDR_PRIx
                  ", entry 0x%08" PRIx32 "\n", size, phys, entry);

    *entry_out = entry;
    return true;
}

/*
 * Read the whole NAND image into memory. See the comment on nfc->data: this
 * is deliberately plain stdio rather than a block backend, because every
 * block-layer entry point a device could use moved or vanished in 9.2.
 */
static bool load_flash_image(mt7621NfcState *nfc, const char *path)
{
    FILE *fp;
    long len;
    uint8_t *buf;

    fp = fopen(path, "rb");
    if (!fp) {
        error_report("mi-router-4: cannot open NAND image '%s'", path);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        error_report("mi-router-4: cannot seek in '%s'", path);
        fclose(fp);
        return false;
    }
    len = ftell(fp);
    if (len <= 0) {
        error_report("mi-router-4: '%s' is empty", path);
        fclose(fp);
        return false;
    }
    rewind(fp);

    buf = g_malloc(len);
    if (fread(buf, 1, len, fp) != (size_t)len) {
        error_report("mi-router-4: short read of '%s'", path);
        g_free(buf);
        fclose(fp);
        return false;
    }
    fclose(fp);

    nfc->data = buf;
    nfc->size = len;
    return true;
}

static void mi_router4_init(MachineState *machine)
{
    MT7621SoCState *soc;
    MiRouter4Reset *reset;
    uint64_t entry = 0x80200000;

    soc = MT7621_SOC(object_new(TYPE_MT7621_SOC));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));

    /*
     * The image path normally arrives as -M mi-router-4,flash=<path>. The
     * MI_ROUTER4_FLASH fallback covers the case where a QEMU upgrade changes
     * the point at which machine properties are applied relative to mc->init,
     * so a wrong ordering costs a clear warning instead of a blank device.
     */
    {
        const char *path = mi_router4_flash_file;
        char *env = NULL;

        if (!path || !*path) {
            env = g_strdup(g_getenv("MI_ROUTER4_FLASH"));
            path = env;
        }
        if (path && *path) {
            if (!load_flash_image(MT7621_NFC(soc->nfc), path)) {
                warn_report("mi-router-4: could not load NAND image '%s'", path);
            }
        } else {
            warn_report("mi-router-4: no NAND image given "
                        "(use -M mi-router-4,flash=<path>)");
        }
        g_free(env);
    }

    qdev_realize(DEVICE(soc), NULL, &error_abort);

    /* MT7621 DDR starts at physical 0; machines map machine->ram themselves */
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    if (!load_spl_from_nand(machine, soc, &entry)) {
        /* still start the CPU so -d guest_errors shows why it stopped */
        warn_report("mi-router-4: no SPL loaded, entering reset vector anyway");
    }

    reset = g_new0(MiRouter4Reset, 1);
    reset->soc = soc;
    reset->vector = entry;
    qemu_register_reset(mi_router4_cpu_reset, reset);
}

/*
 * -M mi-router-4,flash=<path>
 *
 * object_class_property_add_str() is the API that takes these signatures.
 * The generic object_class_property_add(..., "string", ...) expects
 * ObjectPropertyAccessor callbacks that take a Visitor; registering a
 * char *( *)(Object *, ...) getter there compiles with a warning, the setter
 * then never runs visit_type_str(), and QEMU aborts at
 * qobject_input_check_struct() with "Parameter 'flash' is unexpected" -
 * the key was never consumed by a visitor.
 */
static char *mi_router4_get_flash(Object *obj, Error **errp)
{
    return g_strdup(mi_router4_flash_file);
}

static void mi_router4_set_flash(Object *obj, const char *value, Error **errp)
{
    g_free(mi_router4_flash_file);
    mi_router4_flash_file = g_strdup(value);
}

static void mi_router4_class_init(MachineClass *mc)
{
    mc->desc = "Xiaomi Mi Router 4 (R4) - MediaTek MT7621 (mipsel)";
    mc->init = mi_router4_init;
    mc->default_ram_size = 128 * MiB;
    mc->default_ram_id = "mi-router-4.ram";

    object_class_property_add_str(OBJECT_CLASS(mc), "flash",
                                  mi_router4_get_flash, mi_router4_set_flash);
}

DEFINE_MACHINE("mi-router-4", mi_router4_class_init)
