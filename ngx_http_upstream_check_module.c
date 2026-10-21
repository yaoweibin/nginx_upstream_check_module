/*
 * Copyright (C) 2010-2014 Weibin Yao (yaoweibin@gmail.com)
 * Copyright (C) 2010-2014 Alibaba Group Holding Limited
 */


#include <nginx.h>
#include "ngx_http_upstream_check_module.h"
#include "ngx_upstream_check.h"


/*
 * NGX_HTTP_UPSTREAM_CHECK is defined by the "config" script only when the
 * nginx core has been patched with one of the check_*.patch files. In that
 * case the core balancers ask this module whether a peer is down.
 *
 * Without the patch, the module registers the round robin peers of every
 * upstream with "check" itself, and each worker periodically copies the
 * check status into the peers' "down" flag, which all the built-in
 * balancers honour.
 *
 * This file also holds the check engine shared with the stream upstreams,
 * see ngx_upstream_check.h and ngx_stream_upstream_check_module.c.
 */

#if !(NGX_HTTP_UPSTREAM_CHECK) && (nginx_version < 1009000)
#error "nginx versions before 1.9.0 need one of the check_*.patch files"
#endif


typedef struct ngx_http_upstream_check_peer_s ngx_http_upstream_check_peer_t;


#pragma pack(push, 1)

typedef struct {
    u_char                                   major;
    u_char                                   minor;
} ngx_ssl_protocol_version_t;


typedef struct {
    u_char                                   msg_type;
    ngx_ssl_protocol_version_t               version;
    uint16_t                                 length;

    u_char                                   handshake_type;
    u_char                                   handshake_length[3];
    ngx_ssl_protocol_version_t               hello_version;

    time_t                                   time;
    u_char                                   random[28];

    u_char                                   others[0];
} ngx_ssl_server_hello_t;


typedef struct {
    u_char                                   packet_length[3];
    u_char                                   packet_number;

    u_char                                   protocol_version;
    u_char                                   others[0];
} ngx_mysql_handshake_init_t;


typedef struct {
    uint16_t                                 preamble;
    uint16_t                                 length;
    u_char                                   type;
} ngx_ajp_raw_packet_t;

#pragma pack(pop)


typedef struct {
    ngx_buf_t                                send;
    ngx_buf_t                                recv;

    ngx_uint_t                               state;
    ngx_http_status_t                        status;

    size_t                                   padding;
    size_t                                   length;
} ngx_http_upstream_check_ctx_t;


typedef struct {
    ngx_shmtx_t                              mutex;
#if (nginx_version >= 1002000)
    ngx_shmtx_sh_t                           lock;
#else
    ngx_atomic_t                             lock;
#endif

    ngx_pid_t                                owner;

    /* the worker which keeps an idle check connection to the peer */
    ngx_pid_t                                keepalive_owner;

    ngx_msec_t                               access_time;

    ngx_uint_t                               fall_count;
    ngx_uint_t                               rise_count;

    ngx_uint_t                               busyness;
    ngx_uint_t                               access_count;

    struct sockaddr                         *sockaddr;
    socklen_t                                socklen;

    ngx_atomic_t                             down;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    /*
     * The slots after the configured peers are for the peers which show
     * up at run time (resolve, upsync, ...), see
     * ngx_http_upstream_check_dyn_alloc().
     */
    ngx_uint_t                               dyn_used;
    ngx_uint_t                               dyn_id;
    ngx_uint_t                               dyn_upstream;
    time_t                                   dyn_seen;
    size_t                                   dyn_name_len;
    u_char                                   dyn_name[NGX_SOCKADDR_STRLEN];
    u_char                                   dyn_sockaddr[NGX_SOCKADDRLEN];
#endif

    u_char                                   padding[64];
} ngx_http_upstream_check_peer_shm_t;


typedef struct {
    ngx_uint_t                               generation;
    ngx_uint_t                               checksum;
    ngx_uint_t                               number;
#if !(NGX_HTTP_UPSTREAM_CHECK)
    /* the dynamic slots: peers[number] .. peers[number + dyn_number - 1] */
    ngx_uint_t                               dyn_number;
#endif

    /* ngx_http_upstream_check_status_peer_t */
    ngx_http_upstream_check_peer_shm_t       peers[1];
} ngx_http_upstream_check_peers_shm_t;


#define NGX_HTTP_CHECK_CONNECT_DONE          0x0001
#define NGX_HTTP_CHECK_SEND_DONE             0x0002
#define NGX_HTTP_CHECK_RECV_DONE             0x0004
#define NGX_HTTP_CHECK_ALL_DONE              0x0008


typedef ngx_int_t (*ngx_http_upstream_check_packet_init_pt)
    (ngx_http_upstream_check_peer_t *peer);
typedef ngx_int_t (*ngx_http_upstream_check_packet_parse_pt)
    (ngx_http_upstream_check_peer_t *peer);
typedef void (*ngx_http_upstream_check_packet_clean_pt)
    (ngx_http_upstream_check_peer_t *peer);

struct ngx_http_upstream_check_peer_s {
    ngx_flag_t                               state;
    ngx_pool_t                              *pool;
    ngx_uint_t                               index;
    ngx_uint_t                               max_busy;
    ngx_str_t                               *upstream_name;
    ngx_addr_t                              *check_peer_addr;
    ngx_addr_t                              *peer_addr;
    ngx_event_t                              check_ev;
    ngx_event_t                              check_timeout_ev;
    ngx_peer_connection_t                    pc;

    void                                    *check_data;
    ngx_event_handler_pt                     send_handler;
    ngx_event_handler_pt                     recv_handler;

    ngx_http_upstream_check_packet_init_pt   init;
    ngx_http_upstream_check_packet_parse_pt  parse;
    ngx_http_upstream_check_packet_clean_pt  reinit;

    ngx_http_upstream_check_peer_shm_t      *shm;
    ngx_http_upstream_check_srv_conf_t      *conf;

    /*
     * Set if an earlier peer (shared_index) has the same address and the
     * same check settings: this peer uses its check and status instead of
     * running one of its own, see ngx_http_upstream_check_share_peers().
     */
    ngx_uint_t                               shared;
    ngx_uint_t                               shared_index;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    /* the id of the shared memory slot of a dynamic peer, 0 otherwise */
    ngx_uint_t                               dyn_id;
#endif
};


/* a peer as shown on the status page */
typedef struct {
    ngx_str_t                               *upstream;
    ngx_str_t                                name;
    ngx_http_upstream_check_peer_shm_t      *shm;
    ngx_http_upstream_check_srv_conf_t      *conf;
} ngx_http_upstream_check_status_peer_t;


#if !(NGX_HTTP_UPSTREAM_CHECK)

/* a peer added at run time, private to a worker */
struct ngx_upstream_check_dyn_peer_s {
    ngx_http_upstream_check_peer_t           peer;

    ngx_pool_t                              *pool;
    ngx_addr_t                               addr;

    /* the sync pass which last saw the peer, and when */
    ngx_uint_t                               pass;
    time_t                                   seen;
};


/* a peer found by a sync pass that has no check peer yet */
typedef struct {
    socklen_t                                socklen;
    size_t                                   name_len;
    u_char                                   sockaddr[NGX_SOCKADDRLEN];
    u_char                                   name[NGX_SOCKADDR_STRLEN];
} ngx_http_upstream_check_new_peer_t;

#define NGX_HTTP_UPSTREAM_CHECK_NEW_PEERS    16

#endif


/*
 * The check peers of a configuration, of all the http and stream upstreams.
 * One is created for each cycle by the first module that registers peers,
 * see ngx_http_upstream_check_conf_peers().
 */

typedef struct {
    ngx_cycle_t                             *cycle;
    ngx_shm_zone_t                          *shm_zone;

    ngx_str_t                                check_shm_name;
    ngx_uint_t                               generation;
    ngx_uint_t                               checksum;
    ngx_array_t                              peers;

    ngx_http_upstream_check_peers_shm_t     *peers_shm;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    /* ngx_upstream_check_upstream_t */
    ngx_array_t                              upstreams;
    ngx_msec_t                               sync_interval;
    ngx_uint_t                               sync_pass;

    ngx_uint_t                               dyn_number;
    ngx_slab_pool_t                         *shpool;
#endif
} ngx_http_upstream_check_peers_t;


#define NGX_HTTP_CHECK_TCP                   0x0001
#define NGX_HTTP_CHECK_HTTP                  0x0002
#define NGX_HTTP_CHECK_SSL_HELLO             0x0004
#define NGX_HTTP_CHECK_MYSQL                 0x0008
#define NGX_HTTP_CHECK_AJP                   0x0010

#define NGX_CHECK_HTTP_2XX                   0x0002
#define NGX_CHECK_HTTP_3XX                   0x0004
#define NGX_CHECK_HTTP_4XX                   0x0008
#define NGX_CHECK_HTTP_5XX                   0x0010
#define NGX_CHECK_HTTP_ERR                   0x8000

typedef struct {
    ngx_uint_t                               type;

    ngx_str_t                                name;

    ngx_str_t                                default_send;

    /* HTTP */
    ngx_uint_t                               default_status_alive;

    ngx_event_handler_pt                     send_handler;
    ngx_event_handler_pt                     recv_handler;

    ngx_http_upstream_check_packet_init_pt   init;
    ngx_http_upstream_check_packet_parse_pt  parse;
    ngx_http_upstream_check_packet_clean_pt  reinit;

    unsigned need_pool;
    unsigned need_keepalive;
} ngx_check_conf_t;


typedef void (*ngx_http_upstream_check_status_format_pt) (ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag);

typedef struct {
    ngx_str_t                                format;
    ngx_str_t                                content_type;

    ngx_http_upstream_check_status_format_pt output;
} ngx_check_status_conf_t;


#define NGX_CHECK_STATUS_DOWN                0x0001
#define NGX_CHECK_STATUS_UP                  0x0002

typedef struct {
    ngx_check_status_conf_t                 *format;
    ngx_flag_t                               flag;
} ngx_http_upstream_check_status_ctx_t;


typedef ngx_int_t (*ngx_http_upstream_check_status_command_pt)
    (ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);

typedef struct {
    ngx_str_t                                 name;
    ngx_http_upstream_check_status_command_pt handler;
} ngx_check_status_command_t;


struct ngx_http_upstream_check_srv_conf_s {
    ngx_uint_t                               port;
    ngx_uint_t                               fall_count;
    ngx_uint_t                               rise_count;
    ngx_msec_t                               check_interval;
    ngx_msec_t                               check_timeout;
    ngx_uint_t                               check_keepalive_requests;

    ngx_check_conf_t                        *check_type_conf;
    ngx_str_t                                send;

    union {
        ngx_uint_t                           return_code;
        ngx_uint_t                           status_alive;
    } code;

    ngx_array_t                             *fastcgi_params;

    ngx_uint_t                               default_down;

    /* NGX_UPSTREAM_CHECK_HTTP or NGX_UPSTREAM_CHECK_STREAM */
    ngx_uint_t                               upstream_type;
};


typedef struct {
    ngx_check_status_conf_t                 *format;
} ngx_http_upstream_check_loc_conf_t;


typedef struct {
    u_char  version;
    u_char  type;
    u_char  request_id_hi;
    u_char  request_id_lo;
    u_char  content_length_hi;
    u_char  content_length_lo;
    u_char  padding_length;
    u_char  reserved;
} ngx_http_fastcgi_header_t;


typedef struct {
    u_char  role_hi;
    u_char  role_lo;
    u_char  flags;
    u_char  reserved[5];
} ngx_http_fastcgi_begin_request_t;


typedef struct {
    u_char  version;
    u_char  type;
    u_char  request_id_hi;
    u_char  request_id_lo;
} ngx_http_fastcgi_header_small_t;


typedef struct {
    ngx_http_fastcgi_header_t         h0;
    ngx_http_fastcgi_begin_request_t  br;
    ngx_http_fastcgi_header_small_t   h1;
} ngx_http_fastcgi_request_start_t;


#define NGX_HTTP_FASTCGI_RESPONDER      1

#define NGX_HTTP_FASTCGI_KEEP_CONN      1

#define NGX_HTTP_FASTCGI_BEGIN_REQUEST  1
#define NGX_HTTP_FASTCGI_ABORT_REQUEST  2
#define NGX_HTTP_FASTCGI_END_REQUEST    3
#define NGX_HTTP_FASTCGI_PARAMS         4
#define NGX_HTTP_FASTCGI_STDIN          5
#define NGX_HTTP_FASTCGI_STDOUT         6
#define NGX_HTTP_FASTCGI_STDERR         7
#define NGX_HTTP_FASTCGI_DATA           8


typedef enum {
    ngx_http_fastcgi_st_version = 0,
    ngx_http_fastcgi_st_type,
    ngx_http_fastcgi_st_request_id_hi,
    ngx_http_fastcgi_st_request_id_lo,
    ngx_http_fastcgi_st_content_length_hi,
    ngx_http_fastcgi_st_content_length_lo,
    ngx_http_fastcgi_st_padding_length,
    ngx_http_fastcgi_st_reserved,
    ngx_http_fastcgi_st_data,
    ngx_http_fastcgi_st_padding
} ngx_http_fastcgi_state_e;


static ngx_http_fastcgi_request_start_t  ngx_http_fastcgi_request_start = {
    { 1,                                               /* version */
      NGX_HTTP_FASTCGI_BEGIN_REQUEST,                  /* type */
      0,                                               /* request_id_hi */
      1,                                               /* request_id_lo */
      0,                                               /* content_length_hi */
      sizeof(ngx_http_fastcgi_begin_request_t),        /* content_length_lo */
      0,                                               /* padding_length */
      0 },                                             /* reserved */

    { 0,                                               /* role_hi */
      NGX_HTTP_FASTCGI_RESPONDER,                      /* role_lo */
      0, /* NGX_HTTP_FASTCGI_KEEP_CONN */              /* flags */
      { 0, 0, 0, 0, 0 } },                             /* reserved[5] */

    { 1,                                               /* version */
      NGX_HTTP_FASTCGI_PARAMS,                         /* type */
      0,                                               /* request_id_hi */
      1 },                                             /* request_id_lo */

};


