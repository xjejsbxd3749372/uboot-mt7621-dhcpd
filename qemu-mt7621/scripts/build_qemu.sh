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
#
# IMPORTANT: meson reads configs/devices/<TARGET>/default.mak where <TARGET> is
# the *endianness-qualified* softmmu name (e.g. "mipsel-softmmu"), not the
# architecture-only "mips-softmmu" directory that merely holds common.mak.
# Appending to the wrong one silently produces a config with no MT7621 symbols,
# so the model is never compiled and the machine never gets registered.
# Derive the name from --target-list below rather than hardcoding it.
DEVCONF=""
for t in mipsel-softmmu mips-softmmu mips64el-softmmu mips64-softmmu; do
    if [ -f "$SRC/configs/devices/$t/default.mak" ]; then
        DEVCONF="$SRC/configs/devices/$t/default.mak"
        break
    fi
done
if [ -z "$DEVCONF" ]; then
    echo "ERROR: no mips devices default.mak found under configs/devices" >&2
    exit 1
fi
echo "-- device config: $DEVCONF"
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
# Reconfigure whenever the generated device config is missing our symbols.
# A tree restored from actions/cache keeps the *old* mipsel-softmmu-config-
# devices.mak, so `ninja` alone would happily link a binary with no model in
# it. Always re-run configure when the wiring is fresh.
if [ ! -f build.ninja ] || \
   ! grep -q "MT7621" mipsel-softmmu-config-devices.mak 2>/dev/null; then
    "$SRC/configure" \
        --target-list=mipsel-softmmu \
        --disable-docs \
        --disable-tools \
        --disable-guest-agent \
        --disable-slirp \
        --disable-cap-ng \
        --disable-fdt \
        --disable-werror
else
    echo "-- reusing existing configure (MT7621 symbols already present)"
fi

# If the model symbols did not survive minikconf, the sources are never
# compiled and the machine is silently absent from the binary. Fail loudly
# here instead of letting the final -M help check be the only signal.
if [ ! -f mipsel-softmmu-config-devices.mak ]; then
    echo "ERROR: mipsel-softmmu-config-devices.mak was not generated" >&2
    exit 1
fi
echo "-- generated MT7621 device config:"
grep -E "MT7621" mipsel-softmmu-config-devices.mak | sed 's/^/     /'
if ! grep -q "CONFIG_MT7621_SOC=y" mipsel-softmmu-config-devices.mak; then
    echo "ERROR: CONFIG_MT7621_SOC is not set in the generated config;" >&2
    echo "       the Kconfig symbols and the devices default.mak disagree." >&2
    exit 1
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
