#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <string.h>
#include "hiredis/hiredis.h"


typedef struct {
  ngx_str_t redis_host;
  ngx_int_t redis_port;
} auth_token_main_conf_t;


typedef struct {
  ngx_flag_t enabled;
  ngx_str_t redirect_location;
  ngx_str_t cookie_name;
  ngx_str_t header_name;
} auth_token_loc_conf_t;


ngx_module_t ngx_http_auth_token_module;


static ngx_int_t
lookup_user(auth_token_main_conf_t *conf, ngx_str_t *auth_token, ngx_str_t *user_id)
{
  redisContext *context = redisConnect((const char*)conf->redis_host.data, conf->redis_port);
  redisReply *reply = redisCommand(context, "GET %s", auth_token->data);
  if (reply->type == REDIS_REPLY_NIL) {
    return NGX_DECLINED;
  } else {
    user_id->len = strlen(reply->str);
    user_id->data = (u_char *) reply->str;
    return NGX_OK;
  }
}


static ngx_int_t
redirect(ngx_http_request_t *r, ngx_str_t *location)
{
  ngx_table_elt_t *h;
  h = ngx_list_push(&r->headers_out.headers);
  h->hash = 1;
  ngx_str_set(&h->key, "Location");
  h->value = *location;

  return NGX_HTTP_MOVED_TEMPORARILY;
}


static void
append_user_id(ngx_http_request_t *r, ngx_str_t *user_id)
{
  ngx_table_elt_t *h;
  h = ngx_list_push(&r->headers_in.headers);
  h->hash = 1;
  ngx_str_set(&h->key, "X-User-Id");
  h->value = *user_id;
}


static ngx_int_t
search_headers(ngx_http_request_t *r, auth_token_loc_conf_t *location_conf, ngx_str_t *token)
{
  ngx_list_part_t *part;
  ngx_table_elt_t *h;
  ngx_uint_t i;

  part = &r->headers_in.headers.part;
  h = part->elts;

  for (i = 0; /**/; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }

      part = part->next;
      h = part->elts;
      i = 0;
    }

    if (ngx_strncmp(h[i].key.data, location_conf->header_name.data, h[i].key.len) == 0) {
      token->data = h[i].value.data;
      token->len  = h[i].value.len;
      return NGX_OK;
    }
  }

  return NGX_DECLINED;
}


static ngx_int_t
header_lookup(ngx_http_request_t *r, auth_token_loc_conf_t *location_conf, ngx_str_t *token)
{
  ngx_int_t result = search_headers(r, location_conf, token);
  if (result == NGX_DECLINED) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Could not locate header %V", &location_conf->header_name);
    return NGX_DECLINED;
  } else {
    return NGX_OK;
  }
}


static ngx_int_t
cookie_lookup(ngx_http_request_t *r, auth_token_loc_conf_t *location_conf, ngx_str_t *token)
{
  ngx_int_t cookie_location;
  cookie_location = ngx_http_parse_multi_header_lines(&r->headers_in.cookies, &location_conf->cookie_name, token);
  if (cookie_location == NGX_DECLINED) {
    return NGX_DECLINED;
  } else {
    return NGX_OK;
  }
}


static ngx_int_t
ngx_http_auth_token_handler(ngx_http_request_t *r)
{
  if (r->main->internal) {
    return NGX_DECLINED;
  }

  auth_token_loc_conf_t *location_conf = ngx_http_get_module_loc_conf(r, ngx_http_auth_token_module);

  if (!location_conf->enabled || location_conf->enabled == NGX_CONF_UNSET) {
    return NGX_DECLINED;
  }

  auth_token_main_conf_t *main_conf = ngx_http_get_module_main_conf(r, ngx_http_auth_token_module);

  ngx_str_t auth_token;
  ngx_int_t search_result;
  if (location_conf->header_name.len != 0) {
    search_result = header_lookup(r, location_conf, &auth_token);
  } else {
    search_result = cookie_lookup(r, location_conf, &auth_token);
  }

  if (search_result == NGX_DECLINED) {
    return redirect(r, &location_conf->redirect_location);
  }

  ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Looking up user by auth token %V", &auth_token);

  ngx_str_t user_id;
  ngx_int_t lookup_result = lookup_user(main_conf, &auth_token, &user_id);

  if (lookup_result == NGX_DECLINED) {
    return redirect(r, &location_conf->redirect_location);
  } else {
    append_user_id(r, &user_id);
    return NGX_DECLINED;
  }
}


