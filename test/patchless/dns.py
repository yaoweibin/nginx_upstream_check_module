# Minimal DNS responder for the resolve tests.
# usage: dns.py <port> <control.json>
# control.json maps a name to an IPv4 address, to "flap" (the address
# 127.0.0.1 and NXDOMAIN on alternate queries), or to null (NXDOMAIN).
# Answers have a TTL of 1 second.
import json, socket, struct, sys
port, control = int(sys.argv[1]), sys.argv[2]
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", port))
n = 0
while True:
    data, addr = s.recvfrom(512)
    tid = struct.unpack("!H", data[:2])[0]
    i, labels = 12, []
    while data[i]:
        labels.append(data[i + 1:i + 1 + data[i]].decode()); i += data[i] + 1
    q = data[12:i + 5]
    qtype = struct.unpack("!H", data[i + 1:i + 3])[0]
    try:
        ans = json.load(open(control)).get(".".join(labels).lower())
    except Exception:
        ans = None
    if ans == "flap":
        n += 1
        ans = "127.0.0.1" if n % 2 else None
    if ans is None:
        s.sendto(struct.pack("!HHHHHH", tid, 0x8183, 1, 0, 0, 0) + q, addr); continue
    if qtype != 1:
        s.sendto(struct.pack("!HHHHHH", tid, 0x8180, 1, 0, 0, 0) + q, addr); continue
    rr = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 1, 4) + socket.inet_aton(ans)
    s.sendto(struct.pack("!HHHHHH", tid, 0x8180, 1, 1, 0, 0) + q + rr, addr)
