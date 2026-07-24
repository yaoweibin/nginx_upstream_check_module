## nginx_http_upstream_check_module 
support upstream health check with Nginx

## example
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

        # Dynamic DNS upstream (nginx 1.27.3+, zone + resolve)
        upstream dynamic_cluster {
            zone dynamic_cluster 512k;

            server api.example.com resolve;

            check interval=3000 rise=2 fall=3 timeout=2000 type=http;
            check_http_send "HEAD /health HTTP/1.0\r\nHost: api.example.com\r\n\r\n";
            check_http_expect_alive http_2xx;

            # reserve slots for up to 4 concurrently resolved IPs (default)
            check_resolve_max_addrs 4;
        }

        server {
            listen 80;

            location / {
                proxy_pass http://cluster;
            }

            location /status {
                check_status;

                access_log   off;
                allow SOME.IP.ADD.RESS;
                deny all;
           }
        }

    }
```

### Description
Add the support of health check with the upstream servers.

## Directives
### check
+ syntax
> check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds] [default_down=true|false] [type=tcp|http|ssl_hello|mysql|ajp|fastcgi]

+ default: 
> *none, if parameters omitted, default parameters are interval=30000 fall=5 rise=2 timeout=1000 default_down=true type=tcp*

+ context: **upstream**

+ description: Add the health check for the upstream servers.

+ parameters

| Parameter | 	Description |
| -------- | --------------- |
| interval | the check request's interval time(ms). |
| fall | After fall_count check failures, the server is marked down. |
| rise | After rise_count check success, the server is marked up |
| timeout | Check request timeout (ms). |
| default_down | Initial server state (true = down, false = up). |
| type | Check protocol type (see below). |
| port | Custom check port (default: same as backend server). |


+ Supported type values

    ​**tcp**: Simple TCP socket connection check.

    ​**ssl_hello**: Send SSL ClientHello and receive ServerHello.

    ​**http**: Send HTTP request and validate response.

    ​**mysql**: Check MySQL server greeting.

    ​**ajp**: Send AJP Cping and validate Cpong response.

    ​**fastcgi**: Validate FastCGI response.


### check_http_send
+ ​Syntax： 
> check_http_send http_packet

+ ​Default:
> "GET / HTTP/1.0\r\n\r\n"

​Context:
> upstream

​Description:
> Defines the HTTP request sent for health checks (when type=http).

### check_http_expect_alive
+ ​Syntax
> check_http_expect_alive [http_2xx | http_3xx | http_4xx | http_5xx]

+ ​Default
> http_2xx http_3xx

+ ​Context:
> upstream

+ ​Description:
> HTTP status codes indicating a healthy server.


### check_keepalive_requests
+ ​Syntax:
> check_keepalive_requests num

+ ​Default:
> 1

+ ​Context:
> upstream

+ ​Description:
> Number of requests sent per keepalive connection.

### check_fastcgi_param
+ ​Syntax:
> check_fastcgi_params parameter value

+ Default
```nginx
check_fastcgi_param "REQUEST_METHOD" "GET";
check_fastcgi_param "REQUEST_URI" "/";
check_fastcgi_param "SCRIPT_FILENAME" "index.php";
```

+ Context
> upstream

+ Description
> FastCGI headers sent for health checks (when type=fastcgi).

### check_shm_size
+ ​Syntax:
> check_shm_size size

+ ​Default:
> 1M

+ ​Context:
> http

+ ​Description:
> Shared memory size for storing health check data.

### check_resolve_max_addrs
+ Syntax:
> check_resolve_max_addrs number

+ Default:
> 4

+ Context:
> upstream

+ Description:
> Maximum number of concurrently resolved IP addresses that can receive independent health-check slots when using `server <hostname> resolve` inside a `zone`-backed upstream (nginx 1.27.3+, open-source dynamic DNS).
>
> When a hostname resolves to more IPs than this value, excess IPs share the health state of the first slot for that hostname (same behaviour as when this directive is absent).
>
> Set to `0` to revert to the legacy (V1) shared-slot mode: all IPs resolved from the same hostname share a single health-check slot.  This is equivalent to not having the directive at all in older module versions.
>
> The default of `4` covers the vast majority of deployments.  Increase the value only if a single hostname can resolve to more than 4 addresses simultaneously.

### check_status (zone+resolve diagnostics)

When using `zone` + `server <hostname> resolve` upstreams, the following log
messages help verify the per-IP health-check slot pool is working correctly.

**Pool exhaustion warning** — emitted at `warn` level (visible with the default
`error_log` level, no `debug` build required):

```
[warn] check resolve pool exhausted (first=N size=M): peer "1.2.3.4:80" has no independent health-check slot
```

This means more IPs resolved simultaneously for a hostname than
`check_resolve_max_addrs` allows.  Increase the value to fix it:

```nginx
upstream backend {
    zone backend 512k;
    server api.example.com resolve;
    check interval=3000 rise=2 fall=3 timeout=2000 type=http;
    check_resolve_max_addrs 8;   # raise if this warning appears
}
```

**Debug messages** — visible only with `error_log /path/to/error.log debug;`:

| Message | What it confirms |
|---|---|
| `check resolve pool alloc: upstream "X" host "Y" N slot(s)` | Slot pool created at config load / reload |
| `check resolve peer claim: slot N for "1.2.3.4" (pool first=M)` | A resolved IP claimed its own independent slot |
| `check resolve peer release: slot N "1.2.3.4"` | A slot was returned when the IP was removed from DNS |
| `check resolve pool slot N is free, skipping probe` | A free (unclaimed) slot timer fired — expected background noise |

Use `check_status?format=json` alongside these logs to confirm each resolved IP
appears with its own independent rise/fall counters and up/down state.

### check_status
+ ​Syntax:
> check_status [html | csv | json]

+ ​Default:
> html

+ ​Context:
> location

+ ​Description:
Displays upstream server status. Use URL parameters to customize output

+ ​URL parameters:
    + ?format=html|csv|json
    + ?status=up|down

Below it's the sample html page: 
```http://IP:PORT/status?format=html```
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
            <td>0</td>
            <td>backend</td>
            <td>106.187.48.116:80</td>
            <td>up</td>
            <td>39</td>
            <td>0</td>
            <td>http</td>
            <td>80</td>
            .....
