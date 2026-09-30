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

# ---------------------------------------------------------------- Kconfig wiring
# The unimplemented-device Kconfig symbol was renamed across QEMU releases,
# so pick whichever name this tree actually uses.
UNIMP_SYM="UNIMP"
if grep -q "^config HW_MISC_UNIMP" "$SRC/hw/misc/Kconfig" 2>/dev/null; then
    UNIMP_SYM="HW_MISC_UNIMP"
fi
echo "-- using UNIMP symbol: $UNIMP_SYM"

# NOTE: these symbol names MUST match the CONFIG_* lines appended to
# configs/devices/mips-softmmu/default.mak below. minikconfig resolves the
# .mak against the Kconfig tree and aborts the whole configure step with
# "undefined symbol" if any name is not declared, which leaves the model
# sources silently never compiled.
if ! grep -q "MT7621_SOC" "$SRC/hw/mips/Kconfig"; then
    cat >> "$SRC/hw/mips/Kconfig" <<EOF

config MT7621_NFC
    bool

config MT7621_SOC
    bool
    select SERIAL_MM
    select $UNIMP_SYM

config MT7621_MI_ROUTER4
    bool
    select MT7621_SOC
EOF
    echo "-- wired hw/mips/Kconfig"
fi

# ---------------------------------------------------------------- meson wiring
# hw/misc/meson.build -- the NAND controller
if ! grep -q "mt7621-nfc.c" "$SRC/hw/misc/meson.build"; then
    cat >> "$SRC/hw/misc/meson.build" <<'EOF'
system_ss.add(when: 'CONFIG_MT7621_NFC', if_true: files('mt7621-nfc.c'))
EOF
    echo "-- wired hw/misc/meson.build"
fi

# hw/mips/meson.build -- the SoC + machine
if ! grep -q "files('mt7621.c')" "$SRC/hw/mips/meson.build"; then
    cat >> "$SRC/hw/mips/meson.build" <<'EOF'
mips_ss.add(when: 'CONFIG_MT7621_SOC', if_true: files('mt7621.c'))
EOF
    echo "-- wired hw/mips/meson.build"
fi

# ---------------------------------------------------------------- default devices
# These CONFIG_ names are resolved by minikconf against the Kconfig symbols
# declared above; they have to agree exactly.
DEVCONF="$SRC/configs/devices/mips-softmmu/default.mak"
if ! grep -q "CONFIG_MT7621_MI_ROUTER4" "$DEVCONF"; then
    cat >> "$DEVCONF" <<'EOF'
CONFIG_MT7621_MI_ROUTER4=y
CONFIG_MT7621_SOC=y
CONFIG_MT7621_NFC=y
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

# If the model symbols did not survive minikconf, the machine is silently
# absent from the binary. Fail here with a clear message instead.
if [ -f "$BUILD/mipsel-softmmu-config-devices.mak" ]; then
    echo "-- generated MT7621 device config:"
    grep -E "MT7621" "$BUILD/mipsel-softmmu-config-devices.mak" | sed 's/^/     /' \
        || echo "     ERROR: no MT7621 symbols in the generated config" >&2
fi

# ---------------------------------------------------------------- build
echo "-- building (this takes a while)"
ninja -j"$(nproc)"

echo "== built =="
ls -l "$BUILD/qemu-system-mipsel"
if "$BUILD/qemu-system-mipsel" -M help 2>/dev/null | grep -q "mi-router-4"; then
    echo "machine registered OK"
else
    echo "ERROR: machine mi-router-4 not registered" >&2
    exit 1
fi
