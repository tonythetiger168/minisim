"""echo_peer.py - the other end of mvplat's -net dgram link for the network
test: bind LOCAL, send the first frame that arrives back to REMOTE, and exit.

    echo_peer.py LOCAL REMOTE
"""
import os
import socket
import sys

local, remote = sys.argv[1], sys.argv[2]
if os.path.exists(local):
    os.unlink(local)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(local)
s.settimeout(60)
frame = s.recv(65536)
s.sendto(frame, remote)
print("peer: sent back a frame of %d bytes" % len(frame))
os.unlink(local)
