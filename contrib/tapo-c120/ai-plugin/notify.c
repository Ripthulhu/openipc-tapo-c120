#define _POSIX_C_SOURCE 200809L
#include "notify.h"
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct json_object J;
#define QUEUE_LIMIT 8
static const char *categories[] = {"person", "pet", "vehicle", "bird", "bark", "meow", "cry", "glass"};
static J *config, *queue[QUEUE_LIMIT], *active;
static unsigned queued, delivered, failed, dropped, suppressed, visual_notified;
static double last_event[8], queued_at[QUEUE_LIMIT];
static double last_seen[4];
static CURLM *multi;
static CURL *request;
static struct curl_slist *headers;
static char *body;
static long last_http;
static const char *status = "Disabled";

static J *field(J *o, const char *key) { return json_object_object_get(o, key); }
static const char *string(J *o, const char *key) { return json_object_get_string(field(o, key)); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec+t.tv_nsec/1e9; }

J *notify_defaults(void)
{
    return json_tokener_parse("{\"enabled\":false,\"format\":\"webhook\",\"url\":\"\",\"token\":\"\","
        "\"cooldownSeconds\":60,\"categories\":[\"person\",\"pet\",\"vehicle\"]}");
}

static int safe_string(J *value, size_t limit)
{
    if (!json_object_is_type(value, json_type_string)) return 0;
    const char *s = json_object_get_string(value);
    size_t length = json_object_get_string_len(value);
    if (length > limit || strlen(s) != length) return 0;
    for (size_t i = 0; i < length; ++i) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] == 127) return 0;
    return 1;
}

int notify_valid(J *o)
{
    if (!o || !json_object_is_type(o, json_type_object) || json_object_object_length(o) != 6 ||
        !json_object_is_type(field(o,"enabled"),json_type_boolean) ||
        !safe_string(field(o,"format"),16) || !safe_string(field(o,"url"),512) ||
        !safe_string(field(o,"token"),512) ||
        !json_object_is_type(field(o,"cooldownSeconds"),json_type_int)) return 0;
    int64_t cooldown = json_object_get_int64(field(o,"cooldownSeconds"));
    if (cooldown < 10 || cooldown > 3600 ||
        (strcmp(string(o,"format"),"webhook") && strcmp(string(o,"format"),"ntfy"))) return 0;
    J *list = field(o,"categories"); unsigned mask = 0;
    if (!json_object_is_type(list,json_type_array) || json_object_array_length(list) > 8) return 0;
    for (size_t n = 0; n < json_object_array_length(list); ++n) {
        J *item = json_object_array_get_idx(list,n); unsigned bit = 0;
        if (!safe_string(item,16)) return 0;
        for (int i = 0; i < 8; ++i) if (!strcmp(json_object_get_string(item),categories[i])) bit = 1u << i;
        if (!bit || (mask & bit)) return 0;
        mask |= bit;
    }
    if (json_object_get_boolean(field(o,"enabled")) && (!mask || !*string(o,"url"))) return 0;
    if (!*string(o,"url")) return 1;
    CURLU *url = curl_url(); char *scheme = NULL, *host = NULL, *user = NULL, *fragment = NULL;
    int valid = url && !curl_url_set(url,CURLUPART_URL,string(o,"url"),0) &&
        !curl_url_get(url,CURLUPART_SCHEME,&scheme,0) &&
        (!strcmp(scheme,"http") || !strcmp(scheme,"https")) &&
        !curl_url_get(url,CURLUPART_HOST,&host,0) && *host &&
        curl_url_get(url,CURLUPART_USER,&user,0) == CURLUE_NO_USER &&
        curl_url_get(url,CURLUPART_FRAGMENT,&fragment,0) == CURLUE_NO_FRAGMENT;
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(fragment); curl_url_cleanup(url);
    return valid;
}

static void finish_request(void)
{
    if (request && multi) curl_multi_remove_handle(multi,request);
    curl_easy_cleanup(request); request = NULL;
    curl_slist_free_all(headers); headers = NULL;
    free(body); body = NULL; json_object_put(active); active = NULL;
}

void notify_stop(void)
{
    finish_request();
    if (multi) curl_multi_cleanup(multi);
    multi = NULL;
    for (unsigned i = 0; i < queued; ++i) json_object_put(queue[i]);
    queued = 0; json_object_put(config); config = NULL; status = "Disabled";
}

void notify_configure(J *o)
{
    notify_stop(); config = json_object_get(o);
    status = json_object_get_boolean(field(config,"enabled")) ? "Ready" : "Disabled";
}

void notify_observe(const char *label)
{
    /* Refresh on every accepted object, not only the detector's arrival events. */
    double t = now();
    for (int i = 0; i < 4; ++i) if (!strcmp(label,categories[i])) {
        if (last_seen[i] && t-last_seen[i] >= json_object_get_int(field(config,"cooldownSeconds")))
            visual_notified &= ~(1u << i);
        last_seen[i] = t;
        return;
    }
}

void notify_prime(const char *label)
{
    /* Switching models must not announce everything already in view as a new arrival. */
    for (int i=0;i<4;++i) if (!strcmp(label,categories[i])) { last_seen[i]=now(); visual_notified|=1u<<i; return; }
}