static ngx_int_t ngx_http_upstream_check_add_timers(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upstream_check_init_peer(
    ngx_http_upstream_check_peer_t *peer, ngx_log_t *log);

static ngx_int_t ngx_http_upstream_check_peek_one_byte(ngx_connection_t *c);

static void ngx_http_upstream_check_begin_handler(ngx_event_t *event);
static void ngx_http_upstream_check_connect_handler(ngx_event_t *event);

static void ngx_http_upstream_check_peek_handler(ngx_event_t *event);

static void ngx_http_upstream_check_send_handler(ngx_event_t *event);
static void ngx_http_upstream_check_recv_handler(ngx_event_t *event);

static void ngx_http_upstream_check_discard_handler(ngx_event_t *event);
static void ngx_http_upstream_check_dummy_handler(ngx_event_t *event);

static ngx_int_t ngx_http_upstream_check_http_init(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_http_parse(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_parse_status_line(
    ngx_http_upstream_check_ctx_t *ctx, ngx_buf_t *b,
    ngx_http_status_t *status);
static void ngx_http_upstream_check_http_reinit(
    ngx_http_upstream_check_peer_t *peer);

static ngx_buf_t *ngx_http_upstream_check_create_fastcgi_request(
    ngx_pool_t *pool, ngx_str_t *params, ngx_uint_t num);

static ngx_int_t ngx_http_upstream_check_fastcgi_parse(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_fastcgi_process_record(
    ngx_http_upstream_check_ctx_t *ctx, ngx_buf_t *b,
    ngx_http_status_t *status);
static ngx_int_t ngx_http_upstream_check_parse_fastcgi_status(
    ngx_http_upstream_check_ctx_t *ctx, ngx_buf_t *b,
    ngx_http_status_t *status);

static ngx_int_t ngx_http_upstream_check_ssl_hello_init(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_ssl_hello_parse(
    ngx_http_upstream_check_peer_t *peer);
static void ngx_http_upstream_check_ssl_hello_reinit(
    ngx_http_upstream_check_peer_t *peer);

static ngx_int_t ngx_http_upstream_check_mysql_init(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_mysql_parse(
    ngx_http_upstream_check_peer_t *peer);
static void ngx_http_upstream_check_mysql_reinit(
    ngx_http_upstream_check_peer_t *peer);

static ngx_int_t ngx_http_upstream_check_ajp_init(
    ngx_http_upstream_check_peer_t *peer);
static ngx_int_t ngx_http_upstream_check_ajp_parse(
    ngx_http_upstream_check_peer_t *peer);
static void ngx_http_upstream_check_ajp_reinit(
    ngx_http_upstream_check_peer_t *peer);

static void ngx_http_upstream_check_status_update(
    ngx_http_upstream_check_peer_t *peer,
    ngx_int_t result);

static void ngx_http_upstream_check_clean_event(
    ngx_http_upstream_check_peer_t *peer);
static void ngx_http_upstream_check_reset_connection(ngx_connection_t *c);

static void ngx_http_upstream_check_timeout_handler(ngx_event_t *event);
static void ngx_http_upstream_check_finish_handler(ngx_event_t *event);

static ngx_int_t ngx_http_upstream_check_need_exit();
static void ngx_http_upstream_check_clear_all_events();
static void ngx_http_upstream_check_clear_peers_events(
    ngx_http_upstream_check_peers_t *peers);

static ngx_int_t ngx_http_upstream_check_status_handler(
    ngx_http_request_t *r);

static void ngx_http_upstream_check_status_parse_args(ngx_http_request_t *r,
    ngx_http_upstream_check_status_ctx_t *ctx);

static ngx_int_t ngx_http_upstream_check_status_command_format(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);
static ngx_int_t ngx_http_upstream_check_status_command_status(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value);

static char *ngx_http_upstream_check_upstream_type(
    ngx_http_upstream_check_srv_conf_t *ucscf);
static void ngx_http_upstream_check_status_html_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag);
static void ngx_http_upstream_check_status_csv_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag);
static void ngx_http_upstream_check_status_json_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag);

static ngx_int_t ngx_http_upstream_check_addr_change_port(ngx_pool_t *pool,
    ngx_addr_t *dst, ngx_addr_t *src, ngx_uint_t port);

static ngx_check_conf_t *ngx_http_get_check_type_conf(ngx_str_t *str);

static char *ngx_http_upstream_check_fastcgi_params(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static ngx_check_status_conf_t *ngx_http_get_check_status_format_conf(
    ngx_str_t *str);
static char *ngx_http_upstream_check_status(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static void *ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf,
    void *conf);

static void *ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf);

static void *ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

#define SHM_NAME_LEN 256


static ngx_int_t ngx_http_upstream_check_get_shm_name(ngx_str_t *shm_name,
    ngx_pool_t *pool, ngx_uint_t generation);
static ngx_shm_zone_t *ngx_http_upstream_check_find_shm_zone(
    ngx_cycle_t *cycle, void *tag);
static ngx_http_upstream_check_peer_shm_t *
ngx_http_upstream_check_find_shm_peer(ngx_http_upstream_check_peers_shm_t *peers_shm,
    ngx_addr_t *addr);

static ngx_int_t ngx_http_upstream_check_init_shm_peer(
    ngx_http_upstream_check_peer_shm_t *peer_shm,
    ngx_http_upstream_check_peer_shm_t *opeer_shm,
    ngx_uint_t init_down, ngx_pool_t *pool, ngx_str_t *peer_name);

static ngx_int_t ngx_http_upstream_check_init_shm_zone(
    ngx_shm_zone_t *shm_zone, void *data);


static ngx_int_t ngx_http_upstream_check_init_module(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_upstream_check_init_process(ngx_cycle_t *cycle);

#if !(NGX_HTTP_UPSTREAM_CHECK)
static ngx_int_t ngx_http_upstream_check_postconfiguration(ngx_conf_t *cf);
static ngx_int_t ngx_http_upstream_check_add_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_check_init_sync(ngx_cycle_t *cycle);
static void ngx_http_upstream_check_sync_handler(ngx_event_t *event);
static void ngx_http_upstream_check_sync_peers(void);
static void ngx_http_upstream_check_sync(ngx_upstream_check_upstream_t *cu);
static ngx_uint_t ngx_http_upstream_check_sync_list(
    ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_rr_peers_t *peers, ngx_uint_t update);
static ngx_int_t ngx_http_upstream_check_find_rr_peer(
    ngx_upstream_check_upstream_t *cu, struct sockaddr *sockaddr,
    socklen_t socklen, ngx_uint_t hint);
static ngx_upstream_check_dyn_peer_t *
    ngx_http_upstream_check_find_dyn_peer(
    ngx_upstream_check_upstream_t *cu, struct sockaddr *sockaddr,
    socklen_t socklen, ngx_uint_t *hint);
static void ngx_http_upstream_check_dyn_add(
    ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_check_new_peer_t *np);
static ngx_int_t ngx_http_upstream_check_dyn_alloc(
    ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_check_new_peer_t *np, ngx_uint_t *id);
static void ngx_http_upstream_check_dyn_expire(
    ngx_upstream_check_upstream_t *cu);
static void ngx_http_upstream_check_dyn_free(
    ngx_upstream_check_dyn_peer_t *dyn);
static void ngx_http_upstream_check_dyn_free_all(
    ngx_http_upstream_check_peers_t *peers);
static void ngx_http_upstream_check_dyn_gc(void);
static time_t ngx_http_upstream_check_dyn_grace(
    ngx_http_upstream_check_srv_conf_t *ucscf);
static void ngx_http_upstream_check_dyn_inherit(
    ngx_http_upstream_check_peers_t *peers,
    ngx_http_upstream_check_peers_shm_t *peers_shm,
    ngx_http_upstream_check_peers_t *opeers,
    ngx_http_upstream_check_peers_shm_t *opeers_shm);
static ngx_int_t ngx_http_upstream_check_dyn_status(ngx_http_request_t *r,
    ngx_http_upstream_check_peers_t *peers, ngx_array_t *records);
#endif

static ngx_int_t ngx_http_upstream_check_cmp_sockaddr(struct sockaddr *sa1,
    socklen_t slen1, struct sockaddr *sa2, socklen_t slen2);
static void ngx_http_upstream_check_share_peers(
    ngx_http_upstream_check_peers_t *peers);
static ngx_int_t ngx_http_upstream_check_same_check(
    ngx_http_upstream_check_peer_t *p1, ngx_http_upstream_check_peer_t *p2);


static ngx_conf_bitmask_t  ngx_check_http_expect_alive_masks[] = {
    { ngx_string("http_2xx"), NGX_CHECK_HTTP_2XX },
    { ngx_string("http_3xx"), NGX_CHECK_HTTP_3XX },
    { ngx_string("http_4xx"), NGX_CHECK_HTTP_4XX },
    { ngx_string("http_5xx"), NGX_CHECK_HTTP_5XX },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_upstream_check_commands[] = {

    { ngx_string("check"),
      NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
      ngx_upstream_check_conf_check,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_keepalive_requests"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_keepalive_requests,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_http_send"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_http_send,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_http_expect_alive"),
      NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
      ngx_upstream_check_conf_http_expect_alive,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_fastcgi_param"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE2,
      ngx_http_upstream_check_fastcgi_params,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_shm_size"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_shm_size,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_status"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1|NGX_CONF_NOARGS,
      ngx_http_upstream_check_status,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_upstream_check_module_ctx = {
    NULL,                                    /* preconfiguration */
#if (NGX_HTTP_UPSTREAM_CHECK)
    NULL,                                    /* postconfiguration */
#else
    ngx_http_upstream_check_postconfiguration, /* postconfiguration */
#endif

    ngx_http_upstream_check_create_main_conf,/* create main configuration */
    ngx_http_upstream_check_init_main_conf,  /* init main configuration */

    ngx_http_upstream_check_create_srv_conf, /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_upstream_check_create_loc_conf, /* create location configuration */
    ngx_http_upstream_check_merge_loc_conf   /* merge location configuration */
};


ngx_module_t  ngx_http_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_check_module_ctx,   /* module context */
    ngx_http_upstream_check_commands,      /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_http_upstream_check_init_module,   /* init module */
    ngx_http_upstream_check_init_process,  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_str_t fastcgi_default_request;
static ngx_str_t fastcgi_default_params[] = {
    ngx_string("REQUEST_METHOD"), ngx_string("GET"),
    ngx_string("REQUEST_URI"), ngx_string("/"),
    ngx_string("SCRIPT_FILENAME"), ngx_string("index.php"),
};


#define NGX_SSL_RANDOM "NGX_HTTP_CHECK_SSL_HELLO\n\n\n\n"

/*
 * This is the SSLv3 CLIENT HELLO packet used in conjunction with the
 * check type of ssl_hello to ensure that the remote server speaks SSL.
 *
 * Check RFC 2246 (TLSv1.0) sections A.3 and A.4 for details.
 */
static char sslv3_client_hello_pkt[] = {
    "\x16"                /* ContentType         : 0x16 = Hanshake           */
    "\x03\x01"            /* ProtocolVersion     : 0x0301 = TLSv1.0          */
    "\x00\x6f"            /* ContentLength       : 0x6f bytes after this one */
    "\x01"                /* HanshakeType        : 0x01 = CLIENT HELLO       */
    "\x00\x00\x6b"        /* HandshakeLength     : 0x6b bytes after this one */
    "\x03\x03"            /* Hello Version       : 0x0303 = TLSv1.2          */
    "\x00\x00\x00\x00"    /* Unix GMT Time (s)   : filled with <now> (@0x0B) */
    NGX_SSL_RANDOM        /* Random              : must be exactly 28 bytes  */
    "\x00"                /* Session ID length   : empty (no session ID)     */
    "\x00\x1a"            /* Cipher Suite Length : \x1a bytes after this one */
    "\xc0\x2b" "\xc0\x2f" "\xcc\xa9" "\xcc\xa8"  /* 13 modern ciphers        */
    "\xc0\x0a" "\xc0\x09" "\xc0\x13" "\xc0\x14"
    "\x00\x33" "\x00\x39" "\x00\x2f" "\x00\x35"
    "\x00\x0a"
    "\x01"                /* Compression Length  : 0x01 = 1 byte for types   */
    "\x00"                /* Compression Type    : 0x00 = NULL compression   */
    "\x00\x28"            /* Extensions length */
    "\x00\x0a"            /* EC extension */
    "\x00\x08"            /* extension length */
    "\x00\x06"            /* curves length */
    "\x00\x17" "\x00\x18" "\x00\x19" /* Three curves */
    "\x00\x0d"            /* Signature extension */
    "\x00\x18"            /* extension length */
    "\x00\x16"            /* hash list length */
    "\x04\x01" "\x05\x01" "\x06\x01" "\x02\x01"  /* 11 hash algorithms */
    "\x04\x03" "\x05\x03" "\x06\x03" "\x02\x03"
    "\x05\x02" "\x04\x02" "\x02\x02"
};


#define NGX_SSL_HANDSHAKE    0x16
#define NGX_SSL_SERVER_HELLO 0x02


#define NGX_AJP_CPING        0x0a
#define NGX_AJP_CPONG        0x09


static char ngx_ajp_cping_packet[] = {
    0x12, 0x34, 0x00, 0x01, NGX_AJP_CPING, 0x00
};

static char ngx_ajp_cpong_packet[] = {
    0x41, 0x42, 0x00, 0x01, NGX_AJP_CPONG
};


static ngx_check_conf_t  ngx_check_types[] = {

    { NGX_HTTP_CHECK_TCP,
      ngx_string("tcp"),
      ngx_null_string,
      0,
      ngx_http_upstream_check_peek_handler,
      ngx_http_upstream_check_peek_handler,
      NULL,
      NULL,
      NULL,
      0,
      0 },

    { NGX_HTTP_CHECK_HTTP,
      ngx_string("http"),
      ngx_string("GET / HTTP/1.0\r\n\r\n"),
      NGX_CONF_BITMASK_SET | NGX_CHECK_HTTP_2XX | NGX_CHECK_HTTP_3XX,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_recv_handler,
      ngx_http_upstream_check_http_init,
      ngx_http_upstream_check_http_parse,
      ngx_http_upstream_check_http_reinit,
      1,
      1 },

    { NGX_HTTP_CHECK_HTTP,
      ngx_string("fastcgi"),
      ngx_null_string,
      0,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_recv_handler,
      ngx_http_upstream_check_http_init,
      ngx_http_upstream_check_fastcgi_parse,
      ngx_http_upstream_check_http_reinit,
      1,
      0 },

    { NGX_HTTP_CHECK_SSL_HELLO,
      ngx_string("ssl_hello"),
      ngx_string(sslv3_client_hello_pkt),
      0,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_recv_handler,
      ngx_http_upstream_check_ssl_hello_init,
      ngx_http_upstream_check_ssl_hello_parse,
      ngx_http_upstream_check_ssl_hello_reinit,
      1,
      0 },

    { NGX_HTTP_CHECK_MYSQL,
      ngx_string("mysql"),
      ngx_null_string,
      0,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_recv_handler,
      ngx_http_upstream_check_mysql_init,
      ngx_http_upstream_check_mysql_parse,
      ngx_http_upstream_check_mysql_reinit,
      1,
      0 },

    { NGX_HTTP_CHECK_AJP,
      ngx_string("ajp"),
      ngx_string(ngx_ajp_cping_packet),
      0,
      ngx_http_upstream_check_send_handler,
      ngx_http_upstream_check_recv_handler,
      ngx_http_upstream_check_ajp_init,
      ngx_http_upstream_check_ajp_parse,
      ngx_http_upstream_check_ajp_reinit,
      1,
      0 },

    { 0,
      ngx_null_string,
      ngx_null_string,
      0,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL,
      0,
      0 }
};


static ngx_check_status_conf_t  ngx_check_status_formats[] = {

    { ngx_string("html"),
      ngx_string("text/html"),
      ngx_http_upstream_check_status_html_format },

    { ngx_string("csv"),
      ngx_string("text/plain"),
      ngx_http_upstream_check_status_csv_format },

    { ngx_string("json"),
      ngx_string("application/json"), /* RFC 4627 */
      ngx_http_upstream_check_status_json_format },

    { ngx_null_string, ngx_null_string, NULL }
};


static ngx_check_status_command_t ngx_check_status_commands[] =  {

    { ngx_string("format"),
      ngx_http_upstream_check_status_command_format },

    { ngx_string("status"),
      ngx_http_upstream_check_status_command_status },

    { ngx_null_string, NULL }
};


static ngx_uint_t ngx_http_upstream_check_shm_generation = 0;
static ngx_http_upstream_check_peers_t *check_peers_ctx = NULL;
/* peers of the configuration being loaded, used until it is committed */
static ngx_http_upstream_check_peers_t *check_peers_conf = NULL;

#if !(NGX_HTTP_UPSTREAM_CHECK)
static ngx_event_t ngx_http_upstream_check_sync_event;
static ngx_http_upstream_check_new_peer_t
    ngx_http_upstream_check_new_peers[NGX_HTTP_UPSTREAM_CHECK_NEW_PEERS];
static ngx_uint_t ngx_http_upstream_check_nnew;
#endif


static void
ngx_http_upstream_check_conf_peers_cleanup(void *data)
{
    /* a configuration which failed to load */

    if (check_peers_conf == data) {
        check_peers_conf = NULL;
    }
}


/*
 * The check peers of the configuration being loaded: the http and the
 * stream modules both add their peers to it, in the order their blocks
 * are parsed.
 */

static ngx_http_upstream_check_peers_t *
ngx_http_upstream_check_conf_peers(ngx_conf_t *cf)
{
    ngx_pool_cleanup_t               *cln;
    ngx_http_upstream_check_peers_t  *peers;

    if (check_peers_conf != NULL && check_peers_conf->cycle == cf->cycle) {
        return check_peers_conf;
    }

    peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_peers_t));
    if (peers == NULL) {
        return NULL;
    }

    peers->cycle = cf->cycle;

    if (ngx_array_init(&peers->peers, cf->pool, 16,
                       sizeof(ngx_http_upstream_check_peer_t)) != NGX_OK)
    {
        return NULL;
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_array_init(&peers->upstreams, cf->pool, 4,
                       sizeof(ngx_upstream_check_upstream_t)) != NGX_OK)
    {
        return NULL;
    }

    peers->sync_interval = 500;
#endif

    cln = ngx_pool_cleanup_add(cf->pool, 0);
    if (cln == NULL) {
        return NULL;
    }

    cln->handler = ngx_http_upstream_check_conf_peers_cleanup;
    cln->data = peers;

    /*
     * Don't touch check_peers_ctx here: this configuration may still
     * fail to load, and the running cycle must keep its own peers.
     * check_peers_ctx is switched in init_module once the new cycle
     * is committed.
     */
    check_peers_conf = peers;

    return peers;
}


static ngx_int_t
ngx_http_upstream_check_new_peer(ngx_conf_t *cf,
    ngx_http_upstream_check_srv_conf_t *ucscf, ngx_str_t *upstream_name,
    ngx_addr_t *peer_addr)
{
    ngx_http_upstream_check_peer_t   *peer;
    ngx_http_upstream_check_peers_t  *peers;

    peers = ngx_http_upstream_check_conf_peers(cf);
    if (peers == NULL) {
        return NGX_ERROR;
    }

    peer = ngx_array_push(&peers->peers);
    if (peer == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(peer, sizeof(ngx_http_upstream_check_peer_t));

    peer->index = peers->peers.nelts - 1;
    peer->conf = ucscf;
    peer->upstream_name = upstream_name;
    peer->peer_addr = peer_addr;

    if (ucscf->port) {
        peer->check_peer_addr = ngx_pcalloc(cf->pool, sizeof(ngx_addr_t));
        if (peer->check_peer_addr == NULL) {
            return NGX_ERROR;
        }

        if (ngx_http_upstream_check_addr_change_port(cf->pool,
                peer->check_peer_addr, peer_addr, ucscf->port)
            != NGX_OK) {

            return NGX_ERROR;
        }

    } else {
        peer->check_peer_addr = peer->peer_addr;
    }

    peers->checksum +=
        ngx_murmur_hash2(peer_addr->name.data, peer_addr->name.len);

    return peer->index;
}


ngx_uint_t
ngx_http_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer_addr)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    if (us->srv_conf == NULL) {
        return NGX_ERROR;
    }

    ucscf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_check_module);

    if(ucscf->check_interval == 0) {
        return NGX_ERROR;
    }

    return ngx_http_upstream_check_new_peer(cf, ucscf, &us->host, peer_addr);
}


static ngx_int_t
ngx_http_upstream_check_addr_change_port(ngx_pool_t *pool, ngx_addr_t *dst,
    ngx_addr_t *src, ngx_uint_t port)
{
    size_t                len;
    u_char               *p;
    struct sockaddr_in   *sin;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin6;
#endif

    dst->socklen = src->socklen;
    dst->sockaddr = ngx_palloc(pool, dst->socklen);
    if (dst->sockaddr == NULL) {
        return NGX_ERROR;
    }

    ngx_memcpy(dst->sockaddr, src->sockaddr, dst->socklen);

    switch (dst->sockaddr->sa_family) {

    case AF_INET:

        len = NGX_INET_ADDRSTRLEN + sizeof(":65535") - 1;
        sin = (struct sockaddr_in *) dst->sockaddr;
        sin->sin_port = htons(port);

        break;

#if (NGX_HAVE_INET6)
    case AF_INET6:

        len = NGX_INET6_ADDRSTRLEN + sizeof(":65535") - 1;
        sin6 = (struct sockaddr_in6 *) dst->sockaddr;
        sin6->sin6_port = htons(port);

        break;
#endif

    default:
        return NGX_ERROR;
    }

    p = ngx_pnalloc(pool, len);
    if (p == NULL) {
        return NGX_ERROR;
    }

#if (nginx_version >= 1005012)
    len = ngx_sock_ntop(dst->sockaddr, dst->socklen, p, len, 1);
#else
    len = ngx_sock_ntop(dst->sockaddr, p, len, 1);
#endif

    dst->name.len = len;
    dst->name.data = p;

    return NGX_OK;
}


ngx_uint_t
ngx_http_upstream_check_peer_down(ngx_uint_t index)
{
    ngx_http_upstream_check_peer_t  *peer;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return 0;
    }

    peer = check_peers_ctx->peers.elts;

    return (peer[index].shm->down);
}


/* TODO: this interface can count each peer's busyness */
void
ngx_http_upstream_check_get_peer(ngx_uint_t index)
{
    ngx_http_upstream_check_peer_t  *peer;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return;
    }

    peer = check_peers_ctx->peers.elts;

    ngx_shmtx_lock(&peer[index].shm->mutex);

    peer[index].shm->busyness++;
    peer[index].shm->access_count++;

    ngx_shmtx_unlock(&peer[index].shm->mutex);
}


void
ngx_http_upstream_check_free_peer(ngx_uint_t index)
{
    ngx_http_upstream_check_peer_t  *peer;

    if (check_peers_ctx == NULL || index >= check_peers_ctx->peers.nelts) {
        return;
    }

    peer = check_peers_ctx->peers.elts;

    ngx_shmtx_lock(&peer[index].shm->mutex);

    if (peer[index].shm->busyness > 0) {
        peer[index].shm->busyness--;
    }

    ngx_shmtx_unlock(&peer[index].shm->mutex);
}


static ngx_int_t
ngx_http_upstream_check_add_timers(ngx_cycle_t *cycle)
{
    ngx_uint_t                           i;
    ngx_msec_t                           t, delay;
    ngx_http_upstream_check_peer_t      *peer;
    ngx_http_upstream_check_peers_t     *peers;
    ngx_http_upstream_check_srv_conf_t  *ucscf;
    ngx_http_upstream_check_peer_shm_t  *peer_shm;
    ngx_http_upstream_check_peers_shm_t *peers_shm;

    peers = check_peers_ctx;
    if (peers == NULL) {
        return NGX_OK;
    }

    peers_shm = peers->peers_shm;
    if (peers_shm == NULL) {
        return NGX_OK;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
                   "http check upstream init_process, shm_name: %V, "
                   "peer number: %ud",
                   &peers->check_shm_name,
                   peers->peers.nelts);

    srandom(ngx_pid);

    peer = peers->peers.elts;
    peer_shm = peers_shm->peers;

    for (i = 0; i < peers->peers.nelts; i++) {

        if (peer[i].shared) {
            /* the peer it shares the check with runs it */
            peer[i].shm = peer[peer[i].shared_index].shm;
            continue;
        }

        peer[i].shm = &peer_shm[i];

        if (ngx_http_upstream_check_init_peer(&peer[i], cycle->log) != NGX_OK) {
            return NGX_ERROR;
        }

        ucscf = peer[i].conf;

        /*
         * We add a random start time here, since we don't want to trigger
         * the check events too close to each other at the beginning.
         */
        delay = ucscf->check_interval > 1000 ? ucscf->check_interval : 1000;
        t = ngx_random() % delay;

        ngx_add_timer(&peer[i].check_ev, t);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_peer(ngx_http_upstream_check_peer_t *peer,
    ngx_log_t *log)
{
    ngx_check_conf_t  *cf;

    peer->check_ev.handler = ngx_http_upstream_check_begin_handler;
    peer->check_ev.log = log;
    peer->check_ev.data = peer;
    peer->check_ev.timer_set = 0;

    peer->check_timeout_ev.handler = ngx_http_upstream_check_timeout_handler;
    peer->check_timeout_ev.log = log;
    peer->check_timeout_ev.data = peer;
    peer->check_timeout_ev.timer_set = 0;

    cf = peer->conf->check_type_conf;

    if (cf->need_pool) {
        peer->pool = ngx_create_pool(ngx_pagesize, log);
        if (peer->pool == NULL) {
            return NGX_ERROR;
        }
    }

    peer->send_handler = cf->send_handler;
    peer->recv_handler = cf->recv_handler;

    peer->init = cf->init;
    peer->parse = cf->parse;
    peer->reinit = cf->reinit;

    return NGX_OK;
}


static void
ngx_http_upstream_check_begin_handler(ngx_event_t *event)
{
    ngx_msec_t                           interval, due;
    ngx_http_upstream_check_peer_t      *peer;
    ngx_http_upstream_check_peers_t     *peers;
    ngx_http_upstream_check_srv_conf_t  *ucscf;
    ngx_http_upstream_check_peers_shm_t *peers_shm;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peers = check_peers_ctx;
    if (peers == NULL) {
        return;
    }

    peers_shm = peers->peers_shm;
    if (peers_shm == NULL) {
        return;
    }

    peer = event->data;
    ucscf = peer->conf;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (peer->dyn_id && peer->shm->dyn_id != peer->dyn_id) {
        /* the slot has been released, the sync timer frees the peer */
        return;
    }
#endif

    ngx_add_timer(event, ucscf->check_interval / 2);

    /* This process is processing this peer now. */
    if ((peer->shm->owner == ngx_pid  ||
        peer->check_timeout_ev.timer_set)) {
        return;
    }

    /*
     * Another worker has checked the peer since this one kept its
     * connection, so the connection would only stay idle. Close it, so
     * that the peer does not get an idle check connection from every
     * worker.
     */

    if (peer->pc.connection != NULL
        && peer->shm->keepalive_owner != ngx_pid)
    {
        ngx_close_connection(peer->pc.connection);
        peer->pc.connection = NULL;
    }

    ngx_shmtx_lock(&peer->shm->mutex);

    if (peers_shm->generation != peers->generation) {
        ngx_shmtx_unlock(&peer->shm->mutex);
        return;
    }

    /*
     * Read the access time under the lock: another worker may just have
     * finished a check. The cached time of this worker may also be a bit
     * behind the one the time was written with, which must not wrap around
     * to a huge interval.
     */

    if ((ngx_msec_int_t) (ngx_current_msec - peer->shm->access_time) > 0) {
        interval = ngx_current_msec - peer->shm->access_time;

    } else {
        interval = 0;
    }

    /*
     * The worker which keeps an idle connection to the peer checks it next,
     * the others only if it has not done so within half an interval more
     * (its timer fires every half interval), e.g. when it has exited.
     */

    due = ucscf->check_interval;

    if (peer->shm->keepalive_owner != NGX_INVALID_PID
        && peer->shm->keepalive_owner != ngx_pid)
    {
        due += ucscf->check_interval / 2;
    }

    ngx_log_debug7(NGX_LOG_DEBUG_HTTP, event->log, 0,
                   "http check begin handler index: %ui, owner: %P, "
                   "keepalive owner: %P, ngx_pid: %P, interval: %M, "
                   "due: %M, check_interval: %M",
                   peer->index, peer->shm->owner,
                   peer->shm->keepalive_owner, ngx_pid, interval, due,
                   ucscf->check_interval);

    if ((interval >= due)
         && (peer->shm->owner == NGX_INVALID_PID))
    {
        peer->shm->owner = ngx_pid;

    } else if (interval >= (ucscf->check_interval << 4)) {

        /*
         * If the check peer has been untouched for 2^4 times of
         * the check interval, activate the current timer.
         * Sometimes, the checking process may disappear
         * in some circumstances, and the clean event will never
         * be triggered.
         */
        peer->shm->owner = ngx_pid;
        peer->shm->access_time = ngx_current_msec;
    }

    ngx_shmtx_unlock(&peer->shm->mutex);

    if (peer->shm->owner == ngx_pid) {
        ngx_http_upstream_check_connect_handler(event);
    }
}


static void
ngx_http_upstream_check_connect_handler(ngx_event_t *event)
{
    ngx_int_t                            rc;
    ngx_connection_t                    *c;
    ngx_http_upstream_check_peer_t      *peer;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peer = event->data;
    ucscf = peer->conf;

    if (peer->pc.connection != NULL) {
        c = peer->pc.connection;
        if ((rc = ngx_http_upstream_check_peek_one_byte(c)) == NGX_OK) {
            goto upstream_check_connect_done;
        } else {
            ngx_close_connection(c);
            peer->pc.connection = NULL;
        }
    }
    ngx_memzero(&peer->pc, sizeof(ngx_peer_connection_t));

    peer->pc.sockaddr = peer->check_peer_addr->sockaddr;
    peer->pc.socklen = peer->check_peer_addr->socklen;
    peer->pc.name = &peer->check_peer_addr->name;

    peer->pc.get = ngx_event_get_peer;
    peer->pc.log = event->log;
    peer->pc.log_error = NGX_ERROR_ERR;

    peer->pc.cached = 0;
    peer->pc.connection = NULL;

    rc = ngx_event_connect_peer(&peer->pc);

    if (rc == NGX_ERROR || rc == NGX_DECLINED) {
        ngx_http_upstream_check_status_update(peer, 0);
        ngx_http_upstream_check_clean_event(peer);
        return;
    }

    /* NGX_OK or NGX_AGAIN */
    c = peer->pc.connection;
    c->data = peer;
    c->log = peer->pc.log;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;
    c->pool = peer->pool;

upstream_check_connect_done:
    peer->state = NGX_HTTP_CHECK_CONNECT_DONE;

    c->write->handler = peer->send_handler;
    c->read->handler = peer->recv_handler;

    ngx_add_timer(&peer->check_timeout_ev, ucscf->check_timeout);

    /* The kqueue's loop interface needs it. */
    if (rc == NGX_OK) {
        c->write->handler(c->write);
    }
}

static ngx_int_t
ngx_http_upstream_check_peek_one_byte(ngx_connection_t *c)
{
    char                            buf[1];
    ngx_int_t                       n;
    ngx_err_t                       err;

    n = recv(c->fd, buf, 1, MSG_PEEK);
    err = ngx_socket_errno;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, err,
                   "http check upstream recv(): %i, fd: %d",
                   n, c->fd);

    if (n == 1 || (n == -1 && err == NGX_EAGAIN)) {
        return NGX_OK;
    } else {
        return NGX_ERROR;
    }
}

static void
ngx_http_upstream_check_peek_handler(ngx_event_t *event)
{
    ngx_connection_t               *c;
    ngx_http_upstream_check_peer_t *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    if (ngx_http_upstream_check_peek_one_byte(c) == NGX_OK) {
        ngx_http_upstream_check_status_update(peer, 1);

    } else {
        c->error = 1;
        ngx_http_upstream_check_status_update(peer, 0);
    }

    ngx_http_upstream_check_clean_event(peer);

    ngx_http_upstream_check_finish_handler(event);
}


static void
ngx_http_upstream_check_discard_handler(ngx_event_t *event)
{
    u_char                          buf[4096];
    ssize_t                         size;
    ngx_connection_t               *c;
    ngx_http_upstream_check_peer_t *peer;

    c = event->data;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "upstream check discard handler");

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peer = c->data;

    while (1) {
        size = c->recv(c, buf, 4096);

        if (size > 0) {
            continue;

        } else if (size == NGX_AGAIN) {
            break;

        } else {
            if (size == 0) {
                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                               "peer closed its half side of the connection");
            }

            goto check_discard_fail;
        }
    }

    if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
        goto check_discard_fail;
    }

    return;

 check_discard_fail:
    c->error = 1;
    ngx_http_upstream_check_clean_event(peer);
}


