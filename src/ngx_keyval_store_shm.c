/*
 * Copyright (c) Tatsuya Kamijo
 * Copyright (c) Bengo4.com, Inc.
 */

#include <ngx_event.h>
#include "ngx_keyval.h"

#define NGX_KEYVAL_STATE_MAGIC    "KVST"
#define NGX_KEYVAL_STATE_VERSION  1

typedef struct {
    u_char    magic[4];    /* NGX_KEYVAL_STATE_MAGIC */
    uint32_t  version;     /* NGX_KEYVAL_STATE_VERSION */
} ngx_keyval_state_header_t;

typedef struct {
    uint32_t  key_len;
    uint32_t  val_len;
    int64_t   expires_at;  /* absolute time in seconds, 0 = never expires */
} ngx_keyval_state_record_t;

static void
ngx_keyval_rbtree_insert_value(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t **p;
    ngx_keyval_node_t *n, *nt;

    for ( ;; ) {
        if (node->key < temp->key) {
            p = &temp->left;
        } else if (node->key > temp->key) {
            p = &temp->right;
        } else { /* node->key == temp->key */
            n = (ngx_keyval_node_t *) &node->color;
            nt = (ngx_keyval_node_t *) &temp->color;
            p = (ngx_memn2cmp(n->data, nt->data, n->len, nt->len) < 0)
        ? &temp->left : &temp->right;
        }
        if (*p == sentinel) {
            break;
        }
        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

static ngx_rbtree_node_t *
ngx_keyval_rbtree_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key, uint32_t hash)
{
    ngx_int_t rc;
    ngx_rbtree_node_t *node, *sentinel;
    ngx_keyval_node_t *n;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {
        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */
        n = (ngx_keyval_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, n->data, key->len, (size_t) n->len);
        if (rc == 0) {
            return node;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    /* not found */
    return NULL;
}

ngx_int_t
ngx_keyval_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    size_t len;
    ngx_keyval_shm_ctx_t *ctx, *octx;

    octx = data;
    ctx = shm_zone->data;

    if (octx) {
        ctx->sh = octx->sh;
        ctx->shpool = octx->shpool;
        return NGX_OK;
    }

    ctx->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    if (shm_zone->shm.exists) {
        ctx->sh = ctx->shpool->data;
        return NGX_OK;
    }

    ctx->sh = ngx_slab_alloc(ctx->shpool, sizeof(ngx_keyval_sh_t));
    if (ctx->sh == NULL) {
        return NGX_ERROR;
    }

    ctx->shpool->data = ctx->sh;

    ngx_rbtree_init(&ctx->sh->rbtree, &ctx->sh->sentinel,
                    ngx_keyval_rbtree_insert_value);

    len = sizeof(" in keyval zone \"\"") + shm_zone->shm.name.len;

    ctx->shpool->log_ctx = ngx_slab_alloc(ctx->shpool, len);
    if (ctx->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(ctx->shpool->log_ctx, " in keyval zone \"%V\"%Z",
                &shm_zone->shm.name);

    ctx->shpool->log_nomem = 0;

    /* zone is created anew: restore content from the state file if any */
    if (ctx->state_file.len > 0
        && ngx_keyval_shm_load_data(ctx, shm_zone, &ctx->state_file,
                                    ngx_cycle->log) != NGX_OK)
    {
        return NGX_ERROR;
    }

    return NGX_OK;
}

#if (NGX_HAVE_KEYVAL_ZONE_REDIS)
ngx_int_t
ngx_keyval_init_zone_none(ngx_shm_zone_t *shm_zone, void *data)
{
    return NGX_OK;
}
#endif

ngx_keyval_shm_ctx_t *
ngx_keyval_shm_get_context(ngx_shm_zone_t *shm, ngx_log_t *log)
{
    ngx_keyval_shm_ctx_t *ctx;

    if (!shm) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "keyval: rejected due to not found shared memory zone");
        return NULL;
    }

    ctx = shm->data;
    if (!ctx) {
        ngx_log_error(NGX_LOG_INFO, log, 0,
                      "keyval: rejected due to not found shared memory context");
        return NULL;
    }

    return ctx;
}

ngx_int_t
ngx_keyval_shm_get_data(ngx_keyval_shm_ctx_t *ctx, ngx_shm_zone_t *shm,
    ngx_str_t *key, ngx_str_t *val, ngx_pool_t *pool)
{
    uint32_t hash;
    ngx_rbtree_node_t *node;
    ngx_keyval_node_t *kv;

    if (!ctx || !shm || !key || !val) {
        return NGX_ERROR;
    }

    hash = ngx_crc32_short(key->data, key->len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    node = ngx_keyval_rbtree_lookup(&ctx->sh->rbtree, key, hash);

    if (node == NULL) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return NGX_DECLINED;
    }

    kv = (ngx_keyval_node_t *) &node->color;

    if (kv->expires_at != 0
        && (ngx_msec_int_t) (ngx_current_msec - kv->expires_at) >= 0)
    {
        ngx_rbtree_delete(&ctx->sh->rbtree, node);
        ngx_slab_free_locked(ctx->shpool, node);
        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return NGX_DECLINED;
    }

    val->len = kv->size - kv->len;
    val->data = ngx_pnalloc(pool, val->len);
    if (val->data == NULL) {
        ngx_shmtx_unlock(&ctx->shpool->mutex);
        return NGX_ERROR;
    }
    ngx_memcpy(val->data, kv->data + kv->len, val->len);

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return NGX_OK;
}

ngx_int_t
ngx_keyval_shm_set_data(ngx_keyval_shm_ctx_t *ctx, ngx_shm_zone_t *shm,
    ngx_str_t *key, ngx_str_t *val, ngx_log_t *log)
{
    uint32_t hash;
    size_t n;
    ngx_int_t rc;
    ngx_rbtree_node_t *node;

    if (!ctx || !shm || !key || !val) {
        return NGX_ERROR;
    }

    hash = ngx_crc32_short(key->data, key->len);

    ngx_shmtx_lock(&ctx->shpool->mutex);

    node = ngx_keyval_rbtree_lookup(&ctx->sh->rbtree, key, hash);
    if (node != NULL) {
        ngx_rbtree_delete(&ctx->sh->rbtree, node);
        ngx_slab_free_locked(ctx->shpool, node);
    }

    n = offsetof(ngx_rbtree_node_t, color)
        + offsetof(ngx_keyval_node_t, data)
        + key->len
        + val->len;

    node = ngx_slab_alloc_locked(ctx->shpool, n);
    if (node == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "keyval: failed to allocate slab");
        rc = NGX_ERROR;
    } else {
        ngx_keyval_node_t *kv;
        kv = (ngx_keyval_node_t *) &node->color;

        node->key = hash;
        kv->size = key->len + val->len;
        kv->len = key->len;
        ngx_memcpy(kv->data, key->data, key->len);
        ngx_memcpy(kv->data + key->len, val->data, val->len);

        kv->expires_at = ctx->ttl ? (ngx_current_msec + ctx->ttl * 1000) : 0;

        ngx_rbtree_insert(&ctx->sh->rbtree, node);

        rc = NGX_OK;
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    return rc;
}

ngx_int_t
ngx_keyval_shm_save_data(ngx_keyval_shm_ctx_t *ctx, ngx_shm_zone_t *shm,
    ngx_str_t *filepath, ngx_log_t *log)
{
    u_char                    *p, *tmp;
    size_t                     len;
    ngx_fd_t                   fd;
    ngx_int_t                  rc;
    ngx_file_t                 file;
    ngx_rbtree_node_t         *node, *root, *sentinel;
    ngx_keyval_node_t         *kv;
    time_t                     now_sec;
    ngx_msec_t                 now_msec;
    ngx_keyval_state_header_t  header;
    ngx_keyval_state_record_t  record;

    if (!ctx || !filepath || filepath->len == 0) {
        return NGX_OK;
    }

    len = filepath->len + sizeof(".tmp");

    tmp = ngx_pnalloc(ngx_cycle->pool, len);
    if (tmp == NULL) {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "keyval: failed to allocate temporary state file name"
                      " for zone \"%V\"", &shm->shm.name);
        return NGX_ERROR;
    }

    p = ngx_copy(tmp, filepath->data, filepath->len);
    p = ngx_cpystrn(p, (u_char *) ".tmp", sizeof(".tmp"));

    fd = ngx_open_file(tmp, NGX_FILE_RDWR, NGX_FILE_TRUNCATE,
                       NGX_FILE_DEFAULT_ACCESS);
    if (fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "keyval: failed to open state file \"%s\" for writing"
                      " for zone \"%V\"", tmp, &shm->shm.name);
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.fd = fd;
    file.name.data = tmp;
    file.name.len = p - tmp;
    file.log = log;

    ngx_memcpy(header.magic, NGX_KEYVAL_STATE_MAGIC, 4);
    header.version = NGX_KEYVAL_STATE_VERSION;

    if (ngx_write_file(&file, (u_char *) &header, sizeof(header), file.offset)
        == NGX_ERROR)
    {
        ngx_log_error(NGX_LOG_ERR, log, 0,
                      "keyval: failed to write state file \"%s\""
                      " for zone \"%V\"", tmp, &shm->shm.name);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    now_sec = ngx_time();
    now_msec = ngx_current_msec;

    rc = NGX_OK;

    ngx_shmtx_lock(&ctx->shpool->mutex);

    root = ctx->sh->rbtree.root;
    sentinel = ctx->sh->rbtree.sentinel;

    if (root != sentinel) {
        for (node = ngx_rbtree_min(root, sentinel);
             node;
             node = ngx_rbtree_next(&ctx->sh->rbtree, node))
        {
            kv = (ngx_keyval_node_t *) &node->color;

            /* skip expired entries */
            if (kv->expires_at != 0
                && (ngx_msec_int_t) (ngx_current_msec - kv->expires_at) >= 0)
            {
                continue;
            }

            record.key_len = (uint32_t) kv->len;
            record.val_len = (uint32_t) (kv->size - kv->len);

            if (kv->expires_at != 0) {
                record.expires_at = (int64_t) (now_sec
                     + ((ngx_msec_int_t) (kv->expires_at - now_msec)) / 1000);
            } else {
                record.expires_at = 0;
            }

            if (ngx_write_file(&file, (u_char *) &record, sizeof(record),
                               file.offset) == NGX_ERROR
                || ngx_write_file(&file, kv->data, kv->len,
                                  file.offset) == NGX_ERROR
                || ngx_write_file(&file, kv->data + kv->len, kv->size - kv->len,
                                  file.offset) == NGX_ERROR)
            {
                ngx_log_error(NGX_LOG_ERR, log, 0,
                              "keyval: failed to write state file \"%s\""
                              " for zone \"%V\"", tmp, &shm->shm.name);
                rc = NGX_ERROR;
                break;
            }
        }
    }

    ngx_shmtx_unlock(&ctx->shpool->mutex);

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "keyval: failed to close state file \"%s\""
                      " for zone \"%V\"", tmp, &shm->shm.name);
        rc = NGX_ERROR;
    }

    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_rename_file(tmp, filepath->data) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_ERR, log, ngx_errno,
                      "keyval: failed to rename state file \"%s\" to \"%V\""
                      " for zone \"%V\"", tmp, filepath, &shm->shm.name);
        return NGX_ERROR;
    }

    return NGX_OK;
}

