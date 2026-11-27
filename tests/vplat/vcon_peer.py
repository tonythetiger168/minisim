"""vcon_peer.py - the host end of a virtio-console "socket" for the console
test: listen on PATH, answer the guest's greeting with a typed line, and
print everything the guest sent once it closes or says what it got.

    vcon_peer.py PATH
"""
import os
import socket
import sys

path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(path)
s.listen(1)
s.settimeout(60)
c, _ = s.accept()
c.settimeout(60)
got = b""
sent = False
while b"vcon: got " not in got or not got.endswith(b"\n"):
    b = c.recv(4096)
    if not b:
        break
    got += b
    if not sent and b"hello over virtio-console\n" in got:
        c.sendall(b"line from the host\n")
        sent = True
sys.stdout.write(got.decode(errors="replace"))
os.unlink(path)
