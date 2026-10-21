# Backends on several ports. Control server on CTL port:
#   /set?port=P&healthy=0|1   /start?port=P   /stop?port=P   /stats
import json, socket, sys, threading, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CTL = int(sys.argv[1])
state = {}      # port -> dict(healthy, health_reqs, health_conns, reqs)
servers = {}
conns = {}      # port -> set of open sockets
lock = threading.Lock()

def make_handler(port):
    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        def log_message(self, *a): pass
        def setup(self):
            super().setup()
            self.counted = False
            with lock:
                conns.setdefault(port, set()).add(self.request)
        def finish(self):
            with lock:
                conns.get(port, set()).discard(self.request)
            super().finish()
        def do_GET(self):
            st = state[port]
            if self.path.startswith("/health"):
                with lock:
                    st["health_reqs"] += 1
                    if not self.counted:
                        st["health_conns"] += 1
                        self.counted = True
                code = 200 if st["healthy"] else 500
            else:
                with lock:
                    st["reqs"] += 1
                code = 200
            body = ("%d\n" % port).encode()
            self.send_response(code)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    return H

class Quiet(ThreadingHTTPServer):
    def handle_error(self, request, client_address): pass

def start(port):
    if port in servers:
        return
    state.setdefault(port, dict(healthy=1, health_reqs=0, health_conns=0, reqs=0))
    srv = Quiet(("127.0.0.1", port), make_handler(port))
    srv.daemon_threads = True
    servers[port] = srv
    threading.Thread(target=srv.serve_forever, daemon=True).start()

def stop(port):
    srv = servers.pop(port, None)
    if srv:
        srv.shutdown(); srv.server_close()
    # a dead backend does not keep answering on open connections
    with lock:
        for c in list(conns.get(port, ())):
            try: c.shutdown(socket.SHUT_RDWR)
            except OSError: pass

class Ctl(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        if u.path == "/start": start(int(q["port"]))
        elif u.path == "/stop": stop(int(q["port"]))
        elif u.path == "/set":
            p = int(q["port"]); state.setdefault(p, dict(healthy=1, health_reqs=0, health_conns=0, reqs=0))
            state[p]["healthy"] = int(q["healthy"])
        body = json.dumps({str(k): v for k, v in state.items()}).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
        self.wfile.write(body)

ThreadingHTTPServer(("127.0.0.1", CTL), Ctl).serve_forever()
