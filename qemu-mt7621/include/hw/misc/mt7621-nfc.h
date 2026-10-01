/*
 * MediaTek MT7621 NAND Flash Controller (NFI + ECC) emulation
 *
 * Derived from the register definitions and access patterns in
 * drivers/mtd/nand/mt7621_nand.{c,h} of this U-Boot fork.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MT7621_NFC_H
#define HW_MISC_MT7621_NFC_H

#include "qom/object.h"
#include "hw/sysbus.h"
#include "exec/memory.h"

/* MT7621 NAND geometry as configured for Mi Router 4 (128MB SPI-NAND). */
#define MT7621_NFC_PAGE_SIZE      2048
#define MT7621_NFC_OOB_SIZE       64
#define MT7621_NFC_PAGESIZE_TOTAL (MT7621_NFC_PAGE_SIZE + MT7621_NFC_OOB_SIZE)
#define MT7621_NFC_BLOCK_SIZE     (128 * 1024)
#define MT7621_NFC_PAGES_PER_BLK  (MT7621_NFC_BLOCK_SIZE / MT7621_NFC_PAGE_SIZE)
#define MT7621_NFC_ECC_STEPS      (MT7621_NFC_PAGE_SIZE / 512)
#define MT7621_NFC_SPARE_PER_SEC  16
#define MT7621_NFC_FDM_SIZE       8

#define TYPE_MT7621_NFC "mt7621-nfc"
OBJECT_DECLARE_SIMPLE_TYPE(mt7621NfcState, MT7621_NFC)

struct mt7621NfcState {
    SysBusDevice parent_obj;

    /* 0x1e003000 NFI register block, 0x1e003800 ECC register block */
    MemoryRegion nfi_mr;
    MemoryRegion ecc_mr;

    /*
     * Backing store: the whole NAND image, held in host memory.
     *
     * QEMU 9.2 deleted the non-coroutine blk_pread() and DEFINE_PROP_BLOCK
     * that a device like this used to read a BlockBackend from, and the
     * surviving replacements all take a BdrvChild that is only reachable
     * through private block-layer structures. Rather than chase that, the
     * machine loads the image once and hands the buffer over: a read is then
     * a memcpy, with no dependency on any of the block APIs that moved. The
     * cost is 128MiB of host RAM and no -drive passthrough, which is the right
     * trade for a model whose only job is to boot U-Boot. Writes modify this
     * buffer and are never flushed, so a simulated boot cannot damage the
     * dump it was given.
     */
    uint8_t *data;
    uint64_t size;

    /* registers the guest can read back */
    uint16_t cnfg, con, pagefmt, strdata, csr, iocon, mastersta;
    uint32_t acccon, straddr;
    uint32_t coladdr, rowaddr, addrnob;
    uint32_t fdml[MT7621_NFC_ECC_STEPS];
    uint32_t fdmm[MT7621_NFC_ECC_STEPS];

    /* ECC engine registers */
    uint16_t encccon, encidle, deccon, decidle, decdone;

    /* SPI-NAND command phase state */
    uint8_t cmd;
    uint8_t addr_byte[8];
    int addr_n;
    int in_addr_phase;      /* an address byte was written since the last command */

    /* page being read into the PIO stream */
    uint8_t page_buf[MT7621_NFC_PAGESIZE_TOTAL];
    uint32_t stream_pos;    /* byte offset into page_buf served by NFI_DATAR */
    int page_armed;         /* READ0+address+READSTART done, data available */
    int page_fmt;           /* AUTO_FMT_EN: only 2048B data on the PIO stream */

    /*
     * What the next PIO data phase must deliver. The command arrives before
     * the address cycle, so a page cannot be fetched when the command is
     * decoded; the first NFI_DATAR read arms it instead. NFC_DATA_ID covers
     * NAND_CMD_READID, whose bytes come from the id[] below rather than from
     * the backing image.
     */
    int pending_data;       /* NFC_DATA_NONE / _PAGE / _ID */

    /* statistics, printed on request via info properties */
    uint64_t n_page_reads;
    uint64_t n_cmds;

    /*
     * Bounded access trace. The SPL can sit in a silent NAND polling loop for
     * the whole 90s window with no exception and no console output, which
     * makes the run impossible to diagnose; the first few register accesses
     * name the state machine it is stuck in.
     */
    uint64_t n_trace;
    /*
     * Separate, smaller budget for page loads: the interesting question once
     * the SPL falls through to ymodem is not which register was poked but
     * which pages were fetched and whether they held image data at all.
     */
    uint64_t n_page_trace;
};

#endif /* HW_MISC_MT7621_NFC_H */
