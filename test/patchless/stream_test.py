# usage: stream_test.py <build> <balancer> <zone:0|1>   (nginx --with-stream)
#
# stream { upstream {} } checks. The stream listeners proxy raw TCP to the
# HTTP backends of backend.py: A is healthy, B serves traffic but fails
# /health, C is not running, D is healthy but "down" in the configuration.
# No connection may reach B, C or D while they are down, B and C must get
# connections again once they recover, and the stream upstreams are shown
# on the http check_status page with "upstream_type": "stream", next to an
# http upstream of the same name.
import os, sys, time
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
from common import *

build, balancer, zone = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
base = int(os.environ.get("PORT_BASE", "21080"))
NG, A, B, C, D, CTL = base, base + 1, base + 2, base + 3, base + 4, base + 10
SB, ST, SK = base + 20, base + 21, base + 22
ng = Nginx(build, "stream.conf", NG)

bal = {"rr": "", "hash": "hash $remote_port;", "chash": "hash $remote_port consistent;",
       "least_conn": "least_conn;", "random": "random;",
       "random2": "random two least_conn;"}[balancer]

ng.write("""
%(load)s
worker_processes 2;
error_log logs/error.log info;
pid logs/nginx.pid;
events { worker_connections 1024; }
stream {
    log_format sl '$upstream_addr $status';
    access_log logs/stream.log sl;
    upstream sbe {
        %(bal)s
        %(zone)s
        server 127.0.0.1:%(A)d;
        server 127.0.0.1:%(B)d;
        server 127.0.0.1:%(C)d;
        server 127.0.0.1:%(D)d down;
        check interval=300 rise=1 fall=1 timeout=500 type=http;
        check_http_send "GET /health HTTP/1.1\\r\\nHost: x\\r\\n\\r\\n";
        check_http_expect_alive http_2xx;
        check_keepalive_requests 5;
    }
    upstream stcp {
        %(bal)s
        %(zone2)s
        server 127.0.0.1:%(C)d;
        server 127.0.0.1:%(A)d;
        check interval=300 rise=1 fall=1 timeout=500 type=tcp;
    }
    upstream sbk {
        %(zone3)s
        server 127.0.0.1:%(C)d;
        server 127.0.0.1:%(A)d backup;
        check interval=300 rise=1 fall=1 timeout=500;
    }
    server { listen %(SB)d; proxy_pass sbe; }
    server { listen %(ST)d; proxy_pass stcp; }
    server { listen %(SK)d; proxy_pass sbk; }
}
http {
    access_log off;
    # an http upstream with the same name as a stream one
    upstream sbe {
        server 127.0.0.1:%(B)d;
        check interval=300 rise=1 fall=1 timeout=500 type=http;
        check_http_send "GET /health HTTP/1.0\\r\\n\\r\\n";
    }
    server {
        listen %(NG)d;
        location / { proxy_pass http://sbe; }
        location /status { check_status json; }
    }
}
""" % dict(load=ng.load("ngx_http_upstream_check_module", "ngx_stream_upstream_check_module"),
           bal=bal, zone=("zone sbe 64k;" if zone else ""), zone2=("zone stcp 64k;" if zone else ""),
           zone3=("zone sbk 64k;" if zone else ""),
           A=A, B=B, C=C, D=D, NG=NG, SB=SB, ST=ST, SK=SK))

def tstatus():
    return {(s.get("upstream_type"), s["upstream"], s["name"]): s["status"] for s in ng.entries()}

def wait(want, secs=8):
    deadline = time.time() + secs
    st = {}
    while time.time() < deadline:
        try:
            st = tstatus()
            if all(st.get(k) == v for k, v in want.items()):
                return st
        except Exception:
            pass
        time.sleep(0.1)
    return st

s = lambda up, p: ("stream", up, "127.0.0.1:%d" % p)
h = lambda up, p: ("http", up, "127.0.0.1:%d" % p)

