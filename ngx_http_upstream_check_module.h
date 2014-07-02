#ifndef _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>
#include <ngx_event_pipe.h>
#include <ngx_http.h>
#include <nginx.h>


typedef struct ngx_http_check_peer_s ngx_http_check_peer_t;
typedef struct ngx_http_check_peers_s ngx_http_check_peers_t;
typedef struct check_conf_s check_conf_t;

/* make nginx-0.8.22+ happy */
#if defined(nginx_version) && nginx_version >= 8022
typedef ngx_addr_t ngx_peer_addr_t;
#endif

typedef ngx_int_t (*ngx_http_check_packet_init_pt)(ngx_http_check_peer_t *peer);
typedef ngx_int_t (*ngx_http_check_packet_parse_pt)(ngx_http_check_peer_t *peer);
typedef void (*ngx_http_check_packet_clean_pt)(ngx_http_check_peer_t *peer);

#define NGX_HTTP_CHECK_TCP              0x0001
#define NGX_HTTP_CHECK_HTTP             0x0002
#define NGX_HTTP_CHECK_SSL_HELLO        0x0004
#define NGX_HTTP_CHECK_SMTP             0x0008
#define NGX_HTTP_CHECK_MYSQL            0x0010
#define NGX_HTTP_CHECK_POP3             0x0020
#define NGX_HTTP_CHECK_IMAP             0x0040
#define NGX_HTTP_CHECK_AJP              0x0080
#define NGX_HTTP_CHECK_FASTCGI          0x0100


#define NGX_CHECK_HTTP_2XX             0x0002
#define NGX_CHECK_HTTP_3XX             0x0004
#define NGX_CHECK_HTTP_4XX             0x0008
#define NGX_CHECK_HTTP_5XX             0x0010
#define NGX_CHECK_HTTP_6XX             0x0020
#define NGX_CHECK_HTTP_ERR             0x8000

#define NGX_CHECK_SMTP_2XX             0x0002
#define NGX_CHECK_SMTP_3XX             0x0004
#define NGX_CHECK_SMTP_4XX             0x0008
#define NGX_CHECK_SMTP_5XX             0x0010
#define NGX_CHECK_SMTP_6XX             0x0020
#define NGX_CHECK_SMTP_ERR             0x8000

struct check_conf_s {
    ngx_uint_t                        type;

    char                             *name;

    ngx_str_t                         default_send;

    /* HTTP */
    ngx_uint_t                        default_status_alive;

    ngx_event_handler_pt              send_handler;
    ngx_event_handler_pt              recv_handler;

    ngx_http_check_packet_init_pt     init;
    ngx_http_check_packet_parse_pt    parse;
    ngx_http_check_packet_clean_pt    reinit;

    unsigned need_pool;
};

typedef struct {
    ngx_uint_t                       check_shm_size;
    ngx_http_check_peers_t          *peers;
} ngx_http_upstream_check_main_conf_t;

typedef struct {
    ngx_uint_t                       fall_count;
    ngx_uint_t                       rise_count;
    ngx_msec_t                       check_interval;
    ngx_msec_t                       check_timeout;

    check_conf_t                    *check_type_conf;
    ngx_str_t                        send;

    union {
        ngx_uint_t                   return_code;
        ngx_uint_t                   status_alive;
    } code;

    ngx_uint_t                       default_down;

    ngx_array_t                     *fastcgi_params;
    ngx_str_t                        fastcgi_send;
} ngx_http_upstream_check_srv_conf_t;


ngx_uint_t ngx_http_check_add_peer(ngx_conf_t *cf,
        ngx_http_upstream_srv_conf_t *us, ngx_peer_addr_t *peer);

check_conf_t *ngx_http_get_check_type_conf(ngx_str_t *str);

extern ngx_module_t  ngx_http_upstream_check_module;

#endif //_NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_

