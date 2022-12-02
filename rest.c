#include "rest.h"
#include "timer.h"
#include "can.h"
#include "mb.h"
#include "mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <json-c/json.h>
#include <curl/curl.h>

typedef struct {
  struct json_tokener *tok;
  json_object *json;
} REST_GET_STATE_T;

static int poll_timer;

static json_object *rest_get_json(const char *url, long timeout);
static size_t rest_get_json_callback (void *contents, size_t size, size_t nmemb, void *userp);
static json_object *json_path_lookup(json_object *root, const char *path);
static json_object *json_path_lookup_recursive(json_object *root, char *path);

int rest_startup(void) {
  // initialize libcurl
  if (curl_global_init(CURL_GLOBAL_ALL)) {
    syslog(LOG_ERR, "Failed to initialize libcurl.");
    goto fail0;
  }

  poll_timer = 0;

  return 0;

fail0:
  return -1;
}

void rest_shutdown(void) {
  // cleanup libcurl
  curl_global_cleanup();
}

int rest_task(void) {
  const IOCONF_CHAN_T *chan;
  const char *url;
  json_object *json;
  json_object *json_val;
  double val;

  // check poll period
  poll_timer += TIMER_PERIOD_MS;
  if (poll_timer < REST_POLL_PERIOD_MS) {
    return 0;
  }
  poll_timer -= REST_POLL_PERIOD_MS;

  // process items
  for (url = NULL, json = NULL, chan = ioconf_tab; chan->topic != NULL ; chan++) {
    //skip items without rest config
    if (chan->rest.url == NULL || chan->rest.path == NULL) {
      continue;
    }

    // new url -> execute a get
    if (url == NULL || strcmp(chan->rest.url, url) != 0) {
      url = chan->rest.url;
      json_object_put(json);
      json = rest_get_json(url, REST_POLL_TIMEOUT_SEC);
    }

    // no valid json -> skip item
    if (json == NULL) {
      continue;
    }

    // lookup path, skip if not found
    // convert value to double, if found
    json_val = json_path_lookup(json, chan->rest.path);
    switch (json_object_get_type(json_val)) {
      case json_type_boolean:
        val = json_object_get_boolean(json_val) ? 1.0 : 0.0;
        break;
      case json_type_int:
        val = (double) json_object_get_int(json_val);
        break;
      case json_type_double:
        val = json_object_get_double(json_val);
        break;
      default:
        continue;
    }

    // do scaling
    val = val *chan->rest.scale + chan->rest.offset;

    // send CAN message
    can_send_chan(chan, val);

    // write MODBUS
    mb_write_chan(chan, val);

    // send MQTT topic
    mqtt_publish_chan(chan, val);
  }

  json_object_put(json);

  return 0;
}

static json_object *rest_get_json(const char *url, long timeout) {
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

  // set timeout
  curl_easy_setopt(ch, CURLOPT_TIMEOUT, timeout);

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
  json_object *val;

  if (path == NULL) {
    return NULL;
  }

  sep = strchr(path, '.');
  if (sep != NULL) {
    *sep = 0;
  }

  if (!json_object_object_get_ex(root, path, &val)) {
    return NULL;
  }

  if (sep == NULL) {
    return val;
  }

  return json_path_lookup_recursive(val, sep + 1);
}

