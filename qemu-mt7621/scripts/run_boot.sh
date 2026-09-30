#!/usr/bin/env bash
#
# Run the simulated Mi Router 4 boot and grade the serial log.
#
#   scripts/run_boot.sh <qemu-binary> <flash.bin> [seconds] [logfile] [min_level]
#
#   seconds=0  skip the run and grade an existing logfile only
#
# Grades against what a real boot produces:
#   L1  any bootloader output on the console
#   L2  SPL stage text (DRAM / NAND init)
#   L3  the main U-Boot banner reached
#   L4  a Linux boot message (kernel handed control)
#
# Exits non-zero when the grade is below min_level (default 3), so a boot that
# never happened fails CI instead of reporting success.
set -uo pipefail

QEMU="${1:?qemu binary}"
FLASH="${2:?composed flash image}"
SECS="${3:-90}"
LOG="${4:-boot.log}"
MIN_LEVEL="${5:-3}"

if [ "$SECS" != "0" ]; then
    echo "== simulated boot =="
    echo "   qemu  : $QEMU"
    echo "   flash : $FLASH ($(stat -c%s "$FLASH" 2>/dev/null || echo '?')B)"
    echo "   limit : ${SECS}s"

    rm -f "$LOG"
    # The property is authoritative; the env var is the fallback for the case
    # where machine properties are applied after mc->init.
    MI_ROUTER4_FLASH="$FLASH" timeout "$SECS" \
        "$QEMU" \
        -M mi-router-4,flash="$FLASH" \
        -m 128 \
        -nographic \
        -no-reboot \
        -serial mon:stdio \
        -d guest_errors,unimp \
        > "$LOG" 2>&1
    echo "-- qemu exit: $? (124 = hit the ${SECS}s limit, expected)"
else
    echo "== grading existing log: $LOG =="
fi

[ -f "$LOG" ] || { echo "ERROR: no log at $LOG" >&2; exit 1; }
echo "=================== serial log ==================="
cat "$LOG"
echo "==================================================="

# ---------------------------------------------------------------- grade phase
level=0

if grep -qE "U-Boot 20[0-9]{2}\.|U-Boot SPL|Ralink|MIPS|DRAM|NAND|MTK" "$LOG"; then
    level=1
    echo "PASS L1: bootloader produced console output"
else
    echo "FAIL L1: no bootloader output on the serial console"
    echo "      last log lines:"
    tail -5 "$LOG" | sed 's/^/      /'
fi

if [ "$level" -ge 1 ]; then
    if grep -qE "DRAM|dram|NAND:|nand:|Uncompress|SPL" "$LOG"; then
        level=2
        echo "PASS L2: SPL stage ran (DRAM/NAND/init text present)"
    else
        echo "FAIL L2: no SPL-stage text"
    fi
fi

if [ "$level" -ge 2 ]; then
    if grep -qE "U-Boot 20[0-9]{2}\." "$LOG"; then
        level=3
        echo "PASS L3: main U-Boot banner reached"
    else
        echo "FAIL L3: SPL ran but the main U-Boot banner never appeared"
        echo "      last 20 log lines:"
        tail -20 "$LOG" | sed 's/^/      /'
    fi
fi

if [ "$level" -ge 3 ] && grep -qE "Linux version|Booting Linux|Starting kernel" "$LOG"; then
    level=4
    echo "PASS L4: Linux kernel started"
else
    echo "FAIL L4: no Linux boot message"
fi

# ---------------------------------------------------------------- diagnostics
if grep -qE "Unsupported|access to addr|LOG_GUEST_ERROR|no flash image|qemu: fatal" "$LOG"; then
    echo "-- guest errors / fatal (first 15):"
    grep -E "Unsupported|access to addr|no flash image|qemu: fatal" "$LOG" \
        | head -15 | sed 's/^/   /'
fi

echo
echo "== simulated boot reached level ${level} (need >= ${MIN_LEVEL}) =="
case "$level" in
    4) echo "   full boot chain observed" ;;
    3) echo "   U-Boot up and talking to NAND; kernel not handed control" ;;
    2) echo "   SPL only; main U-Boot not reached" ;;
    *) echo "   no bootloader output" ;;
esac

if [ "$level" -lt "$MIN_LEVEL" ]; then
    echo "RESULT: FAIL (level $level < $MIN_LEVEL)" >&2
    exit 1
fi
echo "RESULT: PASS (level $level >= $MIN_LEVEL)"