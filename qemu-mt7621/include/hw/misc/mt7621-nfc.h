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
/* QEMU 9.2 has no public include/block/block-backend.h; BlockConf (and the
 * BlockBackend typedef) are declared by hw/block/block.h, which forwards to
 * system/block-backend-common.h. */
#include "hw/block/block.h"

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
     * Backing store: the whole 128MiB NAND image, logical layout
     * (2048B of page data per page, no OOB interleaved).
     *
     * QEMU 9.2 dropped DEFINE_PROP_BLOCK, so the backend is carried by a
     * BlockConf; conf.blk is the BlockBackend handed to us by the
     * "drive" property and is released by QEMU when the device is gone.
     */
    BlockConf conf;

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
    int cmd_ready;          /* STA_CMD clear */

    /* page being read into the PIO stream */
    uint8_t page_buf[MT7621_NFC_PAGESIZE_TOTAL];
    uint32_t stream_pos;    /* byte offset into page_buf served by NFI_DATAR */
    int page_armed;         /* READ0+address+READSTART done, data available */
    int page_fmt;           /* AUTO_FMT_EN: only 2048B data on the PIO stream */

    /* statistics, printed on request via info properties */
    uint64_t n_page_reads;
    uint64_t n_cmds;
};

#endif /* HW_MISC_MT7621_NFC_H */
