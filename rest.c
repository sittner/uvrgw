#include "rest.h"
#include "timer.h"
#include "can.h"
#include "mb.h"
#include "mqtt.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <json-c/json.h>
#include <curl/curl.h>
#include <ctype.h>

typedef struct {
  struct json_tokener *tok;
  json_object *json;
} REST_GET_STATE_T;

static int conns_count;
static REST_CONN_T *conns;

static int conn_configure(cfg_t *cfg, void *ctx, void *child);
static int value_configure(cfg_t *cfg, void *ctx, void *child);

static int conn_task(REST_CONN_T *conn);

static json_object *rest_get_json(const char *url, const char *user, const char *pwd, long timeout);
static size_t rest_get_json_callback (void *contents, size_t size, size_t nmemb, void *userp);
static json_object *json_path_lookup(json_object *root, const char *path);
static json_object *json_path_lookup_recursive(json_object *root, char *path);

static int parse_index(const char *s) {
  char *p;
  int val;

  val = (int)strtol(s, &p, 10);
  if (*p != 0) {
    return -1;
  }

  if (val < 0) {
    return -1;
  }

  return val;
}

void rest_init(void) {
  conns_count = 0;
  conns = NULL;
}

int rest_configure(cfg_t *cfg) {
  return uvrgw_conf_config_childs(cfg, "json", &conns_count, (void **) &conns, sizeof(REST_CONN_T), NULL, conn_configure);
}

static int conn_configure(cfg_t *cfg, void *ctx, void *child) {
  REST_CONN_T *conn = (REST_CONN_T *) child;

  conn->url = uvrgw_conf_strdup(cfg_getstr(cfg, "url"));
  conn->interval = cfg_getint(cfg, "interval");
  conn->timeout = cfg_getint(cfg, "timeout");
  conn->user = uvrgw_conf_strdup(cfg_getstr(cfg, "user"));
  conn->pwd = uvrgw_conf_strdup(cfg_getstr(cfg, "pwd"));

  if (conn->url == NULL) {
    syslog(LOG_ERR, "json url name not given.");
    return -1;
  }

  return uvrgw_conf_config_childs(cfg, "value", &conn->values_count, (void **) &conn->values, sizeof(REST_VAL_T), conn, value_configure);
}

static int value_configure(cfg_t *cfg, void *ctx, void *child) {
  REST_VAL_T *val = (REST_VAL_T *) child;

  val->conn = (REST_CONN_T *) ctx;

  val->name = uvrgw_conf_strdup(cfg_title(cfg));
  val->path = uvrgw_conf_strdup(cfg_getstr(cfg, "path"));
  val->scale = cfg_getfloat(cfg, "scale");
  val->offset = cfg_getfloat(cfg, "offset");

  if (val->path == NULL) {
    syslog(LOG_ERR, "json value path not given.");
    return -1;
  }

  val->disp = uvrgw_conf_get_dispatcher(val->name, false);

  return 0;
}

void rest_unconfigure(void) {
  REST_CONN_T *conn;
  int conn_idx;
  REST_VAL_T *val;
  int val_idx;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
      free((void *) val->name);
      free((void *) val->path);
    }
    free((void *) conn->url);
    free((void *) conn->user);
    free((void *) conn->pwd);
    free(conn->values);
  }
  free(conns);
}

int rest_startup(void) {
  // initialize libcurl
  if (curl_global_init(CURL_GLOBAL_ALL)) {
    syslog(LOG_ERR, "Failed to initialize libcurl.");
    goto fail0;
  }

  return 0;

fail0:
  return -1;
}

void rest_shutdown(void) {
  // cleanup libcurl
  curl_global_cleanup();
}

int rest_task(void) {
  REST_CONN_T *conn;
  int conn_idx;

  for (conn = conns, conn_idx = 0; conn_idx < conns_count; conn++, conn_idx++) {
    if (conn_task(conn) < 0) {
      return -1;
    }
  }

  return 0;
}

