/*
 * MediaTek MT7621 NAND Flash Controller (NFI + ECC) emulation
 *
 * Register map and access semantics taken from drivers/mtd/nand/mt7621_nand.c
 * in this U-Boot fork. The device serves page reads out of a backing NAND
 * dump so that SPL, U-Boot MTD and UBI see the real partition contents.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/qdev-properties.h"
#include "hw/misc/mt7621-nfc.h"

/* ---- NFI register offsets (from mt7621_nand.h) ---- */
#define NFI_CNFG_REG16      0x000
#define   OP_MODE_S         12
#define   OP_CUSTOM         6
#define   AUTO_FMT_EN_S      9
#define   HW_ECC_EN_S         8
#define   BYTE_RW_S          6
#define   READ_MODE_S        1
#define NFI_PAGEFMT_REG16   0x004
#define NFI_CON_REG16       0x008
#define   NFI_SEC_S         12
#define   NFI_BWR_S          9
#define   NFI_BRD_S          8
#define   NFI_FIFO_FLUSH_S   0
#define   NFI_STM_RST_S      1
#define NFI_ACCCON_REG32    0x00c
#define NFI_INTR_REG16      0x014
#define NFI_CMD_REG16       0x020
#define NFI_ADDRNOB_REG16   0x030
#define NFI_COLADDR_REG32   0x034
#define NFI_ROWADDR_REG32   0x038
#define NFI_STRDATA_REG16   0x040
#define NFI_DATAW_REG32     0x050
#define NFI_DATAR_REG32     0x054
#define NFI_PIO_DIRDY_REG16 0x058
#define NFI_STA_REG32       0x060
#define   NAND_FSM_S        24
#define   FSM_IDLE           0
#define   FSM_CUSTOM_DATA   14
#define   BUSY_S             8
#define   STA_CMD_S          0
#define NFI_FIFOSTA_REG16   0x064
#define   RD_REMAIN_S        0
#define   RD_REMAIN_M     0x1f
#define   RD_EMPTY_S         6
#define NFI_ADDRCNTR_REG16  0x070
#define   SEC_CNTR_S        12
#define   SEC_ADDR_S         0
#define NFI_CSEL_REG16      0x090
#define NFI_FDML_REG32(n)   (0x0a0 + ((n) << 3))
#define NFI_FDMM_REG32(n)   (0x0a4 + ((n) << 3))
#define NFI_MASTERSTA_REG16 0x210
#define   MAS_ADDR_S        9

/* ---- ECC register offsets ---- */
#define ECC_ENCCON_REG16    0x000
#define   ENC_EN_S           0
#define   ENCIDLE_S          0
#define ECC_ENCIDLE_REG16   0x00c
#define ECC_DECCON_REG16    0x100
#define   DEC_EN_S           0
#define ECC_DECIDLE_REG16   0x10c
#define ECC_DECENUM_REG32   0x114
#define ECC_DECDONE_REG16   0x118
#define ECC_FDMADDR_REG32   0x13c

/*
 * NAND commands, as include/linux/mtd/rawnand.h defines them in this tree.
 * READSTART used to be written as 0x0f here, which never matches anything the
 * driver sends: rawnand.h says 0x30, and it is the command that closes the
 * address cycle of a large-page read.
 */
#define NAND_CMD_READ0       0x00
#define NAND_CMD_READSTART   0x30
#define NAND_CMD_RESET       0xff
#define NAND_CMD_READID      0x90
#define NAND_CMD_PAGE_READ   0x13
#define NAND_CMD_PROGRAM     0x02
#define NAND_CMD_ERASE1      0x60
#define NAND_CMD_ERASE2      0xd0
#define NAND_CMD_SEQIN       0x80
#define NAND_CMD_RNDOUT      0x05
#define NAND_CMD_RNDOUTSTART 0xe0

/* what the next data phase delivers */
#define NFC_DATA_NONE        0
#define NFC_DATA_PAGE        1
#define NFC_DATA_ID          2

