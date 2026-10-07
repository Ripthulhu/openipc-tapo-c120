#define _GNU_SOURCE
#include "record.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

typedef struct json_object J;
#ifndef C120_RECORD_URL
#define C120_RECORD_URL "http://127.0.0.1/video.mp4"
#endif
static J *config;
static char path[512], partial[640], finished[640], directory[512];
static int limit = 95, split = 1200, native_enabled, fd = -1;
static double until, retry, last_data, started, space_check;
static unsigned clips, errors;
static size_t bytes;
static CURLM *multi;
static CURL *stream;
static const char *status = "Disabled";
static J *field(J *o,const char *key) { return json_object_object_get(o,key); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

J *record_defaults(void) { return json_tokener_parse("{\"enabled\":false,\"seconds\":30,\"categories\":[\"person\",\"pet\",\"vehicle\"]}"); }
int record_valid(J *o)
{
    if (!json_object_is_type(o,json_type_object) || json_object_object_length(o) != 3 ||
        !json_object_is_type(field(o,"enabled"),json_type_boolean) ||
        !json_object_is_type(field(o,"seconds"),json_type_int) ||
        json_object_get_int64(field(o,"seconds")) < 1 || json_object_get_int64(field(o,"seconds")) > 600) return 0;
    J *list = field(o,"categories"); unsigned mask = 0;
    const char *names[] = {"person","pet","vehicle","bird","bark","meow","cry","glass"};
    if (!json_object_is_type(list,json_type_array) || json_object_array_length(list) > 8) return 0;
    for (size_t i = 0; i < json_object_array_length(list); ++i) {
        J *item = json_object_array_get_idx(list,i); unsigned bit = 0;
        if (!json_object_is_type(item,json_type_string)) return 0;
        for (int j = 0; j < 8; ++j)
            if (json_object_get_string_len(item) == (int)strlen(names[j]) && !strcmp(json_object_get_string(item),names[j])) bit = 1u << j;
        if (!bit || (mask & bit)) return 0;
        mask |= bit;
    }
    return !json_object_get_boolean(field(o,"enabled")) || mask;
}

void record_stop(void)
{
    if (stream && multi) curl_multi_remove_handle(multi,stream);
    curl_easy_cleanup(stream); stream = NULL;
    if (multi) curl_multi_cleanup(multi);
    multi = NULL;
    if (fd >= 0) {
        int bad = fsync(fd); if (close(fd)) bad = 1; fd = -1;
        if (!bad && bytes && !rename(partial,finished)) ++clips;
        else { ++errors; unlink(partial); }
    }
    bytes = 0;
}
void record_configure(J *o)
{
    record_stop(); json_object_put(config); config = json_object_get(o); until = retry = 0;
    status = json_object_get_boolean(field(config,"enabled")) ? "Waiting for detection" : "Disabled";
}
void record_storage(J *records)
{
    const char *p = json_object_get_string(field(records,"path"));
    if (!p || strlen(p) >= sizeof(path) || strcmp(path,p)) record_stop();
    snprintf(path,sizeof(path),"%s",p && strlen(p) < sizeof(path) ? p : "");
    limit = json_object_get_int(field(records,"maxUsage"));
    if (limit < 1 || limit > 99) limit = 95;
    native_enabled = json_object_get_boolean(field(records,"enabled"));
    int minutes = json_object_get_int(field(records,"split"));
    split = minutes >= 1 && minutes <= 100 ? minutes*60 : 1200;
}
void record_observe(const char *label)
{
    if (!json_object_get_boolean(field(config,"enabled"))) return;
    J *list = field(config,"categories");
    for (size_t i = 0; i < json_object_array_length(list); ++i)
        if (!strcmp(label,json_object_get_string(json_object_array_get_idx(list,i)))) until = now()+json_object_get_int(field(config,"seconds"));
}
static int storage(char dir[512])
{
    time_t t = time(NULL); struct tm tm; localtime_r(&t,&tm);
    if (*path != '/' || strstr(path,"/../") || !strftime(dir,512,path,&tm)) return -1;
    /* Validate the existing ancestor before creating directories: never fall back to flash or RAM. */
    char parent[512]; snprintf(parent,sizeof(parent),"%s",dir);
    struct statfs fs;
    while (statfs(parent,&fs)) {
        char *slash = strrchr(parent,'/'); if (!slash || slash == parent) return -1; *slash = 0;
    }
    struct statfs root;
    if (statfs("/",&root) || (fs.f_type == root.f_type && !memcmp(&fs.f_fsid,&root.f_fsid,sizeof(fs.f_fsid)))) return -1;
    if (fs.f_type != 0x4d44 && fs.f_type != 0x2011bab0 && fs.f_type != 0xef53 &&
        fs.f_type != 0x6969 && fs.f_type != (long)0xff534d42 && fs.f_type != (long)0xfe534d42) return -1;
    struct statvfs space;
    if (statvfs(parent,&space) || !space.f_blocks || space.f_bavail < 1 ||
        (double)(space.f_blocks-space.f_bavail)*100/space.f_blocks >= limit ||
        (double)space.f_bavail*space.f_frsize < 8*1024*1024) return -1;
    for (char *p = dir+1; *p; ++p) if (*p == '/') { *p = 0; int rc = mkdir(dir,0755); *p = '/'; if (rc && errno != EEXIST) return -1; }
    return mkdir(dir,0755) && errno != EEXIST ? -1 : 0;
}
static size_t write_video(char *data,size_t size,size_t count,void *unused)
{
    (void)unused; size_t length = size*count, done = 0; long code = 0;
    curl_easy_getinfo(stream,CURLINFO_RESPONSE_CODE,&code);
    if (code != 200) return 0;
    if (now() >= space_check) {
        struct statvfs space;
        if (statvfs(directory,&space) || !space.f_blocks ||
            (double)(space.f_blocks-space.f_bavail)*100/space.f_blocks >= limit ||
            (double)space.f_bavail*space.f_frsize < 8*1024*1024) return 0;
        space_check = now()+2;
    }
    while (done < length) { ssize_t n = write(fd,data+done,length-done); if (n <= 0) { if (n < 0 && errno == EINTR) continue; return 0; } done += n; }
    bytes += length; last_data = now(); return length;
}
void record_poll(void)
{
    if (!json_object_get_boolean(field(config,"enabled"))) return;
    double t = now();
    if (native_enabled) { record_stop(); status = "Paused: native recorder enabled"; return; }
    if (t >= until) { record_stop(); status = "Waiting for detection"; return; }
    if (stream && t-started >= split) record_stop();
    if (!stream && t >= retry) {
        char dir[512];
        if (storage(dir)) { status = "Waiting for writable recording storage"; retry = t+5; return; }
        snprintf(directory,sizeof(directory),"%s",dir);
        time_t wall = time(NULL);
        int n = snprintf(partial,sizeof(partial),"%s/AI-%lld-%llu-XXXXXX.partial",dir,(long long)wall,(unsigned long long)(t*1000));
        if (n < 0 || n >= (int)sizeof(partial)) goto failed;
        fd = mkstemps(partial,8); if (fd < 0) goto failed;
        fchmod(fd,0644);
        snprintf(finished,sizeof(finished),"%s",partial); strcpy(finished+strlen(finished)-8,".mp4");
        multi = curl_multi_init(); stream = curl_easy_init(); if (!multi || !stream) goto failed;
        curl_easy_setopt(stream,CURLOPT_URL,C120_RECORD_URL);
        curl_easy_setopt(stream,CURLOPT_NOPROXY,"*"); curl_easy_setopt(stream,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(stream,CURLOPT_CONNECTTIMEOUT_MS,500L);
        curl_easy_setopt(stream,CURLOPT_WRITEFUNCTION,write_video);
        curl_easy_setopt(stream,CURLOPT_BUFFERSIZE,16384L);
        if (curl_multi_add_handle(multi,stream)) goto failed;
        started = last_data = t; space_check = 0; status = "Recording";
    }
    if (!stream) return;
    int running, messages;
    if (curl_multi_perform(multi,&running) != CURLM_OK || curl_multi_info_read(multi,&messages) || t-last_data > 5) goto failed;
    return;
failed:
    ++errors; record_stop(); status = "Recording unavailable; retrying"; retry = t+5;
}
J *record_state(void)
{
    J *o = json_object_new_object();
    json_object_object_add(o,"status",json_object_new_string(status));
    json_object_object_add(o,"active",json_object_new_boolean(stream != NULL));
    json_object_object_add(o,"clips",json_object_new_int64(clips));
    json_object_object_add(o,"errors",json_object_new_int64(errors));
    return o;
}
