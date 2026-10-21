# usage: stream_resolve_test.py <build>   (nginx 1.27.3+ --with-stream)
#
# The same as resolve_test.py, for a stream upstream with "resolve".
#
# - "dyn.test" resolves to an address which is not in the configuration:
#   the module checks it as a dynamic peer, marks it down while its backend
#   is dead, up once it comes back, keeps its state over a reload, and
#   releases the check slot once the name no longer resolves.
# - "flap.test" resolves to a configured (dead) address and NXDOMAIN on
#   alternate queries, so nginx keeps re-creating the peer, which must be
#   marked down again every time.
import json, os, re, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, here)
from common import *

build = sys.argv[1]
base = int(os.environ.get("PORT_BASE", "22080"))
NG, A, C, E, CTL, DNS, SL = base, base + 1, base + 3, base + 5, base + 10, base + 53, base + 20
ng = Nginx(build, "stream_resolve.conf", NG)
control = os.path.join(ng.inst, "conf/stream_dns.json")
json.dump({"dyn.test": "127.0.0.1", "flap.test": "flap"}, open(control, "w"))

ng.write("""
%s
worker_processes 2;
error_log logs/error.log debug;
pid logs/nginx.pid;
events { worker_connections 1024; }
http {
    access_log off;
    upstream hb {
        server 127.0.0.1:%d;
        check interval=300 rise=1 fall=1 timeout=500;
    }
    server {
        listen %d;
        location /status { check_status json; }
    }
}
stream {
    # the http block, which comes first here, added the zone: this
    # enlarges it
    check_shm_size 2m;
    log_format sl '$upstream_addr $status';
    access_log logs/stream.log sl;
    upstream rs {
        zone rs 64k;
        resolver 127.0.0.1:%d valid=1s;
        server 127.0.0.1:%d;
        server 127.0.0.1:%d;
        server flap.test:%d resolve;
        server dyn.test:%d resolve;
        check interval=300 rise=1 fall=1 timeout=500 type=http;
        check_http_send "GET /health HTTP/1.0\\r\\n\\r\\n";
    }
    server {
        listen %d;
        proxy_pass rs;
    }
}
""" % (ng.load("ngx_http_upstream_check_module", "ngx_stream_upstream_check_module"),
       A, NG, DNS, A, C, C, E, SL))

dns = subprocess.Popen([sys.executable, "-I", os.path.join(here, "dns.py"), str(DNS), control])
be = Backends(here, CTL)
be("/start?port=%d" % A)
dynE = ("rs", "127.0.0.1:%d" % E)
try:
    ng.start()
    print("== stream resolve %s dynamic=%s" % (os.path.basename(build), ng.dynamic))

    st = ng.wait_status({("rs", "127.0.0.1:%d" % A): "up", dynE: "down"})
    check(st.get(dynE) == "down", "resolve-only server is checked and down while dead: %s" % st)
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SL, 100)
    check(set(codes) == {200} and E not in ports, "no traffic to the dead resolved server: %s" % ports)

    be("/start?port=%d" % E)
    st = ng.wait_status({dynE: "up"})
    check(st.get(dynE) == "up", "resolved server comes up when its backend starts")
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SL, 100)
    check(set(codes) == {200} and ports.get(E, 0) > 0, "resolved server gets traffic: %s" % ports)

    # state is kept over a reload (default_down would make it down)
    ng.signal("reload")
    time.sleep(1.0)
    st = ng.status()
    check(st.get(dynE) == "up", "resolved server is still up right after reload: %s" % st.get(dynE))

    be("/stop?port=%d" % E)
    st = ng.wait_status({dynE: "down"})
    check(st.get(dynE) == "down", "resolved server goes down when its backend stops")
    time.sleep(0.8)
    codes, ports = ng.stream_burst(SL, 100)
    check(set(codes) == {200} and E not in ports, "no traffic to it once down: %s" % ports)

    marks = re.findall(r"stream check sync peer 127\.0\.0\.1:%d in upstream rs: down" % C, ng.log("error.log"))
    check(len(marks) >= 2, "re-created flap.test peer re-marked down %d times" % len(marks))

    # name gone: the check slot is released after the grace period (10s)
    json.dump({"flap.test": "flap"}, open(control, "w"))
    st = ng.wait_status({dynE: None}, secs=25)
    check(dynE not in st, "check slot released once the name no longer resolves")
    check("release dynamic check peer 127.0.0.1:%d" % E in ng.log("error.log"), "release is logged")

    check(not ng.alerts(), "no alerts/crashes: %s" % ng.alerts()[:3])
finally:
    ng.signal("stop"); time.sleep(0.5)
    be.kill(); dns.kill()
done("stream resolve " + os.path.basename(build))
