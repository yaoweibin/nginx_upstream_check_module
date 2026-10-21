# Fake consul KV for nginx-upsync-module.
# usage: consul.py <port> <upstream>; GET /_set?ports=P1,P2 changes the servers
import base64, json, sys, threading, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
port, upstream = int(sys.argv[1]), sys.argv[2]
state = {"index": 1, "ports": []}
value = base64.b64encode(b'{"weight":1,"max_fails":0,"fail_timeout":10}').decode()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/_set":
            q = dict(urllib.parse.parse_qsl(u.query))
            state["ports"] = [int(p) for p in q.get("ports", "").split(",") if p]
            state["index"] += 1
            body = b"ok"
        else:
            body = json.dumps([{"Key": "upstreams/%s/127.0.0.1:%d" % (upstream, p), "Value": value}
                               for p in state["ports"]]).encode()
        self.send_response(200)
        self.send_header("X-Consul-Index", str(state["index"]))
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
