/*
 * Copyright (c) Tatsuya Kamijo
 * Copyright (c) Bengo4.com, Inc.
 */

#include <ngx_stream.h>
#include "ngx_keyval.h"

static char *ngx_stream_keyval_conf_set_zone(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
#if (NGX_HAVE_KEYVAL_ZONE_REDIS)
static char *ngx_stream_keyval_conf_set_zone_redis(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
#endif
static char *ngx_stream_keyval_conf_set_variable(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static void ngx_stream_keyval_variable_set_handler(ngx_stream_session_t *s,
    ngx_stream_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_stream_keyval_variable_get_handler(ngx_stream_session_t *s,
    ngx_stream_variable_value_t *v, uintptr_t data);

static void ngx_stream_keyval_exit_master(ngx_cycle_t *cycle);

static ngx_command_t ngx_stream_keyval_commands[] = {
    { ngx_string("keyval"),
      NGX_STREAM_MAIN_CONF | NGX_CONF_TAKE3,
      ngx_stream_keyval_conf_set_variable,
      0,
      0,
      NULL },
    { ngx_string("keyval_zone"),
      NGX_STREAM_MAIN_CONF | NGX_CONF_1MORE,
      ngx_stream_keyval_conf_set_zone,
      0,
      0,
      NULL },
#if (NGX_HAVE_KEYVAL_ZONE_REDIS)
    { ngx_string("keyval_zone_redis"),
      NGX_STREAM_MAIN_CONF | NGX_CONF_1MORE,
      ngx_stream_keyval_conf_set_zone_redis,
      0,
      0,
      NULL },
#endif
    ngx_null_command
};

static ngx_stream_module_t ngx_stream_keyval_module_ctx = {
    NULL,                             /* preconfiguration */
    NULL,                             /* postconfiguration */
    ngx_keyval_create_main_conf,      /* create main configuration */
    NULL,                             /* init main configuration */
    NULL,                             /* create server configuration */
    NULL,                             /* merge server configuration */
};

ngx_module_t ngx_stream_keyval_module = {
    NGX_MODULE_V1,
    &ngx_stream_keyval_module_ctx, /* module context */
    ngx_stream_keyval_commands,  /* module directives */
    NGX_STREAM_MODULE,           /* module type */
    NULL,                        /* init master */
    NULL,                        /* init module */
    NULL,                        /* init process */
    NULL,                        /* init thread */
    NULL,                        /* exit thread */
    NULL,                        /* exit process */
    ngx_stream_keyval_exit_master, /* exit master */
    NGX_MODULE_V1_PADDING
};

static char *
ngx_stream_keyval_conf_set_zone(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_keyval_conf_t *config;

    config = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_keyval_module);

    return ngx_keyval_conf_set_zone(cf, cmd, conf,
                                    config, &ngx_stream_keyval_module);
}

#if (NGX_HAVE_KEYVAL_ZONE_REDIS)
static char *
ngx_stream_keyval_conf_set_zone_redis(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf)
{
    ngx_keyval_conf_t *config;

    config = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_keyval_module);

    return ngx_keyval_conf_set_zone_redis(cf, cmd, conf,
                                          config, &ngx_stream_keyval_module);
}
#endif

static char *
ngx_stream_keyval_conf_set_variable(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf)
{
    char *retval;
    ngx_uint_t flags;
    ngx_stream_variable_t *v;
    ngx_keyval_conf_t *config;
    ngx_keyval_variable_t *var = NULL;

    config = ngx_stream_conf_get_module_main_conf(cf, ngx_stream_keyval_module);

    retval = ngx_keyval_conf_set_variable(cf, cmd, conf,
                                          config, &ngx_stream_keyval_module,
                                          &var, ngx_stream_get_variable_index);
    if (retval != NGX_CONF_OK) {
        return retval;
    }
    if (!var) {
        return "failed to allocate";
    }

    /* add variable */
    flags = NGX_STREAM_VAR_CHANGEABLE | NGX_STREAM_VAR_NOCACHEABLE;
    v = ngx_stream_add_variable(cf, &(var->variable), flags);
    if (v == NULL) {
        return "failed to add variable";
    }

    v->get_handler = ngx_stream_keyval_variable_get_handler;
    v->set_handler = ngx_stream_keyval_variable_set_handler;
    v->data = (uintptr_t) var;

    return NGX_CONF_OK;
}

static ngx_variable_value_t *
ngx_stream_keyval_get_indexed_variable(void *data, ngx_uint_t index)
{
    return ngx_stream_get_indexed_variable((ngx_stream_session_t *) data,
                                           index);
}

static ngx_int_t
ngx_stream_keyval_variable_init(ngx_stream_session_t *s, uintptr_t data,
    ngx_str_t *key, ngx_keyval_zone_t **zone)
{
    ngx_keyval_variable_t *var;

    if (data == 0) {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "keyval: rejected due to not handler data");
        return NGX_ERROR;
    }

    var = (ngx_keyval_variable_t *) data;

    if (ngx_keyval_variable_get_key(s->connection->pool, s->connection, var,
                                    key,
                                    ngx_stream_keyval_get_indexed_variable,
                                    (void *) s) != NGX_OK)
    {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "keyval: rejected due to not found variable key");
        return NGX_ERROR;
    }

    if (!var->zone) {
        ngx_log_error(NGX_LOG_INFO, s->connection->log, 0,
                      "keyval: rejected due to not found variable zone");
        return NGX_ERROR;
    }

    *zone = var->zone;

    return NGX_OK;
}