static int conn_task(REST_CONN_T *conn) {
  int64_t now;
  REST_VAL_T *val;
  int val_idx;
  json_object *json;
  json_object *json_val;
  enum json_type type;
  double f;

  // check poll period
  now = utl_get_ticks();
  if (conn->next_poll > now) {
    return 0;
  }
  conn->next_poll = now + conn->interval;

  // load json from server
  json = rest_get_json(conn->url, conn->user, conn->pwd, conn->timeout);
  if (json == NULL) {
    return 0;
  }

  // process values
  for (val = conn->values, val_idx = 0; val_idx < conn->values_count; val++, val_idx++) {
    // lookup path, skip if not found
    // convert value to double, if found
    json_val = json_path_lookup(json, val->path);
    type = json_object_get_type(json_val);
    switch (type) {
      case json_type_boolean:
        f = json_object_get_boolean(json_val) ? 1.0 : 0.0;
        break;
      case json_type_int:
        f = (double) json_object_get_int(json_val) * val->scale + val->offset;
        break;
      case json_type_double:
        f = json_object_get_double(json_val) * val->scale + val->offset;
        break;
      case json_type_null:
        syslog(LOG_WARNING, "Failed lookup json path '%s' for url '%s'.", val->path, conn->url);
        continue;
      default:
        syslog(LOG_WARNING, "Invalid value type %d of '%s' for url '%s'.", type, val->path, conn->url);
        continue;
    }

    // dispatch value
    uvrgw_conf_disp_val(val->disp, val, f);
  }

  json_object_put(json);

  return 0;
}

static json_object *rest_get_json(const char *url, const char *user, const char *pwd, long timeout) {
  CURL *ch;
  REST_GET_STATE_T state = { .tok = NULL, .json = NULL } ;
  CURLcode err;
  struct curl_slist *headers = NULL;

  ch = curl_easy_init();
  if (ch == NULL) {
    syslog(LOG_ERR, "Failed to create curl handle in rest_get_json");
    goto fail0;
  }

  state.tok = json_tokener_new();
  if (state.tok == NULL) {
    syslog(LOG_ERR, "Failed to create json tokener in rest_get_json");
    goto fail1;
  }

  // set method and request headers
  headers = curl_slist_append(headers, "Accept: application/json");
  curl_easy_setopt(ch, CURLOPT_CUSTOMREQUEST, "GET");
  curl_easy_setopt(ch, CURLOPT_HTTPHEADER, headers);

  // set url to fetch
  curl_easy_setopt(ch, CURLOPT_URL, url);

  // set default user agent
  curl_easy_setopt(ch, CURLOPT_USERAGENT, "libcurl-agent/1.0");

  // set auth data
  if (user != NULL) {
    curl_easy_setopt(ch, CURLOPT_USERNAME, user);
  }
  if (pwd != NULL) {
    curl_easy_setopt(ch, CURLOPT_PASSWORD, pwd);
  }

  // set timeout
  curl_easy_setopt(ch, CURLOPT_TIMEOUT_MS, timeout);

  // set calback function
  curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, rest_get_json_callback);

  // pass state pointer
  curl_easy_setopt(ch, CURLOPT_WRITEDATA, &state);

  // perform the get
  err = curl_easy_perform(ch);
  if (err != CURLE_OK) {
    syslog(LOG_ERR, "Failed to perform GET of URL '%s' (error %d [%s]).",
      url, err, curl_easy_strerror(err));
    goto fail2;
  }

  // check type
  if (json_object_get_type(state.json) != json_type_object) {
    syslog(LOG_ERR, "No JSON object returned from Server in rest_get_json (URL='%s')", url);
    json_object_put(state.json);
    state.json = NULL;
  }

fail2:
  curl_slist_free_all(headers);
  json_tokener_free(state.tok);
fail1:
  curl_easy_cleanup(ch);
fail0:
  return state.json;
}

static size_t rest_get_json_callback (void *contents, size_t size, size_t nmemb, void *userp) {
  size_t realsize = size * nmemb;
  REST_GET_STATE_T *state = (REST_GET_STATE_T *) userp;

  // skip extra data
  if (state->json != NULL) {
    return realsize;
  }

  // parse JSON chunk
  state->json = json_tokener_parse_ex(state->tok, contents, realsize);
  if (state->tok->err != json_tokener_continue && state->tok->err != json_tokener_success) {
    syslog(LOG_ERR, "Failed to parse JSON in rest_write_callback (json_tokener_error=%d)", state->tok->err);
    return 0;
  }

  return realsize;
}

static json_object *json_path_lookup(json_object *root, const char *path) {
  char *tmp;
  json_object *val;

  tmp = strdup(path);
  val = json_path_lookup_recursive(root, tmp);
  free(tmp);
  return val;
}

static json_object *json_path_lookup_recursive(json_object *root, char *path) {
  char *sep;
  int i;
  json_object *val;

  if (path == NULL) {
    return NULL;
  }

  sep = strchr(path, '.');
  if (sep != NULL) {
    *sep = 0;
  }

  if (json_object_is_type(root, json_type_array)) {
    i = parse_index(path);
    if (i < 0 || (val = json_object_array_get_idx(root, i)) == NULL) {
      return NULL;
    }
  } else {
    if (!json_object_object_get_ex(root, path, &val)) {
      return NULL;
    }
  }

  if (sep == NULL) {
    return val;
  }

  return json_path_lookup_recursive(val, sep + 1);
}