/*
 * The id[] the driver looks up during nand_scan_ident(). W29N01HVSINA 1G is
 * the entry in drivers/mtd/nand/nand_ids.c whose geometry is exactly what
 * this model implements: SZ_2K page, SZ_128 (128MiB) chip, SZ_128K erase
 * block, 64-byte OOB and NAND_ECC_INFO(4, SZ_512), matching
 * MT7621_NFC_PAGE_SIZE / BLOCK_SIZE / OOB_SIZE / ECC_STEPS one for one. An
 * id the table does not know makes nand_scan_ident() fail, nfc_probe() bail
 * out before nand_register(), and the SPL never sees a NAND device at all.
 */
static const uint8_t nfc_id[8] = {
    0xef, 0xf1, 0x00, 0x95, 0x00, 0x00, 0x00, 0x00
};

static uint64_t nfc_page_offset(uint32_t page)
{
    /*
     * The backing file is the logical MTD image: page data only, writesize
     * bytes per page, no OOB interleaving (that is how a full.bin partition
     * dump is laid out, and it is exactly 128MiB for this board). OOB is
     * synthesized below, so UBI EC/VID headers are not present.
     */
    return (uint64_t)page * MT7621_NFC_PAGE_SIZE;
}

/*
 * Bounded access trace.  The SPL can sit in a silent NAND polling loop for the
 * whole 90s window with nothing but these lines to explain where it is, so the
 * budget is generous but hard-capped; a run can never flood the log again.
 */
#define NFC_TRACE_MAX      400
#define NFC_PAGE_TRACE_MAX 16
#define NFC_ADDR_TRACE_MAX 32

/* Load the addressed page into the PIO stream buffer. */
static void nfc_load_page(mt7621NfcState *s, uint32_t page, uint32_t col)
{
    uint64_t off;
    uint64_t len;

    /* erased OOB */
    memset(s->page_buf + MT7621_NFC_PAGE_SIZE, 0xff, MT7621_NFC_OOB_SIZE);
    memset(s->page_buf, 0xff, MT7621_NFC_PAGE_SIZE);
    s->stream_pos = col;
    s->page_fmt = (s->cnfg >> AUTO_FMT_EN_S) & 1;
    s->n_page_reads++;

    if (!s->data || col >= MT7621_NFC_PAGE_SIZE) {
        s->page_armed = 1;
        return;
    }

    /*
     * The image is page data only, so the tail of a short read stays erased
     * (0xff) rather than picking up whatever follows in host memory.
     */
    off = nfc_page_offset(page);
    if (off >= s->size) {
        s->page_armed = 1;
        return;
    }
    len = MIN(MT7621_NFC_PAGE_SIZE - col, s->size - off - col);
    memcpy(s->page_buf + col, s->data + off + col, len);

    /* Per-sector FDM registers mirror the (synthesized) OOB FDM bytes. */
    for (int i = 0; i < MT7621_NFC_ECC_STEPS; i++) {
        const uint8_t *fdm = &s->page_buf[MT7621_NFC_PAGE_SIZE +
                                          i * MT7621_NFC_SPARE_PER_SEC];
        s->fdml[i] = ldl_le_p(fdm);
        s->fdmm[i] = ldl_le_p(fdm + 4);
    }

    if (s->n_page_trace < NFC_PAGE_TRACE_MAX) {
        s->n_page_trace++;
        fprintf(stderr,
                "nfc: load page=%u col=%u addr=0x%llx %s word0=0x%08x "
                "raw=%u:%u:%u:%u:%u n=%d\n",
                page, col, (unsigned long long)(off + col),
                (!s->data || off >= s->size) ? "ABSENT" : "present",
                ldl_le_p(s->page_buf + col),
                s->addr_byte[0], s->addr_byte[1], s->addr_byte[2],
                s->addr_byte[3], s->addr_byte[4], s->addr_n);
    }

    s->page_armed = 1;
}