static ngx_int_t
ngx_http_auth_token_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt *h;
  ngx_http_core_main_conf_t *cmcf;

  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

  h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }

  *h = ngx_http_auth_token_handler;

  return NGX_OK;
}


static void*
ngx_http_auth_token_create_main_conf(ngx_conf_t *cf)
{
  auth_token_main_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(auth_token_main_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  conf->redis_port = NGX_CONF_UNSET_UINT;

  return conf;
}

static void*
ngx_http_auth_token_create_loc_conf(ngx_conf_t *cf)
{
  auth_token_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(auth_token_loc_conf_t));
  if (conf == NULL) {
    return NULL;
  }

  conf->enabled = NGX_CONF_UNSET;

  return conf;
}

static char*
ngx_http_auth_token_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
  auth_token_loc_conf_t *prev = (auth_token_loc_conf_t*)parent;
  auth_token_loc_conf_t *conf = (auth_token_loc_conf_t*)child;

  ngx_conf_merge_value(conf->enabled, prev->enabled, 0);
  ngx_conf_merge_str_value(conf->redirect_location, prev->redirect_location, "");
  ngx_conf_merge_str_value(conf->cookie_name, prev->cookie_name, "");
  ngx_conf_merge_str_value(conf->header_name, prev->header_name, "");

  return NGX_CONF_OK;
}

static ngx_command_t ngx_http_auth_token_commands[] = {
  {
    ngx_string("auth_token_redis_host"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(auth_token_main_conf_t, redis_host),
    NULL
  },
  {
    ngx_string("auth_token_redis_port"),
    NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_MAIN_CONF_OFFSET,
    offsetof(auth_token_main_conf_t, redis_port),
    NULL
  },
  {
    ngx_string("auth_token_cookie_name"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(auth_token_loc_conf_t, cookie_name),
    NULL
  },
  {
    ngx_string("auth_token_header_name"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(auth_token_loc_conf_t, header_name),
    NULL
  },
  {
    ngx_string("auth_token_redirect_location"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(auth_token_loc_conf_t, redirect_location),
    NULL
  },
  {
    ngx_string("auth_token_enabled"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(auth_token_loc_conf_t, enabled),
    NULL
  },

  ngx_null_command
};


static ngx_http_module_t ngx_http_auth_token_module_ctx = {
  NULL,                                 /* preconfiguration */
  ngx_http_auth_token_init,             /* postconfiguration */
  ngx_http_auth_token_create_main_conf, /* create main configuration */
  NULL,                                 /* init main configuration */
  NULL,                                 /* create server configuration */
  NULL,                                 /* merge server configuration */
  ngx_http_auth_token_create_loc_conf,  /* create location configuration */
  ngx_http_auth_token_merge_loc_conf    /* merge location configuration */
};


ngx_module_t ngx_http_auth_token_module = {
  NGX_MODULE_V1,
  &ngx_http_auth_token_module_ctx, /* module context */
  ngx_http_auth_token_commands,    /* module directives */
  NGX_HTTP_MODULE,                 /* module type */
  NULL,                            /* init master */
  NULL,                            /* init module */
  NULL,                            /* init process */
  NULL,                            /* init thread */
  NULL,                            /* exit thread */
  NULL,                            /* exit process */
  NULL,                            /* exit master */
  NGX_MODULE_V1_PADDING
};
