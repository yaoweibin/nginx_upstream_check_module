# usage: run_test.py <build_dir> <balancer> <zone:0|1>
import json, os, re, subprocess, sys, time, urllib.request

S = os.path.dirname(os.path.abspath(__file__))
build, balancer, zone = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
inst = os.path.join(build, "inst")
dynamic = os.path.exists(os.path.join(inst, "modules/ngx_http_upstream_check_module.so"))
nginx = os.path.join(inst, "sbin/nginx")
base = int(os.environ.get("PORT_BASE", "18080"))
NG, CTL = base, base + 10
A, B, C, D = base + 1, base + 2, base + 3, base + 4

failures = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: failures.append(msg)

def get(url, timeout=3):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()

def ctl(path): return json.loads(get("http://127.0.0.1:%d%s" % (CTL, path))[1])

bal = {"rr": "", "hash": "hash $arg_k;", "chash": "hash $arg_k consistent;",
       "ip_hash": "ip_hash;", "least_conn": "least_conn;",
       "random": "random;", "random2": "random two least_conn;"}[balancer]

conf = """
%(load)s
worker_processes %(workers)d;
error_log logs/error.log info;
pid logs/nginx.pid;
events { worker_connections 1024; }
http {
    log_format ul '$request_uri $upstream_addr $status';
    access_log logs/access.log ul;
    upstream be {
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
    upstream bk {
        %(zone2)s
        server 127.0.0.1:%(C)d;
        server 127.0.0.1:%(A)d backup;
        check interval=300 rise=1 fall=1 timeout=500 type=http;
        check_http_send "GET /health HTTP/1.0\\r\\n\\r\\n";
    }
    server {
        listen %(NG)d;
        location / { proxy_pass http://be; }
        location /bk { proxy_pass http://bk; }
        location /status { check_status json; }
    }
}
""" % dict(load=('load_module modules/ngx_http_upstream_check_module.so;' if dynamic else ''),
           bal=bal, zone=("zone be 64k;" if zone else ""), zone2=("zone bk 64k;" if zone else ""),
           A=A, B=B, C=C, D=D, NG=NG, workers=int(os.environ.get("WORKERS", "2")))

os.makedirs(os.path.join(inst, "logs"), exist_ok=True)
open(os.path.join(inst, "conf/test.conf"), "w").write(conf)
for f in ("access.log", "error.log"):
    p = os.path.join(inst, "logs", f)
    if os.path.exists(p): os.unlink(p)

def status():
    code, body = get("http://127.0.0.1:%d/status?format=json" % NG)
    d = json.loads(body)
    return {(s["upstream"], s["name"]): s["status"] for s in d["servers"]["server"]}

def wait_status(want, secs=6):
    deadline = time.time() + secs
    while time.time() < deadline:
        try:
            st = status()
            if all(st.get(k) == v for k, v in want.items()):
                return st
        except Exception:
            pass
        time.sleep(0.1)
    return status()

def access_lines():
    return open(os.path.join(inst, "logs/access.log")).read().splitlines()

def burst(n, path="/"):
    start = len(access_lines())
    codes = [get("http://127.0.0.1:%d%s?k=%d" % (NG, path, i))[0] for i in range(n)]
    time.sleep(0.3)
    lines = [l for l in access_lines()[start:] if not l.startswith("/status")]
    ports = {}
    for l in lines:
        for port in re.findall(r"127\.0\.0\.1:(\d+)", l):
            ports[int(port)] = ports.get(int(port), 0) + 1
    return codes, ports

def a(p): return ("be", "127.0.0.1:%d" % p)