static void
ngx_http_upstream_check_dummy_handler(ngx_event_t *event)
{
    return;
}


static void
ngx_http_upstream_check_send_handler(ngx_event_t *event)
{
    ssize_t                         size;
    ngx_connection_t               *c;
    ngx_http_upstream_check_ctx_t  *ctx;
    ngx_http_upstream_check_peer_t *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "http check send.");

    if (c->pool == NULL) {
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                      "check pool NULL with peer: %V ",
                      &peer->check_peer_addr->name);

        goto check_send_fail;
    }

    if (peer->state != NGX_HTTP_CHECK_CONNECT_DONE) {
        if (ngx_handle_write_event(c->write, 0) != NGX_OK) {

            ngx_log_error(NGX_LOG_ERR, event->log, 0,
                          "check handle write event error with peer: %V ",
                          &peer->check_peer_addr->name);

            goto check_send_fail;
        }

        return;
    }

    if (peer->check_data == NULL) {

        peer->check_data = ngx_pcalloc(peer->pool,
                                       sizeof(ngx_http_upstream_check_ctx_t));
        if (peer->check_data == NULL) {
            goto check_send_fail;
        }

        if (peer->init == NULL || peer->init(peer) != NGX_OK) {

            ngx_log_error(NGX_LOG_ERR, event->log, 0,
                          "check init error with peer: %V ",
                          &peer->check_peer_addr->name);

            goto check_send_fail;
        }
    }

    ctx = peer->check_data;

    while (ctx->send.pos < ctx->send.last) {

        size = c->send(c, ctx->send.pos, ctx->send.last - ctx->send.pos);

#if (NGX_DEBUG)
        {
        ngx_err_t  err;

        err = (size >=0) ? 0 : ngx_socket_errno;
        ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, err,
                       "http check send size: %z, total: %z",
                       size, ctx->send.last - ctx->send.pos);
        }
#endif

        if (size > 0) {
            ctx->send.pos += size;
        } else if (size == 0 || size == NGX_AGAIN) {
            return;
        } else {
            c->error = 1;
            goto check_send_fail;
        }
    }

    if (ctx->send.pos == ctx->send.last) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0, "http check send done.");
        peer->state = NGX_HTTP_CHECK_SEND_DONE;
        c->requests++;
    }

    return;

check_send_fail:
    ngx_http_upstream_check_status_update(peer, 0);
    ngx_http_upstream_check_clean_event(peer);
}


static void
ngx_http_upstream_check_recv_handler(ngx_event_t *event)
{
    u_char                         *new_buf;
    ssize_t                         size, n;
    ngx_int_t                       rc;
    ngx_connection_t               *c;
    ngx_http_upstream_check_ctx_t  *ctx;
    ngx_http_upstream_check_peer_t *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    c = event->data;
    peer = c->data;

    if (peer->state != NGX_HTTP_CHECK_SEND_DONE) {

        if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
            goto check_recv_fail;
        }

        return;
    }

    ctx = peer->check_data;

    if (ctx->recv.start == NULL) {
        /* 1/2 of the page_size, is it enough? */
        ctx->recv.start = ngx_palloc(c->pool, ngx_pagesize / 2);
        if (ctx->recv.start == NULL) {
            goto check_recv_fail;
        }

        ctx->recv.last = ctx->recv.pos = ctx->recv.start;
        ctx->recv.end = ctx->recv.start + ngx_pagesize / 2;
    }

    while (1) {
        n = ctx->recv.end - ctx->recv.last;

        /* buffer not big enough? enlarge it by twice */
        if (n == 0) {
            size = ctx->recv.end - ctx->recv.start;
            new_buf = ngx_palloc(c->pool, size * 2);
            if (new_buf == NULL) {
                goto check_recv_fail;
            }

            ngx_memcpy(new_buf, ctx->recv.start, size);

            ctx->recv.pos = ctx->recv.start = new_buf;
            ctx->recv.last = new_buf + size;
            ctx->recv.end = new_buf + size * 2;

            n = ctx->recv.end - ctx->recv.last;
        }

        size = c->recv(c, ctx->recv.last, n);

#if (NGX_DEBUG)
        {
        ngx_err_t  err;

        err = (size >= 0) ? 0 : ngx_socket_errno;
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, err,
                       "http check recv size: %z, peer: %V ",
                       size, &peer->check_peer_addr->name);
        }
#endif

        if (size > 0) {
            ctx->recv.last += size;
            continue;
        } else if (size == 0 || size == NGX_AGAIN) {
            break;
        } else {
            c->error = 1;
            goto check_recv_fail;
        }
    }

    rc = peer->parse(peer);

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                   "http check parse rc: %i, peer: %V ",
                   rc, &peer->check_peer_addr->name);

    switch (rc) {

    case NGX_AGAIN:
        /* The peer has closed its half side of the connection. */
        if (size == 0) {
            ngx_http_upstream_check_status_update(peer, 0);
            c->error = 1;
            break;
        }

        return;

    case NGX_ERROR:
        ngx_log_error(NGX_LOG_ERR, event->log, 0,
                      "check protocol %V error with peer: %V ",
                      &peer->conf->check_type_conf->name,
                      &peer->check_peer_addr->name);

        ngx_http_upstream_check_status_update(peer, 0);
        break;

    case NGX_OK:
        /* fall through */

    default:
        ngx_http_upstream_check_status_update(peer, 1);
        break;
    }

    peer->state = NGX_HTTP_CHECK_RECV_DONE;
    ngx_http_upstream_check_clean_event(peer);
    return;

check_recv_fail:
    ngx_http_upstream_check_status_update(peer, 0);
    ngx_http_upstream_check_clean_event(peer);
}


static ngx_int_t
ngx_http_upstream_check_http_init(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ctx = peer->check_data;
    ucscf = peer->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    ctx->state = 0;

    ngx_memzero(&ctx->status, sizeof(ngx_http_status_t));

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_http_parse(ngx_http_upstream_check_peer_t *peer)
{
    ngx_int_t                            rc;
    ngx_uint_t                           code, code_n;
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ucscf = peer->conf;
    ctx = peer->check_data;

    if ((ctx->recv.last - ctx->recv.pos) > 0) {

        rc = ngx_http_upstream_check_parse_status_line(ctx,
                                                       &ctx->recv,
                                                       &ctx->status);
        if (rc == NGX_AGAIN) {
            return rc;
        }

        if (rc == NGX_ERROR) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "http parse status line error with peer: %V ",
                          &peer->check_peer_addr->name);
            return rc;
        }

        code = ctx->status.code;

        if (code >= 200 && code < 300) {
            code_n = NGX_CHECK_HTTP_2XX;
        } else if (code >= 300 && code < 400) {
            code_n = NGX_CHECK_HTTP_3XX;
        } else if (code >= 400 && code < 500) {
            peer->pc.connection->error = 1;
            code_n = NGX_CHECK_HTTP_4XX;
        } else if (code >= 500 && code < 600) {
            peer->pc.connection->error = 1;
            code_n = NGX_CHECK_HTTP_5XX;
        } else {
            peer->pc.connection->error = 1;
            code_n = NGX_CHECK_HTTP_ERR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http_parse: code_n: %ui, conf: %ui",
                       code_n, ucscf->code.status_alive);

        if (code_n & ucscf->code.status_alive) {
            return NGX_OK;
        } else {
            return NGX_ERROR;
        }
    } else {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_fastcgi_process_record(
    ngx_http_upstream_check_ctx_t *ctx, ngx_buf_t *b, ngx_http_status_t *status)
{
    u_char                     ch, *p;
    ngx_http_fastcgi_state_e   state;

    state = ctx->state;

    for (p = b->pos; p < b->last; p++) {

        ch = *p;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http fastcgi record byte: %02Xd", ch);

        switch (state) {

        case ngx_http_fastcgi_st_version:
            if (ch != 1) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "upstream sent unsupported FastCGI "
                              "protocol version: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_type;
            break;

        case ngx_http_fastcgi_st_type:
            switch (ch) {
            case NGX_HTTP_FASTCGI_STDOUT:
            case NGX_HTTP_FASTCGI_STDERR:
            case NGX_HTTP_FASTCGI_END_REQUEST:
                status->code = (ngx_uint_t) ch;
                break;
            default:
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "upstream sent invalid FastCGI "
                              "record type: %d", ch);
                return NGX_ERROR;

            }
            state = ngx_http_fastcgi_st_request_id_hi;
            break;

        /* we support the single request per connection */

        case ngx_http_fastcgi_st_request_id_hi:
            if (ch != 0) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "upstream sent unexpected FastCGI "
                              "request id high byte: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_request_id_lo;
            break;

        case ngx_http_fastcgi_st_request_id_lo:
            if (ch != 1) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "upstream sent unexpected FastCGI "
                              "request id low byte: %d", ch);
                return NGX_ERROR;
            }
            state = ngx_http_fastcgi_st_content_length_hi;
            break;

        case ngx_http_fastcgi_st_content_length_hi:
            ctx->length = ch << 8;
            state = ngx_http_fastcgi_st_content_length_lo;
            break;

        case ngx_http_fastcgi_st_content_length_lo:
            ctx->length |= (size_t) ch;
            state = ngx_http_fastcgi_st_padding_length;
            break;

        case ngx_http_fastcgi_st_padding_length:
            ctx->padding = (size_t) ch;
            state = ngx_http_fastcgi_st_reserved;
            break;

        case ngx_http_fastcgi_st_reserved:
            state = ngx_http_fastcgi_st_data;

            b->pos = p + 1;
            ctx->state = state;

            return NGX_OK;

        /* suppress warning */
        case ngx_http_fastcgi_st_data:
        case ngx_http_fastcgi_st_padding:
            break;
        }
    }

    ctx->state = state;

    return NGX_AGAIN;
}