ngx_int_t
ngx_keyval_shm_load_data(ngx_keyval_shm_ctx_t *ctx, ngx_shm_zone_t *shm,
    ngx_str_t *filepath, ngx_log_t *log)
{
    u_char                    *buf, *p, *end;
    size_t                     file_size, n;
    ngx_fd_t                   fd;
    ngx_file_t                 file;
    ngx_file_info_t            fi;
    ngx_rbtree_node_t         *node;
    ngx_keyval_node_t         *kv;
    ngx_str_t                  key, val;
    time_t                     now;
    ngx_uint_t                 loaded, expired;
    ngx_keyval_state_header_t *header;
    ngx_keyval_state_record_t *record;

    if (!ctx || !filepath || filepath->len == 0) {
        return NGX_OK;
    }

    fd = ngx_open_file(filepath->data, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        /* first start: no state file yet */
        return NGX_OK;
    }

    if (ngx_fd_info(fd, &fi) == NGX_FILE_ERROR) {
        ngx_log_error(NGX_LOG_EMERG, log, ngx_errno,
                      "keyval: failed to get state file \"%V\" info"
                      " for zone \"%V\"", filepath, &shm->shm.name);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    file_size = (size_t) ngx_file_size(&fi);

    if (file_size < sizeof(ngx_keyval_state_header_t)) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "keyval: state file \"%V\" for zone \"%V\""
                      " is corrupted", filepath, &shm->shm.name);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    buf = ngx_pnalloc(ngx_cycle->pool, file_size);
    if (buf == NULL) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "keyval: failed to allocate buffer to read"
                      " state file \"%V\" for zone \"%V\"",
                      filepath, &shm->shm.name);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));
    file.fd = fd;
    file.name = *filepath;
    file.log = log;

    if (ngx_read_file(&file, buf, file_size, 0) != (ssize_t) file_size) {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "keyval: failed to read state file \"%V\""
                      " for zone \"%V\"", filepath, &shm->shm.name);
        ngx_close_file(fd);
        return NGX_ERROR;
    }

    ngx_close_file(fd);

    header = (ngx_keyval_state_header_t *) buf;

    if (ngx_memcmp(header->magic, NGX_KEYVAL_STATE_MAGIC, 4) != 0
        || header->version != NGX_KEYVAL_STATE_VERSION)
    {
        ngx_log_error(NGX_LOG_EMERG, log, 0,
                      "keyval: state file \"%V\" for zone \"%V\""
                      " has invalid format", filepath, &shm->shm.name);
        return NGX_ERROR;
    }

    p = buf + sizeof(ngx_keyval_state_header_t);
    end = buf + file_size;

    now = ngx_time();
    loaded = 0;
    expired = 0;

    while (p < end) {
        if ((size_t) (end - p) < sizeof(ngx_keyval_state_record_t)) {
            goto corrupted;
        }

        record = (ngx_keyval_state_record_t *) p;
        p += sizeof(ngx_keyval_state_record_t);

        if ((size_t) (end - p) < (size_t) record->key_len + record->val_len) {
            goto corrupted;
        }

        key.data = p;
        key.len = record->key_len;
        p += record->key_len;

        val.data = p;
        val.len = record->val_len;
        p += record->val_len;

        /* skip expired entries */
        if (record->expires_at != 0 && (time_t) record->expires_at <= now) {
            expired++;
            continue;
        }

        n = offsetof(ngx_rbtree_node_t, color)
            + offsetof(ngx_keyval_node_t, data)
            + key.len
            + val.len;

        node = ngx_slab_alloc(ctx->shpool, n);
        if (node == NULL) {
            ngx_log_error(NGX_LOG_EMERG, log, 0,
                          "keyval: not enough room in zone \"%V\" to restore"
                          " state file \"%V\": %ui entries loaded,"
                          " loading stopped",
                          &shm->shm.name, filepath, loaded);
            return NGX_ERROR;
        }

        kv = (ngx_keyval_node_t *) &node->color;

        node->key = ngx_crc32_short(key.data, key.len);
        kv->size = key.len + val.len;
        kv->len = key.len;
        ngx_memcpy(kv->data, key.data, key.len);
        ngx_memcpy(kv->data + key.len, val.data, val.len);

        if (record->expires_at != 0) {
            kv->expires_at = ngx_current_msec
                             + (ngx_msec_t) ((record->expires_at - now) * 1000);
        } else {
            kv->expires_at = 0;
        }

        ngx_rbtree_insert(&ctx->sh->rbtree, node);
        loaded++;
    }

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "keyval: restored %ui entries from state file \"%V\""
                  " for zone \"%V\" (%ui expired entries skipped)",
                  loaded, filepath, &shm->shm.name, expired);

    return NGX_OK;

corrupted:

    ngx_log_error(NGX_LOG_EMERG, log, 0,
                  "keyval: state file \"%V\" for zone \"%V\" is corrupted"
                  " or truncated", filepath, &shm->shm.name);

    return NGX_ERROR;
}
