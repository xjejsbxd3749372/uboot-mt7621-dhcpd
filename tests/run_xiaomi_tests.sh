#!/bin/sh
# Run the host-side unit tests for failsafe_xiaomi.c.
#
# The test file contains verbatim copies of the functions under test.  If
# the real file is edited and the copies are not refreshed, these tests
# would silently keep validating the old code, so verify the copies match
# before running anything.

set -e

cd "$(dirname "$0")/.."

SRC="failsafe/failsafe_xiaomi.c"
TEST="tests/test_xiaomi_logic.c"

if [ ! -f "$SRC" ]; then
	echo "error: $SRC not found" >&2
	exit 1
fi

echo "--- checking that the tested copies match $SRC ---"
python3 tests/check_copy_sync.py "$SRC" "$TEST" || {
	echo "error: the test file no longer matches the source" >&2
	echo "       re-copy the functions listed above into $TEST" >&2
	exit 1
}
echo "ok: copies are in sync"
echo

CC="${CC:-cc}"
echo "--- building with $CC ---"
"$CC" -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	-fno-strict-aliasing \
	-o /tmp/test_xiaomi_logic "$TEST"

echo "--- running ---"
/tmp/test_xiaomi_logic
