# usage: stream_upsync_test.py <build>
#   (nginx --with-stream, built with nginx-stream-upsync-module)
#
# The same as upsync_test.py, for a stream upstream: servers added through
# nginx-stream-upsync-module (from a fake consul) are checked as dynamic
# peers: dead ones get no connections, they recover, keep their state over
# a reload, and their check slots are released once upsync removes them.
import os, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
from common import *

build = sys.argv[1]
base = int(os.environ.get("PORT_BASE", "23080"))
NG, A, E, F, CTL, CONSUL, SL = base, base + 1, base + 5, base + 6, base + 10, base + 50, base + 20
ng = Nginx(build, "stream_upsync.conf", NG)
dump = os.path.join(ng.inst, "conf/stream_servers_test.conf")
upsync_mods = ["ngx_http_upstream_check_module", "ngx_stream_upstream_check_module",
               "ngx_stream_upsync_module"]
# a dynamic nginx-stream-upsync-module built with nginx-upsync-module uses
# the latter's cJSON, so it must be loaded first
if os.path.exists(os.path.join(ng.inst, "modules/ngx_http_upsync_module.so")):
    upsync_mods.insert(2, "ngx_http_upsync_module")
open(dump, "w").write("server 127.0.0.1:%d weight=1 max_fails=0 fail_timeout=10s;\n" % A)

ng.write("""
%s
worker_processes 2;
error_log logs/error.log info;
pid logs/nginx.pid;
events { worker_connections 1024; }
stream {
    log_format sl '$upstream_addr $status';
    access_log logs/stream.log sl;
    upstream test {
        upsync 127.0.0.1:%d/v1/kv/upstreams/test/ upsync_timeout=6m upsync_interval=300ms upsync_type=consul strong_dependency=off;
        upsync_dump_path %s;
        include %s;
        check interval=300 rise=1 fall=1 timeout=500 type=http;
        check_http_send "GET /health HTTP/1.0\\r\\n\\r\\n";
    }
    server {
        listen %d;
        proxy_pass test;
    }
}
http {
    access_log off;
    server {
        listen %d;
        location /status { check_status json; }
    }
}
""" % (ng.load(*upsync_mods), CONSUL, dump, dump, SL, NG))

consul = subprocess.Popen([sys.executable, "-I", os.path.join(here, "consul.py"), str(CONSUL), "test"])
be = Backends(here, CTL)
for p in (A, E): be("/start?port=%d" % p)
def setc(*ports): get("http://127.0.0.1:%d/_set?ports=%s" % (CONSUL, ",".join(map(str, ports))))
s = lambda p: ("test", "127.0.0.1:%d" % p)
try:
    setc(A)
    ng.start()
    print("== stream upsync %s dynamic=%s" % (os.path.basename(build), ng.dynamic))
    st = ng.wait_status({s(A): "up"})
    check(st.get(s(A)) == "up", "configured server up: %s" % st)

    setc(A, E, F)   # E alive, F dead
    st = ng.wait_status({s(E): "up", s(F): "down"})
    check(st.get(s(E)) == "up" and st.get(s(F)) == "down", "upsync servers are checked: %s" % st)
    time.sleep(1.0)
    codes, ports = ng.stream_burst(SL, 200)
    check(set(codes) == {200} and F not in ports and ports.get(E, 0) > 0,
          "connections to A and E, none to dead F: %s" % ports)

    be("/start?port=%d" % F)
    st = ng.wait_status({s(F): "up"})
    check(st.get(s(F)) == "up", "F comes up")
    time.sleep(1.0)
    codes, ports = ng.stream_burst(SL, 200)
    check(set(codes) == {200} and ports.get(F, 0) > 0, "F gets connections: %s" % ports)

    be("/stop?port=%d" % E)
    st = ng.wait_status({s(E): "down"})
    check(st.get(s(E)) == "down", "E goes down")
    time.sleep(1.0)
    codes, ports = ng.stream_burst(SL, 200)
    check(set(codes) == {200} and E not in ports, "no connections to E: %s" % ports)

    setc(A, F)      # upsync removes E
    st = ng.wait_status({s(E): None}, secs=25)
    check(s(E) not in st and st.get(s(F)) == "up", "E's slot released after removal: %s" % st)
    check("release dynamic check peer 127.0.0.1:%d" % E in ng.log("error.log"), "release is logged")
    codes, ports = ng.stream_burst(SL, 100)
    check(set(codes) == {200} and set(ports) == {A, F}, "connections to A and F: %s" % ports)

    # upsync's dump file now lists F, so after a reload F is a configured
    # server; it keeps the state it had as a dynamic peer
    be("/stop?port=%d" % F)
    st = ng.wait_status({s(F): "down"})
    check(st.get(s(F)) == "down", "F goes down")
    ng.signal("reload")
    time.sleep(1.0)
    st = ng.status()
    check(st.get(s(F)) == "down", "F still down right after reload: %s" % st)
    # upsync rebuilds the new workers' peers from consul, which are up
    # until the next sync
    time.sleep(3)
    codes, ports = ng.stream_burst(SL, 100)
    check(set(codes) == {200} and set(ports) == {A}, "after reload only A: %s" % ports)

    check(not ng.alerts(), "no alerts/crashes: %s" % ng.alerts()[:3])
finally:
    ng.signal("stop"); time.sleep(0.5)
    be.kill(); consul.kill()
done("stream upsync " + os.path.basename(build))
