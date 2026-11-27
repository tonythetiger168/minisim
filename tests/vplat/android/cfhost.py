#!/usr/bin/env python3
# cfhost.py DIR [N] - the host end of the Android guest's console ports.
#
# A Cuttlefish guest's emulated HALs talk to programs on the host over
# virtio-console ports (/dev/hvc0 .. /dev/hvcN in the guest). mvplat's
# virtio-console connects each port to a Unix socket ("socket" in the
# platform file); this listens on DIR/hvc0.sock .. DIR/hvc<N-1>.sock (N
# defaults to 20), so start it before mvplat. It answers what a guest needs
# to get through its boot and logs the rest to DIR/hvc<i>.log:
#
#   hvc18, hvc19: the sensors HAL's control and data channels. Messages are
#   Cuttlefish transport messages (an 8-byte header: command in bits 0-30
#   and "is a response" in bit 31 of the first word, payload size in the
#   second, then the payload), carrying the goldfish sensor protocol as text.
#   "list-sensors" is answered with the host's sensor mask: none, so the
#   guest's SensorService starts with no sensors instead of waiting.
import os, selectors, socket, struct, sys

d = sys.argv[1]
n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
SENSORS_CONTROL = 18

sel = selectors.DefaultSelector()
logs, bufs = {}, {}
for i in range(n):
    p = os.path.join(d, 'hvc%d.sock' % i)
    if os.path.exists(p):
        os.unlink(p)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(p)
    s.listen(1)
    sel.register(s, selectors.EVENT_READ, ('listen', i))
    logs[i] = open(os.path.join(d, 'hvc%d.log' % i), 'ab', buffering=0)
print('cfhost: listening on %s/hvc0.sock .. hvc%d.sock' % (d, n - 1), flush=True)


def message(payload, response=True):
    return struct.pack('<II', (1 << 31 if response else 0), len(payload)) + payload


def sensors(conn, i):
    b = bufs[i]
    while len(b) >= 8:
        w0, size = struct.unpack('<II', b[:8])
        if len(b) < 8 + size:
            break
        payload = bytes(b[8:8 + size])
        del b[:8 + size]
        logs[i].write(b'<- %d %r\n' % (w0 & 0x7fffffff, payload))
        if payload.rstrip(b'\0') == b'list-sensors':
            conn.sendall(message(b'0'))
            logs[i].write(b'-> 0\n')


while True:
    for key, _ in sel.select():
        kind, i = key.data
        if kind == 'listen':
            conn, _ = key.fileobj.accept()
            bufs[i] = bytearray()
            sel.register(conn, selectors.EVENT_READ, ('conn', i))
            continue
        try:
            data = key.fileobj.recv(65536)
        except ConnectionError:
            data = b''
        if not data:
            sel.unregister(key.fileobj)
            key.fileobj.close()
            continue
        if i == SENSORS_CONTROL:
            bufs[i] += data
            sensors(key.fileobj, i)
        else:
            logs[i].write(data)
