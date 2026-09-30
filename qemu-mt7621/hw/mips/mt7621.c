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
    hwaddr memmap[sizeof(mt7621_memmap) / sizeof(hwaddr)];
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

    /* QEMU 9.2 added the endianness argument to mips_cpu_create_with_clock. */
    s->cpu = mips_cpu_create_with_clock(MIPS_CPU_TYPE_NAME("24KEc"), cpuclk,
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
         * serial_hd() is a private helper with no stable declaration in the
         * 9.2 headers; qemu_chr_find() (chardev/char.h) is the documented
         * lookup. It returns NULL when the chardev was not created, which is
         * fine - the UART just stays disconnected and the console is unused.
         */
        Chardev *chr = qemu_chr_find(i == 0 ? "serial0" :
                                     i == 1 ? "serial1" : "serial2");
        if (chr) {
            qdev_prop_set_chr(DEVICE(s->uart[i]), "chardev", chr);
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
     * SPL and early U-Boot write their console to the 0xbe000c00 alias.
     * serial-mm only publishes a single MMIO region (index 0), so the
     * alias is a second mapping of that same region rather than a second
     * sysbus region; memory_region_add_subregion() takes the ref itself.
     */
    memory_region_add_subregion(get_system_memory(), s->memmap[DEV_DBG_UART],
                                sysbus_mmio_get_region(
                                    SYS_BUS_DEVICE(s->uart[0]), 0));

    /*
     * Everything else is modelled as unimplemented: the register reads return
     * zero and writes are dropped. That is enough for the SPL and U-Boot to
     * probe, and it makes a missing peripheral non-fatal instead of a bus
     * error, which is what lets the simulated boot make progress.
     */
    static const struct {
        const char *name;
        int dev;
        hwaddr size;
    } unimp[] = {
        { "sysc",      DEV_SYSC,     0x100 },
        { "wdt",       DEV_WDT,      0x100 },
        { "gpio",      DEV_GPIO,     0x600 },
        { "i2c",       DEV_I2C,      0x200 },
        { "spi",       DEV_SPI,      0x200 },
        { "gdma",      DEV_GDMA,     0x800 },
        { "crypto",    DEV_CRYPTO,   0x400 },
        { "dramc",     DEV_DRAMC,    0x1000 },
        { "eth",       DEV_ETH,      0xe000 },
        { "sdhci",     DEV_SDHCI,    0x2000 },
        { "pcie",      DEV_PCIE,     0x8000 },
        { "xhci",      DEV_XHCI,     0x2000 },
        { "usb-phy",   DEV_USB_PHY,  0x2000 },
        { "gic",       DEV_GIC,      0x20000 },
        { "cpc",       DEV_CPC,      0x2000 },
        { "cdmm",      DEV_CDMM,     0x2000 },
    };

    for (unsigned i = 0; i < ARRAY_SIZE(unimp); i++) {
        create_unimplemented_device(unimp[i].name, s->memmap[unimp[i].dev],
                                    unimp[i].size);
    }
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
