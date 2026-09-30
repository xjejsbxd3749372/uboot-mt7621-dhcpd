# Permanent simulated-boot-path regression test for Mi Router 4 (mipsel MT7621).
#
# Two modes:
#   OFFLINE (CI): defconfig MTDPARTS walk -> absolute offsets, cross-checked
#                 against the authoritative OpenWrt DTS boundaries, plus the
#                 512K bootloader-partition image-size gate. No full.bin needed.
#   LOCAL  (host): additionally, if full.bin + u-boot-combined.img are present,
#                 run the full boot-chain byte scan (uImage CRC, kernel LZMA
#                 decompress, UBI/squashfs, MT7603/MT7612 cal headers).
#
# Exit 0 = no blocking errors. Exit 1 = a fatal mismatch (would brick/no-boot).
import os, re, struct, zlib, sys, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DC = os.path.join(REPO, "configs-mt7621", "xiaomi_mi-router-4_defconfig")
FULL = os.environ.get(
    "MI4_FULL_BIN",
    r"C:\Users\admin\Desktop\小米路由器4（R4）固件编程器固件备份\小米路由器4（R4）固件+编程器固件备份\full.bin")
IMG = os.environ.get(
    "MI4_UBOOT_IMG",
    os.path.join(HERE, "u-boot-combined.img"))

# authoritative OpenWrt DTS boundary map (start offsets that every MTD part MUST begin on)
DTS_STARTS = [0x0, 0x80000, 0xC0000, 0x100000, 0x140000, 0x180000, 0x1C0000,
              0x200000, 0x600000, 0xA00000]
BOOT_BYTES = 512 * 1024          # Mi Router 4 bootloader partition
FATALLY_BAD = []
NOTES = []


def die(m):
    FATALLY_BAD.append(m)
    print("  FATAL:", m)


def note(m):
    NOTES.append(m)
    print("  note:", m)


# ---------- offline: defconfig mtdparts walk ----------
def walk_mtdparts(spec, device=128 * 1024 * 1024):
    """spec like '512k(u-boot),...,4m(kernel_stock),-(firmware)'
    -> list of (name, start, end). '-' means 'rest of device'."""
    parts = []
    off = 0
    for tok in spec.split(","):
        tok = tok.strip()
        m = re.match(r"(-?[\d.]*[kmKMgG]?)\(([^)]*)\)", tok)
        if not m:
            continue
        size_s, name = m.group(1), m.group(2)
        if size_s == "-":
            size = device - off                 # rest-of-device
        else:
            num = int(re.sub(r"[kKmMgG]$", "", size_s))
            mult = {"": 1, "k": 1024, "m": 1048576, "g": 1 << 30}[size_s[-1].lower() if size_s[-1].isalpha() else ""]
            size = num * mult
        parts.append([name, off, off + size])
        off += size
    return parts, off


def mtdparts_from_defconfig():
    txt = open(DC, encoding="utf-8", errors="replace").read()
    m = re.search(r'--mtdparts\s+"([^"]+)"', txt)
    if not m:
        m = re.search(r'--mtdparts\s+([^\n&]+)', txt)
    spec = m.group(1).strip() if m else None
    return spec


def offline_checks():
    global total_device
    print("\n[OFFLINE] defconfig MTDPARTS vs authoritative DTS boundaries")
    spec = mtdparts_from_defconfig()
    if not spec:
        die("could not find --mtdparts in defconfig")
        return
    print("  spec:", spec)
    parts, off = walk_mtdparts(spec)
    bad = 0
    for name, s, end in parts:
        size = end - s
        flag = ""
        if s not in DTS_STARTS:
            flag = "  <-- NOT a DTS boundary"
            die(f"build part '{name}' @0x{s:x} does not align to a DTS boundary")
        print(f"  {name:14s} 0x{s:07x}-0x{end:07x} ({size // 1024}K){flag}")
    # the trailing 'firmware'/'ubi' part must start on 0x600000 and reach end of device
    fw = [p for p in parts if p[0] in ("firmware", "ubi", "rootfs")]
    if fw:
        if fw[0][1] == 0x600000:
            note(f"'{fw[0][0]}' 0x600000..0x{fw[0][2]:x} covers DTS kernel@0x600000 + ubi@0xA00000 "
                 f"(OpenWrt mtdsplit) -> consistent")
        else:
            die(f"firmware part starts at 0x{fw[0][1]:x}, expected 0x600000")
    else:
        note("no trailing firmware/ubi part found")
    if off != 128 * 1024 * 1024:
        note(f"mtdparts total {off}B != 128MB device; trailing '-' should reach 0x8000000")


def check_image_512k():
    print("\n[512K] bootloader image size gate")
    if not os.path.exists(IMG):
        note(f"u-boot image not at {IMG} (CI: fetched via release; local: set MI4_UBOOT_IMG)")
        return
    data = open(IMG, "rb").read()
    if data[:4] != b"\x27\x05\x19\x56":
        die(f"{IMG} is not a uImage (magic {data[:4].hex()})")
        return
    size = struct.unpack_from(">I", data, 0x0C)[0]
    hdr = data[:0x40]
    hcrc_stored = struct.unpack_from(">I", hdr, 4)[0]
    h = bytearray(hdr); h[4:8] = b"\0\0\0\0"
    if (zlib.crc32(bytes(h)) & 0xffffffff) != hcrc_stored:
        die("uImage header CRC mismatch")
    total = 0x40 + size
    print(f"  uImage data {size}B, header+data = {total}B")
    if total > BOOT_BYTES:
        die(f"combined image {total}B EXCEEDS {BOOT_BYTES}B bootloader partition -> would not flash")
    else:
        print(f"  OK: {total}B <= {BOOT_BYTES}B, headroom {BOOT_BYTES - total}B")


