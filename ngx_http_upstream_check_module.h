#ifndef _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_
#define _NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

ngx_uint_t ngx_http_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer);

/*
 * V2: zone+resolve support.  Call add_resolve_peer instead of add_peer for
 * upstreams that use "server <host> resolve;" inside a shared zone.  It
 * pre-allocates check_resolve_max_addrs slots so that each runtime-resolved
 * IP can receive its own independent health-check slot at runtime.
 *
 * get_resolve_peer / put_resolve_peer are the runtime slot-pool API called
 * from the nginx zone module (ngx_http_upstream_zone_copy_peer /
 * ngx_http_upstream_zone_remove_peer_locked).
 */
ngx_uint_t ngx_http_upstream_check_add_resolve_peer(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us, ngx_addr_t *peer);

ngx_uint_t ngx_http_upstream_check_get_resolve_peer(ngx_uint_t first_index,
    struct sockaddr *sa, socklen_t socklen, ngx_str_t *name);

void ngx_http_upstream_check_put_resolve_peer(ngx_uint_t index);

ngx_uint_t ngx_http_upstream_check_peer_down(ngx_uint_t index);

void ngx_http_upstream_check_get_peer(ngx_uint_t index);
void ngx_http_upstream_check_free_peer(ngx_uint_t index);


#endif //_NGX_HTTP_UPSTREAM_CHECK_MODELE_H_INCLUDED_

