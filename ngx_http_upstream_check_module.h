#ifndef _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

ngx_uint_t ngx_http_upstream_check_make_peer_index(
	ngx_http_upstream_srv_conf_t *us,ngx_uint_t peer_seq);

ngx_uint_t ngx_http_upstream_check_peer_down(ngx_uint_t index);

ngx_int_t ngx_http_upstream_check_update_upstream_peers(
	ngx_http_upstream_srv_conf_t *us, ngx_pool_t *pool);

#endif //_NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_

