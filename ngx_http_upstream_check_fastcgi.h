#ifndef _NGX_HTTP_UPSTREAM_CHECK_FASTCGI_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_CHECK_FASTCGI_H_INCLUDED_

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

typedef struct {
    u_char                        *start;
    u_char                        *end;
} ngx_http_fastcgi_split_part_t;

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

typedef struct {
    ngx_buf_t          send;
    ngx_buf_t          recv;

    ngx_uint_t         state;
    ngx_http_status_t  status;

    //----------additional fields
    ngx_http_fastcgi_state_e fastcgi_state;
    ngx_uint_t               fastcgi_type;
    size_t                   fastcgi_length;
    size_t                   fastcgi_padding;

    ngx_str_t                fastcgi_split;

    unsigned                 fastcgi_stdout_body:1;
    unsigned                 fastcgi_stdout:1;
    unsigned                 fastcgi_large_stderr:1;
} ngx_http_check_fastcgi_ctx;

static ngx_int_t ngx_http_upstream_check_fastcgi_process(ngx_http_check_fastcgi_ctx *ctx, ngx_pool_t *pool);

#endif /* _NGX_HTTP_UPSTREAM_CHECK_FASTCGI_H_INCLUDED_ */