void notify_event(J *event)
{
    if (!json_object_get_boolean(field(config,"enabled"))) return;
    const char *label = string(event,"label");
    int category = -1;
    for (int i = 0; label && i < 8; ++i) if (!strcmp(label,categories[i])) category = i;
    int test = label && !strcmp(label,"test") && !strcmp(string(event,"type"),"test");
    if (category < 0 && !test) return;
    J *list = field(config,"categories"); int selected = 0;
    for (size_t i = 0; i < json_object_array_length(list); ++i)
        if (!strcmp(label,json_object_get_string(json_object_array_get_idx(list,i)))) selected = 1;
    double t = now();
    if (!test && !selected) return;
    if (!test && ((category < 4 && (visual_notified & (1u << category))) ||
        (last_event[category] && t-last_event[category] < json_object_get_int(field(config,"cooldownSeconds"))))) {
        ++suppressed; return;
    }
    if (queued == QUEUE_LIMIT) { ++dropped; return; }
    queue[queued] = json_object_get(event); queued_at[queued++] = t;
    if (!test) {
        last_event[category] = t;
        if (category < 4) visual_notified |= 1u << category;
    }
}

static int header(const char *value)
{
    struct curl_slist *next = curl_slist_append(headers,value);
    if (!next) return -1;
    headers = next; return 0;
}

static size_t discard(char *data, size_t size, size_t count, void *unused)
{
    (void)data; (void)unused;
    return size * count;
}

void notify_poll(void)
{
    if (!json_object_get_boolean(field(config,"enabled"))) return;
    if (!request && queued) {
        active = queue[0]; double age = now()-queued_at[0];
        --queued;
        memmove(queue,queue+1,queued*sizeof(*queue)); memmove(queued_at,queued_at+1,queued*sizeof(*queued_at));
        if (age > 30) { ++dropped; finish_request(); return; }
        char hostname[64] = "camera", message[160], authorization[540];
        gethostname(hostname,sizeof(hostname)-1); hostname[sizeof(hostname)-1] = 0;
        for (char *p = hostname; *p; ++p) if ((unsigned char)*p < 32 || (unsigned char)*p == 127) *p = '_';
        snprintf(message,sizeof(message),"%s detected on %s (%.0f%%)",string(active,"label"),hostname,
            json_object_get_double(field(active,"confidence"))*100);
        if (!strcmp(string(config,"format"),"ntfy")) {
            body = strdup(message);
            if (header("Content-Type: text/plain; charset=utf-8") || header("Title: Camera AI detection")) goto failure;
        } else {
            J *payload = json_object_new_object();
            json_object_object_add(payload,"schemaVersion",json_object_new_int(1));
            json_object_object_add(payload,"source",json_object_new_string("ai"));
            json_object_object_add(payload,"camera",json_object_new_string(hostname));
            json_object_object_add(payload,"event",json_object_get(active));
            body = strdup(json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN)); json_object_put(payload);
            if (header("Content-Type: application/json")) goto failure;
        }
        if (*string(config,"token")) {
            snprintf(authorization,sizeof(authorization),"Authorization: Bearer %s",string(config,"token"));
            if (header(authorization)) goto failure;
        }
        if (!multi) multi = curl_multi_init();
        request = curl_easy_init();
        last_http = 0;
        if (!body || !headers || !multi || !request) goto failure;
        curl_easy_setopt(request,CURLOPT_URL,string(config,"url"));
        curl_easy_setopt(request,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(request,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(request,CURLOPT_NOPROXY,"*");
        curl_easy_setopt(request,CURLOPT_CONNECTTIMEOUT_MS,1000L);
        curl_easy_setopt(request,CURLOPT_TIMEOUT_MS,3000L);
        curl_easy_setopt(request,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(request,CURLOPT_POSTFIELDS,body);
        curl_easy_setopt(request,CURLOPT_POSTFIELDSIZE,(long)strlen(body));
        curl_easy_setopt(request,CURLOPT_WRITEFUNCTION,discard);
        curl_easy_setopt(request,CURLOPT_FOLLOWLOCATION,0L);
        if (curl_multi_add_handle(multi,request)) goto failure;
        status = "Sending";
    }
    if (!request) return;
    int running, remaining;
    if (curl_multi_perform(multi,&running) != CURLM_OK) goto failure;
    CURLMsg *message = curl_multi_info_read(multi,&remaining);
    if (!message) return;
    curl_easy_getinfo(request,CURLINFO_RESPONSE_CODE,&last_http);
    if (message->data.result != CURLE_OK || last_http < 200 || last_http >= 300) goto failure;
    ++delivered; status = "Delivered"; finish_request(); return;
failure:
    ++failed; status = "Delivery failed"; finish_request();
}

J *notify_state(void)
{
    J *o = json_object_new_object();
    json_object_object_add(o,"status",json_object_new_string(status));
    json_object_object_add(o,"queued",json_object_new_int(queued+(request != NULL)));
    json_object_object_add(o,"delivered",json_object_new_int64(delivered));
    json_object_object_add(o,"failed",json_object_new_int64(failed));
    json_object_object_add(o,"dropped",json_object_new_int64(dropped));
    json_object_object_add(o,"suppressed",json_object_new_int64(suppressed));
    json_object_object_add(o,"lastHttpStatus",json_object_new_int(last_http));
    return o;
}
