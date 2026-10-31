#!/usr/bin/env python3
"""Drive the OTW-FW 5 provisioning protocol over the USB serial bridge.

Usage: python3 tools/prov_serial.py <port> <command> [<command> ...]
Sends each command, prints every reply line until the stream goes idle.
"""
import sys
import time

import serial

port, *cmds = sys.argv[1:]
s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(2)  # let the board settle / print its boot banner

def drain(idle=1.5, cap=10.0):
    end = time.time() + idle
    lines = []
    buf = b""
    while time.time() < end:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="replace").strip()
                if line:
                    lines.append(line)
                    print("  <", line)
            end = time.time() + idle
    return lines

print("== boot banner ==")
drain()
for c in cmds:
    print("== >", c)
    s.write((c + "\n").encode())
    s.flush()
    drain()
s.close()