be = Backends(here, CTL)
try:
    for p in (A, B, D): be("/start?port=%d" % p)
    be("/set?port=%d&healthy=0" % B)
    ng.start()
    print("== stream %s balancer=%s zone=%s dynamic=%s" % (os.path.basename(build), balancer, zone, ng.dynamic))

    st = wait({s("sbe", A): "up", s("sbe", B): "down", s("sbe", C): "down",
               s("stcp", C): "down", s("stcp", A): "up", h("sbe", B): "down"})
    check(st.get(s("sbe", A)) == "up" and st.get(s("sbe", B)) == "down" and st.get(s("sbe", C)) == "down",
          "status page: stream A up, B down (500), C down (dead): %s" % st)
    check(st.get(s("stcp", C)) == "down" and st.get(s("stcp", A)) == "up", "tcp check: C down, A up")
    check(s("sbe", D) not in st, "configured-down server is not checked")
    check(st.get(h("sbe", B)) == "down" and h("sbe", A) not in st,
          "http upstream of the same name is listed apart, as http")
    time.sleep(0.8)   # let every worker sync

    codes, ports = ng.stream_burst(SB, 200)
    check(set(codes) == {200}, "all connections succeed with B,C down: %s" % sorted(set(map(str, codes))))
    check(set(ports) == {A}, "connections only to A with B,C down: %s" % ports)
    codes, ports = ng.stream_burst(ST, 50)
    check(set(codes) == {200} and set(ports) == {A}, "tcp check: connections only to A: %s" % ports)

    # B becomes healthy
    be("/set?port=%d&healthy=1" % B)
    st = wait({s("sbe", B): "up"}); check(st.get(s("sbe", B)) == "up", "B recovers in status page")
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SB, 200)
    check(set(codes) == {200}, "all connections succeed after B recovers")
    check(ports.get(B, 0) > 0 and set(ports) <= {A, B}, "B gets connections again, C/D none: %s" % ports)

    # C (dead) comes back
    be("/start?port=%d" % C)
    st = wait({s("sbe", C): "up", s("stcp", C): "up"})
    check(st.get(s("sbe", C)) == "up" and st.get(s("stcp", C)) == "up", "dead backend C comes back up")
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SB, 300)
    check(set(codes) == {200}, "all connections succeed with A,B,C up")
    check(ports.get(C, 0) > 0, "C gets connections after coming back: %s" % ports)
    check(D not in ports, "configured-down D never gets connections: %s" % ports)
    codes, ports = ng.stream_burst(ST, 100)
    check(set(codes) == {200} and ports.get(C, 0) > 0, "tcp check: C gets connections again: %s" % ports)

    # B fails again, C dies again
    be("/set?port=%d&healthy=0" % B); be("/stop?port=%d" % C)
    st = wait({s("sbe", B): "down", s("sbe", C): "down", s("stcp", C): "down"})
    check(st.get(s("sbe", B)) == "down" and st.get(s("sbe", C)) == "down", "B and C marked down again: %s" % st)
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SB, 200)
    check(set(codes) == {200} and set(ports) == {A}, "connections only to A again: %s" % ports)

    # backup peers are registered and used
    st = tstatus()
    check(st.get(s("sbk", A)) == "up", "backup server is checked")
    codes, ports = ng.stream_burst(SK, 20)
    check(set(codes) == {200} and set(ports) == {A}, "primary down -> backup used: %s" % ports)

    # reload keeps state and keeps working
    ng.signal("reload")
    time.sleep(1.5)
    st = tstatus()
    check(st.get(s("sbe", B)) == "down" and st.get(s("sbe", C)) == "down",
          "B, C still down right after reload: %s" % st)
    st = wait({s("sbe", A): "up", s("sbe", B): "down", s("sbe", C): "down"})
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SB, 200)
    check(set(codes) == {200} and set(ports) == {A}, "after reload, connections only to A: %s" % ports)

    # keepalive checks keep going (check_keepalive_requests 5)
    s1 = be("/stats")[str(A)]
    time.sleep(3)
    s2 = be("/stats")[str(A)]
    n = s2["health_reqs"] - s1["health_reqs"]
    conns = s2["health_conns"] - s1["health_conns"]
    check(n >= 6, "keepalive: checks continue (%d checks in 3s)" % n)
    check(conns > 0 and n / conns > 1.2, "keepalive: connections reused (%d checks on %d connections)" % (n, conns))

    check(not ng.alerts(), "no alerts/crashes: %s" % ng.alerts()[:3])
finally:
    ng.signal("stop"); time.sleep(0.5)
    be.kill()
done("stream %s %s zone=%d" % (os.path.basename(build), balancer, zone))