static ngx_int_t
ngx_http_upstream_check_fastcgi_parse(ngx_http_upstream_check_peer_t *peer)
{
    ngx_int_t                            rc;
    ngx_flag_t                           done;
    ngx_uint_t                           type, code, code_n;
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ucscf = peer->conf;
    ctx = peer->check_data;

    if ((ctx->recv.last - ctx->recv.pos) <= 0) {
        return NGX_AGAIN;
    }

    done = 0;

    for ( ;; ) {

        if (ctx->state < ngx_http_fastcgi_st_data) {
            rc = ngx_http_upstream_check_fastcgi_process_record(ctx,
                    &ctx->recv, &ctx->status);

            type = ctx->status.code;

            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                           "fastcgi_parse rc: [%i], type: [%ui]", rc, type);

            if (rc == NGX_AGAIN) {
                return rc;
            }

            if (rc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                   "check fastcgi parse status line error with peer: %V",
                   &peer->check_peer_addr->name);

                return rc;
            }

            if (type != NGX_HTTP_FASTCGI_STDOUT
                && type != NGX_HTTP_FASTCGI_STDERR)
            {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                   "check fastcgi sent unexpected FastCGI record: %d", type);

                return NGX_ERROR;
            }

            if (type == NGX_HTTP_FASTCGI_STDOUT && ctx->length == 0) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                   "check fastcgi prematurely closed FastCGI stdout");

                return NGX_ERROR;
            }
        }

        if (ctx->state == ngx_http_fastcgi_st_padding) {

            if (ctx->recv.pos + ctx->padding < ctx->recv.last) {
                ctx->status.code = ngx_http_fastcgi_st_version;
                ctx->recv.pos += ctx->padding;

                continue;
            }

            if (ctx->recv.pos + ctx->padding == ctx->recv.last) {
                ctx->status.code = ngx_http_fastcgi_st_version;
                ctx->recv.pos = ctx->recv.last;

                return NGX_AGAIN;
            }

            ctx->padding -= ctx->recv.last - ctx->recv.pos;
            ctx->recv.pos = ctx->recv.last;

            return NGX_AGAIN;
        }

        if (ctx->status.code == NGX_HTTP_FASTCGI_STDERR) {

            ngx_log_error(NGX_LOG_WARN, ngx_cycle->log, 0,
                          "fastcgi check error");

            return NGX_ERROR;
        }

        /* ctx->status.code == NGX_HTTP_FASTCGI_STDOUT */

        if (ctx->recv.pos + ctx->length < ctx->recv.last) {
            ctx->recv.last = ctx->recv.pos + ctx->length;
        } else {
            return NGX_ERROR;
        }

        ctx->status.code = 0;

        for ( ;; ) {
            rc = ngx_http_upstream_check_parse_fastcgi_status(ctx,
                                                              &ctx->recv,
                                                              &ctx->status);
            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                          "fastcgi http parse status line rc: %i ", rc);

            if (rc == NGX_ERROR) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                   "fastcgi http parse status line error with peer: %V ",
                    &peer->check_peer_addr->name);
                return NGX_ERROR;
            }

            if (rc == NGX_AGAIN) {
                break;
            }

            if (rc == NGX_DONE) {
                done = 1;
                ngx_log_error(NGX_LOG_DEBUG, ngx_cycle->log, 0,
                              "fastcgi http parse status: %i",
                              ctx->status.code);
                break;
            }

            /* rc = NGX_OK */
        }

        if (ucscf->code.status_alive == 0 || done == 0) {
            return NGX_OK;
        }

        code = ctx->status.code;

        if (code >= 200 && code < 300) {
            code_n = NGX_CHECK_HTTP_2XX;
        } else if (code >= 300 && code < 400) {
            code_n = NGX_CHECK_HTTP_3XX;
        } else if (code >= 400 && code < 500) {
            code_n = NGX_CHECK_HTTP_4XX;
        } else if (code >= 500 && code < 600) {
            code_n = NGX_CHECK_HTTP_5XX;
        } else {
            code_n = NGX_CHECK_HTTP_ERR;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "fastcgi http_parse: code_n: %ui, conf: %ui",
                       code_n, ucscf->code.status_alive);

        if (code_n & ucscf->code.status_alive) {
            return NGX_OK;
        } else {
            return NGX_ERROR;
        }

    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_parse_fastcgi_status(ngx_http_upstream_check_ctx_t *ctx,
    ngx_buf_t *b, ngx_http_status_t *status)
{
    u_char      c, ch, *p, *name_s, *name_e;
    ngx_flag_t  find;

    enum {
        sw_start = 0,
        sw_name,
        sw_space_before_value,
        sw_value,
        sw_space_after_value,
        sw_ignore_line,
        sw_almost_done,
        sw_header_almost_done
    } state;

    /* the last '\0' is not needed because string is zero terminated */

    static u_char  lowcase[] =
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0-\0\0" "0123456789\0\0\0\0\0\0"
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
        "\0abcdefghijklmnopqrstuvwxyz\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
        "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";

    status->count = 0;
    status->code = 0;
    find = 0;
    name_s = name_e = NULL;
    state = sw_start;

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

        /* first char */
        case sw_start:

            switch (ch) {
            case CR:
                state = sw_header_almost_done;
                break;
            case LF:
                goto header_done;
            default:
                state = sw_name;

                c = lowcase[ch];

                if (c) {
                    name_s = p;
                    break;
                }

                if (ch == '\0') {
                    return NGX_ERROR;
                }


                break;
            }

            break;

        /* header name */
        case sw_name:
            c = lowcase[ch];

            if (c) {
                break;
            }

            if (ch == ':') {
                name_e = p;
#if (NGX_DEBUG)
                ngx_str_t name;
                name.data = name_s;
                name.len = name_e - name_s;
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                               "fastcgi header: %V", &name);
#endif
                state = sw_space_before_value;

                if (ngx_strncasecmp(name_s, (u_char *) "status",
                                    name_e - name_s)
                    == 0)
                {

                    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                                   "find status header");

                    find = 1;
                }

                break;
            }

            if (ch == CR) {
                state = sw_almost_done;
                break;
            }

            if (ch == LF) {
                goto done;
            }

            /* IIS may send the duplicate "HTTP/1.1 ..." lines */
            if (ch == '\0') {
                return NGX_ERROR;
            }

            break;

        /* space* before header value */
        case sw_space_before_value:
            switch (ch) {
            case ' ':
                break;
            case CR:
                state = sw_almost_done;
                break;
            case LF:
                goto done;
            case '\0':
                return NGX_ERROR;
            default:
                state = sw_value;
                if (find) {
                    if (ch < '1' || ch > '9') {
                        return NGX_ERROR;
                    }

                    status->code = status->code * 10 + ch - '0';
                    if (status->count++ != 0) {
                        return NGX_ERROR;
                    }
                }

                break;
            }

            break;

        /* header value */
        case sw_value:

            if (find) {
                if (ch < '0' || ch > '9') {
                    return NGX_ERROR;
                }

                status->code = status->code * 10 + ch - '0';

                if (++status->count == 3) {
                    return NGX_DONE;
                }
            }

            switch (ch) {
            case ' ':
                state = sw_space_after_value;
                break;
            case CR:
                state = sw_almost_done;
                break;
            case LF:
                goto done;
            case '\0':
                return NGX_ERROR;
            }

            break;

        /* space* before end of header line */
        case sw_space_after_value:
            switch (ch) {
            case ' ':
                break;
            case CR:
                state = sw_almost_done;
                break;
            case LF:
                state = sw_start;
                break;
            case '\0':
                return NGX_ERROR;
            default:
                state = sw_value;
                break;
            }
            break;

        /* ignore header line */
        case sw_ignore_line:
            switch (ch) {
            case LF:
                state = sw_start;
                break;
            default:
                break;
            }
            break;

        /* end of header line */
        case sw_almost_done:
            switch (ch) {
            case LF:
                goto done;
            case CR:
                break;
            default:
                return NGX_ERROR;
            }
            break;

        /* end of header */
        case sw_header_almost_done:
            switch (ch) {
            case LF:
                goto header_done;
            default:
                return NGX_ERROR;
            }
        }
    }

    b->pos = p;
    ctx->state = state;

    return NGX_AGAIN;

done:

    b->pos = p + 1;
    ctx->state = sw_start;

    return NGX_OK;

