#!/usr/bin/env bash
#
# Run the simulated Mi Router 4 boot and grade the serial log.
#
#   scripts/run_boot.sh <qemu-binary> <flash.bin> [seconds] [logfile]
#
# Grades against the levels of a real boot:
#   L1  QEMU starts and the machine is accepted
#   L2  SPL runs (any output from the bootloader at all)
#   L3  main U-Boot banner + NAND/MTD probe
#   L4  a Linux boot message
set -uo pipefail

QEMU="${1:?qemu binary}"
FLASH="${2:?composed flash image}"
SECS="${3:-60}"
LOG="${4:-boot.log}"

echo "== simulated boot =="
echo "   qemu  : $QEMU"
echo "   flash : $FLASH ($(stat -c%s "$FLASH")B)"
echo "   limit : ${SECS}s"

rm -f "$LOG"
timeout "$SECS" \
    "$QEMU" \
    -M mi-router-4 \
    -m 128 \
    -drive if=mtd,file="$FLASH",format=raw \
    -nographic \
    -no-reboot \
    -serial mon:stdio \
    -d guest_errors,unimp \
    > "$LOG" 2>&1
rc=$?

echo "-- exit code: $rc (124 = timeout, expected)"
echo "=================== serial log ==================="
cat "$LOG"
echo "==================================================="

# ---------------------------------------------------------------- grading
grade() {
    local level=0

    if grep -qE "MT7621|Mi Router 4" "$LOG" 2>/dev/null; then :; fi

    # L2: the SPL or U-Boot printed anything at all
    if [ -s "$LOG" ] && grep -qE "U-Boot|SPL|MTK|Ralink|DRAM|NAND" "$LOG"; then
        level=2
        echo "PASS L2: bootloader produced output"
    else
        echo "FAIL L1: no bootloader output on the serial console"
        echo "      last log lines:"
        tail -5 "$LOG" | sed 's/^/      /'
        echo "$level"
        return
    fi

    # L3: main U-Boot banner and the MTD/NAND probe
    if grep -qE "U-Boot 20[0-9]{2}\." "$LOG"; then
        level=3
        echo "PASS L3: main U-Boot banner reached"
    else
        echo "PARTIAL: SPL ran but no main U-Boot banner (stuck in SPL)"
        tail -15 "$LOG" | sed 's/^/      /'
        echo "$level"
        return
    fi

    if grep -qiE "nand|mtd" "$LOG"; then
        echo "PASS L3: NAND/MTD probe produced output"
    else
        echo "PARTIAL: U-Boot up but no NAND/MTD probe output"
    fi

    # L4: Linux started
    if grep -qE "Linux version|Booting Linux|Starting kernel" "$LOG"; then
        level=4
        echo "PASS L4: Linux kernel started"
    fi

    echo "$level"
}

LEVEL="$(grade | tee /dev/stderr | tail -1)"

echo
echo "== simulated boot reached level ${LEVEL} =="
case "$LEVEL" in
    4) echo "   full boot chain observed" ;;
    3) echo "   U-Boot up and reading NAND; kernel not handed control" ;;
    2) echo "   SPL only; main U-Boot not reached" ;;
    *) echo "   no bootloader output" ;;
esac

# Guest errors are worth surfacing even on success.
if grep -qE "Unsupported|access to addr|LOG_GUEST_ERROR" "$LOG"; then
    echo
    echo "-- guest errors observed (first 10):"
    grep -E "Unsupported|access to addr" "$LOG" | head -10 | sed 's/^/   /'
fi

exit 0
