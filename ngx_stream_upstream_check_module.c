/*
 * Health checks for stream { upstream {} }.
 *
 * The checks themselves are run by the engine in
 * ngx_http_upstream_check_module.c (see ngx_upstream_check.h), which keeps
 * the peers of the http and the stream upstreams in the same shared memory
 * and shows them on the same check_status page.  This module registers the
 * stream upstreams with "check" and their round robin peers, and copies the
 * check status into the peers' "down" flag, which the stream balancers
 * (round robin, hash, least_conn, random) honour.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_stream.h>
#include "ngx_upstream_check.h"


static ngx_int_t ngx_stream_upstream_check_postconfiguration(ngx_conf_t *cf);
static ngx_int_t ngx_stream_upstream_check_add_upstream(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *us);
static void ngx_stream_upstream_check_sync(ngx_upstream_check_upstream_t *cu);
static ngx_uint_t ngx_stream_upstream_check_sync_list(
    ngx_upstream_check_upstream_t *cu, ngx_stream_upstream_rr_peers_t *peers,
    ngx_uint_t update);

static void *ngx_stream_upstream_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf,
    void *conf);
static void *ngx_stream_upstream_check_create_srv_conf(ngx_conf_t *cf);


static ngx_command_t  ngx_stream_upstream_check_commands[] = {

    { ngx_string("check"),
      NGX_STREAM_UPS_CONF|NGX_CONF_1MORE,
      ngx_upstream_check_conf_check,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_keepalive_requests"),
      NGX_STREAM_UPS_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_keepalive_requests,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_http_send"),
      NGX_STREAM_UPS_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_http_send,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_http_expect_alive"),
      NGX_STREAM_UPS_CONF|NGX_CONF_1MORE,
      ngx_upstream_check_conf_http_expect_alive,
      NGX_STREAM_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("check_shm_size"),
      NGX_STREAM_MAIN_CONF|NGX_CONF_TAKE1,
      ngx_upstream_check_conf_shm_size,
      NGX_STREAM_MAIN_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_stream_module_t  ngx_stream_upstream_check_module_ctx = {
    NULL,                                    /* preconfiguration */
    ngx_stream_upstream_check_postconfiguration, /* postconfiguration */

    ngx_stream_upstream_check_create_main_conf, /* create main configuration */
    ngx_stream_upstream_check_init_main_conf, /* init main configuration */

    ngx_stream_upstream_check_create_srv_conf, /* create server configuration */
    NULL                                     /* merge server configuration */
};


ngx_module_t  ngx_stream_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_stream_upstream_check_module_ctx, /* module context */
    ngx_stream_upstream_check_commands,    /* module directives */
    NGX_STREAM_MODULE,                     /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_stream_upstream_check_create_main_conf(ngx_conf_t *cf)
{
    return ngx_pcalloc(cf->pool, sizeof(ngx_upstream_check_main_conf_t));
}


static void *
ngx_stream_upstream_check_create_srv_conf(ngx_conf_t *cf)
{
    return ngx_upstream_check_create_srv_conf(cf, NGX_UPSTREAM_CHECK_STREAM);
}


static char *
ngx_stream_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{
    ngx_uint_t                         i;
    ngx_stream_upstream_srv_conf_t   **uscfp;
    ngx_stream_upstream_main_conf_t   *umcf;

    umcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_upstream_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {

        if (uscfp[i]->srv_conf == NULL) {
            continue;
        }

        if (ngx_upstream_check_init_srv_conf(cf,
                ngx_stream_conf_upstream_srv_conf(uscfp[i],
                                              ngx_stream_upstream_check_module))
            != NGX_CONF_OK)
        {
            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_stream_upstream_check_postconfiguration(ngx_conf_t *cf)
{
    ngx_uint_t                          i;
    ngx_stream_upstream_srv_conf_t    **uscfp;
    ngx_stream_upstream_main_conf_t    *umcf;
    ngx_upstream_check_main_conf_t     *ucmcf;

    /* the round robin peers are set up by the upstream's init_main_conf */

    umcf = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_upstream_module);
    ucmcf = ngx_stream_conf_get_module_main_conf(cf,
                                              ngx_stream_upstream_check_module);

    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (ngx_stream_upstream_check_add_upstream(cf, uscfp[i]) != NGX_OK) {
            return NGX_ERROR;
        }
    }

    return ngx_upstream_check_init_shm(cf, ucmcf);
}


static ngx_int_t
ngx_stream_upstream_check_add_upstream(ngx_conf_t *cf,
    ngx_stream_upstream_srv_conf_t *us)
{
    ngx_addr_t                          *addr;
    ngx_stream_upstream_rr_peer_t       *peer;
    ngx_stream_upstream_rr_peers_t      *peers, *list;
    ngx_upstream_check_upstream_t       *cu;
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    if (us->srv_conf == NULL) {
        return NGX_OK;
    }

    ucscf = ngx_stream_conf_upstream_srv_conf(us,
                                              ngx_stream_upstream_check_module);

    if (!ngx_upstream_check_enabled(ucscf)) {
        return NGX_OK;
    }

    /*
     * The built-in balancers (round robin, hash, least_conn, random) keep
     * the round robin peers in us->peer.data, which point back to the
     * upstream name.
     */

    peers = us->peer.data;

    if (peers == NULL || peers->name != &us->host) {
        ngx_log_error(NGX_LOG_WARN, cf->log, 0,
                      "upstream check is ignored for stream upstream \"%V\" "
                      "in %s:%ui: its load balancing method is not supported",
                      &us->host, us->file_name, us->line);
        return NGX_OK;
    }

    cu = ngx_upstream_check_add_upstream(cf, NGX_UPSTREAM_CHECK_STREAM,
                                         &us->host, us, ucscf,
                                         ngx_stream_upstream_check_sync);
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
ngx_stream_upstream_check_sync(ngx_upstream_check_upstream_t *cu)
{
    ngx_stream_upstream_srv_conf_t  *us;
    ngx_stream_upstream_rr_peers_t  *peers;

    us = cu->upstream;

    /*
     * With "zone", us->peer.data points to the copy of the peers in the
     * shared memory, which all the workers update.
     */

    for (peers = us->peer.data; peers; peers = peers->next) {

        ngx_stream_upstream_rr_peers_rlock(peers);

        if (ngx_stream_upstream_check_sync_list(cu, peers, 0) == 0) {
            ngx_stream_upstream_rr_peers_unlock(peers);
            continue;
        }

        ngx_stream_upstream_rr_peers_unlock(peers);

        ngx_stream_upstream_rr_peers_wlock(peers);

        (void) ngx_stream_upstream_check_sync_list(cu, peers, 1);

        ngx_stream_upstream_rr_peers_unlock(peers);
    }
}


static ngx_uint_t
ngx_stream_upstream_check_sync_list(ngx_upstream_check_upstream_t *cu,
    ngx_stream_upstream_rr_peers_t *peers, ngx_uint_t update)
{
    ngx_uint_t                      down, changed;
    ngx_upstream_check_sync_t       s;
    ngx_stream_upstream_rr_peer_t  *peer;

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
            ngx_log_debug3(NGX_LOG_DEBUG_STREAM, ngx_cycle->log, 0,
                           "stream check sync peer %V in upstream %V: %s",
                           &peer->name, cu->name, down ? "down" : "up");

            peer->down = down;
        }
    }

    return changed;
}