header_done:

    b->pos = p + 1;
    ctx->state = sw_start;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_parse_status_line(ngx_http_upstream_check_ctx_t *ctx,
    ngx_buf_t *b, ngx_http_status_t *status)
{
    u_char ch, *p;
    enum {
        sw_start = 0,
        sw_H,
        sw_HT,
        sw_HTT,
        sw_HTTP,
        sw_first_major_digit,
        sw_major_digit,
        sw_first_minor_digit,
        sw_minor_digit,
        sw_status,
        sw_space_after_status,
        sw_status_text,
        sw_almost_done
    } state;

    state = ctx->state;

    for (p = b->pos; p < b->last; p++) {
        ch = *p;

        switch (state) {

        /* "HTTP/" */
        case sw_start:
            if (ch != 'H') {
                return NGX_ERROR;
            }

            state = sw_H;
            break;

        case sw_H:
            if (ch != 'T') {
                return NGX_ERROR;
            }

            state = sw_HT;
            break;

        case sw_HT:
            if (ch != 'T') {
                return NGX_ERROR;
            }

            state = sw_HTT;
            break;

        case sw_HTT:
            if (ch != 'P') {
                return NGX_ERROR;
            }

            state = sw_HTTP;
            break;

        case sw_HTTP:
            if (ch != '/') {
                return NGX_ERROR;
            }

            state = sw_first_major_digit;
            break;

        /* the first digit of major HTTP version */
        case sw_first_major_digit:
            if (ch < '1' || ch > '9') {
                return NGX_ERROR;
            }

            state = sw_major_digit;
            break;

        /* the major HTTP version or dot */
        case sw_major_digit:
            if (ch == '.') {
                state = sw_first_minor_digit;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            break;

        /* the first digit of minor HTTP version */
        case sw_first_minor_digit:
            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            state = sw_minor_digit;
            break;

        /* the minor HTTP version or the end of the request line */
        case sw_minor_digit:
            if (ch == ' ') {
                state = sw_status;
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            break;

        /* HTTP status code */
        case sw_status:
            if (ch == ' ') {
                break;
            }

            if (ch < '0' || ch > '9') {
                return NGX_ERROR;
            }

            status->code = status->code * 10 + ch - '0';

            if (++status->count == 3) {
                state = sw_space_after_status;
                status->start = p - 2;
            }

            break;

        /* space or end of line */
        case sw_space_after_status:
            switch (ch) {
            case ' ':
                state = sw_status_text;
                break;
            case '.':                    /* IIS may send 403.1, 403.2, etc */
                state = sw_status_text;
                break;
            case CR:
                state = sw_almost_done;
                break;
            case LF:
                goto done;
            default:
                return NGX_ERROR;
            }
            break;

        /* any text until end of line */
        case sw_status_text:
            switch (ch) {
            case CR:
                state = sw_almost_done;

                break;
            case LF:
                goto done;
            }
            break;

        /* end of status line */
        case sw_almost_done:
            status->end = p - 1;
            if (ch == LF) {
                goto done;
            } else {
                return NGX_ERROR;
            }
        }
    }

    b->pos = p;
    ctx->state = state;

    return NGX_AGAIN;

done:

    b->pos = p + 1;

    if (status->end == NULL) {
        status->end = p;
    }

    ctx->state = sw_start;

    return NGX_OK;
}


static void
ngx_http_upstream_check_http_reinit(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t  *ctx;

    ctx = peer->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;

    ctx->state = 0;

    ngx_memzero(&ctx->status, sizeof(ngx_http_status_t));
}


static ngx_int_t
ngx_http_upstream_check_ssl_hello_init(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ctx = peer->check_data;
    ucscf = peer->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


/* a rough check of server ssl_hello responses */
static ngx_int_t
ngx_http_upstream_check_ssl_hello_parse(ngx_http_upstream_check_peer_t *peer)
{
    size_t                         size;
    ngx_ssl_server_hello_t        *resp;
    ngx_http_upstream_check_ctx_t *ctx;

    ctx = peer->check_data;

    size = ctx->recv.last - ctx->recv.pos;
    if (size < sizeof(ngx_ssl_server_hello_t)) {
        return NGX_AGAIN;
    }

    resp = (ngx_ssl_server_hello_t *) ctx->recv.pos;

    ngx_log_debug7(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "http check ssl_parse, type: %ud, version: %ud.%ud, "
                   "length: %ud, handshanke_type: %ud, hello_version: %ud.%ud",
                   resp->msg_type, resp->version.major, resp->version.minor,
                   ntohs(resp->length), resp->handshake_type,
                   resp->hello_version.major, resp->hello_version.minor);

    if (resp->msg_type != NGX_SSL_HANDSHAKE) {
        return NGX_ERROR;
    }

    if (resp->handshake_type != NGX_SSL_SERVER_HELLO) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_upstream_check_ssl_hello_reinit(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t *ctx;

    ctx = peer->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static ngx_int_t
ngx_http_upstream_check_mysql_init(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ctx = peer->check_data;
    ucscf = peer->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


/* a rough check of mysql greeting responses */
static ngx_int_t
ngx_http_upstream_check_mysql_parse(ngx_http_upstream_check_peer_t *peer)
{
    size_t                         size;
    ngx_mysql_handshake_init_t    *handshake;
    ngx_http_upstream_check_ctx_t *ctx;

    ctx = peer->check_data;

    size = ctx->recv.last - ctx->recv.pos;
    if (size < sizeof(ngx_mysql_handshake_init_t)) {
        return NGX_AGAIN;
    }

    handshake = (ngx_mysql_handshake_init_t *) ctx->recv.pos;

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "mysql_parse: packet_number=%ud, protocol=%ud, server=%s",
                   handshake->packet_number, handshake->protocol_version,
                   handshake->others);

    /* The mysql greeting packet's serial number always begins with 0. */
    if (handshake->packet_number != 0x00) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_upstream_check_mysql_reinit(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t *ctx;

    ctx = peer->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static ngx_int_t
ngx_http_upstream_check_ajp_init(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t       *ctx;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ctx = peer->check_data;
    ucscf = peer->conf;

    ctx->send.start = ctx->send.pos = (u_char *)ucscf->send.data;
    ctx->send.end = ctx->send.last = ctx->send.start + ucscf->send.len;

    ctx->recv.start = ctx->recv.pos = NULL;
    ctx->recv.end = ctx->recv.last = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_ajp_parse(ngx_http_upstream_check_peer_t *peer)
{
    size_t                         size;
    u_char                        *p;
    ngx_http_upstream_check_ctx_t *ctx;

    ctx = peer->check_data;

    size = ctx->recv.last - ctx->recv.pos;
    if (size < sizeof(ngx_ajp_cpong_packet)) {
        return NGX_AGAIN;
    }

    p = ctx->recv.pos;

#if (NGX_DEBUG)
    {
    ngx_ajp_raw_packet_t  *ajp;

    ajp = (ngx_ajp_raw_packet_t *) p;
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                   "ajp_parse: preamble=0x%uxd, length=0x%uxd, type=0x%uxd",
                   ntohs(ajp->preamble), ntohs(ajp->length), ajp->type);
    }
#endif

    if (ngx_memcmp(ngx_ajp_cpong_packet, p, sizeof(ngx_ajp_cpong_packet)) == 0)
    {
        return NGX_OK;
    } else {
        return NGX_ERROR;
    }
}


static void
ngx_http_upstream_check_ajp_reinit(ngx_http_upstream_check_peer_t *peer)
{
    ngx_http_upstream_check_ctx_t  *ctx;

    ctx = peer->check_data;

    ctx->send.pos = ctx->send.start;
    ctx->send.last = ctx->send.end;

    ctx->recv.pos = ctx->recv.last = ctx->recv.start;
}


static void
ngx_http_upstream_check_status_update(ngx_http_upstream_check_peer_t *peer,
    ngx_int_t result)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (peer->dyn_id && peer->shm->dyn_id != peer->dyn_id) {
        return;
    }
#endif

    ucscf = peer->conf;

    if (result) {
        peer->shm->rise_count++;
        peer->shm->fall_count = 0;
        if (peer->shm->down && peer->shm->rise_count >= ucscf->rise_count) {
            peer->shm->down = 0;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "enable check peer: %V ",
                          &peer->check_peer_addr->name);
        }
    } else {
        peer->shm->rise_count = 0;
        peer->shm->fall_count++;
        if (!peer->shm->down && peer->shm->fall_count >= ucscf->fall_count) {
            peer->shm->down = 1;
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "disable check peer: %V ",
                          &peer->check_peer_addr->name);
        }
    }

    peer->shm->access_time = ngx_current_msec;
}


/*
 * A tcp check only connects, so it closes the connection with a reset
 * (SO_LINGER 0) rather than a FIN. Otherwise every check would leave a
 * socket in TIME_WAIT on the nginx side for 60 seconds, i.e. 60 / interval
 * sockets per checked server.
 */

static void
ngx_http_upstream_check_reset_connection(ngx_connection_t *c)
{
    struct linger  linger;

    linger.l_onoff = 1;
    linger.l_linger = 0;

    if (setsockopt(c->fd, SOL_SOCKET, SO_LINGER,
                   (const void *) &linger, sizeof(struct linger)) == -1)
    {
        ngx_log_error(NGX_LOG_ALERT, c->log, ngx_socket_errno,
                      "setsockopt(SO_LINGER) failed");
    }
}


static void
ngx_http_upstream_check_clean_event(ngx_http_upstream_check_peer_t *peer)
{
    ngx_uint_t                           keep;
    ngx_connection_t                    *c;
    ngx_http_upstream_check_srv_conf_t  *ucscf;
    ngx_check_conf_t                    *cf;

    c = peer->pc.connection;
    ucscf = peer->conf;
    cf = ucscf->check_type_conf;

    keep = 0;

    if (c) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, c->log, 0,
                       "http check clean event: index:%i, fd: %d",
                       peer->index, c->fd);
        if (c->error == 0 &&
            cf->need_keepalive &&
            (c->requests < ucscf->check_keepalive_requests))
        {
            c->write->handler = ngx_http_upstream_check_dummy_handler;
            c->read->handler = ngx_http_upstream_check_discard_handler;
            keep = 1;

        } else {
            if (cf->type == NGX_HTTP_CHECK_TCP) {
                ngx_http_upstream_check_reset_connection(c);
            }

            ngx_close_connection(c);
            peer->pc.connection = NULL;
        }
    }

    if (peer->check_timeout_ev.timer_set) {
        ngx_del_timer(&peer->check_timeout_ev);
    }

    peer->state = NGX_HTTP_CHECK_ALL_DONE;

    if (peer->check_data != NULL && peer->reinit) {
        peer->reinit(peer);
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (peer->dyn_id && peer->shm->dyn_id != peer->dyn_id) {
        return;
    }
#endif

    if (keep) {
        peer->shm->keepalive_owner = ngx_pid;

    } else if (peer->shm->keepalive_owner == ngx_pid) {
        peer->shm->keepalive_owner = NGX_INVALID_PID;
    }

    /*
     * The discard handler gets here too when the peer closes an idle
     * connection, while another worker may be checking the peer.
     */

    if (peer->shm->owner == ngx_pid) {
        peer->shm->owner = NGX_INVALID_PID;
    }
}


static void
ngx_http_upstream_check_timeout_handler(ngx_event_t *event)
{
    ngx_http_upstream_check_peer_t  *peer;

    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    peer = event->data;
    peer->pc.connection->error = 1;

    ngx_log_error(NGX_LOG_ERR, event->log, 0,
                  "check time out with peer: %V ",
                  &peer->check_peer_addr->name);

    ngx_http_upstream_check_status_update(peer, 0);
    ngx_http_upstream_check_clean_event(peer);
}


static void
ngx_http_upstream_check_finish_handler(ngx_event_t *event)
{
    if (ngx_http_upstream_check_need_exit()) {
        return;
    }
}


static ngx_int_t
ngx_http_upstream_check_need_exit()
{
    if (ngx_terminate || ngx_exiting || ngx_quit) {
        ngx_http_upstream_check_clear_all_events();
        return 1;
    }

    return 0;
}


static void
ngx_http_upstream_check_clear_all_events()
{
    static ngx_flag_t                has_cleared = 0;

    if (has_cleared || check_peers_ctx == NULL) {
        return;
    }

    ngx_log_error(NGX_LOG_NOTICE, ngx_cycle->log, 0,
                  "clear all the events on %P ", ngx_pid);

    has_cleared = 1;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_http_upstream_check_sync_event.timer_set) {
        ngx_del_timer(&ngx_http_upstream_check_sync_event);
    }
#endif

    ngx_http_upstream_check_clear_peers_events(check_peers_ctx);
}


static void
ngx_http_upstream_check_clear_peers_events(
    ngx_http_upstream_check_peers_t *peers)
{
    ngx_uint_t                       i;
    ngx_connection_t                *c;
    ngx_http_upstream_check_peer_t  *peer;

    peer = peers->peers.elts;
    for (i = 0; i < peers->peers.nelts; i++) {

        if (peer[i].check_ev.timer_set) {
            ngx_del_timer(&peer[i].check_ev);
        }

        if (peer[i].check_timeout_ev.timer_set) {
            ngx_del_timer(&peer[i].check_timeout_ev);
        }

        c = peer[i].pc.connection;
        if (c) {
            ngx_close_connection(c);
            peer[i].pc.connection = NULL;
        }

        if (peer[i].pool != NULL) {
            ngx_destroy_pool(peer[i].pool);
            peer[i].pool = NULL;
        }
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    ngx_http_upstream_check_dyn_free_all(peers);
#endif
}


static char *
ngx_http_upstream_check_upstream_type(ngx_http_upstream_check_srv_conf_t *ucscf)
{
    return ucscf->upstream_type == NGX_UPSTREAM_CHECK_STREAM ? "stream" : "http";
}


static ngx_int_t
ngx_http_upstream_check_status_handler(ngx_http_request_t *r)
{
    size_t                                 buffer_size;
    ngx_int_t                              rc;
    ngx_buf_t                             *b;
    ngx_uint_t                             i;
    ngx_array_t                           *records;
    ngx_chain_t                            out;
    ngx_http_upstream_check_peer_t        *peer;
    ngx_http_upstream_check_peers_t       *peers;
    ngx_http_upstream_check_loc_conf_t    *uclcf;
    ngx_http_upstream_check_status_peer_t *record;
    ngx_http_upstream_check_status_ctx_t  *ctx;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    uclcf = ngx_http_get_module_loc_conf(r, ngx_http_upstream_check_module);

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_upstream_check_status_ctx_t));
    if (ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_upstream_check_status_parse_args(r, ctx);

    if (ctx->format == NULL) {
        ctx->format = uclcf->format;
    }

    r->headers_out.content_type = ctx->format->content_type;

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    peers = check_peers_ctx;
    if (peers == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "http upstream check module can not find any check "
                      "server, make sure you've added the check servers");

        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    records = ngx_array_create(r->pool, peers->peers.nelts + 1,
                               sizeof(ngx_http_upstream_check_status_peer_t));
    if (records == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    peer = peers->peers.elts;

    for (i = 0; i < peers->peers.nelts; i++) {
        record = ngx_array_push(records);
        if (record == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        record->upstream = peer[i].upstream_name;
        record->name = peer[i].peer_addr->name;
        record->shm = peer[i].shm;
        record->conf = peer[i].conf;
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_http_upstream_check_dyn_status(r, peers, records) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
#endif

    /* 1/4 pagesize for each record */
    buffer_size = records->nelts * ngx_pagesize / 4;
    buffer_size = ngx_align(buffer_size, ngx_pagesize) + ngx_pagesize;

    b = ngx_create_temp_buf(r->pool, buffer_size);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;

    ctx->format->output(b, records, peers->generation, ctx->flag);

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    if (r->headers_out.content_length_n == 0) {
        r->header_only = 1;
    }

    b->last_buf = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}


static void
ngx_http_upstream_check_status_parse_args(ngx_http_request_t *r,
    ngx_http_upstream_check_status_ctx_t *ctx)
{
    ngx_str_t                    value;
    ngx_uint_t                   i;
    ngx_check_status_command_t  *command;

    if (r->args.len == 0) {
        return;
    }

    for (i = 0; /* void */ ; i++) {

        command = &ngx_check_status_commands[i];

        if (command->name.len == 0) {
            break;
        }

        if (ngx_http_arg(r, command->name.data, command->name.len, &value)
            == NGX_OK) {

           if (command->handler(ctx, &value) != NGX_OK) {
               ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                             "http upstream check, bad argument: \"%V\"",
                             &value);
           }
        }
    }

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
            "http upstream check, flag: \"%ui\"", ctx->flag);
}


static ngx_int_t
ngx_http_upstream_check_status_command_format(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value)
{
    ctx->format = ngx_http_get_check_status_format_conf(value);
    if (ctx->format == NULL) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_status_command_status(
    ngx_http_upstream_check_status_ctx_t *ctx, ngx_str_t *value)
{
    if (value->len == (sizeof("down") - 1)
        && ngx_strncasecmp(value->data, (u_char *) "down", value->len) == 0) {

        ctx->flag |= NGX_CHECK_STATUS_DOWN;

    } else if (value->len == (sizeof("up") - 1)
               && ngx_strncasecmp(value->data, (u_char *) "up", value->len)
               == 0) {

        ctx->flag |= NGX_CHECK_STATUS_UP;

    } else {
        return NGX_ERROR;
    }

    return NGX_OK;
}


static void
ngx_http_upstream_check_status_html_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag)
{
    ngx_uint_t                      i, count;
    ngx_http_upstream_check_status_peer_t *peer;

    peer = peers->elts;

    count = 0;

    for (i = 0; i < peers->nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        count++;
    }

    b->last = ngx_snprintf(b->last, b->end - b->last,
            "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\n"
            "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
            "<head>\n"
            "  <title>Nginx http upstream check status</title>\n"
            "</head>\n"
            "<body>\n"
            "<h1>Nginx http upstream check status</h1>\n"
            "<h2>Check upstream server number: %ui, generation: %ui</h2>\n"
            "<table style=\"background-color:white\" cellspacing=\"0\" "
            "       cellpadding=\"3\" border=\"1\">\n"
            "  <tr bgcolor=\"#C0C0C0\">\n"
            "    <th>Index</th>\n"
            "    <th>Upstream</th>\n"
            "    <th>Name</th>\n"
            "    <th>Status</th>\n"
            "    <th>Rise counts</th>\n"
            "    <th>Fall counts</th>\n"
            "    <th>Check type</th>\n"
            "    <th>Check port</th>\n"
            "    <th>Upstream type</th>\n"
            "  </tr>\n",
            count, generation);

    for (i = 0; i < peers->nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        b->last = ngx_snprintf(b->last, b->end - b->last,
                "  <tr%s>\n"
                "    <td>%ui</td>\n"
                "    <td>%V</td>\n"
                "    <td>%V</td>\n"
                "    <td>%s</td>\n"
                "    <td>%ui</td>\n"
                "    <td>%ui</td>\n"
                "    <td>%V</td>\n"
                "    <td>%ui</td>\n"
                "    <td>%s</td>\n"
                "  </tr>\n",
                peer[i].shm->down ? " bgcolor=\"#FF0000\"" : "",
                i,
                peer[i].upstream,
                &peer[i].name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port,
                ngx_http_upstream_check_upstream_type(peer[i].conf));
    }

    b->last = ngx_snprintf(b->last, b->end - b->last,
            "</table>\n"
            "</body>\n"
            "</html>\n");
}


static void
ngx_http_upstream_check_status_csv_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag)
{
    ngx_uint_t                       i;
    ngx_http_upstream_check_status_peer_t  *peer;

    peer = peers->elts;
    for (i = 0; i < peers->nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        b->last = ngx_snprintf(b->last, b->end - b->last,
                "%ui,%V,%V,%s,%ui,%ui,%V,%ui,%s\n",
                i,
                peer[i].upstream,
                &peer[i].name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port,
                ngx_http_upstream_check_upstream_type(peer[i].conf));
    }
}


static void
ngx_http_upstream_check_status_json_format(ngx_buf_t *b,
    ngx_array_t *peers, ngx_uint_t generation, ngx_uint_t flag)
{
    ngx_uint_t                       count, i;
    ngx_http_upstream_check_status_peer_t  *peer;

    peer = peers->elts;

    count = 0;

    for (i = 0; i < peers->nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        count++;
    }

    b->last = ngx_snprintf(b->last, b->end - b->last,
            "{\"servers\": {\n"
            "  \"total\": %ui,\n"
            "  \"generation\": %ui,\n"
            "  \"server\": [\n",
            count,
            generation);

    for (i = 0; i < peers->nelts; i++) {

        if (flag & NGX_CHECK_STATUS_DOWN) {

            if (!peer[i].shm->down) {
                continue;
            }

        } else if (flag & NGX_CHECK_STATUS_UP) {

            if (peer[i].shm->down) {
                continue;
            }
        }

        b->last = ngx_snprintf(b->last, b->end - b->last,
                "    {\"index\": %ui, "
                "\"upstream\": \"%V\", "
                "\"name\": \"%V\", "
                "\"status\": \"%s\", "
                "\"rise\": %ui, "
                "\"fall\": %ui, "
                "\"type\": \"%V\", "
                "\"port\": %ui, "
                "\"upstream_type\": \"%s\"}"
                "%s\n",
                i,
                peer[i].upstream,
                &peer[i].name,
                peer[i].shm->down ? "down" : "up",
                peer[i].shm->rise_count,
                peer[i].shm->fall_count,
                &peer[i].conf->check_type_conf->name,
                peer[i].conf->port,
                ngx_http_upstream_check_upstream_type(peer[i].conf),
                (--count == 0) ? "" : ",");
    }

    b->last = ngx_snprintf(b->last, b->end - b->last,
            "  ]\n");

    b->last = ngx_snprintf(b->last, b->end - b->last,
            "}}\n");
}


static ngx_check_conf_t *
ngx_http_get_check_type_conf(ngx_str_t *str)
{
    ngx_uint_t  i;

    for (i = 0; /* void */ ; i++) {

        if (ngx_check_types[i].type == 0) {
            break;
        }

        if (str->len != ngx_check_types[i].name.len) {
            continue;
        }

        if (ngx_strncmp(str->data, ngx_check_types[i].name.data,
                        str->len) == 0)
        {
            return &ngx_check_types[i];
        }
    }

    return NULL;
}


char *
ngx_upstream_check_conf_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t                           *value, s;
    ngx_uint_t                           i, port, rise, fall, default_down;
    ngx_msec_t                           interval, timeout;

    /* default values */
    port = 0;
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;
    default_down = 1;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "type=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            ucscf->check_type_conf = ngx_http_get_check_type_conf(&s);

            if (ucscf->check_type_conf == NULL) {
                goto invalid_check_parameter;
            }

            /* there is no check_fastcgi_param in stream */

            if (ucscf->upstream_type != NGX_UPSTREAM_CHECK_HTTP
                && s.len == sizeof("fastcgi") - 1
                && ngx_strncmp(s.data, "fastcgi", s.len) == 0)
            {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "port=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            port = ngx_atoi(s.data, s.len);
            if (port == (ngx_uint_t) NGX_ERROR || port > 65535) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            interval = ngx_atoi(s.data, s.len);
            if (interval == (ngx_msec_t) NGX_ERROR || interval == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            s.len = value[i].len - 8;
            s.data = value[i].data + 8;

            timeout = ngx_atoi(s.data, s.len);
            if (timeout == (ngx_msec_t) NGX_ERROR || timeout == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            rise = ngx_atoi(s.data, s.len);
            if (rise == (ngx_uint_t) NGX_ERROR || rise == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            fall = ngx_atoi(s.data, s.len);
            if (fall == (ngx_uint_t) NGX_ERROR || fall == 0) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "default_down=", 13) == 0) {
            s.len = value[i].len - 13;
            s.data = value[i].data + 13;

            if (ngx_strcasecmp(s.data, (u_char *) "true") == 0) {
                default_down = 1;
            } else if (ngx_strcasecmp(s.data, (u_char *) "false") == 0) {
                default_down = 0;
            } else {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "invalid value \"%s\", "
                                   "it must be \"true\" or \"false\"",
                                   value[i].data);
                return NGX_CONF_ERROR;
            }

            continue;
        }

        goto invalid_check_parameter;
    }

    ucscf->port = port;
    ucscf->check_interval = interval;
    ucscf->check_timeout = timeout;
    ucscf->fall_count = fall;
    ucscf->rise_count = rise;
    ucscf->default_down = default_down;

    if (ucscf->check_type_conf == NGX_CONF_UNSET_PTR) {
        ngx_str_set(&s, "tcp");
        ucscf->check_type_conf = ngx_http_get_check_type_conf(&s);
    }

    return NGX_CONF_OK;

invalid_check_parameter:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


char *
ngx_upstream_check_conf_keepalive_requests(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t                           *value;
    ngx_uint_t                           requests;

    value = cf->args->elts;

    requests = ngx_atoi(value[1].data, value[1].len);
    if (requests == (ngx_uint_t) NGX_ERROR || requests == 0) {
        return "invalid value";
    }

    ucscf->check_keepalive_requests = requests;

    return NGX_CONF_OK;
}


char *
ngx_upstream_check_conf_http_send(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t                           *value;

    value = cf->args->elts;

    ucscf->send = value[1];

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_fastcgi_params(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t                           *value, *k, *v;

    value = cf->args->elts;

    k = ngx_array_push(ucscf->fastcgi_params);
    if (k == NULL) {
        return NGX_CONF_ERROR;
    }

    v = ngx_array_push(ucscf->fastcgi_params);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    *k = value[1];
    *v = value[2];

    return NGX_CONF_OK;
}


char *
ngx_upstream_check_conf_http_expect_alive(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf = conf;

    ngx_str_t                           *value;
    ngx_uint_t                           bit, i, m;
    ngx_conf_bitmask_t                  *mask;

    value = cf->args->elts;
    mask = ngx_check_http_expect_alive_masks;

    bit = ucscf->code.status_alive;

    for (i = 1; i < cf->args->nelts; i++) {
        for (m = 0; mask[m].name.len != 0; m++) {

            if (mask[m].name.len != value[i].len
                || ngx_strcasecmp(mask[m].name.data, value[i].data) != 0)
            {
                continue;
            }

            if (bit & mask[m].mask) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "duplicate value \"%s\"", value[i].data);

            } else {
                bit |= mask[m].mask;
            }

            break;
        }

        if (mask[m].name.len == 0) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "invalid value \"%s\"", value[i].data);

            return NGX_CONF_ERROR;
        }
    }

    ucscf->code.status_alive = bit;

    return NGX_CONF_OK;
}


char *
ngx_upstream_check_conf_shm_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_upstream_check_main_conf_t  *ucmcf = conf;

    ngx_str_t                       *value;

    if (ucmcf->check_shm_size) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ucmcf->check_shm_size = ngx_parse_size(&value[1]);
    if (ucmcf->check_shm_size == (size_t) NGX_ERROR) {
        return "invalid value";
    }

    return NGX_CONF_OK;
}


static ngx_check_status_conf_t *
ngx_http_get_check_status_format_conf(ngx_str_t *str)
{
    ngx_uint_t  i;

    for (i = 0; /* void */ ; i++) {

        if (ngx_check_status_formats[i].format.len == 0) {
            break;
        }

        if (str->len != ngx_check_status_formats[i].format.len) {
            continue;
        }

        if (ngx_strncmp(str->data, ngx_check_status_formats[i].format.data,
                        str->len) == 0)
        {
            return &ngx_check_status_formats[i];
        }
    }

    return NULL;
}


