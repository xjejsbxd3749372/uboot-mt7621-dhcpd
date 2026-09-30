#!/usr/bin/env bash
#
# Build a QEMU with the MT7621 SoC + Mi Router 4 machine from this repo.
#
#   scripts/build_qemu.sh [workdir]
#
# Clones QEMU (pinned), drops the model files in, wires up Kconfig/meson,
# then builds only the mipsel-softmmu target.
set -euo pipefail

QEMU_VERSION="${QEMU_VERSION:-9.2.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${1:-${WORK:-/tmp/qemu-mt7621-build}}"
SRC="$WORK/qemu"
BUILD="$WORK/build"

echo "== MT7621 QEMU model build =="
echo "   version : $QEMU_VERSION"
echo "   source  : $HERE"
echo "   workdir : $WORK"

mkdir -p "$WORK"
mkdir -p "$SRC/include/hw/misc" "$SRC/hw/misc" "$SRC/hw/mips"

# ---------------------------------------------------------------- fetch
if [ ! -d "$SRC/.git" ]; then
    echo "-- cloning qemu v$QEMU_VERSION"
    rm -rf "$SRC"
    git clone --depth 1 --branch "v$QEMU_VERSION" \
        https://github.com/qemu/qemu.git "$SRC"
else
    echo "-- reusing existing qemu checkout"
fi

# ---------------------------------------------------------------- install model
install -m 0644 "$HERE/include/hw/misc/mt7621-nfc.h" \
    "$SRC/include/hw/misc/mt7621-nfc.h"
install -m 0644 "$HERE/hw/misc/mt7621-nfc.c" \
    "$SRC/hw/misc/mt7621-nfc.c"
install -m 0644 "$HERE/hw/mips/mt7621.c" \
    "$SRC/hw/mips/mt7621.c"
echo "-- installed model sources"

# ---------------------------------------------------------------- meson wiring
# hw/misc/meson.build
if ! grep -q "mt7621-nfc.c" "$SRC/hw/misc/meson.build"; then
    cat >> "$SRC/hw/misc/meson.build" <<'EOF'
system_ss.add(when: 'CONFIG_MT7621_NFC', if_true: files('mt7621-nfc.c'))
EOF
    echo "-- wired hw/misc/meson.build"
fi

# hw/mips/meson.build
if ! grep -q "files('mt7621.c')" "$SRC/hw/mips/meson.build"; then
    cat >> "$SRC/hw/mips/meson.build" <<'EOF'
mips_ss.add(when: 'CONFIG_MT7621', if_true: files('mt7621.c'))
EOF
    echo "-- wired hw/mips/meson.build"
fi

# ---------------------------------------------------------------- Kconfig wiring
# The unimplemented-device Kconfig symbol was renamed across QEMU releases,
# so pick whichever name this tree actually uses.
UNIMP_SYM="UNIMP"
if grep -q "^config HW_MISC_UNIMP" "$SRC/hw/misc/Kconfig" 2>/dev/null; then
    UNIMP_SYM="HW_MISC_UNIMP"
fi
echo "-- using UNIMP symbol: $UNIMP_SYM"

if ! grep -q "MIPS_MT7621" "$SRC/hw/mips/Kconfig"; then
    cat >> "$SRC/hw/mips/Kconfig" <<EOF

config MT7621_NFC
    bool

config MIPS_MT7621
    bool
    select SERIAL
    select $UNIMP_SYM

config MIPS_MI_ROUTER4
    bool
    select MIPS_MT7621
EOF
    echo "-- wired hw/mips/Kconfig"
fi

# ---------------------------------------------------------------- default devices
DEVCONF="$SRC/configs/devices/mips-softmmu/default.mak"
if ! grep -q "CONFIG_MI_ROUTER4" "$DEVCONF"; then
    cat >> "$DEVCONF" <<'EOF'
CONFIG_MT7621_NFC=y
CONFIG_MT7621=y
CONFIG_MI_ROUTER4=y
EOF
    echo "-- wired $DEVCONF"
fi

# ---------------------------------------------------------------- configure
echo "-- configuring"
mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f build.ninja ]; then
    "$SRC/configure" \
        --target-list=mipsel-softmmu \
        --disable-docs \
        --disable-tools \
        --disable-guest-agent \
        --disable-slirp \
        --disable-cap-ng \
        --disable-fdt \
        --disable-werror
fi

# ---------------------------------------------------------------- build
echo "-- building (this takes a while)"
ninja -j"$(nproc)"

echo "== built =="
ls -l "$BUILD/qemu-system-mipsel"
"$BUILD/qemu-system-mipsel" -M help | grep -i "mi-router-4" && \
    echo "machine registered OK" || {
        echo "ERROR: machine mi-router-4 not registered" >&2
        exit 1
    }
