#!/usr/bin/env python3
"""Dump registers and the instructions at $pc from QEMU's monitor, just before
the run is killed.

The stall is only nameable from a PC: the serial log shows where the console
stopped, not what instruction is spinning. The model's PC sampler says "it sits
at 0x87f99514"; this says what 0x87f99514 is. It fires before the timeout
because once the process has been SIGTERMed there is nothing left to ask.

Usage: monitor_dump.py <unix-socket-path | tcp:host:port> <seconds-to-wait> [cmds]

The target argument takes a unix socket path, or "tcp:host:port" - the latter
exists so the same code can be exercised where AF_UNIX is unavailable.
"""
import socket
import sys
import time

TARGET = sys.argv[1]
DELAY = float(sys.argv[2])
CMDS = (sys.argv[3] if len(sys.argv) > 3 else "info registers\nx/24i $pc\n")


def connect(target):
    if target.startswith("tcp:"):
        host, port = target[4:].rsplit(":", 1)
        return socket.create_connection((host, int(port)), timeout=2.0)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.0)
    s.connect(target)
    return s


time.sleep(DELAY)

# Bounded: if the monitor never comes up this must give up rather than hold the
# whole run. An unreachable TCP target stalls on connect() for ~2s each time on
# some systems, so a plain attempt count is not a time limit.
s = None
deadline = time.time() + 12.0
while time.time() < deadline:
    try:
        s = connect(TARGET)
        break
    except OSError:
        time.sleep(0.25)
        s = None
if s is None:
    print("monitor: no connection to %s after 12s" % TARGET, file=sys.stderr)
    sys.exit(1)

s.settimeout(5.0)
s.sendall(CMDS.encode())

buf = b""
last = time.time()
while time.time() - last < 4.0:          # stop once nothing arrives for 4s
    try:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        last = time.time()
    except socket.timeout:
        continue
s.close()

print("---------- monitor dump ($pc) ----------")
out = buf.decode("utf-8", "replace")
for line in out.splitlines():
    if line.strip():
        print(line.rstrip()[:200])
print("-----------------------------------------")
