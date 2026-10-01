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
#   L2  SPL stage text specifically (the SPL banner, "loaded SPL", or the
#       boot-mode line) - not generic DRAM/NAND words, which the main
#       U-Boot banner also prints
#   L3  the main U-Boot banner reached, graded on its own
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

# `[ "$level" -lt "$MIN_LEVEL" ]` on a non-numeric value prints
# "integer expression expected", evaluates false, and the script reports PASS.
case "$MIN_LEVEL" in
    ''|*[!0-9]*) echo "ERROR: min_level must be a number, got '$MIN_LEVEL'" >&2; exit 2 ;;
esac

if [ "$SECS" != "0" ]; then
    echo "== simulated boot =="
    echo "   qemu  : $QEMU"
    echo "   flash : $FLASH ($(stat -c%s "$FLASH" 2>/dev/null || echo '?')B)"
    echo "   limit : ${SECS}s"

    rm -f "$LOG"
    # The property is authoritative; the env var is the fallback for the case
    # where machine properties are applied after mc->init.
    # -d int records every exception/interrupt with its code and PC. Without
    # it an early exception is only visible as the CPU spinning on the
    # unmapped 0xBFC00380 vector, which looks like "nothing happened".
    # head -c bounds the file: a guest looping on an unmapped address fills
    # gigabytes in 90s (observed: 4.2GB), which makes the grading crawl.
    # -d writes to stderr, so 2>&1 has to be merged in before the pipe for
    # head to bound the diagnostics as well as the console.
    MI_ROUTER4_FLASH="$FLASH" timeout -k 10 "$SECS" \
        "$QEMU" \
        -M mi-router-4,flash="$FLASH" \
        -m 128 \
        -nographic \
        -no-reboot \
        -serial mon:stdio \
        -d guest_errors,unimp,int \
        2>&1 | head -c 400000000 > "$LOG"
    rc=${PIPESTATUS[0]}
    echo "-- qemu exit: $rc (124 = hit the ${SECS}s limit, expected)"
else
    echo "== grading existing log: $LOG =="
fi

[ -f "$LOG" ] || { echo "ERROR: no log at $LOG" >&2; exit 1; }

# A guest stuck in a loop writing to the UART will fill this file with
# hundreds of megabytes in 90s. Printing all of it makes the CI step itself
# crawl (the runner ships the log to GitHub over a throttled link), so show
# the head - where the boot starts - and the tail - where it stops - and
# report the total size.
LOG_BYTES=$(stat -c%s "$LOG")
HEAD_C=32768
TAIL_C=16384
echo "==================================================="
echo "serial log: $LOG_BYTES bytes (showing first $HEAD_C + last $TAIL_C)"
echo "==================================================="
echo "-------------------- head -----------------------"
head -c "$HEAD_C" "$LOG"
echo
echo "-------------------- tail -----------------------"
tail -c "$TAIL_C" "$LOG"
echo
echo "==================================================="

# Keep a bounded copy for the CI artefact; the raw log can be gigabytes.
{
    echo "raw log: $LOG_BYTES bytes; showing first $HEAD_C + last $TAIL_C"
    echo "---- head ----"
    head -c "$HEAD_C" "$LOG"
    echo
    echo "---- tail ----"
    tail -c "$TAIL_C" "$LOG"
    echo
} > "$LOG.extract"


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
    # SPL-specific words only. "DRAM" and "NAND:" also appear in the main
    # U-Boot banner, so grepping them let a direct-boot u-boot_*.img pass L2.
    if grep -qE "U-Boot SPL|loaded SPL|Trying to boot from" "$LOG"; then
        level=2
        echo "PASS L2: SPL stage ran (SPL banner / boot-mode text present)"
    else
        echo "FAIL L2: no SPL-specific text (generic DRAM/NAND words are not enough)"
    fi
fi

# Graded on its own: gating this behind L2 meant a log whose only flaw was a
# silent SPL never got the banner test at all.
if grep -qE "U-Boot 20[0-9]{2}\." "$LOG"; then
    level=3
    echo "PASS L3: main U-Boot banner reached"
else
    echo "FAIL L3: the main U-Boot banner never appeared"
    echo "      last 20 log lines:"
    tail -20 "$LOG" | sed 's/^/      /'
fi

if [ "$level" -ge 3 ] && grep -qE "Linux version|Booting Linux|Starting kernel" "$LOG"; then
    level=4
    echo "PASS L4: Linux kernel started"
else
    echo "FAIL L4: no Linux boot message"
fi

# ---------------------------------------------------------------- diagnostics
# Guard and printer share one pattern, and it matches what QEMU 9.2 prints:
# physmem.c says "Invalid access to non-RAM device at addr", -d unimp prefixes
# lines with "unimp:", and LOG_GUEST_ERROR is a log-mask name that never
# reaches stderr. With the old pair the guard could fire and the printer then
# print nothing, dropping the diagnostics this block exists for.
GUEST_ERR='unimp:|Invalid access to non-RAM device at addr|Unsupported|no flash image|qemu: fatal'
if grep -qE "$GUEST_ERR" "$LOG"; then
    echo "-- guest errors / fatal (first 15):"
    grep -E "$GUEST_ERR" "$LOG" | head -15 | sed 's/^/   /'
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