/*
 * Split the latched address bytes into column and row.
 *
 * mtk_nfc_send_address() pushes every wire byte through NFI_COLADDR (ROWADDR
 * is always written as 0, ADDRNOB.COL_NOB as 1), so the register file carries
 * no col/row distinction: the model has to derive the shape from the command
 * that opened the cycle plus the geometry.  s->cmd is that command, because
 * only READSTART and RNDOUTSTART leave the address accumulator alone.
 *
 *   - nand_base.c nand_command_lp() is installed by nand_scan_ident() for
 *     every chip with mtd->writesize > 512 (2KiB page here) and always
 *     latches "column" then "column >> 8": two little-endian column bytes.
 *     The SPL's own cmdfunc in mt7621_nand_spl.c drops the second column
 *     byte for READID only.
 *   - the row is 2 bytes on this part: nand_scan_ident() sets NAND_ROW_ADDR_3
 *     only when chip_shift - page_shift > 16, and 128MiB / 2KiB is exactly
 *     65536 pages (chip_shift 27 - page_shift 11 = 16), so nand_command_lp()
 *     stops after page_addr >> 8.  Three row bytes are still tolerated for a
 *     chip that does get NAND_ROW_ADDR_3.
 *   - erase latches the row with no column at all, so its row sits at
 *     addr_byte[0], not [2].
 */
static int nfc_col_nob(mt7621NfcState *s)
{
    int n;

    if (s->cmd == NAND_CMD_ERASE1 || s->cmd == NAND_CMD_ERASE2) {
        return 0;
    }

    n = 2;                          /* column, column >> 8 */
    if (n > s->addr_n) {
        n = s->addr_n;
    }
    if (n < 0) {
        n = 0;
    }
    return n;
}

static int nfc_row_nob(mt7621NfcState *s)
{
    int n;

    if (s->cmd == NAND_CMD_READID) {
        return 0;                   /* an id cycle carries no row */
    }

    n = s->addr_n - nfc_col_nob(s);
    if (n < 0) {
        n = 0;
    }
    if (n > 3) {
        n = 3;                      /* NAND_ROW_ADDR_3 caps the row at 3 */
    }
    return n;
}

static uint32_t nfc_decode_col(mt7621NfcState *s)
{
    uint32_t col = 0;
    int n = nfc_col_nob(s);

    for (int i = 0; i < n; i++) {
        if (i >= (int)sizeof(s->addr_byte)) {
            break;
        }
        col |= (uint32_t)s->addr_byte[i] << (8 * i);
    }
    return col;
}

static uint32_t nfc_decode_page(mt7621NfcState *s)
{
    uint32_t page = 0;
    int base = nfc_col_nob(s);
    int n = nfc_row_nob(s);

    /*
     * No row on the wire (RNDOUT and friends only move the column): reuse the
     * page the last complete cycle latched instead of decoding residue.
     */
    if (n == 0) {
        return s->row_page;
    }

    for (int i = 0; i < n; i++) {
        int idx = base + i;

        if (idx < 0 || idx >= (int)sizeof(s->addr_byte)) {
            break;
        }
        page |= (uint32_t)s->addr_byte[idx] << (8 * i);
    }
    s->row_page = page;
    return page;
}

/* Length of the PIO byte stream for the armed page. */
static uint32_t nfc_stream_len(mt7621NfcState *s)
{
    return s->page_fmt ? MT7621_NFC_PAGE_SIZE : MT7621_NFC_PAGESIZE_TOTAL;
}


static void nfc_trace(mt7621NfcState *s, const char *rw, hwaddr addr,
                      uint64_t val, unsigned size)
{
    if (s->n_trace >= NFC_TRACE_MAX) {
        return;
    }
    s->n_trace++;
    fprintf(stderr, "nfc[%llu/%d]: %s off=0x%02x size=%u val=0x%" PRIx64 "\n",
            (unsigned long long)s->n_trace, NFC_TRACE_MAX, rw,
            (unsigned)addr, size, val);
}

/*
 * Materialise the bytes the next data phase must hand out. Called from the
 * first NFI_DATAR read rather than from the command decoder, because the
 * address cycle has not happened yet when the command arrives.
 */
static void nfc_arm_pending(mt7621NfcState *s)
{
    if (s->pending_data == NFC_DATA_ID) {
        if (s->n_page_trace < NFC_PAGE_TRACE_MAX) {
            s->n_page_trace++;
            fprintf(stderr, "nfc: read id %02x %02x %02x %02x\n",
                    nfc_id[0], nfc_id[1], nfc_id[2], nfc_id[3]);
        }
        memset(s->page_buf, 0xff, sizeof(s->page_buf));
        memcpy(s->page_buf, nfc_id, sizeof(nfc_id));
        s->stream_pos = 0;
        s->page_fmt = 1;            /* id only, no OOB on the stream */
        s->page_armed = 1;
        return;
    }
    nfc_load_page(s, nfc_decode_page(s), nfc_decode_col(s));
}