```
Below it's the sample of csv page:
```csv
0,backend,106.187.48.116:80,up,46,0,http,80
```
Below it's the sample of json page:
```json
{
    "servers": {
        "total": 1,
        "generation": 3,
        "server": [
            {
                "index": 0,
                "upstream": "backend",
                "name": "106.187.48.116:80",
                "status": "up",
                "rise": 58,
                "fall": 0,
                "type": "http",
                "port": 80
            }
        ]
    }
}
```

## Dynamic DNS upstreams (zone + resolve)

Nginx 1.27.3+ (open-source) supports runtime DNS re-resolution when you combine
`zone` with `server <hostname> resolve` in an upstream block.  Without special
handling the check module assigns every runtime-resolved IP the same health-check
slot as the hostname placeholder, so all IPs behind a single hostname appear to
share one health state — a resolved IP that is actually down can still receive
traffic if any sibling IP is healthy.

This fork adds **per-IP health checking** for zone+resolve upstreams via a
pre-allocated slot pool.

### How it works

**Config time** — `ngx_http_upstream_check_add_resolve_peer()` reserves
`check_resolve_max_addrs` (default: 4) consecutive check slots for each
`server <hostname> resolve` entry.  Only the first (template) slot is armed
with a check timer; the rest are marked *available* in shared memory.

**Peer birth** (nginx calls `ngx_http_upstream_zone_copy_peer()` with
`src=NULL` for each newly resolved IP) — `ngx_http_upstream_check_get_resolve_peer()`
atomically claims a free slot from the pool, writes the resolved IP into the
slot's shared-memory probe address, and arms a per-worker check timer for it.

**Peer death** (nginx calls `ngx_http_upstream_zone_remove_peer_locked()`) —
`ngx_http_upstream_check_put_resolve_peer()` resets the slot's rise/fall
counters, marks it available again, and cancels its per-worker check timer so
the slot is immediately reusable for the next resolved address.

### Example config

```nginx
http {
    check_shm_size 2M;   # increase if you have many upstreams or large pools

    upstream backend {
        zone backend 512k;

        server api.example.com resolve;   # DNS re-resolved every TTL seconds

        check interval=3000 rise=2 fall=3 timeout=2000 type=http;
        check_http_send "HEAD /health HTTP/1.0\r\nHost: api.example.com\r\n\r\n";
        check_http_expect_alive http_2xx;

        # allow up to 8 concurrently resolved IPs for this hostname
        check_resolve_max_addrs 8;
    }

    server {
        listen 80;

        location / {
            proxy_pass http://backend;
        }

        location /upstream_status {
            check_status json;
            access_log off;
        }
    }
}
```

### V1 vs V2 behaviour

| Scenario | Without zone patch (original) | With zone patch, `check_resolve_max_addrs 0` (V1) | With zone patch, `check_resolve_max_addrs N` (V2, default) |
|---|---|---|---|
| Static upstreams | Correct per-IP health check | Correct per-IP health check | Correct per-IP health check |
| `zone` + `resolve`, first resolved IP | Correct | Correct | Correct, own slot |
| `zone` + `resolve`, additional IPs | All share slot 0 (bug) | All share template slot (V1) | Each gets an independent slot (V2) |
| DNS flap (IP removed then re-added) | Slot 0 state bleeds across IPs | Template slot state bleeds | Slot reset on peer death; fresh state on peer birth |

**V1 mode** (`check_resolve_max_addrs 0`) eliminates the "shares slot 0" bug
by propagating the correct template check slot to all runtime peers, but all
IPs behind a hostname still share one health state.  Choose V1 if you use
`zone`+`resolve` but every hostname always resolves to exactly one IP.

**V2 mode** (default) gives each resolved IP its own independent health state.
Use V2 whenever a hostname may resolve to multiple IPs simultaneously.

## Installation
### 1. ​Download the module:
```bash
git clone https://github.com/dnj12345/nginx_upstream_check_module.git
```
### 2. Download and patch Nginx:
```bash
wget http://nginx.org/download/nginx-1.30.0.tar.gz
tar -xzvf nginx-1.30.0.tar.gz
cd nginx-1.30.0/
# Base check-module patch (static upstreams + round-robin integration):
patch -p1 < ../nginx_upstream_check_module/check_1.30.0+.patch
# zone+resolve per-IP health check patch (requires zone module):
patch -p1 < ../nginx_upstream_check_module/check_zone_1.30+.patch
```

### 3. ​Compile and install:
```bash
./configure --add-module=../nginx_upstream_check_module
make
sudo make install
```

> **Note:** `check_zone_1.30+.patch` is only needed when you use
> `zone` + `server <hostname> resolve` upstreams.  It is safe to apply even
> when you do not use dynamic DNS — it adds no overhead for static upstreams.

## Compatibility Notes
| Nginx Version | Required Patch |
| ----------- | ----------- |
| 1.2.1 | check_1.2.1.patch |
| 1.2.2+ | check_1.2.2+.patch |
| 1.2.6+ | check_1.2.6+.patch |
| 1.5.12+ | check_1.5.12+.patch |
| 1.7.2+ | check_1.7.2+.patch |
| 1.7.5+ | check_1.7.5+.patch |
| 1.9.2+ | check_1.9.2+.patch |
| 1.11.1+ | check_1.11.1+.patch |
| 1.11.5+ | check_1.11.5+.patch |
| 1.12.1+ | check_1.12.1+.patch |
| 1.14.0+ | check_1.14.0+.patch |
| 1.16.1+ | check_1.16.1+.patch |
| 1.20.1+ | check_1.20.1+.patch |
| 1.26.3+ | check_1.26.3+.patch |
| 1.28.1+ | check_1.28.1+.patch |
| 1.30.0+ | check_1.30.0+.patch + check_zone_1.30+.patch (zone+resolve only) |


## Authors
+ Weibin Yao(姚伟斌) （yaoweibin@gmail.com)
+ Matthieu Tourne

    

## Copyright & License
```license
This README template copy from agentzh (<http://github.com/agentzh>).

The health check part is borrowed the design of Jack Lindamood's
healthcheck module healthcheck_nginx_upstreams
(<http://github.com/cep21/healthcheck_nginx_upstreams>);

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

*   Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

*   Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