static char *
ngx_http_upstream_check_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                           *value;
    ngx_http_core_loc_conf_t            *clcf;
    ngx_http_upstream_check_loc_conf_t  *uclcf;

    value = cf->args->elts;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_upstream_check_status_handler;

    if (cf->args->nelts == 2) {
        uclcf = ngx_http_conf_get_module_loc_conf(cf,
                                              ngx_http_upstream_check_module);

        uclcf->format = ngx_http_get_check_status_format_conf(&value[1]);
        if (uclcf->format == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid check format \"%V\"", &value[1]);

            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_upstream_check_main_conf_t));
}


static ngx_buf_t *
ngx_http_upstream_check_create_fastcgi_request(ngx_pool_t *pool,
    ngx_str_t *params, ngx_uint_t num)
{
    size_t                      size, len, padding;
    ngx_buf_t                  *b;
    ngx_str_t                  *k, *v;
    ngx_uint_t                  i, j;
    ngx_http_fastcgi_header_t  *h;

    len = 0;
    for (i = 0, j = 0; i < num; i++, j = i * 2) {
        k = &params[j];
        v = &params[j + 1];

        len += 1 + k->len + ((v->len > 127) ? 4 : 1) + v->len;
    }

    padding = 8 - len % 8;
    padding = (padding == 8) ? 0 : padding;

    size = sizeof(ngx_http_fastcgi_header_t)
        + sizeof(ngx_http_fastcgi_begin_request_t)

        + sizeof(ngx_http_fastcgi_header_t)  /* NGX_HTTP_FASTCGI_PARAMS */
        + len + padding
        + sizeof(ngx_http_fastcgi_header_t)  /* NGX_HTTP_FASTCGI_PARAMS */

        + sizeof(ngx_http_fastcgi_header_t); /* NGX_HTTP_FASTCGI_STDIN */


    b = ngx_create_temp_buf(pool, size);
    if (b == NULL) {
        return NULL;
    }

    ngx_http_fastcgi_request_start.br.flags = 0;

    ngx_memcpy(b->pos, &ngx_http_fastcgi_request_start,
               sizeof(ngx_http_fastcgi_request_start_t));

    h = (ngx_http_fastcgi_header_t *)
        (b->pos + sizeof(ngx_http_fastcgi_header_t)
         + sizeof(ngx_http_fastcgi_begin_request_t));

    h->content_length_hi = (u_char) ((len >> 8) & 0xff);
    h->content_length_lo = (u_char) (len & 0xff);
    h->padding_length = (u_char) padding;
    h->reserved = 0;

    b->last = b->pos + sizeof(ngx_http_fastcgi_header_t)
        + sizeof(ngx_http_fastcgi_begin_request_t)
        + sizeof(ngx_http_fastcgi_header_t);

    for (i = 0, j = 0; i < num; i++, j = i * 2) {
        k = &params[j];
        v = &params[j + 1];

        if (k->len > 127) {
            *b->last++ = (u_char) (((k->len >> 24) & 0x7f) | 0x80);
            *b->last++ = (u_char) ((k->len >> 16) & 0xff);
            *b->last++ = (u_char) ((k->len >> 8) & 0xff);
            *b->last++ = (u_char) (k->len & 0xff);

        } else {
            *b->last++ = (u_char) k->len;
        }

        if (v->len > 127) {
            *b->last++ = (u_char) (((v->len >> 24) & 0x7f) | 0x80);
            *b->last++ = (u_char) ((v->len >> 16) & 0xff);
            *b->last++ = (u_char) ((v->len >> 8) & 0xff);
            *b->last++ = (u_char) (v->len & 0xff);

        } else {
            *b->last++ = (u_char) v->len;
        }

        b->last = ngx_copy(b->last, k->data, k->len);
        b->last = ngx_copy(b->last, v->data, v->len);
    }

    if (padding) {
        ngx_memzero(b->last, padding);
        b->last += padding;
    }

    h = (ngx_http_fastcgi_header_t *) b->last;
    b->last += sizeof(ngx_http_fastcgi_header_t);

    h->version = 1;
    h->type = NGX_HTTP_FASTCGI_PARAMS;
    h->request_id_hi = 0;
    h->request_id_lo = 1;
    h->content_length_hi = 0;
    h->content_length_lo = 0;
    h->padding_length = 0;
    h->reserved = 0;

    h = (ngx_http_fastcgi_header_t *) b->last;
    b->last += sizeof(ngx_http_fastcgi_header_t);

    return b;
}


static char *
ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_buf_t                      *b;
    ngx_uint_t                      i;
    ngx_http_upstream_srv_conf_t  **uscfp;
    ngx_http_upstream_main_conf_t  *umcf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);

    b = ngx_http_upstream_check_create_fastcgi_request(cf->pool,
            fastcgi_default_params,
            sizeof(fastcgi_default_params) / sizeof(ngx_str_t) / 2);

    if (b == NULL) {
        return NGX_CONF_ERROR;
    }

    fastcgi_default_request.data = b->pos;
    fastcgi_default_request.len = b->last - b->pos;

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->srv_conf == NULL) {
            continue;
        }

        if (ngx_upstream_check_init_srv_conf(cf,
                ngx_http_conf_upstream_srv_conf(uscfp[i],
                                                ngx_http_upstream_check_module))
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

#if (NGX_HTTP_UPSTREAM_CHECK)
    if (ngx_upstream_check_init_shm(cf, conf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
#else
    /*
     * The upstream peers may not be initialized yet, so they are registered
     * and the shared memory is added in the postconfiguration handler.
     */
    return NGX_CONF_OK;
#endif
}


static void *
ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf)
{
    return ngx_upstream_check_create_srv_conf(cf, NGX_UPSTREAM_CHECK_HTTP);
}


void *
ngx_upstream_check_create_srv_conf(ngx_conf_t *cf, ngx_uint_t type)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ucscf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_srv_conf_t));
    if (ucscf == NULL) {
        return NULL;
    }

    ucscf->fastcgi_params = ngx_array_create(cf->pool, 2 * 4, sizeof(ngx_str_t));
    if (ucscf->fastcgi_params == NULL) {
        return NULL;
    }

    ucscf->port = NGX_CONF_UNSET_UINT;
    ucscf->fall_count = NGX_CONF_UNSET_UINT;
    ucscf->rise_count = NGX_CONF_UNSET_UINT;
    ucscf->check_timeout = NGX_CONF_UNSET_MSEC;
    ucscf->check_keepalive_requests = NGX_CONF_UNSET_UINT;
    ucscf->check_type_conf = NGX_CONF_UNSET_PTR;
    ucscf->upstream_type = type;

    return ucscf;
}


static void *
ngx_http_upstream_check_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_loc_conf_t  *uclcf;

    uclcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_loc_conf_t));
    if (uclcf == NULL) {
        return NULL;
    }

    uclcf->format = NGX_CONF_UNSET_PTR;

    return uclcf;
}


ngx_uint_t
ngx_upstream_check_enabled(ngx_http_upstream_check_srv_conf_t *ucscf)
{
    return ucscf->check_interval != 0;
}