static uint64_t nfc_nfi_read(void *opaque, hwaddr addr, unsigned size)
{
    mt7621NfcState *s = MT7621_NFC(opaque);
    uint32_t val = 0;

    switch (addr & ~3u) {
    case NFI_CNFG_REG16:
        val = s->cnfg;
        break;
    case NFI_PAGEFMT_REG16:
        val = s->pagefmt;
        break;
    case NFI_CON_REG16:
        val = s->con;
        break;
    case NFI_ACCCON_REG32:
        val = s->acccon;
        break;
    case NFI_INTR_REG16:
        val = 0;
        break;
    case NFI_COLADDR_REG32:
        val = s->coladdr;
        break;
    case NFI_ROWADDR_REG32:
        val = s->rowaddr;
        break;
    case NFI_DATAR_REG32: {
        uint32_t len;
        unsigned step;

        if (!s->page_armed && s->pending_data != NFC_DATA_NONE) {
            nfc_arm_pending(s);
            s->pending_data = NFC_DATA_NONE;
        }
        len = nfc_stream_len(s);
        /*
         * CNFG.BYTE_RW selects a one-byte port, not a four-byte one.  The
         * probe sets it before nand_scan_ident(), which then asks for the ID
         * one byte at a time; stepping four bytes per access skipped every
         * other byte and made the id lookup fail.  NFI_DATAR is a 32-bit
         * register either way, so the byte is returned zero-extended, which
         * is what nfc_read_byte() masks off with "& 0xff".
         */
        step = (s->cnfg & (1u << BYTE_RW_S)) ? 1 : 4;
        if (s->page_armed && s->stream_pos + step <= len) {
            if (step == 4) {
                val = ldl_le_p(s->page_buf + s->stream_pos);
            } else {
                val = s->page_buf[s->stream_pos];
            }
            s->stream_pos += step;
        } else {
            val = 0xffffffff;
        }
        break;
    }
    case NFI_PIO_DIRDY_REG16:
        /* data is always "ready": this model completes instantaneously */
        val = 1;
        break;
    case NFI_STA_REG32: {
        /*
         * Everything here stays clear.
         *
         * STA_CMD (bit 0) means "a command is in flight"; the driver waits
         * for it to CLEAR - nfc_wait_status_ready() polls !(val & STA_CMD) -
         * and treats BUSY (bit 8) as an error, so a model that finishes a
         * command the moment it is written must report both as clear.
         *
         * NAND_FSM must read FSM_IDLE for the same reason the real hardware
         * drops it after every access (see the comment in nfc_pio_read):
         * nfc_pio_read() only reprograms CNFG - and thereby selects byte
         * versus word PIO mode - when it sees a state other than
         * FSM_CUSTOM_DATA.  Leaving the FSM parked in CUSTOM_DATA meant the
         * driver read 8 ID bytes through a word-sized port, so id[1] came
         * from byte 5 instead of byte 1, nand_scan_ident() found no match,
         * nfc_probe() returned before nand_register(), and the SPL never had
         * a NAND device to boot from.
         */
        val = 0;
        break;
    }
    case NFI_FIFOSTA_REG16: {
        uint32_t len = nfc_stream_len(s);
        uint32_t remain = (s->page_armed && s->stream_pos < len)
                              ? len - s->stream_pos : 0;

        val = (remain & RD_REMAIN_M) | (remain ? 0 : (1u << RD_EMPTY_S));
        break;
    }
    case NFI_ADDRCNTR_REG16: {
        uint32_t sec = s->stream_pos / 512;
        uint32_t col = s->stream_pos & 0x1ff;

        val = ((sec & 0xf) << SEC_CNTR_S) | (col & 0x3ff);
        break;
    }
    case NFI_CSEL_REG16:
        val = s->csr;
        break;
    case NFI_MASTERSTA_REG16:
        /* bit 9 (MAS_ADDR) stays set while a page is being streamed; the
         * driver polls for this to become clear after a reset. */
        val = s->page_armed ? (1u << MAS_ADDR_S) : 0;
        break;
    default:
        if (addr >= 0x0a0 && addr < 0x0a0 + (MT7621_NFC_ECC_STEPS * 8)) {
            int idx = (addr - 0x0a0) / 8;
            int hi = ((addr - 0x0a0) % 8) >= 4;

            val = hi ? s->fdmm[idx] : s->fdml[idx];
        } else {
            val = 0;
        }
        break;
    }

    /* 16-bit registers only return their low half on a 16-bit access. */
    if (size == 2) {
        val &= 0xffff;
    }
    /*
     * Trace after the value is known: tracing before the switch logged every
     * single read as 0x0, which made the status polls look like a deadlock
     * when the device was answering normally.
     */
    nfc_trace(s, "rd", addr, val, size);
    return val;
}

