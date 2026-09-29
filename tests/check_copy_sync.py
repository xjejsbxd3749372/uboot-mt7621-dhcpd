#!/usr/bin/env python3
"""Verify that the functions copied into the test file still match the source.

The host tests compile copies of a handful of functions from
failsafe_xiaomi.c.  Those copies are only meaningful while they are
identical to the real thing, so compare them here and report any function
whose body has drifted.
"""

import re
import sys


def extract(path, name):
    """Return the full definition text of function `name`, or None."""
    text = open(path, encoding="utf-8", errors="replace").read()
    # find a definition: optional static, a return type, then name(
    pattern = re.compile(
        r"^(?:static\s+)?[A-Za-z_][\w \t\*]*?\b" + re.escape(name) + r"\s*\([^;{]*?\)\s*\n\{",
        re.M,
    )
    m = pattern.search(text)
    if not m:
        return None
    start = m.start()
    # brace match from the opening brace
    depth = 0
    i = m.end() - 1
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start : i + 1]
        i += 1
    return None


def normalise(body):
    """Strip the comment banner that precedes the copy in the test file."""
    return body.strip()


FUNCTIONS = [
    "hexval",
    "parse_mac",
    "format_mac",
    "json_escape_x",
    "urldecode_inplace",
    "tar_octal",
    "tar_header_sane",
    "stock_find_member",
    "xiaomi_stock_model_ok",
    "oc_is_valid_mhz",
]


def main():
    src, test = sys.argv[1], sys.argv[2]
    drifted = []
    for fn in FUNCTIONS:
        a = extract(src, fn)
        b = extract(test, fn)
        if a is None:
            print(f"  ?? {fn}: not found in {src}")
            drifted.append(fn)
            continue
        if b is None:
            print(f"  ?? {fn}: not found in {test}")
            drifted.append(fn)
            continue
        if normalise(a) != normalise(b):
            print(f"  -> {fn}: DIFFERS from the source")
            drifted.append(fn)
        else:
            print(f"  ok {fn}")

    if drifted:
        print(f"\n{len(drifted)} function(s) out of sync: {', '.join(drifted)}")
        return 1
    print(f"\nall {len(FUNCTIONS)} functions in sync")
    return 0


if __name__ == "__main__":
    sys.exit(main())
