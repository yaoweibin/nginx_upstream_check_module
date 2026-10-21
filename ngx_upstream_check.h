#ifndef _NGX_UPSTREAM_CHECK_H_INCLUDED_
#define _NGX_UPSTREAM_CHECK_H_INCLUDED_


/*
 * The interface between the check engine, in ngx_http_upstream_check_module.c,
 * and the code for each kind of upstream: http { upstream {} } in the same
 * file, stream { upstream {} } in ngx_stream_upstream_check_module.c.
 *
 * The engine keeps the check peers, their shared memory, the dynamic peers
 * and the check timers.  An upstream module registers its upstreams and
 * their configured peers, and provides a sync handler which walks the
 * upstream's round robin peers under their locks and asks the engine, for
 * each peer, whether it is down.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#define NGX_UPSTREAM_CHECK_HTTP              0
#define NGX_UPSTREAM_CHECK_STREAM            1

/*
 * The value put into the round robin peers' "down" field for the peers which
 * fail the checks. The value from the configuration is 0 or 1, so this
 * leaves the servers marked "down" in the configuration alone.
 */
#define NGX_UPSTREAM_CHECK_PEER_DOWN         2


typedef struct ngx_http_upstream_check_srv_conf_s
    ngx_http_upstream_check_srv_conf_t;
typedef struct ngx_upstream_check_upstream_s ngx_upstream_check_upstream_t;
typedef struct ngx_upstream_check_dyn_peer_s ngx_upstream_check_dyn_peer_t;

typedef void (*ngx_upstream_check_sync_pt)(ngx_upstream_check_upstream_t *cu);


typedef struct {
    ngx_uint_t                               check_shm_size;
} ngx_upstream_check_main_conf_t;


struct ngx_upstream_check_upstream_s {
    ngx_uint_t                               type;
    ngx_str_t                               *name;

    /* ngx_http_upstream_srv_conf_t or ngx_stream_upstream_srv_conf_t */
    void                                    *upstream;
    ngx_http_upstream_check_srv_conf_t      *conf;

    /* copies the check status into the upstream's peers */
    ngx_upstream_check_sync_pt               sync;

    ngx_uint_t                               index;

    /* the check peers of this upstream: [first, first + number) */
    ngx_uint_t                               first;
    ngx_uint_t                               number;

    /* the dynamic peers known to this worker */
    ngx_upstream_check_dyn_peer_t          **dyn;
    ngx_uint_t                               ndyn;
    ngx_uint_t                               nalloc;
};


/* the state of a walk over one list of round robin peers */
typedef struct {
    ngx_upstream_check_upstream_t           *cu;
    ngx_uint_t                               hint;
    ngx_uint_t                               dhint;
    ngx_uint_t                               collect;
} ngx_upstream_check_sync_t;


void *ngx_upstream_check_create_srv_conf(ngx_conf_t *cf, ngx_uint_t type);
char *ngx_upstream_check_init_srv_conf(ngx_conf_t *cf,
    ngx_http_upstream_check_srv_conf_t *ucscf);
ngx_uint_t ngx_upstream_check_enabled(ngx_http_upstream_check_srv_conf_t *ucscf);

/* directive handlers, "conf" is the module's srv conf (main conf for size) */
char *ngx_upstream_check_conf_check(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *ngx_upstream_check_conf_keepalive_requests(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_upstream_check_conf_http_send(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *ngx_upstream_check_conf_http_expect_alive(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
char *ngx_upstream_check_conf_shm_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

/*
 * From a postconfiguration handler: register an upstream, then its
 * configured peers, then make sure the shared memory zone is there.
 */
ngx_upstream_check_upstream_t *ngx_upstream_check_add_upstream(ngx_conf_t *cf,
    ngx_uint_t type, ngx_str_t *name, void *upstream,
    ngx_http_upstream_check_srv_conf_t *ucscf, ngx_upstream_check_sync_pt sync);
ngx_int_t ngx_upstream_check_add_peer(ngx_conf_t *cf,
    ngx_upstream_check_upstream_t *cu, ngx_addr_t *addr);
ngx_int_t ngx_upstream_check_init_shm(ngx_conf_t *cf,
    ngx_upstream_check_main_conf_t *ucmcf);

/*
 * From a sync handler: for each list of peers, call sync_init(), then
 * sync_peer() for each peer, in order.  sync_peer() returns the value the
 * peer's "down" should have: 0 or NGX_UPSTREAM_CHECK_PEER_DOWN.  With
 * "collect" set, peers the engine has no check for yet are remembered and
 * get one after the sync handler returns.
 */
void ngx_upstream_check_sync_init(ngx_upstream_check_sync_t *s,
    ngx_upstream_check_upstream_t *cu, ngx_uint_t collect);
ngx_uint_t ngx_upstream_check_sync_peer(ngx_upstream_check_sync_t *s,
    struct sockaddr *sockaddr, socklen_t socklen, ngx_str_t *name);


#endif /* _NGX_UPSTREAM_CHECK_H_INCLUDED_ */
