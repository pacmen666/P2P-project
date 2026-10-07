#!/usr/bin/env python3
import socket
import struct
import sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 0))
s.settimeout(2.0)
hdr = struct.pack("!HBBHH", 0x4641, 1, 0x0D, 0, 3)
body = struct.pack("!HB", 1, 2)
s.sendto(hdr + body, ("127.0.0.1", 37402))
try:
    data, addr = s.recvfrom(1024)
    print("ACK_OK", addr, data.hex(), len(data))
    sys.exit(0)
except Exception as e:
    print("NO_ACK", e)
    sys.exit(1)
