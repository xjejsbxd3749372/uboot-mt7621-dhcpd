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
import os
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

# The socket only exists once QEMU has created it, and QEMU is busy mapping a
# 128MiB flash image first, so waiting is normal. Keep trying until the run's
# own deadline is close: DELAY is already "seconds before the kill", so a fixed
# 12s window gave up while the guest was still starting (observed in CI:
# "no connection to unix:/tmp/qemu-mon-13551.sock after 12s" and no dump).
# The loop still terminates - it is bounded by CONNECT_WINDOW - so a monitor
# that never appears cannot hold the run.
s = None
last_err = None
deadline = time.time() + float(os.environ.get("MON_CONNECT_WINDOW", "75"))
while time.time() < deadline:
    try:
        s = connect(TARGET)
        break
    except OSError as e:
        last_err = e
        time.sleep(0.5)
        s = None
if s is None:
    print("monitor: no connection to %s (waited %.0fs, last error: %s)"
          % (TARGET, float(os.environ.get("MON_CONNECT_WINDOW", "75")), last_err),
          file=sys.stderr)
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