static void
ngx_stream_keyval_variable_set_handler(ngx_stream_session_t *s,
    ngx_stream_variable_value_t *v,
    uintptr_t data)
{
    ngx_str_t key, val;
    ngx_keyval_zone_t *zone;

    if (ngx_stream_keyval_variable_init(s, data, &key, &zone) != NGX_OK) {
        return;
    }

    val.data = v->data;
    val.len = v->len;

    ngx_keyval_store_set(zone, &key, &val, s->connection->pool,
                         s->connection->log);
}

static ngx_int_t
ngx_stream_keyval_variable_get_handler(ngx_stream_session_t *s,
    ngx_stream_variable_value_t *v,
    uintptr_t data)
{
    ngx_int_t rc;
    ngx_str_t key, val;
    ngx_keyval_zone_t *zone;

    if (ngx_stream_keyval_variable_init(s, data, &key, &zone) != NGX_OK) {
        v->not_found = 1;
        return NGX_OK;
    }

    rc = ngx_keyval_store_get(zone, &key, &val, s->connection->pool,
                              s->connection->log);

    if (rc == NGX_OK) {
        v->data = val.data;
        v->len = val.len;
        v->not_found = 0;
    } else {
        v->data = NULL;
        v->len = 0;
        v->not_found = 1;
    }
    v->valid = 1;
    v->no_cacheable = 0;

    return NGX_OK;
}

static void
ngx_stream_keyval_exit_master(ngx_cycle_t *cycle)
{
    ngx_uint_t            i;
    ngx_keyval_conf_t    *conf;
    ngx_keyval_zone_t    *zones;
    ngx_keyval_shm_ctx_t *ctx;

    conf = ngx_stream_cycle_get_module_main_conf(cycle,
                                                 ngx_stream_keyval_module);
    if (conf == NULL || conf->zones == NULL) {
        return;
    }

    zones = conf->zones->elts;

    for (i = 0; i < conf->zones->nelts; i++) {
        if (zones[i].type != NGX_KEYVAL_ZONE_SHM
            || zones[i].state_file.len == 0)
        {
            continue;
        }

        ctx = ngx_keyval_shm_get_context(zones[i].shm, cycle->log);
        if (ctx == NULL) {
            continue;
        }

        ngx_keyval_shm_save_data(ctx, zones[i].shm, &zones[i].state_file,
                                 cycle->log);
    }
}
