#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <mosquitto.h>
#include <mqtt_protocol.h>

#ifndef MQTT_ROOT
#define MQTT_ROOT ""
#endif
#ifndef CAMERA_ORIGIN
#define CAMERA_ORIGIN "http://127.0.0.1"
#endif
#define CONFIG MQTT_ROOT "/etc/openipc-mqtt.json"
#define AI_CONFIG MQTT_ROOT "/etc/c120-ai.json"
#define AI_STATE MQTT_ROOT "/run/c120-ai-state.json"
#define LATEST MQTT_ROOT "/run/c120-recording-latest.json"
#define AP MQTT_ROOT "/run/c120-setup-ap.active"
#define VERSION "1.0"
typedef struct json_object J;
static struct mosquitto *client;
static CURL *http;
static J *config, *previous;
static volatile sig_atomic_t stopping;
static int connected, need_discovery, baseline;
static char id[64], base[160], discovery[256], previous_boot[48], latest_id[64];
static unsigned long long event_id;
static double next_poll, next_config, last_publish, motion_until;
static unsigned long long motion_count;
static int motion_baseline;
static J *native_config;
static const char *labels[]={"person","pet","vehicle","bird","bark","meow","cry","glass"};
static const struct { const char *name; unsigned mask; } categories[]={
    {"People",1},{"Pets",2},{"People and pets",3},{"Vehicles",4},{"Birds",8},
    {"People, pets and vehicles",7},{"All detections",255},{"Custom",0}
};
static J *get(J *o,const char *key) { return json_object_object_get(o,key); }
static const char *str(J *o,const char *key) { const char *s=json_object_get_string(get(o,key)); return s?s:""; }
static void text(J *o,const char *key,const char *s)
{
    char expanded[256];
    if (!strncmp(s,"~/",2) && (strstr(key,"topic") || !strcmp(key,"topic"))) {
        snprintf(expanded,sizeof(expanded),"%s/%s",base,s+2); s=expanded;
    }
    json_object_object_add(o,key,json_object_new_string(s));
}
static void flag(J *o,const char *key,int value) { json_object_object_add(o,key,json_object_new_boolean(value)); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static J *parse(const char *s,size_t n)
{
    if (!n || n>65536) return NULL;
    struct json_tokener *t=json_tokener_new(); if (!t) return NULL;
    json_tokener_set_flags(t,JSON_TOKENER_STRICT);
    J *o=json_tokener_parse_ex(t,s,(int)n); size_t end=json_tokener_get_parse_end(t);
    if (json_tokener_get_error(t)==json_tokener_continue && end==n) o=json_tokener_parse_ex(t," ",1);
    while (end<n && isspace((unsigned char)s[end])) ++end;
    if (json_tokener_get_error(t)!=json_tokener_success || end!=n) { json_object_put(o); o=NULL; }
    json_tokener_free(t); return o;
}
static J *read_json(const char *path)
{
    FILE *f=fopen(path,"r"); if (!f) return NULL;
    char buf[65537]; size_t n=fread(buf,1,sizeof(buf),f); int bad=ferror(f) || n==sizeof(buf);
    fclose(f); return bad?NULL:parse(buf,n);
}
struct buffer { char data[65537]; size_t length; };
static size_t receive(char *s,size_t size,size_t count,void *arg)
{
    struct buffer *b=arg; size_t n=size*count;
    if (n>sizeof(b->data)-1-b->length) return 0;
    memcpy(b->data+b->length,s,n); b->length+=n; b->data[b->length]=0; return n;
}
static int request(const char *path,J *body,struct buffer *out)
{
    char url[512]; if (snprintf(url,sizeof(url),"%s%s",CAMERA_ORIGIN,path)>=(int)sizeof(url)) return -1;
    curl_easy_reset(http); out->length=0; out->data[0]=0;
    curl_easy_setopt(http,CURLOPT_URL,url); curl_easy_setopt(http,CURLOPT_NOPROXY,"*");
    curl_easy_setopt(http,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(http,CURLOPT_CONNECTTIMEOUT_MS,200L);
    curl_easy_setopt(http,CURLOPT_TIMEOUT_MS,700L); curl_easy_setopt(http,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(http,CURLOPT_WRITEDATA,out); curl_easy_setopt(http,CURLOPT_BUFFERSIZE,4096L);
    struct curl_slist *headers=NULL;
    if (body) {
        headers=curl_slist_append(NULL,"Content-Type: application/json"); curl_easy_setopt(http,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(http,CURLOPT_POSTFIELDS,json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN));
    }
    CURLcode rc=curl_easy_perform(http); long status=0; curl_easy_getinfo(http,CURLINFO_RESPONSE_CODE,&status);
    curl_slist_free_all(headers); return rc==CURLE_OK && status>=200 && status<300?0:-1;
}
static J *api(const char *path)
{ struct buffer b; return request(path,NULL,&b)?NULL:parse(b.data,b.length); }
static void publish(const char *suffix,const char *payload,int retain)
{
    if (!connected) return;
    char topic[256]; snprintf(topic,sizeof(topic),"%s/%s",base,suffix);
    int rc=mosquitto_publish(client,NULL,topic,(int)strlen(payload),payload,0,retain);
    if (rc) syslog(LOG_WARNING,"MQTT publish failed: %s",mosquitto_strerror(rc));
}
static void publish_json(const char *suffix,J *o,int retain)
{ publish(suffix,json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN),retain); }

struct setting { const char *key,*name,*group,*field; int low,high; };
static const struct setting settings[]={
    {"motion_enabled","Motion detection","motionDetect","enabled",-1,0},
    {"motion_sensitivity","Motion sensitivity","motionDetect","sensitivity",0,8},
    {"ai_enabled","AI detection","ai","enabled",-1,0},
    {"ai_confidence","AI confidence","ai","confidence",5,95},
    {"ai_regions","AI uses motion regions","ai","motionRegions",-1,0},
    {"sound_enabled","Sound detection","ai","soundEnabled",-1,0},
    {"sound_sensitivity","Sound sensitivity","ai","soundSensitivity",0,2},
    {"recording_enabled","AI recording","recording","enabled",-1,0},
    {"record_seconds","Clip duration","recording","seconds",1,600}
};
static J *component(J *components,const char *key,const char *name,const char *platform,const char *availability)
{
    J *o=json_object_new_object(); json_object_object_add(components,key,o);
    text(o,"p",platform); text(o,"name",name);
    char unique[128],tpl[160]; snprintf(unique,sizeof(unique),"%s_%s",id,key); text(o,"unique_id",unique);
    snprintf(tpl,sizeof(tpl),"{{ value_json.%s }}",key); text(o,"value_template",tpl);
    if (availability) {
        J *a=json_object_new_array(),*main=json_object_new_object(),*specific=json_object_new_object();
        text(main,"topic","~/availability"); text(specific,"topic",availability);
        json_object_array_add(a,main); json_object_array_add(a,specific);
        json_object_object_add(o,"availability",a); text(o,"availability_mode","all");
    }
    return o;
}
static void boolean_component(J *o)
{
    const char *tpl=str(o,"value_template"); char converted[220];
    if (sscanf(tpl,"{{ value_json.%159[^ ]",converted)!=1) return;
    char out[256]; snprintf(out,sizeof(out),"{{ 'ON' if value_json.%s else 'OFF' }}",converted);
    text(o,"value_template",out); text(o,"payload_on","ON"); text(o,"payload_off","OFF");
}
static J *discovery_payload(void)
{
    J *o=json_object_new_object(),*dev=json_object_new_object(),*origin=json_object_new_object(),*parts=json_object_new_object();
    text(o,"state_topic","~/state");
    text(dev,"identifiers",id); text(dev,"name",str(config,"name")); text(dev,"manufacturer","OpenIPC");
    text(dev,"model","Tapo C120"); if (*str(config,"url")) text(dev,"configuration_url",str(config,"url"));
    text(origin,"name","OpenIPC MQTT"); text(origin,"sw_version",VERSION);
    text(origin,"support_url","https://github.com/Ripthulhu/openipc-tapo-c120");
    json_object_object_add(o,"device",dev); json_object_object_add(o,"origin",origin); json_object_object_add(o,"components",parts);
    J *c=component(parts,"motion","Motion","binary_sensor","~/camera_available"); boolean_component(c); text(c,"device_class","motion");
    c=component(parts,"night","Night mode","switch","~/camera_available"); boolean_component(c); text(c,"command_topic","~/set/night");
    c=component(parts,"floodlight","Floodlight","switch","~/camera_available"); boolean_component(c); text(c,"command_topic","~/set/floodlight");
    for (int i=0;i<8;++i) {
        c=component(parts,labels[i],labels[i],"binary_sensor",i<4?"~/visual_available":"~/sound_available");
        boolean_component(c); text(c,"device_class",i<4?"occupancy":"sound");
    }
    for (unsigned i=0;i<sizeof(settings)/sizeof(settings[0]);++i) {
        const struct setting *s=&settings[i]; int boolean=s->low<0;
        c=component(parts,s->key,s->name,boolean?"switch":"number",!strcmp(s->group,"motionDetect")?"~/camera_available":"~/ai_available");
        char command[96]; snprintf(command,sizeof(command),"~/set/%s",s->key); text(c,"command_topic",command); text(c,"entity_category","config");
        if (boolean) boolean_component(c);
        else {
            json_object_object_add(c,"min",json_object_new_int(s->low)); json_object_object_add(c,"max",json_object_new_int(s->high));
            json_object_object_add(c,"step",json_object_new_int(1)); text(c,"mode","box");
            if (!strcmp(s->key,"ai_confidence")) text(c,"unit_of_measurement","%");
            if (!strcmp(s->key,"record_seconds")) text(c,"unit_of_measurement","s");
        }
    }
    c=component(parts,"record_categories","Recording categories","select","~/ai_available");
    text(c,"command_topic","~/set/record_categories"); text(c,"entity_category","config");
    J *options=json_object_new_array();
    for (unsigned i=0;i<sizeof(categories)/sizeof(categories[0]);++i) json_object_array_add(options,json_object_new_string(categories[i].name));
    json_object_object_add(c,"options",options);
    c=component(parts,"recording","Recording","binary_sensor","~/ai_available"); boolean_component(c); text(c,"device_class","running");
    c=component(parts,"ai_status","AI status","sensor","~/ai_available"); text(c,"entity_category","diagnostic");
    c=component(parts,"recording_status","Recording status","sensor","~/ai_available"); text(c,"entity_category","diagnostic");
    c=component(parts,"record_clip","Record clip","button","~/ai_available");
    json_object_object_del(c,"value_template"); text(c,"command_topic","~/set/record_clip"); text(c,"payload_press","PRESS");
    c=component(parts,"detection_event","Detection","event","~/ai_available");
    json_object_object_del(c,"value_template"); text(c,"state_topic","~/event/detection");
    J *types=json_object_new_array(); for (int i=0;i<8;++i) json_object_array_add(types,json_object_new_string(labels[i]));
    json_object_object_add(c,"event_types",types);
    c=component(parts,"recording_event","New recording","event","~/ai_available");
    json_object_object_del(c,"value_template"); text(c,"state_topic","~/event/recording");
    types=json_object_new_array(); json_object_array_add(types,json_object_new_string("complete")); json_object_object_add(c,"event_types",types);
    return o;
}
static int metric(const char *path,const char *name,unsigned long long *out)
{
    struct buffer b; if (request(path,NULL,&b)) return -1;
    size_t n=strlen(name); char *save=NULL;
    for (char *line=strtok_r(b.data,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        if (strncmp(line,name,n) || !isspace((unsigned char)line[n])) continue;
        char *end; errno=0; unsigned long long v=strtoull(line+n,&end,10);
        while (isspace((unsigned char)*end)) ++end;
        if (!errno && end!=line+n && !*end) { *out=v; return 0; }
    }
    return -1;
}
static int availability_state[]={-1,-1,-1,-1};
static void availability(const char *topic,int online)
{
    const char *names[]={"camera_available","ai_available","visual_available","sound_available"};
    for (int i=0;i<4;++i) if (!strcmp(names[i],topic) && availability_state[i]!=online) {
        publish(topic,online?"online":"offline",1); availability_state[i]=online;
    }
}
static void poll_camera(void)
{
    double t=now(); int setup=!access(AP,F_OK);
    if (setup) { availability("camera_available",0); availability("ai_available",0); availability("visual_available",0); availability("sound_available",0); return; }
    if (t>=next_config) { json_object_put(native_config); native_config=api("/api/v1/config.json"); next_config=t+5; }
    J *ai=read_json(AI_STATE),*cfg=read_json(AI_CONFIG),*state=json_object_new_object();
    double age=t-json_object_get_double(get(ai,"monotonic"));
    int fresh=ai && age>=0 && age<5;
    availability("ai_available",fresh);
    availability("visual_available",fresh && json_object_get_boolean(get(ai,"running")));
    availability("sound_available",fresh && json_object_get_boolean(get(ai,"soundRunning")));
    unsigned long long count=0,night=0;
    int ok=!metric("/metrics/motion","md_rects_acc_total",&count);
    if (ok) { if (motion_baseline && count>motion_count) motion_until=t+3; motion_count=count; motion_baseline=1; }
    else motion_baseline=0;
    int night_ok=!metric("/metrics/night","night_enabled",&night);
    availability("camera_available",ok && night_ok && native_config);
    flag(state,"motion",ok && json_object_get_boolean(get(get(native_config,"motionDetect"),"enabled")) && t<motion_until);
    flag(state,"night",night_ok && night);
    FILE *gpio=fopen(MQTT_ROOT "/sys/class/gpio/gpio14/value","r"); int white=0;
    if (gpio) { if (fscanf(gpio,"%d",&white)!=1) white=0; fclose(gpio); } flag(state,"floodlight",white==1);
    for (int i=0;i<8;++i) {
        int active=0; J *items=get(ai,i<4?"objects":"sounds");
        if (fresh && json_object_is_type(items,json_type_array) && json_object_get_boolean(get(ai,i<4?"running":"soundRunning")))
            for (size_t j=0;j<json_object_array_length(items);++j)
                if (!strcmp(str(json_object_array_get_idx(items,j),"label"),labels[i])) active=1;
        flag(state,labels[i],active);
    }
    for (unsigned i=0;i<sizeof(settings)/sizeof(settings[0]);++i) {
        const struct setting *s=&settings[i];
        J *group=!strcmp(s->group,"motionDetect")?get(native_config,s->group):!strcmp(s->group,"recording")?get(cfg,"recording"):cfg;
        J *value=get(group,s->field);
        if (!strcmp(s->key,"ai_confidence")) json_object_object_add(state,s->key,json_object_new_int((int)lround(json_object_get_double(value)*100)));
        else json_object_object_add(state,s->key,json_object_get(value));
    }
    flag(state,"recording",fresh && json_object_get_boolean(get(get(ai,"recording"),"active")));
    J *selected=get(get(cfg,"recording"),"categories"); unsigned mask=0;
    for (size_t i=0;json_object_is_type(selected,json_type_array) && i<json_object_array_length(selected);++i)
        for (int j=0;j<8;++j) if (!strcmp(json_object_get_string(json_object_array_get_idx(selected,i)),labels[j])) mask|=1u<<j;
    const char *selection="Custom";
    for (unsigned i=0;i<sizeof(categories)/sizeof(categories[0]);++i) if (categories[i].mask==mask) selection=categories[i].name;
    text(state,"record_categories",selection);
    text(state,"ai_status",fresh?str(ai,"status"):"Service unavailable");
    text(state,"recording_status",fresh?str(get(ai,"recording"),"status"):"Service unavailable");
    /* New connections establish a baseline so old events cannot trigger automations. */
    if (strcmp(previous_boot,str(ai,"bootId"))) { snprintf(previous_boot,sizeof(previous_boot),"%s",str(ai,"bootId")); baseline=0; }
    J *events=get(ai,"events"); unsigned long long newest=(unsigned long long)json_object_get_int64(get(ai,"lastEventId"));
    for (size_t i=0;fresh && baseline && json_object_is_type(events,json_type_array) && i<json_object_array_length(events);++i) {
        J *e=json_object_array_get_idx(events,i); unsigned long long seq=json_object_get_int64(get(e,"id"));
        int accepted=0; for (int j=0;j<8;++j) if (!strcmp(str(e,"label"),labels[j])) accepted=1;
        if (accepted && seq>event_id && time(NULL)-json_object_get_int64(get(e,"time"))<10) {
            J *copy=NULL; json_object_deep_copy(e,&copy,NULL); text(copy,"event_type",str(e,"label")); publish_json("event/detection",copy,0); json_object_put(copy);
        }
    }
    if (fresh) { event_id=newest; baseline=1; }
    J *clip=read_json(LATEST);
    if (clip && strcmp(latest_id,str(clip,"id"))) {
        if (*latest_id) { text(clip,"event_type","complete"); publish_json("event/recording",clip,0); }
        snprintf(latest_id,sizeof(latest_id),"%s",str(clip,"id"));
    } else if (!clip && !*latest_id) snprintf(latest_id,sizeof(latest_id),"none");
    if (clip) text(state,"last_recording_id",str(clip,"id"));
    if (!previous || !json_object_equal(previous,state) || t-last_publish>=30) {
        publish_json("state",state,1); json_object_put(previous); previous=json_object_get(state); last_publish=t;
    }
    json_object_put(clip); json_object_put(ai); json_object_put(cfg); json_object_put(state);
}
static int set_ai(const struct setting *s,J *value,int clip)
{
    J *response=api("/cgi-bin/c120-ai-api.cgi"),*body=NULL;
    if (!response || strlen(str(response,"csrf"))!=64) { json_object_put(response); return -1; }
    if (clip) { body=json_object_new_object(); json_object_object_add(body,"recordClipSeconds",json_object_new_int(clip)); }
    else {
        if (!json_object_is_type(get(response,"config"),json_type_object)) { json_object_put(response); return -1; }
        json_object_deep_copy(get(response,"config"),&body,NULL);
        J *group=!strcmp(s->group,"recording")?get(body,"recording"):body;
        J *desired=!strcmp(s->key,"ai_confidence")?json_object_new_double(json_object_get_int(value)/100.0):json_object_get(value);
        if (json_object_equal(get(group,s->field),desired)) { json_object_put(desired); json_object_put(body); json_object_put(response); return 0; }
        json_object_object_add(group,s->field,desired);
    }
    text(body,"csrf",str(response,"csrf")); struct buffer b;
    int rc=request("/cgi-bin/c120-ai-api.cgi",body,&b); json_object_put(body); json_object_put(response); return rc;
}
static int command(const char *key,const char *payload,size_t length)
{
    if (!access(AP,F_OK) || !length || length>128 || memchr(payload,0,length)) return -1;
    char s[129]; memcpy(s,payload,length); s[length]=0;
    int boolean=!strcmp(s,"ON")?1:!strcmp(s,"OFF")?0:-1;
    struct buffer b;
    if (!strcmp(key,"record_clip")) {
        if (strcmp(s,"PRESS")) return -1;
        J *cfg=read_json(AI_CONFIG); int seconds=json_object_get_int(get(get(cfg,"recording"),"seconds")); json_object_put(cfg);
        return seconds>=1 && seconds<=600?set_ai(NULL,NULL,seconds):-1;
    }
    if (!strcmp(key,"record_categories")) {
        for (unsigned i=0;i<sizeof(categories)/sizeof(categories[0]);++i) if (categories[i].mask && !strcmp(s,categories[i].name)) {
            J *list=json_object_new_array();
            for (int j=0;j<8;++j) if (categories[i].mask&(1u<<j)) json_object_array_add(list,json_object_new_string(labels[j]));
            struct setting setting={"record_categories","","recording","categories",0,0};
            int rc=set_ai(&setting,list,0); json_object_put(list); return rc;
        }
        return -1;
    }
    if (!strcmp(key,"night") || !strcmp(key,"floodlight")) {
        if (boolean<0) return -1;
        const char *path=!strcmp(key,"night")?(boolean?"/night/on":"/night/off"):
            (boolean?"/cgi-bin/c120-floodlight.cgi?action=on":"/cgi-bin/c120-floodlight.cgi?action=off");
        return request(path,NULL,&b);
    }
    for (unsigned i=0;i<sizeof(settings)/sizeof(settings[0]);++i) if (!strcmp(key,settings[i].key)) {
        const struct setting *setting=&settings[i]; J *value=NULL;
        if (setting->low<0) { if (boolean<0) return -1; value=json_object_new_boolean(boolean); }
        else {
            value=parse(s,length);
            if (!json_object_is_type(value,json_type_int) || json_object_get_int64(value)<setting->low || json_object_get_int64(value)>setting->high) { json_object_put(value); return -1; }
        }
        int rc;
        if (!strcmp(setting->group,"motionDetect")) {
            J *current=api("/api/v1/config.json");
            if (json_object_equal(get(get(current,setting->group),setting->field),value)) rc=0;
            else {
                J *body=json_object_new_object(),*group=json_object_new_object(); json_object_object_add(body,setting->group,group);
                json_object_object_add(group,setting->field,json_object_get(value)); rc=request("/api/v1/config",body,&b); json_object_put(body);
            }
            json_object_put(current);
        } else rc=set_ai(setting,value,0);
        json_object_put(value); return rc;
    }
    return -1;
}
static void on_message(struct mosquitto *m,void *unused,const struct mosquitto_message *message,const mosquitto_property *properties)
{
    (void)m; (void)unused; (void)properties;
    if (!strcmp(message->topic,"homeassistant/status")) {
        if (message->payloadlen==6 && !memcmp(message->payload,"online",6)) need_discovery=1;
        return;
    }
    char prefix[180]; snprintf(prefix,sizeof(prefix),"%s/set/",base); size_t n=strlen(prefix);
    int rc=-1; const char *key="unknown";
    if (!strncmp(message->topic,prefix,n)) {
        key=message->topic+n;
        if (!message->retain) rc=command(key,message->payload,(size_t)message->payloadlen);
    }
    J *result=json_object_new_object(); text(result,"command",key); flag(result,"accepted",!rc);
    if (rc) text(result,"error",message->retain?"Retained commands are forbidden":"Invalid command or camera API unavailable");
    publish_json("result",result,0); json_object_put(result); next_poll=next_config=0;
}
static void on_connect(struct mosquitto *m,void *unused,int reason,int flags,const mosquitto_property *properties)
{
    (void)unused; (void)flags; (void)properties;
    if (reason) { connected=0; syslog(LOG_WARNING,"MQTT connection rejected (%d)",reason); return; }
    char topic[192]; snprintf(topic,sizeof(topic),"%s/set/+",base);
    /* MQTT 5 keeps the original retain flag and suppresses stored-command replay. */
    int options=MQTT_SUB_OPT_RETAIN_AS_PUBLISHED|MQTT_SUB_OPT_SEND_RETAIN_NEVER;
    if (mosquitto_subscribe_v5(m,NULL,topic,0,options,NULL) || mosquitto_subscribe_v5(m,NULL,"homeassistant/status",0,0,NULL)) return;
    connected=need_discovery=1; baseline=0; latest_id[0]=0; motion_baseline=0; next_poll=next_config=0;
    for (int i=0;i<4;++i) availability_state[i]=-1;
    json_object_put(previous); previous=NULL; publish("availability","online",1);
}
static void on_disconnect(struct mosquitto *m,void *unused,int reason,const mosquitto_property *properties)
{ (void)m; (void)unused; (void)reason; (void)properties; connected=0; }
static int identifier(const char *s)
{
    if (!*s || strlen(s)>48) return 0;
    for (;*s;++s) if (!isalnum((unsigned char)*s) && *s!='_' && *s!='-') return 0;
    return 1;
}
static int valid_config(J *o)
{
    if (!json_object_is_type(o,json_type_object) || json_object_object_length(o)!=8 ||
        !json_object_is_type(get(o,"enabled"),json_type_boolean) || !identifier(str(o,"id")) ||
        !json_object_is_type(get(o,"port"),json_type_int) || json_object_get_int(get(o,"port"))!=1883 ||
        !*str(o,"host") || strlen(str(o,"host"))>253 || !*str(o,"name") || strlen(str(o,"name"))>64 ||
        strlen(str(o,"username"))>128 || strlen(str(o,"password"))>256 || strlen(str(o,"url"))>256) return 0;
    const char *keys[]={"host","id","name","username","password","url"};
    for (unsigned i=0;i<6;++i) {
        J *v=get(o,keys[i]); const char *s=str(o,keys[i]);
        if (!json_object_is_type(v,json_type_string) || (size_t)json_object_get_string_len(v)!=strlen(s)) return 0;
        for (;*s;++s) if ((unsigned char)*s<32 || (unsigned char)*s==127) return 0;
    }
    return !json_object_get_boolean(get(o,"enabled")) || (*str(o,"username") && *str(o,"password"));
}
static void stop(int sig) { (void)sig; stopping=1; }
int main(int argc,char **argv)
{
    config=read_json(CONFIG);
    if (!valid_config(config)) { fputs("Invalid /etc/openipc-mqtt.json (LAN broker port 1883 required)\n",stderr); return 2; }
    snprintf(id,sizeof(id),"openipc_%s",str(config,"id")); snprintf(base,sizeof(base),"openipc/%s",str(config,"id"));
    snprintf(discovery,sizeof(discovery),"homeassistant/device/%s/config",id);
    if (argc==2 && !strcmp(argv[1],"--discovery")) { J *o=discovery_payload(); puts(json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN)); json_object_put(o); json_object_put(config); return 0; }
    if (argc!=1) return 2;
    if (!json_object_get_boolean(get(config,"enabled"))) { json_object_put(config); return 0; }
    struct stat st; if (stat(CONFIG,&st) || st.st_uid!=geteuid() || st.st_mode&0077) { fputs("MQTT credentials must be owner-only (chmod 600)\n",stderr); return 2; }
    int lock=open(MQTT_ROOT "/run/openipc-mqtt.pid",O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) return 1;
    if (ftruncate(lock,0) || dprintf(lock,"%ld\n",(long)getpid())<0) return 1;
    openlog("openipc-mqtt",LOG_PID,LOG_DAEMON); signal(SIGTERM,stop); signal(SIGINT,stop);
    curl_global_init(CURL_GLOBAL_DEFAULT); http=curl_easy_init(); mosquitto_lib_init(); client=mosquitto_new(id,true,NULL);
    if (!http || !client) return 1;
    mosquitto_int_option(client,MOSQ_OPT_PROTOCOL_VERSION,MQTT_PROTOCOL_V5);
    mosquitto_username_pw_set(client,str(config,"username"),str(config,"password"));
    mosquitto_connect_v5_callback_set(client,on_connect); mosquitto_disconnect_v5_callback_set(client,on_disconnect); mosquitto_message_v5_callback_set(client,on_message);
    char will[256]; snprintf(will,sizeof(will),"%s/availability",base);
    mosquitto_will_set(client,will,7,"offline",0,true);
    double retry=0;
    while (!stopping) {
        double t=now();
        if (!connected && t>=retry) {
            mosquitto_property *properties=NULL;
            mosquitto_property_add_int32(&properties,MQTT_PROP_MAXIMUM_PACKET_SIZE,4096);
            mosquitto_connect_bind_v5(client,str(config,"host"),1883,30,NULL,properties);
            mosquitto_property_free_all(&properties); retry=t+10;
        }
        int rc=mosquitto_loop(client,50,1);
        if (rc && rc!=MOSQ_ERR_NO_CONN) connected=0;
        if (connected && need_discovery) {
            J *o=discovery_payload(); const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
            if (!mosquitto_publish(client,NULL,discovery,(int)strlen(s),s,0,true)) need_discovery=0;
            json_object_put(o); next_poll=0; json_object_put(previous); previous=NULL;
        }
        if (connected && t>=next_poll) { poll_camera(); next_poll=now()+.5; }
        if (!connected) usleep(50000);
    }
    if (connected) { publish("availability","offline",1); mosquitto_loop(client,100,1); mosquitto_disconnect(client); }
    mosquitto_destroy(client); mosquitto_lib_cleanup(); curl_easy_cleanup(http); curl_global_cleanup();
    json_object_put(previous); json_object_put(native_config); json_object_put(config); unlink(MQTT_ROOT "/run/openipc-mqtt.pid"); close(lock); closelog(); return 0;
}