static void nfc_nfi_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    nfc_trace(MT7621_NFC(opaque), "wr", addr, val64, size);
    mt7621NfcState *s = MT7621_NFC(opaque);
    uint32_t val = (uint32_t)val64;

    if (size == 2) {
        val &= 0xffff;
    }

    switch (addr & ~3u) {
    case NFI_CNFG_REG16:
        s->cnfg = val;
        break;
    case NFI_PAGEFMT_REG16:
        s->pagefmt = val;
        break;
    case NFI_CON_REG16: {
        s->con = val;
        if (val & (1u << NFI_STM_RST_S)) {
            /* state machine reset: drop any armed page */
            s->page_armed = 0;
            s->stream_pos = 0;
            s->con &= ~(1u << NFI_STM_RST_S);
        }
        if (val & (1u << NFI_FIFO_FLUSH_S)) {
            s->stream_pos = 0;
        }
        break;
    }
    case NFI_ACCCON_REG32:
        s->acccon = val;
        break;
    case NFI_CMD_REG16:
        s->cmd = val & 0xff;
        s->n_cmds++;
        /*
         * A large-page read sends READ0, then the five address bytes, then
         * READSTART to close the cycle. Resetting the address accumulator on
         * the closing command would throw the page number away, so only the
         * commands that begin a transaction clear it.
         */
        if (s->cmd != NAND_CMD_READSTART && s->cmd != NAND_CMD_RNDOUTSTART) {
            s->addr_n = 0;
            /*
             * Drop the old bytes as well as the count: addr_n alone would
             * leave the previous cycle's bytes in place for any decoder that
             * indexes past the bytes this cycle actually sent.
             */
            memset(s->addr_byte, 0, sizeof(s->addr_byte));
            s->in_addr_phase = 0;
        }
        s->page_armed = 0;
        s->stream_pos = 0;
        if (s->cmd == NAND_CMD_READID) {
            s->pending_data = NFC_DATA_ID;
        } else if (s->cmd == NAND_CMD_READ0 || s->cmd == NAND_CMD_PAGE_READ ||
                   s->cmd == NAND_CMD_READSTART) {
            s->pending_data = NFC_DATA_PAGE;
        } else {
            s->pending_data = NFC_DATA_NONE;
        }
        break;
    case NFI_ADDRNOB_REG16:
        s->addrnob = val;
        break;
    case NFI_COLADDR_REG32:
        s->coladdr = val;
        /* one wire byte per write, in order */
        if (s->addr_n < (int)sizeof(s->addr_byte)) {
            /*
             * Every address byte arrives through here - mtk_nfc_send_address()
             * writes the byte to NFI_COLADDR and 0 to NFI_ROWADDR - so logging
             * them as they land proves which byte sequence the driver really
             * sent, independently of any later state the loader may reuse.
             */
            if (s->n_addr_trace < NFC_ADDR_TRACE_MAX) {
                s->n_addr_trace++;
                fprintf(stderr, "nfc: addr[%d] = 0x%02x (cmd=0x%02x)\n",
                        s->addr_n, val & 0xff, s->cmd);
            }
            s->addr_byte[s->addr_n++] = val & 0xff;
            s->in_addr_phase = 1;
        }
        break;
    case NFI_ROWADDR_REG32:
        s->rowaddr = val;
        break;
    case NFI_STRDATA_REG16:
        s->strdata = val;
        break;
    case NFI_DATAW_REG32:
        /*
         * In custom mode the data register is the PIO data path. On write it
         * is a program source; the model keeps it in memory only so the
         * backing dump is never modified by a simulated boot.
         */
        if (s->page_armed && s->stream_pos + 4 <= sizeof(s->page_buf)) {
            stl_le_p(s->page_buf + s->stream_pos, val);
            s->stream_pos += 4;
        }
        break;
    case NFI_CSEL_REG16:
        s->csr = val;
        break;
    default:
        if (addr >= 0x0a0 && addr < 0x0a0 + (MT7621_NFC_ECC_STEPS * 8)) {
            int idx = (addr - 0x0a0) / 8;
            int hi = ((addr - 0x0a0) % 8) >= 4;

            if (hi) {
                s->fdmm[idx] = val;
            } else {
                s->fdml[idx] = val;
            }
        }
        break;
    }
}