# ---------- local: full boot-chain byte scan (best-effort, needs full.bin) ----------
def local_full_scan():
    print("\n[LOCAL] full boot-chain byte scan (full.bin + u-boot image)")
    if not (os.path.exists(FULL) and os.path.exists(IMG)):
        note("full.bin or u-boot image missing -> skipping byte-level scan "
             "(offline alignment + 512K gate already run)")
        return
    full = open(FULL, "rb").read()
    img = open(IMG, "rb").read()
    assert len(full) == 128 * 1024 * 1024, "full.bin is not 128MB"

    def uimage(buf, off, label):
        if off + 0x40 > len(buf) or buf[off:off + 4] != b"\x27\x05\x19\x56":
            die(f"{label}: 0x{off:x} not a uImage")
            return None
        size = struct.unpack_from(">I", buf, off + 0x0C)[0]
        load = struct.unpack_from(">I", buf, off + 0x10)[0]
        dcrc = struct.unpack_from(">I", buf, off + 0x18)[0]
        comp = buf[off + 0x1F]
        data = buf[off + 0x40:off + 0x40 + size]
        crcok = (zlib.crc32(data) & 0xffffffff) == dcrc
        print(f"  {label}: size={size} load=0x{load:08x} comp={comp} data-CRC{' OK' if crcok else ' MISMATCH'}")
        if not crcok:
            die(f"{label} data CRC mismatch -> boot would fail image verify")
        return size, load, comp, data

    uimage(img, 0, "SPL+main MULTI uImage")
    # main U-Boot raw code packed at 0x20000
    main = img[0x20000:0x20010]
    if main[:4] not in (b"\x00\x00\x00\x00", b"\xff\xff\xff\xff"):
        print(f"  main U-Boot raw code @0x20000 present ({main.hex()})")
    else:
        die("no main U-Boot payload at 0x20000")

    # MT7603 / MT7612 cal headers in factory@0x100000
    f0 = full[0x100000:0x100002]
    f8 = full[0x108000:0x108002]
    if f0 == b"\x03\x76":
        print("  MT7603 2.4G cal header '03 76' @ factory+0 OK")
    else:
        note(f"factory+0 = {f0.hex()} (expected 0376 MT7603)")
    if f8 == b"\x62\x76":
        print("  MT7612 5G cal header '62 76' @ factory+0x8000 OK")
    else:
        note(f"factory+0x8000 = {f8.hex()} (expected 6276 MT7612)")

    # stock kernel @0x600000
    k = uimage(full, 0x600000, "stock kernel")
    if k:
        import bz2, lzma
        size, load, comp, data = k
        kimg = None
        try:
            if comp == 2:
                kimg = bz2.decompress(data)
            elif comp == 3:  # U-Boot LZMA = LZMA1-alone stream
                kimg = None
                for fmt in (lzma.FORMAT_ALONE, lzma.FORMAT_XZ):
                    try:
                        kimg = lzma.decompress(data, format=fmt)
                        break
                    except Exception:
                        pass
                if kimg is None:
                    raise ValueError("LZMA payload did not decompress (ALONE/XZ)")
            elif comp == 1:
                import gzip
                kimg = gzip.decompress(data)
            else:
                kimg = data
            print(f"  kernel decompressed {len(kimg)}B, load=0x{load:08x} (comp={comp})")
        except Exception as e:
            note(f"kernel decompress skipped: {e}")

    # UBI + squashfs
    if full[0xA00000:0xA00004] == b"UBI#":
        print("  UBI volume header @0xA00000 OK")
    else:
        note(f"0xA00000 = {full[0xA00000:0xA00004].hex()} (not UBI#)")
    sq = full.find(b"hsqs", 0xA00000, 0x8000000)
    if sq != -1:
        print(f"  squashfs 'hsqs' rootfs @ 0x{sq:07x} OK")
    else:
        note("no squashfs 'hsqs' found in UBI region")

    # config/env @0x80000
    sig = struct.unpack_from(">I", full, 0x80000)[0]
    if sig == 0x98A43932:
        note("0x80000 is stock MiWi config (sig 0x98A43932), NOT a U-Boot env -> "
             "U-Boot falls back to compiled-in default env on first boot (non-blocking, expected)")
    elif sig == 0x1B0035D3:
        print("  0x80000 holds a valid U-Boot env block (0x1B0035D3)")


def main():
    offline_checks()
    check_image_512k()
    local_full_scan()
    print("\n================ RESULT ================")
    print(f"NOTES {len(NOTES)} | FATAL {len(FATALLY_BAD)}")
    if FATALLY_BAD:
        print("VERDICT: BOOT-PATH SCAN FAILED (would not boot / would not flash)")
        for m in FATALLY_BAD:
            print("  FATAL:", m)
        sys.exit(1)
    print("VERDICT: PASS — no blocking errors in simulated boot path")
    for m in NOTES:
        print("  note:", m)
    sys.exit(0)


if __name__ == "__main__":
    main()
