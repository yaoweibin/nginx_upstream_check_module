# nginx_upstream_check_module

Health checks for the servers of nginx upstreams, in `http { upstream {} }`
and `stream { upstream {} }`.

This is a maintained fork of
[yaoweibin/nginx_upstream_check_module](https://github.com/yaoweibin/nginx_upstream_check_module),
which no longer reviews pull requests. It works on stock nginx without
patching the nginx core. See [Changes in this fork](#changes-in-this-fork)
for what it adds and fixes.

## Table of contents

- [Synopsis](#synopsis)
- [Description](#description)
- [Changes in this fork](#changes-in-this-fork)
- [Directives](#directives)
  - [check](#check)
  - [check_http_send](#check_http_send)
  - [check_http_expect_alive](#check_http_expect_alive)
  - [check_keepalive_requests](#check_keepalive_requests)
  - [check_fastcgi_param](#check_fastcgi_param)
  - [check_shm_size](#check_shm_size)
  - [check_status](#check_status)
- [Installation](#installation)
- [Choosing a method: stock nginx or a patched core](#choosing-a-method-stock-nginx-or-a-patched-core)
- [How it works without the patch](#how-it-works-without-the-patch)
  - [Servers added at run time](#servers-added-at-run-time)
  - [Stream upstreams](#stream-upstreams)
  - [Limitations without the patch](#limitations-without-the-patch)
- [Patching the nginx core](#patching-the-nginx-core)
- [Compatibility](#compatibility)
- [Changelog](#changelog)
- [Authors](#authors)
- [Copyright and license](#copyright-and-license)

## Synopsis

```nginx
http {

    upstream cluster {

        # simple round-robin
        server 192.168.0.1:80;
        server 192.168.0.2:80;

        check interval=5000 rise=1 fall=3 timeout=4000;

        #check interval=3000 rise=2 fall=5 timeout=1000 type=ssl_hello;

        #check interval=3000 rise=2 fall=5 timeout=1000 type=http;
        #check_http_send "HEAD / HTTP/1.0\r\n\r\n";
        #check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 80;

        location / {
            proxy_pass http://cluster;
        }

        location /hmstatus {
            check_status;

            access_log   off;
            allow SOME.IP.ADD.RESS;
            deny all;
        }
    }
}

stream {

    upstream db {
        server 192.168.0.1:3306;
        server 192.168.0.2:3306;

        check interval=3000 rise=2 fall=5 timeout=1000 type=mysql;
    }

    server {
        listen 3306;
        proxy_pass db;
    }
}
```

## Description

Adds health checks for the servers of `http { upstream {} }` and, on stock
nginx, `stream { upstream {} }` (see [Stream upstreams](#stream-upstreams)).
A server that fails its checks is taken out of the load balancing until it
passes them again.

## Changes in this fork

Everything below is in this fork only, not in
[upstream](https://github.com/yaoweibin/nginx_upstream_check_module). The
fork is otherwise in sync with upstream, up to and including
`check_1.30.4+.patch` (September 2026).

### New features

- **No nginx patch needed.** On nginx 1.9.0 and later the module works on
  stock nginx. You no longer need a `check_*.patch` for each nginx release.
  See [How it works without the patch](#how-it-works-without-the-patch).
- **Dynamic module.** It can be built with `--add-dynamic-module` (nginx
  1.9.11+) and loaded with `load_module`.
- **Stream upstreams.** `check` works in `stream { upstream {} }`, with the
  `tcp`, `http`, `ssl_hello`, `mysql` and `ajp` check types. See
  [Stream upstreams](#stream-upstreams).
- **Servers added at run time are checked.** This covers servers resolved
  with the `resolve` parameter (nginx 1.27.3+), and servers added by
  [nginx-upsync-module](https://github.com/weibocom/nginx-upsync-module) or
  [nginx-stream-upsync-module](https://github.com/xiaokai-wang/nginx-stream-upsync-module).
  See [Servers added at run time](#servers-added-at-run-time).
- **All built-in load balancing methods.** On stock nginx, round robin,
  `hash`, `hash ... consistent`, `ip_hash`, `least_conn` and `random`
  (including `random two`) all skip failed servers, with or without `zone`.
  The patches never covered `random`.
- **A shared server is checked once.** If several upstreams list the same
  server with identical check settings, the server is checked once and all
  of them use the result. See [check](#check).

### Fixes

- **Checks stopped with `check_keepalive_requests` > 1.** Each worker ran
  one check and then never checked again, so the rise count stopped at the
  number of workers. Upstream
  [#91](https://github.com/yaoweibin/nginx_upstream_check_module/pull/91),
  [#268](https://github.com/yaoweibin/nginx_upstream_check_module/issues/268).
- **`type=tcp` kept a dead server up.** The check connection was never
  closed, so later checks only looked at an idle socket. A server that
  disappeared without closing its connections stayed up forever, and every
  worker held a connection to every server. Upstream
  [#115](https://github.com/yaoweibin/nginx_upstream_check_module/issues/115),
  [#267](https://github.com/yaoweibin/nginx_upstream_check_module/issues/267),
  [#275](https://github.com/yaoweibin/nginx_upstream_check_module/issues/275),
  [#285](https://github.com/yaoweibin/nginx_upstream_check_module/issues/285),
  [#287](https://github.com/yaoweibin/nginx_upstream_check_module/pull/287).
- **`type=tcp` checks left sockets in TIME_WAIT.** Each check left a socket
  in TIME_WAIT on the nginx side for 60 seconds (60 / interval sockets per
  server). They now close with a reset, which leaves none. Fork
  [#2](https://github.com/gfrankliu/nginx_upstream_check_module/issues/2).
- **One idle check connection per worker.** With `check_keepalive_requests`
  > 1, a server could end up with an idle check connection from almost every
  worker (up to 64 with `worker_processes auto` on a 64 CPU host). It now
  has one.
- **Health state lost after a reload.** After a reload that failed late
  (e.g. a `bind()` error), the next good reload reset every server to its
  initial state, which is down by default.
- **Crash with `master_process off`.** Upstream
  [#225](https://github.com/yaoweibin/nginx_upstream_check_module/issues/225).
- **Invalid JSON on the status page** with `?status=up` or `?status=down`.
  Upstream
  [#94](https://github.com/yaoweibin/nginx_upstream_check_module/issues/94),
  [#121](https://github.com/yaoweibin/nginx_upstream_check_module/issues/121).
- **`port=0` rejected** although it is the documented default, while ports
  above 65535 were accepted. Upstream
  [#203](https://github.com/yaoweibin/nginx_upstream_check_module/issues/203).
- **Two workers checking a server at the same time.** A worker whose cached
  time was behind could take over a check another worker was running. An
  idle connection closed by the server could also release another worker's
  check.

## Directives

### check

```
Syntax:  check interval=milliseconds [fall=count] [rise=count]
               [timeout=milliseconds] [default_down=true|false]
               [type=tcp|http|ssl_hello|mysql|ajp|fastcgi] [port=check_port];
Default: interval=30000 fall=5 rise=2 timeout=1000 default_down=true type=tcp
Context: upstream (http, stream)
```

Adds health checks for the servers of the upstream. The parameters are:

- `interval`: the interval between two checks of a server.
- `fall` (fall_count): the server is marked down after this many failed
  checks in a row.
- `rise` (rise_count): the server is marked up after this many successful
  checks in a row.
- `timeout`: the timeout of a check.
- `default_down`: the initial state of the servers. The default is down.
- `port`: the port to check on the servers. It can differ from the port of
  the server. The default, 0, means the port of the server.
- `type`: the check protocol:
  1. `tcp`: connects to the server and peeks one byte. The connection is
     then closed with a reset (RST), so the checks leave no sockets in
     TIME_WAIT. The server may log the check as a connection reset by the
     peer.
  2. `ssl_hello`: sends an SSL client hello and receives the server hello.
  3. `http`: sends an HTTP request, then receives and parses the response
     to decide whether the server is alive.
  4. `mysql`: connects to the MySQL server and receives the greeting.
  5. `ajp`: sends an AJP Cping packet, then receives and parses the Cpong.
  6. `fastcgi`: sends a FastCGI request, then receives and parses the
     response. This type is only available in http upstreams.

Each server is checked once per interval, however many worker processes
there are. If the same server (address and port) is listed in several
upstreams, or twice in one, and all its check settings are the same
(`check`, `check_keepalive_requests`, `check_http_send`,
`check_http_expect_alive` and `check_fastcgi_param`), it is checked only
once and all its entries share the result. They are still listed separately
on the status page. If any of the settings differ, e.g. a different `Host`
header in `check_http_send`, each upstream checks the server on its own.

### check_http_send

```
Syntax:  check_http_send http_packet;
Default: "GET / HTTP/1.0\r\n\r\n"
Context: upstream (http, stream)
```

The request sent to the servers by the `http` check type.

### check_http_expect_alive

```
Syntax:  check_http_expect_alive [http_2xx | http_3xx | http_4xx | http_5xx];
Default: http_2xx | http_3xx
Context: upstream (http, stream)
```

The response status codes that mean the server is alive.

### check_keepalive_requests

```
Syntax:  check_keepalive_requests num;
Default: check_keepalive_requests 1;
Context: upstream (http, stream)
```

The number of check requests sent on one connection. The default, 1, means
the connection is closed after each check. The `tcp` check type always uses
a new connection.

### check_fastcgi_param

```
Syntax:  check_fastcgi_param parameter value;
Default: see below
Context: upstream (http only)
```

The FastCGI parameters sent by the `fastcgi` check type. The defaults are:

```nginx
check_fastcgi_param "REQUEST_METHOD" "GET";
check_fastcgi_param "REQUEST_URI" "/";
check_fastcgi_param "SCRIPT_FILENAME" "index.php";
```

### check_shm_size

```
Syntax:  check_shm_size size;
Default: 1M
Context: http, stream
```

The size of the shared memory used by the checks. If you check thousands of
servers, 1M may not be enough. The http and the stream upstreams share one
zone: if both blocks set it, the larger size is used.

### check_status

```
Syntax:  check_status [html|csv|json];
Default: none
Context: location
```

Shows the status of the checked servers over HTTP. Put it in a location in
the http block.

The default format is `html`. You can pick the format with the `format`
argument. If the `check_status` location is `/hmstatus`:

```
/hmstatus?format=html
/hmstatus?format=csv
/hmstatus?format=json
```

The servers of the stream upstreams are listed on the same page. The last
column (`Upstream type` in html, the last field in csv, `upstream_type` in
json) is `http` or `stream`, since an http and a stream upstream may have
the same name. There is no `check_status` in the stream block: put it in an
http server, even if all the checked upstreams are stream ones.

The `status` argument lists only the servers that are up or down:

```
/hmstatus?format=html&status=down
/hmstatus?format=csv&status=up
```

Sample html page:

```html
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Strict//EN
"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd">
<title>Nginx http upstream check status</title>
    <h1>Nginx http upstream check status</h1>
    <h2>Check upstream server number: 1, generation: 3</h2>
            <th>Index</th>
            <th>Upstream</th>
            <th>Name</th>
            <th>Status</th>
            <th>Rise counts</th>
            <th>Fall counts</th>
            <th>Check type</th>
            <th>Check port</th>
            <th>Upstream type</th>
            <td>0</td>
            <td>backend</td>
            <td>106.187.48.116:80</td>
            <td>up</td>
            <td>39</td>
            <td>0</td>
            <td>http</td>
            <td>80</td>
            <td>http</td>
```

Sample csv page:

```
0,backend,106.187.48.116:80,up,46,0,http,80,http
```

Sample json page:

```json
{"servers": {
  "total": 1,
  "generation": 3,
  "server": [
   {"index": 0, "upstream": "backend", "name": "106.187.48.116:80", "status": "up", "rise": 58, "fall": 0, "type": "http", "port": 80, "upstream_type": "http"}
  ]
 }}
```

## Installation

Get the module from
[github.com/gfrankliu/nginx_upstream_check_module](https://github.com/gfrankliu/nginx_upstream_check_module)
and the nginx source code from [nginx.org](https://nginx.org/), then build
nginx with the module. Since nginx 1.9.0 no patch to the nginx core is
needed, and the module can be built either statically or as a dynamic
module:

```sh
wget 'https://nginx.org/download/nginx-1.30.6.tar.gz'
tar -xzvf nginx-1.30.6.tar.gz
cd nginx-1.30.6/

./configure --add-module=/path/to/nginx_upstream_check_module
# or
./configure --add-dynamic-module=/path/to/nginx_upstream_check_module

make
make install
```

For the dynamic module, load it at the top of `nginx.conf`:

```nginx
load_module modules/ngx_http_upstream_check_module.so;
```

When nginx is configured with `--with-stream`, the checks for the stream
upstreams are built too, as `ngx_stream_upstream_check_module`. As a dynamic
module it is a separate file, which uses the http module and must be loaded
after it (and, with `--with-stream=dynamic`, after the stream module):

```nginx
load_module modules/ngx_stream_module.so;    # --with-stream=dynamic
load_module modules/ngx_http_upstream_check_module.so;
load_module modules/ngx_stream_upstream_check_module.so;
```

With `--with-stream=dynamic`, the stream checks are only built with
`--add-dynamic-module`. They are not built with the patched core.

## Choosing a method: stock nginx or a patched core

The module can work on stock nginx (no patch, nginx 1.9.0+) or with a
patched nginx core (one of the `check_*.patch` files). The `config` script
picks the method by looking at the nginx sources: if the core is patched it
builds the patched method, otherwise the stock one.

| | Stock nginx (no patch) | Patched core |
|---|---|---|
| nginx versions | 1.9.0+ | any version with a matching patch |
| `--add-dynamic-module` | yes (1.9.11+) | no, `--add-module` only |
| Re-patch for new nginx releases | no | yes |
| http upstreams | yes | yes |
| stream upstreams | yes | no |
| Servers added at run time (`resolve`, nginx-upsync-module, nginx-stream-upsync-module) | checked | not checked |
| Built-in balancers (round robin, hash, ip_hash, least_conn, random) | yes | yes, except random |
| Third party balancers patched for this module (upstream fair, sticky, jvm_route) | no | yes |
| A failed server leaves the balancers | within one sync period (at most 500 ms) | on the next request |

Use stock nginx unless you need nginx older than 1.9.0, or one of the third
party balancers patched for this module, or cannot accept a failed server
taking requests for up to one sync period. The run time servers, the stream
upstreams and the dynamic module are only available on stock nginx.

With the patched core, nginx-upsync-module's own check hooks call
`ngx_http_upstream_check_add_dynamic_peer()`, which this module does not
provide, so nginx-upsync-module cannot be built together with this module
on a patched core.

## How it works without the patch

When the configuration is loaded, the module registers the servers of every
upstream with `check` from the upstream's round robin peers, matching them
by address. Every worker then copies the check status from the shared
memory into the peers' `down` flag at least every 500 milliseconds (or every
half of the smallest check interval). All the built-in load balancing
methods (round robin, hash, hash consistent, ip_hash, least_conn and random)
skip peers with this flag set, with or without `zone`. Servers marked `down`
in the configuration are not checked and are never marked up.

### Servers added at run time

Servers which show up in such an upstream at run time are checked too (on
stock nginx only, the patched core does not check them), for example
servers resolved with the `resolve` parameter (nginx 1.27.3+) or added by
nginx-upsync-module. The sync notices a peer it has no check for, and adds a
check for its address in a slot of the shared memory, shared by all the
workers. The new check starts in the `default_down` state. Once no worker
has seen the address in the upstream for 10 check intervals (at least 10
seconds), the slot is released. The state of these checks is kept over a
reload, and they are listed on the status page like the configured servers.
Half of `check_shm_size` (1M by default, about 1000 slots) is reserved for
them; raise `check_shm_size` if nginx logs "no free slot to check peer".

### Stream upstreams

The `check` directives work the same way in `stream { upstream {} }`, with
the check types `tcp`, `http`, `ssl_hello`, `mysql` and `ajp`. The stream
round robin peers have the same `down` flag, which the stream balancers
(round robin, hash, hash consistent, least_conn and random) honour, so the
stream servers are checked and marked down as above, with or without
`zone`, including servers added at run time by `resolve` (nginx 1.27.3+)
and by nginx-stream-upsync-module. The check engine, its shared memory, the
slots for the run time servers and the status page are shared with the http
upstreams. The stream checks need stock nginx: with a patched core they are
not built.

### Limitations without the patch

- Third party load balancing methods which do not keep the round robin
  peers in the upstream (for example upstream fair) are not supported;
  nginx logs a warning and the `check` of such an upstream is ignored. Use
  the patched core for them.
- A server goes down in the balancers up to one sync period after the check
  marks it down. A server added at run time takes traffic until the next
  sync, when it gets its `default_down` state, and a peer re-created by the
  resolver starts as up until the next sync.
- nginx-upsync-module (and nginx-stream-upsync-module): servers in the file
  named by `upsync_dump_path` are configured servers once nginx reloads, and
  stay checked (and listed on the status page) until the next reload, even
  if upsync removes them. Its own check hooks (`NGX_HTTP_UPSTREAM_CHECK`)
  are not used, nor needed, without the patch.

The tests for the unpatched mode are in [`test/patchless/`](test/patchless/).

## Patching the nginx core

Patching the nginx core, as in the older versions of this module, is still
supported. The `config` script detects a patched core and then builds the
module in the patched mode (`NGX_HTTP_UPSTREAM_CHECK` is defined and the
balancers call into the module directly). The patched mode needs
`--add-module`, it cannot be built as a dynamic module. It is required for
nginx versions before 1.9.0 and for the patched third party modules below.
To use it, apply the patch matching your nginx version before running
`configure`:

```sh
patch -p1 < /path/to/nginx_upstream_check_module/check_1.20.1+.patch
```

| nginx version | Patch |
|---|---|
| 1.2.1, 1.3.0 | `check_1.2.1.patch` |
| 1.2.2+, 1.3.1+ (added least_conn) | `check_1.2.2+.patch` |
| 1.2.6+, 1.3.9+ (round robin changes) | `check_1.2.6+.patch` |
| 1.5.12+ | `check_1.5.12+.patch` |
| 1.7.2+ | `check_1.7.2+.patch` |
| 1.20.1+ | `check_1.20.1+.patch` |
| 1.30.4+ (sticky `sid` code and per-peer locking in round robin, hash and ip_hash) | `check_1.30.4+.patch` |

The patch adds support for the official round robin, ip_hash and
least_conn upstream modules. Other upstream modules can be supported in the
same way; see the patch for details.

To add support for the upstream fair module:

```sh
git clone git://github.com/gnosek/nginx-upstream-fair.git
cd nginx-upstream-fair
patch -p2 < /path/to/nginx_upstream_check_module/upstream_fair.patch
cd /path/to/nginx-1.0.14
./configure --add-module=/path/to/nginx_upstream_check_module --add-module=/path/to/nginx-upstream-fair-module
make
make install
```

To add support for the nginx sticky module:

```sh
svn checkout http://nginx-sticky-module.googlecode.com/svn/trunk/ nginx-sticky-module
cd nginx-sticky-module
patch -p0 < /path/to/nginx_upstream_check_module/nginx-sticky-module.patch
cd /path/to/nginx-1.0.14
./configure --add-module=/path/to/nginx_upstream_check_module --add-module=/path/to/nginx-sticky-module
make
make install
```

Note that the nginx-sticky-module also needs the original `check.patch`.

## Compatibility

- The module version 0.1.5 should be compatible with nginx 0.7.67+.
- The module version 0.1.8 should be compatible with nginx 1.0.14+.
- This fork works on stock nginx 1.9.0+ and has been tested up to nginx
  1.31.7. Older nginx versions need a patched core.

## Changelog

### This fork

See [Changes in this fork](#changes-in-this-fork).

### v0.3

- Support keepalive check requests
- FastCGI check requests
- json/csv check status page support

### v0.1

- First release

## Authors

- Weibin Yao (姚伟斌) *yaoweibin at gmail dot com*
- Matthieu Tourne

## Copyright and license

This README template was copied from [agentzh](http://github.com/agentzh).

The health check part borrows the design of Jack Lindamood's healthcheck
module
[healthcheck_nginx_upstreams](http://github.com/cep21/healthcheck_nginx_upstreams).

This module is licensed under the BSD license.

Copyright (C) 2014 by Weibin Yao <yaoweibin@gmail.com>

Copyright (C) 2010-2014 Alibaba Group Holding Limited

Copyright (C) 2014 by LiangBin Li

Copyright (C) 2014 by Zhuo Yuan

Copyright (C) 2012 by Matthieu Tourne

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
