﻿#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static ngx_int_t ngx_http_last_modified_filter_init(ngx_conf_t *cf);
static void *ngx_http_last_modified_filter_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_last_modified_filter_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);

typedef struct {
    ngx_flag_t enable;
    ngx_http_complex_value_t *source;
    ngx_flag_t clear_etag;
} ngx_http_last_modified_loc_conf_t;

static ngx_command_t ngx_http_last_modified_commands[] = {
    { ngx_string("last_modified_override"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_last_modified_loc_conf_t, enable),
      NULL },

    { ngx_string("last_modified_source"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_last_modified_loc_conf_t, source),
      NULL },

    { ngx_string("last_modified_clear_etag"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_last_modified_loc_conf_t, clear_etag),
      NULL },

      ngx_null_command
};

static ngx_http_module_t ngx_http_last_modified_filter_module_ctx = {
    NULL,         /* preconfiguration */
    ngx_http_last_modified_filter_init,     /* postconfiguration */

    NULL,         /* create main configuration */
    NULL,         /* init main configuration */

    NULL,         /* create server configuration */
    NULL,        /* merge server configuration */

    ngx_http_last_modified_filter_create_loc_conf,          /* create location configuration */
    ngx_http_last_modified_filter_merge_loc_conf            /* merge location configuration */
};

ngx_module_t ngx_http_last_modified_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_last_modified_filter_module_ctx, /* module context */
    ngx_http_last_modified_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;

static ngx_int_t update_header(ngx_http_request_t *r, ngx_http_complex_value_t *source, ngx_flag_t update_etag)
{
    ngx_str_t uri, path;
    ngx_http_core_loc_conf_t *clcf;
    u_char* last;
    ngx_open_file_info_t of;

    if (ngx_http_complex_value(r, source, &uri) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "last_modified: could not generate last_modified_source value");
        return -1;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    path.len = clcf->root.len + uri.len + 1;
    path.data = ngx_pcalloc(r->pool, path.len);
    if (path.data == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "last_modified: could not allocate memory for full path");
        return -2;
    }

    last = ngx_copy(path.data, clcf->root.data, clcf->root.len);
    last = ngx_copy(last, uri.data, uri.len);

    ngx_memzero(&of, sizeof(ngx_open_file_info_t));
    of.read_ahead = clcf->read_ahead;
    of.directio = clcf->directio;
    of.valid = clcf->open_file_cache_valid;
    of.min_uses = clcf->open_file_cache_min_uses;
    of.errors = clcf->open_file_cache_errors;
    of.events = clcf->open_file_cache_events;

    if (ngx_http_set_disable_symlinks(r, clcf, &path, &of) == NGX_OK) {
        if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool) == NGX_OK) {
            if (of.is_file) {
                if (r->headers_out.last_modified_time < of.mtime) {
                    r->headers_out.last_modified_time = of.mtime;

                    if (update_etag && -1 != of.size) {
                        off_t prev_size = r->headers_out.content_length_n;
                        r->headers_out.content_length_n = of.size;
                        r->headers_out.last_modified_time = of.mtime;
                        ngx_http_set_etag(r);
                        r->headers_out.content_length_n = prev_size;
                    }
                }
            }
            else {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "last_modified: %V is not a file", &path);
            }
        }
        else {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "last_modified: could not open %V", &path);
        }
    }
    else {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "last_modified: count not disable symlinks on %V", &path);
    }

    ngx_pfree(r->pool, path.data);

    return 0;
}

static ngx_int_t
ngx_http_last_modified_header_filter(ngx_http_request_t *r)
{
    ngx_http_last_modified_loc_conf_t *sllc;
    sllc = ngx_http_get_module_loc_conf(r, ngx_http_last_modified_filter_module);

    if (!sllc->enable || !sllc->source || r != r->main || !(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        return ngx_http_next_header_filter(r);
    }

    if (sllc->clear_etag) {
        ngx_http_clear_etag(r);
    }

    update_header(r, sllc->source, !sllc->clear_etag);

    return ngx_http_next_header_filter(r);
}

static void *
ngx_http_last_modified_filter_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_last_modified_loc_conf_t  *sllc;

    sllc = ngx_pcalloc(cf->pool, sizeof(ngx_http_last_modified_loc_conf_t));
    if (sllc == NULL) {
        return NULL;
    }

    sllc->enable = NGX_CONF_UNSET;
    sllc->clear_etag = NGX_CONF_UNSET;

    return sllc;
}

static char *
ngx_http_last_modified_filter_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_last_modified_loc_conf_t *prev = parent;
    ngx_http_last_modified_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    if (conf->source == NULL) {
        conf->source = prev->source;
    }
    ngx_conf_merge_value(conf->clear_etag, prev->clear_etag, 1);

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_last_modified_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_last_modified_header_filter;

    return NGX_OK;
}