char *
ngx_upstream_check_init_srv_conf(ngx_conf_t *cf,
    ngx_http_upstream_check_srv_conf_t *ucscf)
{
    ngx_str_t                           s;
    ngx_buf_t                          *b;
    ngx_check_conf_t                   *check;

    if (ucscf->port == NGX_CONF_UNSET_UINT) {
        ucscf->port = 0;
    }

    if (ucscf->fall_count == NGX_CONF_UNSET_UINT) {
        ucscf->fall_count = 2;
    }

    if (ucscf->rise_count == NGX_CONF_UNSET_UINT) {
        ucscf->rise_count = 5;
    }

    if (ucscf->check_interval == NGX_CONF_UNSET_MSEC) {
        ucscf->check_interval = 0;
    }

    if (ucscf->check_timeout == NGX_CONF_UNSET_MSEC) {
        ucscf->check_timeout = 1000;
    }

    if (ucscf->check_keepalive_requests == NGX_CONF_UNSET_UINT) {
        ucscf->check_keepalive_requests = 1;
    }

    if (ucscf->check_type_conf == NGX_CONF_UNSET_PTR) {
        ucscf->check_type_conf = NULL;
    }

    check = ucscf->check_type_conf;

    if (check) {
        if (ucscf->send.len == 0) {
            ngx_str_set(&s, "fastcgi");

            if (check == ngx_http_get_check_type_conf(&s)) {

                if (ucscf->fastcgi_params->nelts == 0) {
                    ucscf->send.data = fastcgi_default_request.data;
                    ucscf->send.len = fastcgi_default_request.len;

                } else {
                    b = ngx_http_upstream_check_create_fastcgi_request(
                            cf->pool, ucscf->fastcgi_params->elts,
                            ucscf->fastcgi_params->nelts / 2);
                    if (b == NULL) {
                        return NGX_CONF_ERROR;
                    }

                    ucscf->send.data = b->pos;
                    ucscf->send.len = b->last - b->pos;
                }
            } else {
                ucscf->send.data = check->default_send.data;
                ucscf->send.len = check->default_send.len;
            }
        }


        if (ucscf->code.status_alive == 0) {
            ucscf->code.status_alive = check->default_status_alive;
        }
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_str_t                            format = ngx_string("html");
    ngx_http_upstream_check_loc_conf_t  *prev = parent;
    ngx_http_upstream_check_loc_conf_t  *conf = child;

    ngx_conf_merge_ptr_value(conf->format, prev->format,
                             ngx_http_get_check_status_format_conf(&format));

    return NGX_CONF_OK;
}


/*
 * Called at the end of the http{} and of the stream{} blocks: the first one
 * with checks adds the shared memory zone, which serves both.
 */

ngx_int_t
ngx_upstream_check_init_shm(ngx_conf_t *cf,
    ngx_upstream_check_main_conf_t *ucmcf)
{
    ngx_str_t                        *shm_name;
    ngx_uint_t                        shm_size;
    ngx_shm_zone_t                   *shm_zone;
    ngx_http_upstream_check_peers_t  *peers;

    peers = check_peers_conf;

    if (peers == NULL || peers->cycle != cf->cycle) {
        return NGX_OK;
    }

#if (NGX_HTTP_UPSTREAM_CHECK)
    if (peers->peers.nelts == 0) {
#else
    if (peers->peers.nelts == 0 && peers->upstreams.nelts == 0) {
#endif
        return NGX_OK;
    }

    /* The default check shared memory size is 1M */
    shm_size = 1 * 1024 * 1024;

    shm_size = shm_size < ucmcf->check_shm_size ?
                          ucmcf->check_shm_size : shm_size;

    if (peers->shm_zone) {

        /* the zone is only created once the configuration is parsed */

        if (peers->shm_zone->shm.size < shm_size) {
            peers->shm_zone->shm.size = shm_size;
        }

        return NGX_OK;
    }

    ngx_http_upstream_check_shm_generation++;

    shm_name = &peers->check_shm_name;

    if (ngx_http_upstream_check_get_shm_name(shm_name, cf->pool,
                                    ngx_http_upstream_check_shm_generation)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    shm_zone = ngx_shared_memory_add(cf, shm_name, shm_size,
                                     &ngx_http_upstream_check_module);
    if (shm_zone == NULL) {
        return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, cf->log, 0,
                   "http upstream check, upsteam:%V, shm_zone size:%ui",
                   shm_name, shm_size);

    shm_zone->data = cf->pool;
    peers->generation = ngx_http_upstream_check_shm_generation;
    peers->shm_zone = shm_zone;

    shm_zone->init = ngx_http_upstream_check_init_shm_zone;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_get_shm_name(ngx_str_t *shm_name, ngx_pool_t *pool,
    ngx_uint_t generation)
{
    u_char  *last;

    shm_name->data = ngx_palloc(pool, SHM_NAME_LEN);
    if (shm_name->data == NULL) {
        return NGX_ERROR;
    }

    last = ngx_snprintf(shm_name->data, SHM_NAME_LEN, "%s#%ui",
                        "ngx_http_upstream_check", generation);

    shm_name->len = last - shm_name->data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t                               size;
    ngx_int_t                            rc;
    ngx_uint_t                           i, same, number, total;
    ngx_pool_t                          *pool;
    ngx_shm_zone_t                      *oshm_zone;
    ngx_slab_pool_t                     *shpool;
    ngx_http_upstream_check_peer_t      *peer;
    ngx_http_upstream_check_peers_t     *peers;
    ngx_http_upstream_check_srv_conf_t  *ucscf;
    ngx_http_upstream_check_peer_shm_t  *peer_shm, *opeer_shm;
    ngx_http_upstream_check_peers_shm_t *peers_shm, *opeers_shm;

    opeers_shm = NULL;
    peers_shm = NULL;

    same = 0;
    peers = check_peers_conf;
    if (peers == NULL) {
        return NGX_OK;
    }

    number = peers->peers.nelts;
    total = number;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    /*
     * Half of the zone is reserved for the peers which show up at run
     * time, the rest is left for the configured peers.
     */
    peers->dyn_number = 0;

    if (peers->upstreams.nelts > 0) {
        peers->dyn_number =
            shm_zone->shm.size / 2 / sizeof(ngx_http_upstream_check_peer_shm_t);
    }

    total += peers->dyn_number;
#endif

    if (total == 0) {
        return NGX_OK;
    }

    pool = shm_zone->data;
    if (pool == NULL) {
        pool = ngx_cycle->pool;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (data) {
        opeers_shm = data;

        if ((opeers_shm->number == number)
#if !(NGX_HTTP_UPSTREAM_CHECK)
            && (opeers_shm->dyn_number == peers->dyn_number)
#endif
            && (opeers_shm->checksum == peers->checksum)) {

            peers_shm = data;
            same = 1;
        }
    }

    if (!same) {

        if (ngx_http_upstream_check_shm_generation > 1) {

            /*
             * The global variable ngx_cycle still points to the old one.
             * Look the old zone up by tag rather than by generation
             * number: a reload that failed after parsing still bumped
             * the generation, so "generation - 1" may not be the zone
             * the running cycle is actually using.
             */
            oshm_zone = ngx_http_upstream_check_find_shm_zone(
                            (ngx_cycle_t *) ngx_cycle,
                            &ngx_http_upstream_check_module);

            if (oshm_zone) {
                opeers_shm = oshm_zone->data;

                ngx_log_debug2(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                               "http upstream check, find oshm_zone:%p, "
                               "opeers_shm: %p",
                               oshm_zone, opeers_shm);
            }
        }

        size = sizeof(*peers_shm) +
               (total - 1) * sizeof(ngx_http_upstream_check_peer_shm_t);

        peers_shm = ngx_slab_alloc(shpool, size);

        if (peers_shm == NULL) {
            goto failure;
        }

        ngx_memzero(peers_shm, size);
    }

    peers_shm->generation = peers->generation;
    peers_shm->checksum = peers->checksum;
    peers_shm->number = number;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    peers_shm->dyn_number = peers->dyn_number;
    peers->shpool = shpool;

    for (i = number; i < total; i++) {
        peer_shm = &peers_shm->peers[i];

        peer_shm->owner = NGX_INVALID_PID;
        peer_shm->keepalive_owner = NGX_INVALID_PID;

        if (same) {
            continue;
        }

        peer_shm->sockaddr = (struct sockaddr *) peer_shm->dyn_sockaddr;

        rc = ngx_http_upstream_check_init_shm_peer(peer_shm, NULL, 0, pool,
                                                   &peers->check_shm_name);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }
    }
#endif

    ngx_http_upstream_check_share_peers(peers);

    peer = peers->peers.elts;

    for (i = 0; i < number; i++) {

        peer_shm = &peers_shm->peers[i];

        /*
         * Set it here rather than in init_process, so that it is valid
         * in every process type, including "master_process off".
         * A shared peer uses the slot of the peer it shares the check with,
         * which comes earlier, so its shm is already set.
         */
        peer[i].shm = peer[i].shared ? peer[peer[i].shared_index].shm
                                     : peer_shm;

        /*
         * This function may be triggered before the old stale
         * work process exits. The owner may stick to the old
         * pid.
         */
        peer_shm->owner = NGX_INVALID_PID;
        peer_shm->keepalive_owner = NGX_INVALID_PID;

        if (same) {
            continue;
        }

        peer_shm->socklen = peer[i].peer_addr->socklen;
        peer_shm->sockaddr = ngx_slab_alloc(shpool, peer_shm->socklen);
        if (peer_shm->sockaddr == NULL) {
            goto failure;
        }

        ngx_memcpy(peer_shm->sockaddr, peer[i].peer_addr->sockaddr,
                   peer_shm->socklen);

        if (opeers_shm) {

            opeer_shm = ngx_http_upstream_check_find_shm_peer(opeers_shm,
                                                              peer[i].peer_addr);
            if (opeer_shm) {
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
                               "http upstream check, inherit opeer: %V ",
                               &peer[i].peer_addr->name);

                rc = ngx_http_upstream_check_init_shm_peer(peer_shm, opeer_shm,
                         0, pool, &peer[i].peer_addr->name);
                if (rc != NGX_OK) {
                    return NGX_ERROR;
                }

                continue;
            }
        }

        ucscf = peer[i].conf;
        rc = ngx_http_upstream_check_init_shm_peer(peer_shm, NULL,
                                                   ucscf->default_down, pool,
                                                   &peer[i].peer_addr->name);
        if (rc != NGX_OK) {
            return NGX_ERROR;
        }
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    /* keep the state of the dynamic peers of the running configuration */

    if (!same && opeers_shm && check_peers_ctx
        && check_peers_ctx->peers_shm == opeers_shm)
    {
        ngx_http_upstream_check_dyn_inherit(peers, peers_shm, check_peers_ctx,
                                            opeers_shm);
    }
#endif

    peers->peers_shm = peers_shm;
    shm_zone->data = peers_shm;

    return NGX_OK;

failure:
    ngx_log_error(NGX_LOG_EMERG, shm_zone->shm.log, 0,
                  "http upstream check_shm_size is too small, "
                  "you should specify a larger size.");
    return NGX_ERROR;
}


static ngx_shm_zone_t *
ngx_http_upstream_check_find_shm_zone(ngx_cycle_t *cycle, void *tag)
{
    ngx_uint_t        i;
    ngx_shm_zone_t   *shm_zone;
    ngx_list_part_t  *part;

    part = (ngx_list_part_t *) &(cycle->shared_memory.part);
    shm_zone = part->elts;

    for (i = 0; /* void */ ; i++) {

        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            shm_zone = part->elts;
            i = 0;
        }

        if (tag != shm_zone[i].tag || shm_zone[i].data == NULL) {
            continue;
        }

        return &shm_zone[i];
    }

    return NULL;
}


static ngx_http_upstream_check_peer_shm_t *
ngx_http_upstream_check_find_shm_peer(ngx_http_upstream_check_peers_shm_t *p,
    ngx_addr_t *addr)
{
    ngx_uint_t                          i, n;
    ngx_http_upstream_check_peer_shm_t *peer_shm;

    n = p->number;

#if !(NGX_HTTP_UPSTREAM_CHECK)
    n += p->dyn_number;
#endif

    for (i = 0; i < n; i++) {

        peer_shm = &p->peers[i];

#if !(NGX_HTTP_UPSTREAM_CHECK)
        if (i >= p->number && !peer_shm->dyn_used) {
            continue;
        }
#endif

        if (addr->socklen != peer_shm->socklen) {
            continue;
        }

        if (ngx_memcmp(addr->sockaddr, peer_shm->sockaddr, addr->socklen) == 0) {
            return peer_shm;
        }
    }

    return NULL;
}


static ngx_int_t
ngx_http_upstream_check_init_shm_peer(ngx_http_upstream_check_peer_shm_t *psh,
    ngx_http_upstream_check_peer_shm_t *opsh, ngx_uint_t init_down,
    ngx_pool_t *pool, ngx_str_t *name)
{
    u_char  *file;

    if (opsh) {
        psh->access_time  = opsh->access_time;
        psh->access_count = opsh->access_count;

        psh->fall_count   = opsh->fall_count;
        psh->rise_count   = opsh->rise_count;
        psh->busyness     = opsh->busyness;

        psh->down         = opsh->down;

    } else {
        psh->access_time  = 0;
        psh->access_count = 0;

        psh->fall_count   = 0;
        psh->rise_count   = 0;
        psh->busyness     = 0;

        psh->down         = init_down;
    }

#if (NGX_HAVE_ATOMIC_OPS)

    file = NULL;

#else

    file = ngx_pnalloc(pool, ngx_cycle->lock_file.len + name->len);
    if (file == NULL) {
        return NGX_ERROR;
    }

    (void) ngx_sprintf(file, "%V%V%Z", &ngx_cycle->lock_file, name);

#endif

#if (nginx_version >= 1002000)
    if (ngx_shmtx_create(&psh->mutex, &psh->lock, file) != NGX_OK) {
#else
    if (ngx_shmtx_create(&psh->mutex, (void *) &psh->lock, file) != NGX_OK) {
#endif
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_module(ngx_cycle_t *cycle)
{
    ngx_http_upstream_check_peers_t     *peers;

    /*
     * The new cycle is committed at this point.  Switch to its peers only
     * now, so that a reload which fails after parsing leaves the running
     * peers in place for the workers the master may still respawn.
     *
     * The peers of both the http and the stream upstreams are there, and
     * this is called whether there is an http{} block or not.
     */

    peers = check_peers_conf;

    if (peers != NULL
        && (peers->cycle != cycle || peers->shm_zone == NULL))
    {
        peers = NULL;
    }

    /*
     * With "master_process off", nginx reloads in the same process and
     * does not call init_process again.  Stop the old cycle's checks: its
     * shared memory is about to be freed.
     */
    if (ngx_process == NGX_PROCESS_SINGLE
        && check_peers_ctx != NULL && check_peers_ctx != peers)
    {
        ngx_http_upstream_check_clear_peers_events(check_peers_ctx);
    }

    check_peers_ctx = peers;
    check_peers_conf = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_process(ngx_cycle_t *cycle)
{
    if (ngx_process != NGX_PROCESS_WORKER
        && ngx_process != NGX_PROCESS_SINGLE)
    {
        return NGX_OK;
    }

    if (ngx_http_upstream_check_add_timers(cycle) != NGX_OK) {
        return NGX_ERROR;
    }

#if !(NGX_HTTP_UPSTREAM_CHECK)
    return ngx_http_upstream_check_init_sync(cycle);
#else
    return NGX_OK;
#endif
}


#if !(NGX_HTTP_UPSTREAM_CHECK)

static ngx_int_t
ngx_http_upstream_check_postconfiguration(ngx_conf_t *cf)
{
    ngx_uint_t                       i;
    ngx_http_upstream_srv_conf_t   **uscfp;
    ngx_http_upstream_main_conf_t   *umcf;
    ngx_upstream_check_main_conf_t  *ucmcf;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    ucmcf = ngx_http_conf_get_module_main_conf(cf,
                                               ngx_http_upstream_check_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (ngx_http_upstream_check_add_upstream(cf, uscfp[i]) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return ngx_upstream_check_init_shm(cf, ucmcf);
}


static ngx_int_t
ngx_http_upstream_check_add_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_addr_t                          *addr;
    ngx_http_upstream_rr_peer_t         *peer;
    ngx_http_upstream_rr_peers_t        *peers, *list;
    ngx_upstream_check_upstream_t       *cu;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    if (us->srv_conf == NULL) {
        return NGX_OK;
    }

    ucscf = ngx_http_conf_upstream_srv_conf(us, ngx_http_upstream_check_module);

    if (ucscf->check_interval == 0) {
        return NGX_OK;
    }

    /*
     * All the built-in balancers (round robin, hash, ip_hash, least_conn,
     * random) keep the round robin peers in us->peer.data, and those always
     * point back to the upstream name.
     */

    peers = us->peer.data;

    if (peers == NULL || peers->name != &us->host) {
        ngx_log_error(NGX_LOG_WARN, cf->log, 0,
                      "upstream check is ignored for upstream \"%V\" "
                      "in %s:%ui: its load balancing method is not "
                      "supported without the check patch",
                      &us->host, us->file_name, us->line);
        return NGX_OK;
    }

    cu = ngx_upstream_check_add_upstream(cf, NGX_UPSTREAM_CHECK_HTTP,
                                         &us->host, us, ucscf,
                                         ngx_http_upstream_check_sync);
    if (cu == NULL) {
        return NGX_ERROR;
    }

    /* primary peers, then backup peers */

    for (list = peers; list; list = list->next) {

        /*
         * Servers resolved at run time ("resolve") are not in this list
         * yet, they are added as dynamic peers by the sync timer.
         */

        for (peer = list->peer; peer; peer = peer->next) {

            if (peer->down) {
                continue;
            }

            addr = ngx_palloc(cf->pool, sizeof(ngx_addr_t));
            if (addr == NULL) {
                return NGX_ERROR;
            }

            addr->sockaddr = peer->sockaddr;
            addr->socklen = peer->socklen;
            addr->name = peer->name;

            if (ngx_upstream_check_add_peer(cf, cu, addr) != NGX_OK) {
                return NGX_ERROR;
            }
        }
    }

    return NGX_OK;
}


static void
ngx_http_upstream_check_sync(ngx_upstream_check_upstream_t *cu)
{
    ngx_http_upstream_srv_conf_t  *us;
    ngx_http_upstream_rr_peers_t  *peers;

    us = cu->upstream;

    /*
     * With "zone", us->peer.data points to the copy of the peers in the
     * shared memory, which all the workers update.
     */

    for (peers = us->peer.data; peers; peers = peers->next) {

        ngx_http_upstream_rr_peers_rlock(peers);

        if (ngx_http_upstream_check_sync_list(cu, peers, 0) == 0) {
            ngx_http_upstream_rr_peers_unlock(peers);
            continue;
        }

        ngx_http_upstream_rr_peers_unlock(peers);

        ngx_http_upstream_rr_peers_wlock(peers);

        (void) ngx_http_upstream_check_sync_list(cu, peers, 1);

        ngx_http_upstream_rr_peers_unlock(peers);
    }
}


static ngx_uint_t
ngx_http_upstream_check_sync_list(ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_rr_peers_t *peers, ngx_uint_t update)
{
    ngx_uint_t                    down, changed;
    ngx_upstream_check_sync_t     s;
    ngx_http_upstream_rr_peer_t  *peer;

    ngx_upstream_check_sync_init(&s, cu, !update);

    changed = 0;

    for (peer = peers->peer; peer; peer = peer->next) {

        /*
         * Leave the servers marked "down" in the configuration, or by
         * anything else, alone.
         */

        if (peer->down != 0 && peer->down != NGX_UPSTREAM_CHECK_PEER_DOWN) {
            continue;
        }

        down = ngx_upstream_check_sync_peer(&s, peer->sockaddr, peer->socklen,
                                            &peer->name);

        if (peer->down == down) {
            continue;
        }

        changed++;

        if (update) {
            ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                           "http check sync peer %V in upstream %V: %s",
                           &peer->name, cu->name, down ? "down" : "up");

            peer->down = down;
        }
    }

    return changed;
}


/* the part of the configuration-time setup shared by http and stream */

ngx_upstream_check_upstream_t *
ngx_upstream_check_add_upstream(ngx_conf_t *cf, ngx_uint_t type,
    ngx_str_t *name, void *upstream, ngx_http_upstream_check_srv_conf_t *ucscf,
    ngx_upstream_check_sync_pt sync)
{
    ngx_msec_t                        interval;
    ngx_upstream_check_upstream_t    *cu;
    ngx_http_upstream_check_peers_t  *peers;

    peers = ngx_http_upstream_check_conf_peers(cf);
    if (peers == NULL) {
        return NULL;
    }

    cu = ngx_array_push(&peers->upstreams);
    if (cu == NULL) {
        return NULL;
    }

    ngx_memzero(cu, sizeof(ngx_upstream_check_upstream_t));

    cu->type = type;
    cu->name = name;
    cu->upstream = upstream;
    cu->conf = ucscf;
    cu->sync = sync;
    cu->index = peers->upstreams.nelts - 1;
    cu->first = peers->peers.nelts;

    interval = ucscf->check_interval / 2;

    if (interval < 100) {
        interval = 100;
    }

    if (interval < peers->sync_interval) {
        peers->sync_interval = interval;
    }

    return cu;
}


ngx_int_t
ngx_upstream_check_add_peer(ngx_conf_t *cf, ngx_upstream_check_upstream_t *cu,
    ngx_addr_t *addr)
{
    if (ngx_http_upstream_check_new_peer(cf, cu->conf, cu->name, addr)
        == NGX_ERROR)
    {
        return NGX_ERROR;
    }

    /* the peers of an upstream are added one after another */

    cu->number++;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstream_check_init_sync(ngx_cycle_t *cycle)
{
    ngx_event_t  *ev;

    if (check_peers_ctx == NULL
        || check_peers_ctx->peers_shm == NULL
        || check_peers_ctx->upstreams.nelts == 0)
    {
        return NGX_OK;
    }

    ngx_http_upstream_check_sync_peers();

    ev = &ngx_http_upstream_check_sync_event;

    ev->handler = ngx_http_upstream_check_sync_handler;
    ev->log = cycle->log;
    ev->data = check_peers_ctx;

    ngx_add_timer(ev, check_peers_ctx->sync_interval);

    return NGX_OK;
}


static void
ngx_http_upstream_check_sync_handler(ngx_event_t *event)
{
    if (ngx_http_upstream_check_need_exit()) {
        return;
    }

    /* with "master_process off" a reload may leave no peers to check */

    if (check_peers_ctx == NULL || check_peers_ctx->upstreams.nelts == 0) {
        return;
    }

    ngx_http_upstream_check_sync_peers();

    ngx_add_timer(event, check_peers_ctx->sync_interval);
}


static void
ngx_http_upstream_check_sync_peers(void)
{
    ngx_uint_t                      i, j;
    ngx_upstream_check_upstream_t  *cu;

    if (check_peers_ctx == NULL || check_peers_ctx->peers_shm == NULL) {
        return;
    }

    check_peers_ctx->sync_pass++;

    cu = check_peers_ctx->upstreams.elts;

    for (i = 0; i < check_peers_ctx->upstreams.nelts; i++) {

        ngx_http_upstream_check_nnew = 0;

        cu[i].sync(&cu[i]);

        /*
         * The peers added at run time (resolve, upsync, ...) get their check
         * peers outside of the peers lock, and are marked on the next pass.
         */

        for (j = 0; j < ngx_http_upstream_check_nnew; j++) {
            ngx_http_upstream_check_dyn_add(&cu[i],
                                        &ngx_http_upstream_check_new_peers[j]);
        }

        ngx_http_upstream_check_dyn_expire(&cu[i]);
    }

    ngx_http_upstream_check_dyn_gc();
}


void
ngx_upstream_check_sync_init(ngx_upstream_check_sync_t *s,
    ngx_upstream_check_upstream_t *cu, ngx_uint_t collect)
{
    s->cu = cu;
    s->hint = cu->first;
    s->dhint = 0;
    s->collect = collect;
}


ngx_uint_t
ngx_upstream_check_sync_peer(ngx_upstream_check_sync_t *s,
    struct sockaddr *sockaddr, socklen_t socklen, ngx_str_t *name)
{
    ngx_int_t                            index;
    ngx_http_upstream_check_peer_t      *cpeer;
    ngx_upstream_check_dyn_peer_t       *dyn;
    ngx_http_upstream_check_new_peer_t  *np;

    /*
     * The peers are matched by address on every pass, since with
     * "resolve" or upsync the peers may be recreated at run time.
     */

    cpeer = check_peers_ctx->peers.elts;

    index = ngx_http_upstream_check_find_rr_peer(s->cu, sockaddr, socklen,
                                                 s->hint);

    if (index != NGX_ERROR) {
        s->hint = index + 1;

        return cpeer[index].shm->down ? NGX_UPSTREAM_CHECK_PEER_DOWN : 0;
    }

    dyn = ngx_http_upstream_check_find_dyn_peer(s->cu, sockaddr, socklen,
                                                &s->dhint);

    if (dyn) {
        dyn->pass = check_peers_ctx->sync_pass;

        if (dyn->peer.shm->dyn_id == dyn->peer.dyn_id
            && dyn->peer.shm->down)
        {
            return NGX_UPSTREAM_CHECK_PEER_DOWN;
        }

        return 0;
    }

    if (s->collect
        && ngx_http_upstream_check_nnew < NGX_HTTP_UPSTREAM_CHECK_NEW_PEERS
        && socklen <= NGX_SOCKADDRLEN)
    {
        np = &ngx_http_upstream_check_new_peers[ngx_http_upstream_check_nnew++];

        np->socklen = socklen;
        ngx_memcpy(np->sockaddr, sockaddr, socklen);

        np->name_len = ngx_min(name->len, NGX_SOCKADDR_STRLEN);
        ngx_memcpy(np->name, name->data, np->name_len);
    }

    return 0;
}


static ngx_int_t
ngx_http_upstream_check_find_rr_peer(ngx_upstream_check_upstream_t *cu,
    struct sockaddr *sockaddr, socklen_t socklen, ngx_uint_t hint)
{
    ngx_uint_t                       i, last;
    ngx_addr_t                      *addr;
    ngx_http_upstream_check_peer_t  *cpeer;

    cpeer = check_peers_ctx->peers.elts;
    last = cu->first + cu->number;

    /* the peers are usually in the order they were registered in */

    if (hint < last) {
        addr = cpeer[hint].peer_addr;

        if (ngx_http_upstream_check_cmp_sockaddr(addr->sockaddr, addr->socklen,
                                                 sockaddr, socklen)
            == NGX_OK)
        {
            return hint;
        }
    }

    for (i = cu->first; i < last; i++) {
        addr = cpeer[i].peer_addr;

        if (ngx_http_upstream_check_cmp_sockaddr(addr->sockaddr, addr->socklen,
                                                 sockaddr, socklen)
            == NGX_OK)
        {
            return i;
        }
    }

    return NGX_ERROR;
}


static ngx_upstream_check_dyn_peer_t *
ngx_http_upstream_check_find_dyn_peer(ngx_upstream_check_upstream_t *cu,
    struct sockaddr *sockaddr, socklen_t socklen, ngx_uint_t *hint)
{
    ngx_uint_t                           i;
    ngx_upstream_check_dyn_peer_t       *dyn;

    if (*hint < cu->ndyn) {
        dyn = cu->dyn[*hint];

        if (ngx_http_upstream_check_cmp_sockaddr(dyn->addr.sockaddr,
                                                 dyn->addr.socklen,
                                                 sockaddr, socklen)
            == NGX_OK)
        {
            (*hint)++;
            return dyn;
        }
    }

    for (i = 0; i < cu->ndyn; i++) {
        dyn = cu->dyn[i];

        if (ngx_http_upstream_check_cmp_sockaddr(dyn->addr.sockaddr,
                                                 dyn->addr.socklen,
                                                 sockaddr, socklen)
            == NGX_OK)
        {
            *hint = i + 1;
            return dyn;
        }
    }

    return NULL;
}


/*
 * A peer added at run time has a slot in the shared memory, shared by all
 * the workers (so a single worker checks it, as for the configured peers),
 * and a check peer in every worker which sees it in the upstream.
 */

static void
ngx_http_upstream_check_dyn_add(ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_check_new_peer_t *np)
{
    ngx_int_t                            slot;
    ngx_uint_t                           id, hint, n;
    ngx_msec_t                           delay;
    ngx_pool_t                          *pool;
    ngx_http_upstream_check_peer_t      *peer;
    ngx_upstream_check_dyn_peer_t       *dyn, **dynp;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    /* the same address may be both in the primary and the backup peers */

    hint = 0;

    if (ngx_http_upstream_check_find_dyn_peer(cu,
                                              (struct sockaddr *) np->sockaddr,
                                              np->socklen, &hint))
    {
        return;
    }

    if (cu->ndyn == cu->nalloc) {
        n = cu->nalloc ? cu->nalloc * 2 : 8;

        dynp = ngx_alloc(n * sizeof(ngx_upstream_check_dyn_peer_t *),
                         ngx_cycle->log);
        if (dynp == NULL) {
            return;
        }

        if (cu->dyn) {
            ngx_memcpy(dynp, cu->dyn,
                       cu->ndyn * sizeof(ngx_upstream_check_dyn_peer_t *));
            ngx_free(cu->dyn);
        }

        cu->dyn = dynp;
        cu->nalloc = n;
    }

    slot = ngx_http_upstream_check_dyn_alloc(cu, np, &id);
    if (slot == NGX_ERROR) {
        return;
    }

    ucscf = cu->conf;

    pool = ngx_create_pool(1024, ngx_cycle->log);
    if (pool == NULL) {
        return;
    }

    dyn = ngx_pcalloc(pool, sizeof(ngx_upstream_check_dyn_peer_t));
    if (dyn == NULL) {
        goto failed;
    }

    dyn->pool = pool;

    dyn->addr.sockaddr = ngx_palloc(pool, np->socklen);
    dyn->addr.name.data = ngx_pnalloc(pool, np->name_len);
    if (dyn->addr.sockaddr == NULL || dyn->addr.name.data == NULL) {
        goto failed;
    }

    ngx_memcpy(dyn->addr.sockaddr, np->sockaddr, np->socklen);
    dyn->addr.socklen = np->socklen;
    ngx_memcpy(dyn->addr.name.data, np->name, np->name_len);
    dyn->addr.name.len = np->name_len;

    peer = &dyn->peer;

    peer->index = slot;
    peer->conf = ucscf;
    peer->upstream_name = cu->name;
    peer->peer_addr = &dyn->addr;
    peer->shm = &check_peers_ctx->peers_shm->peers[slot];
    peer->dyn_id = id;

    if (ucscf->port) {
        peer->check_peer_addr = ngx_pcalloc(pool, sizeof(ngx_addr_t));
        if (peer->check_peer_addr == NULL) {
            goto failed;
        }

        if (ngx_http_upstream_check_addr_change_port(pool,
                peer->check_peer_addr, &dyn->addr, ucscf->port)
            != NGX_OK)
        {
            goto failed;
        }

    } else {
        peer->check_peer_addr = &dyn->addr;
    }

    if (ngx_http_upstream_check_init_peer(peer, ngx_cycle->log) != NGX_OK) {
        goto failed;
    }

    dyn->pass = check_peers_ctx->sync_pass;
    dyn->seen = ngx_time();

    cu->dyn[cu->ndyn++] = dyn;

    delay = ucscf->check_interval < 1000 ? ucscf->check_interval : 1000;
    ngx_add_timer(&peer->check_ev, delay ? ngx_random() % delay : 0);

    ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                  "add dynamic check peer %V in upstream %V",
                  &dyn->addr.name, cu->name);

    return;

failed:

    if (dyn && dyn->peer.pool) {
        ngx_destroy_pool(dyn->peer.pool);
    }

    ngx_destroy_pool(pool);
}


static ngx_int_t
ngx_http_upstream_check_dyn_alloc(ngx_upstream_check_upstream_t *cu,
    ngx_http_upstream_check_new_peer_t *np, ngx_uint_t *id)
{
    ngx_int_t                             slot;
    ngx_uint_t                            i, n;
    ngx_slab_pool_t                      *shpool;
    ngx_http_upstream_check_peer_shm_t   *psh;
    ngx_http_upstream_check_srv_conf_t   *ucscf;
    ngx_http_upstream_check_peers_shm_t  *peers_shm;

    static time_t                         logged;

    peers_shm = check_peers_ctx->peers_shm;
    shpool = check_peers_ctx->shpool;

    if (shpool == NULL || peers_shm->dyn_number == 0) {
        return NGX_ERROR;
    }

    n = peers_shm->number + peers_shm->dyn_number;
    slot = NGX_ERROR;

    ngx_shmtx_lock(&shpool->mutex);

    for (i = peers_shm->number; i < n; i++) {
        psh = &peers_shm->peers[i];

        if (!psh->dyn_used) {
            if (slot == NGX_ERROR) {
                slot = i;
            }

            continue;
        }

        /* another worker already added it */

        if (psh->dyn_upstream == cu->index
            && ngx_http_upstream_check_cmp_sockaddr(psh->sockaddr,
                   psh->socklen, (struct sockaddr *) np->sockaddr,
                   np->socklen)
               == NGX_OK)
        {
            psh->dyn_seen = ngx_time();
            *id = psh->dyn_id;

            ngx_shmtx_unlock(&shpool->mutex);

            return i;
        }
    }

    if (slot == NGX_ERROR) {
        ngx_shmtx_unlock(&shpool->mutex);

        if (logged != ngx_time()) {
            logged = ngx_time();

            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                          "no free slot to check peer %*s in upstream %V, "
                          "increase check_shm_size",
                          np->name_len, np->name, cu->name);
        }

        return NGX_ERROR;
    }

    ucscf = cu->conf;

    psh = &peers_shm->peers[slot];

    if (++psh->dyn_id == 0) {
        psh->dyn_id = 1;
    }

    psh->dyn_used = 1;
    psh->dyn_upstream = cu->index;
    psh->dyn_seen = ngx_time();

    psh->sockaddr = (struct sockaddr *) psh->dyn_sockaddr;
    psh->socklen = np->socklen;
    ngx_memcpy(psh->dyn_sockaddr, np->sockaddr, np->socklen);

    psh->dyn_name_len = np->name_len;
    ngx_memcpy(psh->dyn_name, np->name, np->name_len);

    psh->owner = NGX_INVALID_PID;
    psh->keepalive_owner = NGX_INVALID_PID;
    psh->access_time = 0;
    psh->access_count = 0;
    psh->busyness = 0;
    psh->fall_count = 0;
    psh->rise_count = 0;
    psh->down = ucscf->default_down;

    *id = psh->dyn_id;

    ngx_shmtx_unlock(&shpool->mutex);

    return slot;
}


static void
ngx_http_upstream_check_dyn_expire(ngx_upstream_check_upstream_t *cu)
{
    time_t                               now, grace;
    ngx_uint_t                           i;
    ngx_upstream_check_dyn_peer_t       *dyn;
    ngx_http_upstream_check_peer_shm_t  *psh;

    now = ngx_time();
    grace = ngx_http_upstream_check_dyn_grace(cu->conf);

    for (i = 0; i < cu->ndyn; /* void */) {
        dyn = cu->dyn[i];
        psh = dyn->peer.shm;

        if (psh->dyn_used && psh->dyn_id == dyn->peer.dyn_id) {

            if (dyn->pass == check_peers_ctx->sync_pass) {
                dyn->seen = now;
                psh->dyn_seen = now;
                i++;
                continue;
            }

            /* gone from this worker's peers, may come back */

            if (now - dyn->seen <= grace) {
                i++;
                continue;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                       "http check free dynamic peer %V in upstream %V",
                       &dyn->addr.name, cu->name);

        ngx_http_upstream_check_dyn_free(dyn);

        cu->dyn[i] = cu->dyn[--cu->ndyn];
    }
}


static void
ngx_http_upstream_check_dyn_free(ngx_upstream_check_dyn_peer_t *dyn)
{
    ngx_http_upstream_check_peer_t  *peer;

    peer = &dyn->peer;

    if (peer->check_ev.timer_set) {
        ngx_del_timer(&peer->check_ev);
    }

    if (peer->check_timeout_ev.timer_set) {
        ngx_del_timer(&peer->check_timeout_ev);
    }

    if (peer->pc.connection) {
        ngx_close_connection(peer->pc.connection);
        peer->pc.connection = NULL;
    }

    if (peer->shm->dyn_id == peer->dyn_id && peer->shm->owner == ngx_pid) {
        peer->shm->owner = NGX_INVALID_PID;
    }

    if (peer->shm->dyn_id == peer->dyn_id
        && peer->shm->keepalive_owner == ngx_pid)
    {
        peer->shm->keepalive_owner = NGX_INVALID_PID;
    }

    if (peer->pool) {
        ngx_destroy_pool(peer->pool);
    }

    ngx_destroy_pool(dyn->pool);
}


static void
ngx_http_upstream_check_dyn_free_all(ngx_http_upstream_check_peers_t *peers)
{
    ngx_uint_t                           i, j;
    ngx_upstream_check_upstream_t       *cu;

    cu = peers->upstreams.elts;

    for (i = 0; i < peers->upstreams.nelts; i++) {

        for (j = 0; j < cu[i].ndyn; j++) {
            ngx_http_upstream_check_dyn_free(cu[i].dyn[j]);
        }

        if (cu[i].dyn) {
            ngx_free(cu[i].dyn);
        }

        cu[i].dyn = NULL;
        cu[i].ndyn = 0;
        cu[i].nalloc = 0;
    }
}


/*
 * Release the slots of the peers which no worker has seen in their upstream
 * for a while.  The workers notice it by the changed slot id, and free their
 * check peers.
 */

static void
ngx_http_upstream_check_dyn_gc(void)
{
    time_t                                now, grace;
    ngx_uint_t                            i, n, up;
    ngx_slab_pool_t                      *shpool;
    ngx_upstream_check_upstream_t        *cu;
    ngx_http_upstream_check_peer_shm_t   *psh;
    ngx_http_upstream_check_peers_shm_t  *peers_shm;

    peers_shm = check_peers_ctx->peers_shm;
    shpool = check_peers_ctx->shpool;

    if (shpool == NULL) {
        return;
    }

    cu = check_peers_ctx->upstreams.elts;
    now = ngx_time();
    n = peers_shm->number + peers_shm->dyn_number;

    for (i = peers_shm->number; i < n; i++) {
        psh = &peers_shm->peers[i];

        if (!psh->dyn_used) {
            continue;
        }

        up = psh->dyn_upstream;

        grace = (up < check_peers_ctx->upstreams.nelts)
                ? ngx_http_upstream_check_dyn_grace(cu[up].conf)
                : 0;

        if (now - psh->dyn_seen <= grace) {
            continue;
        }

        ngx_shmtx_lock(&shpool->mutex);

        if (psh->dyn_used && now - psh->dyn_seen > grace) {
            psh->dyn_used = 0;
            psh->dyn_id++;
            psh->owner = NGX_INVALID_PID;
            psh->keepalive_owner = NGX_INVALID_PID;

            ngx_log_error(NGX_LOG_INFO, ngx_cycle->log, 0,
                          "release dynamic check peer %*s",
                          psh->dyn_name_len, psh->dyn_name);
        }

        ngx_shmtx_unlock(&shpool->mutex);
    }
}


static time_t
ngx_http_upstream_check_dyn_grace(ngx_http_upstream_check_srv_conf_t *ucscf)
{
    time_t  grace;

    /* 10 check intervals, at least 10 seconds */

    grace = ucscf->check_interval / 100;

    return grace < 10 ? 10 : grace;
}


static void
ngx_http_upstream_check_dyn_inherit(ngx_http_upstream_check_peers_t *peers,
    ngx_http_upstream_check_peers_shm_t *peers_shm,
    ngx_http_upstream_check_peers_t *opeers,
    ngx_http_upstream_check_peers_shm_t *opeers_shm)
{
    ngx_str_t                           *name;
    ngx_uint_t                           i, j, k, n, u, type;
    ngx_addr_t                          *addr;
    ngx_http_upstream_check_peer_t      *cpeer;
    ngx_upstream_check_upstream_t       *cu, *ocu;
    ngx_http_upstream_check_peer_shm_t  *psh, *opsh;

    cu = peers->upstreams.elts;
    ocu = opeers->upstreams.elts;
    cpeer = peers->peers.elts;

    k = peers_shm->number;
    n = k + peers_shm->dyn_number;

    for (i = opeers_shm->number;
         i < opeers_shm->number + opeers_shm->dyn_number && k < n;
         i++)
    {
        opsh = &opeers_shm->peers[i];

        if (!opsh->dyn_used || opsh->dyn_upstream >= opeers->upstreams.nelts) {
            continue;
        }

        /* an http and a stream upstream may have the same name */

        name = ocu[opsh->dyn_upstream].name;
        type = ocu[opsh->dyn_upstream].type;

        for (u = 0; u < peers->upstreams.nelts; u++) {
            if (cu[u].type == type
                && cu[u].name->len == name->len
                && ngx_strncmp(cu[u].name->data, name->data, name->len) == 0)
            {
                break;
            }
        }

        if (u == peers->upstreams.nelts) {
            continue;
        }

        /* now in the configuration, it inherits the state as such */

        for (j = cu[u].first; j < cu[u].first + cu[u].number; j++) {
            addr = cpeer[j].peer_addr;

            if (ngx_http_upstream_check_cmp_sockaddr(addr->sockaddr,
                    addr->socklen, opsh->sockaddr, opsh->socklen)
                == NGX_OK)
            {
                break;
            }
        }

        if (j < cu[u].first + cu[u].number) {
            continue;
        }

        psh = &peers_shm->peers[k++];

        psh->dyn_used = 1;
        psh->dyn_id = 1;
        psh->dyn_upstream = u;
        psh->dyn_seen = ngx_time();

        psh->socklen = opsh->socklen;
        ngx_memcpy(psh->dyn_sockaddr, opsh->dyn_sockaddr, opsh->socklen);

        psh->dyn_name_len = opsh->dyn_name_len;
        ngx_memcpy(psh->dyn_name, opsh->dyn_name, opsh->dyn_name_len);

        psh->access_time = opsh->access_time;
        psh->access_count = opsh->access_count;
        psh->fall_count = opsh->fall_count;
        psh->rise_count = opsh->rise_count;
        psh->down = opsh->down;
    }
}


static ngx_int_t
ngx_http_upstream_check_dyn_status(ngx_http_request_t *r,
    ngx_http_upstream_check_peers_t *peers, ngx_array_t *records)
{
    ngx_uint_t                              i, n, u;
    ngx_upstream_check_upstream_t          *cu;
    ngx_http_upstream_check_peer_shm_t     *psh;
    ngx_http_upstream_check_peers_shm_t    *peers_shm;
    ngx_http_upstream_check_status_peer_t  *record;

    peers_shm = peers->peers_shm;
    if (peers_shm == NULL) {
        return NGX_OK;
    }

    cu = peers->upstreams.elts;
    n = peers_shm->number + peers_shm->dyn_number;

    for (i = peers_shm->number; i < n; i++) {
        psh = &peers_shm->peers[i];

        u = psh->dyn_upstream;

        if (!psh->dyn_used || u >= peers->upstreams.nelts) {
            continue;
        }

        record = ngx_array_push(records);
        if (record == NULL) {
            return NGX_ERROR;
        }

        record->upstream = cu[u].name;
        record->shm = psh;
        record->conf = cu[u].conf;

        record->name.len = psh->dyn_name_len;
        record->name.data = ngx_pnalloc(r->pool, record->name.len);
        if (record->name.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(record->name.data, psh->dyn_name, record->name.len);
    }

    return NGX_OK;
}

#endif


static ngx_int_t
ngx_http_upstream_check_cmp_sockaddr(struct sockaddr *sa1, socklen_t slen1,
    struct sockaddr *sa2, socklen_t slen2)
{
    struct sockaddr_in   *sin1, *sin2;
#if (NGX_HAVE_INET6)
    struct sockaddr_in6  *sin61, *sin62;
#endif

    if (sa1->sa_family != sa2->sa_family) {
        return NGX_DECLINED;
    }

    switch (sa1->sa_family) {

#if (NGX_HAVE_INET6)
    case AF_INET6:

        sin61 = (struct sockaddr_in6 *) sa1;
        sin62 = (struct sockaddr_in6 *) sa2;

        if (sin61->sin6_port != sin62->sin6_port) {
            return NGX_DECLINED;
        }

        if (ngx_memcmp(&sin61->sin6_addr, &sin62->sin6_addr, 16) != 0) {
            return NGX_DECLINED;
        }

        return NGX_OK;
#endif

    case AF_INET:

        sin1 = (struct sockaddr_in *) sa1;
        sin2 = (struct sockaddr_in *) sa2;

        if (sin1->sin_port != sin2->sin_port) {
            return NGX_DECLINED;
        }

        if (sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) {
            return NGX_DECLINED;
        }

        return NGX_OK;

    default: /* AF_UNIX */

        if (slen1 != slen2 || ngx_memcmp(sa1, sa2, slen1) != 0) {
            return NGX_DECLINED;
        }

        return NGX_OK;
    }
}


/*
 * Several upstreams may list the same server, e.g.
 *
 *     upstream vh01 { server server1:8080; ... check ...; }
 *     upstream vh02 { server server1:8080; ... check ...; }
 *
 * If the address checked and all the check settings are the same, the
 * server is checked once: the later peers use the check and the status of
 * the first one. They are still listed separately on the status page.
 * Peers added at run time are not shared.
 */

static void
ngx_http_upstream_check_share_peers(ngx_http_upstream_check_peers_t *peers)
{
    ngx_uint_t                       i, j;
    ngx_http_upstream_check_peer_t  *peer;

    peer = peers->peers.elts;

    for (i = 0; i < peers->peers.nelts; i++) {

        peer[i].shared = 0;

        for (j = 0; j < i; j++) {

            if (peer[j].shared) {
                continue;
            }

            if (ngx_http_upstream_check_same_check(&peer[i], &peer[j])
                == NGX_OK)
            {
                peer[i].shared = 1;
                peer[i].shared_index = j;

                ngx_log_debug3(NGX_LOG_DEBUG_HTTP, ngx_cycle->log, 0,
                               "http check peer %V in upstream %V shares "
                               "the check of index %ui",
                               &peer[i].check_peer_addr->name,
                               peer[i].upstream_name, j);
                break;
            }
        }
    }
}


static ngx_int_t
ngx_http_upstream_check_same_check(ngx_http_upstream_check_peer_t *p1,
    ngx_http_upstream_check_peer_t *p2)
{
    ngx_uint_t                           i, n1, n2;
    ngx_str_t                           *s1, *s2;
    ngx_http_upstream_check_srv_conf_t  *c1, *c2;

    if (ngx_http_upstream_check_cmp_sockaddr(p1->check_peer_addr->sockaddr,
                                             p1->check_peer_addr->socklen,
                                             p2->check_peer_addr->sockaddr,
                                             p2->check_peer_addr->socklen)
        != NGX_OK)
    {
        return NGX_DECLINED;
    }

    c1 = p1->conf;
    c2 = p2->conf;

    if (c1 == c2) {
        /* the same server twice in one upstream */
        return NGX_OK;
    }

    /* check */

    if (c1->check_type_conf != c2->check_type_conf
        || c1->check_interval != c2->check_interval
        || c1->check_timeout != c2->check_timeout
        || c1->rise_count != c2->rise_count
        || c1->fall_count != c2->fall_count
        || c1->default_down != c2->default_down
        || c1->port != c2->port)
    {
        return NGX_DECLINED;
    }

    /* check_keepalive_requests */

    if (c1->check_keepalive_requests != c2->check_keepalive_requests) {
        return NGX_DECLINED;
    }

    /* check_http_send */

    if (c1->send.len != c2->send.len
        || ngx_memcmp(c1->send.data, c2->send.data, c1->send.len) != 0)
    {
        return NGX_DECLINED;
    }

    /* check_http_expect_alive */

    if (c1->code.status_alive != c2->code.status_alive) {
        return NGX_DECLINED;
    }

    /* check_fastcgi_param */

    n1 = c1->fastcgi_params ? c1->fastcgi_params->nelts : 0;
    n2 = c2->fastcgi_params ? c2->fastcgi_params->nelts : 0;

    if (n1 != n2) {
        return NGX_DECLINED;
    }

    if (n1) {
        s1 = c1->fastcgi_params->elts;
        s2 = c2->fastcgi_params->elts;

        for (i = 0; i < n1; i++) {
            if (s1[i].len != s2[i].len
                || ngx_memcmp(s1[i].data, s2[i].data, s1[i].len) != 0)
            {
                return NGX_DECLINED;
            }
        }
    }

    return NGX_OK;
}
