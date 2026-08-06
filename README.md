# nginx_http_upstream_check_module

[![License](https://img.shields.io/badge/license-BSD-blue.svg)](LICENSE)
[![Nginx](https://img.shields.io/badge/nginx-0.7.67%2B-green.svg)](http://nginx.org)

Health check module for Nginx upstream servers. Supports periodic active health checks for TCP, HTTP, SSL Hello, MySQL, AJP, and FastCGI upstreams.

---

## Table of Contents

- [Synopsis](#synopsis)
- [Description](#description)
- [Directives](#directives)
  - [check](#check)
  - [check_http_send](#check_http_send)
  - [check_http_expect_alive](#check_http_expect_alive)
  - [check_keepalive_requests](#check_keepalive_requests)
  - [check_fastcgi_param](#check_fastcgi_param)
  - [check_shm_size](#check_shm_size)
  - [check_status](#check_status)
- [Status Page Formats](#status-page-formats)
- [Installation](#installation)
- [Compatibility](#compatibility)
- [Changelog](#changelog)
- [Authors & License](#authors--license)

---

## Synopsis

```nginx
http {

    upstream cluster {

        # Simple round-robin backend servers
        server 192.168.0.1:80;
        server 192.168.0.2:80;

        # Health check configuration
        check interval=5000 rise=1 fall=3 timeout=4000 type=http;
        check_http_send "HEAD / HTTP/1.0\r\n\r\n";
        check_http_expect_alive http_2xx http_3xx;
    }

    server {
        listen 80;

        location / {
            proxy_pass http://cluster;
        }

        # Status monitoring page
        location /status {
            check_status;

            access_log off;
            allow SOME.IP.ADD.RESS;
            deny all;
        }
    }
}
```

---

## Description

This module adds support for active health checking of Nginx upstream servers. It periodically sends health check requests (TCP, HTTP, SSL Hello, MySQL, AJP, FastCGI) to backend servers and dynamically marks failed servers as `down` or healthy servers as `up`.

---

## Directives

### `check`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check interval=milliseconds [fall=count] [rise=count] [timeout=milliseconds] [default_down=true\|false] [type=tcp\|http\|ssl_hello\|mysql\|ajp\|fastcgi] [port=number]` |
| **Default** | `interval=30000 fall=5 rise=2 timeout=1000 default_down=true type=tcp port=0` |
| **Context** | `upstream` |

**Parameters:**

- `interval`: The check request interval time in milliseconds.
- `fall` (`fall_count`): Number of consecutive check failures before marking the server as **down**.
- `rise` (`rise_count`): Number of consecutive check successes before marking the server as **up**.
- `timeout`: Timeout for each check request in milliseconds.
- `default_down`: Initial state of the backend server (`true` = down, `false` = up). Default is `true`.
- `port`: Specifies the check port for backend servers. Default is `0` (uses the upstream server's default port).
- `type`: Check protocol type:
  1. `tcp`: Simple TCP socket connect and peek one byte.
  2. `ssl_hello`: Sends client SSL hello packet and receives server SSL hello packet.
  3. `http`: Sends HTTP request packet, receives and parses HTTP response to diagnose upstream health.
  4. `mysql`: Connects to MySQL server and receives greeting response.
  5. `ajp`: Sends AJP `Cping` packet and expects AJP `Cpong` response.
  6. `fastcgi`: Sends FastCGI request, receives and parses FastCGI response.

---

### `check_http_send`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_http_send "http_packet"` |
| **Default** | `"GET / HTTP/1.0\r\n\r\n"` |
| **Context** | `upstream` |

Configures the HTTP request packet sent to check the upstream server when `type=http` is set.

---

### `check_http_expect_alive`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_http_expect_alive [ http_2xx \| http_3xx \| http_4xx \| http_5xx ]` |
| **Default** | `http_2xx \| http_3xx` |
| **Context** | `upstream` |

Specifies HTTP status codes indicating that the upstream server response is OK and backend is alive.

---

### `check_keepalive_requests`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_keepalive_requests num` |
| **Default** | `1` |
| **Context** | `upstream` |

Specifies the number of requests sent on a connection. Default value `1` indicates that Nginx will close the connection after each request.

---

### `check_fastcgi_param`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_fastcgi_param parameter value` |
| **Default** | See below |
| **Context** | `upstream` |

Sets FastCGI headers when `type=fastcgi` is configured.

**Default FastCGI parameters:**
```nginx
check_fastcgi_param "REQUEST_METHOD" "GET";
check_fastcgi_param "REQUEST_URI" "/";
check_fastcgi_param "SCRIPT_FILENAME" "index.php";
```

---

### `check_shm_size`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_shm_size size` |
| **Default** | `1M` |
| **Context** | `http` |

Shared memory size allocated for health checks. Default is 1MB. Increase if checking thousands of upstream servers.

---

### `check_status`

| Field | Description |
| :--- | :--- |
| **Syntax** | `check_status [html\|csv\|json]` |
| **Default** | `html` |
| **Context** | `location` |

Displays health check status over HTTP. Supports query parameters for format selection and status filtering:

- `/status?format=html`
- `/status?format=csv`
- `/status?format=json`
- `/status?format=html&status=down`
- `/status?format=csv&status=up`

---

## Status Page Formats

### HTML Sample

```html
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Strict//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd">
<html>
<head><title>Nginx http upstream check status</title></head>
<body>
    <h1>Nginx http upstream check status</h1>
    <h2>Check upstream server number: 1, generation: 3</h2>
    <table>
        <tr>
            <th>Index</th>
            <th>Upstream</th>
            <th>Name</th>
            <th>Status</th>
            <th>Rise counts</th>
            <th>Fall counts</th>
            <th>Check type</th>
            <th>Check port</th>
        </tr>
        <tr>
            <td>0</td>
            <td>backend</td>
            <td>106.187.48.116:80</td>
            <td>up</td>
            <td>39</td>
            <td>0</td>
            <td>http</td>
            <td>80</td>
        </tr>
    </table>
</body>
</html>
```

### CSV Sample

```csv
0,backend,106.187.48.116:80,up,46,0,http,80
```

### JSON Sample

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

---

## Installation

1. Download the module source code from [GitHub](http://github.com/yaoweibin/nginx_upstream_check_module).
2. Download Nginx source code from [nginx.org](http://nginx.org/).
3. Build Nginx with this module:

```bash
$ wget 'http://nginx.org/download/nginx-1.0.14.tar.gz'
$ tar -xzvf nginx-1.0.14.tar.gz
$ cd nginx-1.0.14/
$ ./configure --add-module=/path/to/nginx_upstream_check_module
$ make
$ make install
```

---

## Compatibility

- **v0.1.5**: Compatible with Nginx `0.7.67+`
- **v0.1.8**: Compatible with Nginx `1.0.14+`

Supports official upstream algorithms including **Round-Robin**, **Ip_hash**, and **least_conn**.

---

## Changelog

### `v0.3`
- Support keepalive check requests.
- FastCGI check requests.
- JSON/CSV check status page support.

### `v0.1`
- First release.

---

## Authors & License

### Authors
- **Weibin Yao (姚伟斌)** (*yaoweibin at gmail dot com*)
- **Matthieu Tourne**

### License
This module is licensed under the **BSD License**.

```
Copyright (C) 2014 by Weibin Yao <yaoweibin@gmail.com>
Copyright (C) 2010-2014 Alibaba Group Holding Limited
Copyright (C) 2014 by LiangBin Li
Copyright (C) 2014 by Zhuo Yuan
Copyright (C) 2012 by Matthieu Tourne

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
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