static uint64_t nfc_ecc_read(void *opaque, hwaddr addr, unsigned size)
{
    mt7621NfcState *s = MT7621_NFC(opaque);
    uint32_t val = 0;

    switch (addr & ~3u) {
    case ECC_ENCCON_REG16:
        val = s->encccon;
        break;
    case ECC_ENCIDLE_REG16:
        val = 1u << ENCIDLE_S;      /* encoder is always idle here */
        break;
    case ECC_DECCON_REG16:
        val = s->deccon;
        break;
    case ECC_DECIDLE_REG16:
        val = 1u << ENCIDLE_S;      /* decoder is always idle here */
        break;
    case ECC_DECENUM_REG32:
        /* zero corrected errors: the dump is a good image */
        val = 0;
        break;
    case ECC_DECDONE_REG16:
        /* every sector decoded */
        val = (1u << MT7621_NFC_ECC_STEPS) - 1;
        break;
    default:
        if (addr >= 0x11c && addr < 0x11c + 12 * 4) {
            val = 0;                 /* no error locations */
        }
        break;
    }

    if (size == 2) {
        val &= 0xffff;
    }
    return val;
}

static void nfc_ecc_write(void *opaque, hwaddr addr, uint64_t val64,
                          unsigned size)
{
    mt7621NfcState *s = MT7621_NFC(opaque);
    uint32_t val = (uint32_t)val64;

    if (size == 2) {
        val &= 0xffff;
    }

    switch (addr & ~3u) {
    case ECC_ENCCON_REG16:
        s->encccon = val;
        break;
    case ECC_DECCON_REG16:
        s->deccon = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps nfc_nfi_ops = {
    .read = nfc_nfi_read,
    .write = nfc_nfi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static const MemoryRegionOps nfc_ecc_ops = {
    .read = nfc_ecc_read,
    .write = nfc_ecc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void nfc_init(Object *obj)
{
    mt7621NfcState *s = MT7621_NFC(obj);

    memory_region_init_io(&s->nfi_mr, obj, &nfc_nfi_ops, s, "mt7621-nfi",
                          0x800);
    memory_region_init_io(&s->ecc_mr, obj, &nfc_ecc_ops, s, "mt7621-nfc-ecc",
                          0x800);

    /*
     * Both register blocks are published as sysbus MMIO regions so the
     * SoC can map them at 0x1e003000 (NFI) and 0x1e003800 (ECC).
     */
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->nfi_mr);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ecc_mr);
}

static void nfc_realize(DeviceState *dev, Error **errp)
{
    mt7621NfcState *s = MT7621_NFC(dev);

    if (!s->data) {
        warn_report("mt7621-nfi: no flash image was given, "
                    "every NAND read will return 0xff");
    }
}

/*
 * The machine hands the loaded image over before realizing the SoC, so there
 * is no block property to declare here - see the comment on nfc->data.
 */
static Property nfc_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static void nfc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = nfc_realize;
    device_class_set_props(dc, nfc_properties);
    dc->desc = "MediaTek MT7621 NAND Flash Controller (NFI + ECC)";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo nfc_info = {
    .name = TYPE_MT7621_NFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(mt7621NfcState),
    .instance_init = nfc_init,
    .class_init = nfc_class_init,
};

static void nfc_register_types(void)
{
    type_register_static(&nfc_info);
}

type_init(nfc_register_types)