be = subprocess.Popen([sys.executable, "-I", os.path.join(S, "backend.py"), str(CTL)], stderr=subprocess.DEVNULL)
time.sleep(0.5)
try:
    for p in (A, B, D): ctl("/start?port=%d" % p)
    ctl("/set?port=%d&healthy=0" % B)

    t = subprocess.run([nginx, "-p", inst, "-c", "conf/test.conf", "-t"], capture_output=True, text=True)
    if t.returncode: print(t.stderr); sys.exit(1)
    subprocess.run([nginx, "-p", inst, "-c", "conf/test.conf"], check=True)

    print("== %s balancer=%s zone=%s dynamic=%s" % (os.path.basename(build), balancer, zone, dynamic))

    st = wait_status({a(A): "up", a(B): "down", a(C): "down"})
    check(st.get(a(A)) == "up" and st.get(a(B)) == "down" and st.get(a(C)) == "down",
          "status page: A up, B down (500), C down (dead): %s" % st)
    check(a(D) not in st, "configured-down server is not checked")
    time.sleep(0.8)   # let every worker sync

    codes, ports = burst(200)
    check(set(codes) == {200}, "all requests succeed with B,C down: %s" % sorted(set(codes)))
    check(set(ports) == {A}, "traffic only to A with B,C down: %s" % ports)

    # B becomes healthy
    ctl("/set?port=%d&healthy=1" % B)
    st = wait_status({a(B): "up"}); check(st.get(a(B)) == "up", "B recovers in status page")
    time.sleep(0.8)
    codes, ports = burst(200)
    check(set(codes) == {200}, "all requests succeed after B recovers")
    if balancer == "ip_hash":
        check(set(ports) <= {A, B}, "ip_hash: traffic to A/B only: %s" % ports)
    else:
        check(ports.get(B, 0) > 0 and set(ports) <= {A, B}, "B gets traffic again, C/D none: %s" % ports)

    # C (dead) comes back
    ctl("/start?port=%d" % C)
    st = wait_status({a(C): "up"}); check(st.get(a(C)) == "up", "dead backend C comes back up")
    time.sleep(0.8)
    codes, ports = burst(300)
    check(set(codes) == {200}, "all requests succeed with A,B,C up")
    if balancer != "ip_hash":
        check(ports.get(C, 0) > 0, "C gets traffic after coming back: %s" % ports)
    check(D not in ports, "configured-down D never gets traffic: %s" % ports)

    # B fails again, C dies again
    ctl("/set?port=%d&healthy=0" % B); ctl("/stop?port=%d" % C)
    st = wait_status({a(B): "down", a(C): "down"})
    check(st.get(a(B)) == "down" and st.get(a(C)) == "down", "B and C marked down again: %s" % st)
    time.sleep(0.8)
    codes, ports = burst(200)
    check(set(codes) == {200} and set(ports) == {A}, "traffic only to A again: %s %s" % (sorted(set(codes)), ports))

    # backup peers are registered and used
    st = status()
    check(st.get(("bk", "127.0.0.1:%d" % A)) == "up", "backup server is checked")
    codes, ports = burst(20, "/bk")
    check(set(codes) == {200} and set(ports) == {A}, "primary down -> backup used: %s" % ports)

    # reload keeps state and keeps working
    subprocess.run([nginx, "-p", inst, "-c", "conf/test.conf", "-s", "reload"], check=True)
    time.sleep(1.5)
    st = wait_status({a(A): "up", a(B): "down", a(C): "down"})
    check(st.get(a(A)) == "up" and st.get(a(B)) == "down", "after reload: %s" % st)
    time.sleep(0.8)
    codes, ports = burst(200)
    check(set(codes) == {200} and set(ports) == {A}, "after reload, traffic only to A: %s %s" % (sorted(set(codes)), ports))

    # keepalive checks keep going (check_keepalive_requests 5)
    s1 = ctl("/stats")[str(A)]
    time.sleep(3)
    s2 = ctl("/stats")[str(A)]
    n = s2["health_reqs"] - s1["health_reqs"]
    conns = s2["health_conns"] - s1["health_conns"]
    check(n >= 6, "keepalive: checks continue (%d checks in 3s)" % n)
    check(conns > 0 and n / conns > 1.2, "keepalive: connections reused (%d checks on %d connections)" % (n, conns))

    err = open(os.path.join(inst, "logs/error.log")).read()
    crit = [l for l in err.splitlines() if re.search(r"\[(alert|crit|emerg)\]", l)]
    check(not crit, "no alert/crit in error.log: %s" % crit[:3])
finally:
    subprocess.run([nginx, "-p", inst, "-c", "conf/test.conf", "-s", "stop"])
    time.sleep(0.5)
    be.kill()

print("RESULT %s %s zone=%d: %s" % (os.path.basename(build), balancer, zone, "PASS" if not failures else "FAIL (%d)" % len(failures)))
sys.exit(1 if failures else 0)
