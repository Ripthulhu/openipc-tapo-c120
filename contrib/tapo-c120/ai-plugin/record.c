#if defined(__arm__)
#define _FILE_OFFSET_BITS 64
#endif
#define _GNU_SOURCE
#include "record.h"
#include "catalogue.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include "mp4.h"

typedef struct json_object J;
#ifndef C120_RECORD_URL
#define C120_RECORD_URL "http://127.0.0.1/video.mp4"
#endif
#ifndef C120_SNAPSHOT_URL
#define C120_SNAPSHOT_URL "http://127.0.0.1/image.jpg"
#endif
static J *config;
static char path[512], partial[640], finished[640], directory[512];
static int limit = 95, split = 1200, native_enabled, fd = -1;
static double until, manual_until, retry, last_data, started, space_check;
static int manual_clip;
static unsigned clips, errors;
static size_t bytes;
static CURLM *multi;
static CURL *stream;
static CURL *snapshot;
static int snapshot_fd=-1, broken;
static char snapshot_partial[680], snapshot_path[680];
static size_t snapshot_bytes;
static double captured, wall_started;
static J *detections;
static const char *status = "Disabled";
static const char *last_error = "";
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

static void snapshot_stop(int success)
{
    if (!snapshot) return;
    curl_multi_remove_handle(multi,snapshot); curl_easy_cleanup(snapshot); snapshot=NULL;
    unsigned char magic[2]={0};
    int bad=!success || snapshot_bytes<4 || pread(snapshot_fd,magic,2,0)!=2 || magic[0]!=255 || magic[1]!=216;
    if (fsync(snapshot_fd)) bad=1;
    if (close(snapshot_fd)) bad=1;
    snapshot_fd=-1;
    if (!bad && rename(snapshot_partial,snapshot_path)) bad=1;
    if (bad) { unlink(snapshot_partial); snapshot_path[0]=0; captured=0; }
}
void record_stop(void)
{
    snapshot_stop(0);
    if (stream && multi) curl_multi_remove_handle(multi,stream);
    curl_easy_cleanup(stream); stream = NULL;
    if (multi) curl_multi_cleanup(multi);
    multi = NULL;
    if (fd >= 0) {
        off_t complete=broken?0:mp4_complete_bytes(fd,0);
        int bad = !complete;
        if (!complete) last_error=broken?"Stream interrupted":"No complete MP4 fragment";
        if (!bad && (ftruncate(fd,complete) || fsync(fd))) { bad=1; last_error="Cannot finalize MP4"; }
        if (close(fd)) { bad=1; last_error="Cannot close MP4"; } fd = -1;
        if (!bad && rename(partial,finished)) bad=1;
        if (!bad && bytes) {
            ++clips;
            if (catalogue_complete(finished,manual_clip?"manual":"ai",wall_started,now()-started,detections,
                *snapshot_path?snapshot_path:NULL,captured,1)) status="Recorded; catalogue update failed";
        }
        else { ++errors; unlink(partial); }
        if (bad && *snapshot_path) unlink(snapshot_path);
        json_object_put(detections); detections=NULL;
    }
    bytes = 0; broken=0; snapshot_path[0]=0;
}
void record_configure(J *o)
{
    record_stop(); json_object_put(config); config = json_object_get(o); until = manual_until = retry = 0;
    json_object_put(detections); detections=NULL;
    catalogue_recording_enabled(json_object_get_boolean(field(config,"enabled")));
    status = json_object_get_boolean(field(config,"enabled")) ? "Waiting for detection" : "Disabled";
}
void record_storage(J *records)
{
    catalogue_storage(records);
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
int record_trigger(unsigned seconds)
{
    if (!seconds || seconds>600 || native_enabled) return -1;
    manual_until=now()+seconds; retry=0; catalogue_recording_enabled(1); return 0;
}
void record_detection(const char *label,const char *type,const char *model,double confidence)
{
    record_observe(label);
    if (native_enabled || (now()>=until && now()>=manual_until)) return;
    if (!detections) detections=json_object_new_array();
    J *item=NULL;
    for (size_t i=0;i<json_object_array_length(detections);++i) {
        J *o=json_object_array_get_idx(detections,i);
        if (!strcmp(json_object_get_string(field(o,"category")),label) && !strcmp(json_object_get_string(field(o,"model")),model)) { item=o; break; }
    }
    time_t t=time(NULL); struct tm tm; char stamp[32]; gmtime_r(&t,&tm); strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&tm);
    if (!item) {
        if (json_object_array_length(detections)>=16) return;
        item=json_object_new_object(); json_object_array_add(detections,item);
        json_object_object_add(item,"category",json_object_new_string(label));
        json_object_object_add(item,"type",json_object_new_string(type));
        json_object_object_add(item,"model",json_object_new_string(model));
        json_object_object_add(item,"firstSeen",json_object_new_string(stamp));
    }
    json_object_object_add(item,"lastSeen",json_object_new_string(stamp));
    if (confidence>json_object_get_double(field(item,"maxConfidence"))) json_object_object_add(item,"maxConfidence",json_object_new_double(confidence));
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
    if (statvfs(parent,&space) || (space.f_flag & ST_RDONLY) || !space.f_blocks || space.f_bavail < 1 ||
        (double)(space.f_blocks-space.f_bavail)*100/space.f_blocks >= limit ||
        (double)space.f_bavail*space.f_frsize < 8*1024*1024) return -1;
    for (char *p=dir+1,*component=p;;++p) if (*p=='/' || !*p) {
        char end=*p; *p=0;
        if (!strcmp(component,".") || !strcmp(component,"..")) { *p=end; return -1; }
        struct stat st; int bad=mkdir(dir,0755) && errno!=EEXIST;
        if (!bad && (lstat(dir,&st) || !S_ISDIR(st.st_mode))) bad=1;
        *p=end; if (bad) return -1; if (!end) break; component=p+1;
    }
    return 0;
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
static size_t write_snapshot(char *data,size_t size,size_t count,void *unused)
{
    (void)unused; size_t n=size*count,done=0; long code=0; curl_easy_getinfo(snapshot,CURLINFO_RESPONSE_CODE,&code);
    if (code!=200 || n>2*1024*1024-snapshot_bytes) return 0;
    while (done<n) { ssize_t w=write(snapshot_fd,data+done,n-done); if (w<0 && errno==EINTR) continue; if (w<=0) return 0; done+=w; }
    snapshot_bytes+=n; return n;
}
static void snapshot_start(void)
{
    snprintf(snapshot_path,sizeof(snapshot_path),"%s.jpg",finished);
    snprintf(snapshot_partial,sizeof(snapshot_partial),"%s.jpg.partial",finished);
    snapshot_fd=open(snapshot_partial,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0644);
    if (snapshot_fd<0) { snapshot_path[0]=0; return; }
    snapshot=curl_easy_init();
    if (!snapshot) { close(snapshot_fd); snapshot_fd=-1; unlink(snapshot_partial); snapshot_path[0]=0; return; }
    snapshot_bytes=0; captured=time(NULL);
    curl_easy_setopt(snapshot,CURLOPT_URL,C120_SNAPSHOT_URL); curl_easy_setopt(snapshot,CURLOPT_NOPROXY,"*");
    curl_easy_setopt(snapshot,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(snapshot,CURLOPT_CONNECTTIMEOUT_MS,500L);
    curl_easy_setopt(snapshot,CURLOPT_TIMEOUT_MS,1500L); curl_easy_setopt(snapshot,CURLOPT_BUFFERSIZE,16384L);
    curl_easy_setopt(snapshot,CURLOPT_WRITEFUNCTION,write_snapshot);
    if (curl_multi_add_handle(multi,snapshot)) snapshot_stop(0);
}
void record_poll(void)
{
    double t = now();
    const char *failure="Recording stream interrupted";
    CURLcode stream_result=CURLE_OK;
    int automatic=json_object_get_boolean(field(config,"enabled"));
    if (!automatic && t>=manual_until) { record_stop(); catalogue_recording_enabled(0); status="Disabled"; return; }
    if (native_enabled) { record_stop(); status = "Paused: native recorder enabled"; return; }
    if (t >= until && t>=manual_until) { record_stop(); json_object_put(detections); detections=NULL; status = "Waiting for detection"; return; }
    if (stream && t-started >= split) record_stop();
    if (!stream && t >= retry) {
        char dir[512];
        if (storage(dir)) { status = "Waiting for writable recording storage"; retry = t+5; return; }
        snprintf(directory,sizeof(directory),"%s",dir);
        time_t wall = time(NULL);
        int n = snprintf(partial,sizeof(partial),"%s/AI-%lld-%llu-XXXXXX.partial",dir,(long long)wall,(unsigned long long)(t*1000));
        if (n < 0 || n >= (int)sizeof(partial)) { failure="Recording path too long"; goto failed; }
        fd = mkstemps(partial,8); if (fd < 0) { failure="Cannot create recording file"; goto failed; }
        fcntl(fd,F_SETFD,FD_CLOEXEC);
        fchmod(fd,0644);
        snprintf(finished,sizeof(finished),"%s",partial); strcpy(finished+strlen(finished)-8,".mp4");
        multi = curl_multi_init(); stream = curl_easy_init();
        if (!multi || !stream) { failure="Cannot initialize recording stream"; goto failed; }
        curl_easy_setopt(stream,CURLOPT_URL,C120_RECORD_URL);
        curl_easy_setopt(stream,CURLOPT_NOPROXY,"*"); curl_easy_setopt(stream,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(stream,CURLOPT_CONNECTTIMEOUT_MS,500L);
        curl_easy_setopt(stream,CURLOPT_WRITEFUNCTION,write_video);
        curl_easy_setopt(stream,CURLOPT_BUFFERSIZE,16384L);
        if (curl_multi_add_handle(multi,stream)) { failure="Cannot attach recording stream"; goto failed; }
        started = last_data = t; wall_started=wall; space_check = 0; status = "Recording";
        manual_clip=t<manual_until;
        snapshot_start();
    }
    if (!stream) return;
    int running, messages;
    if (curl_multi_perform(multi,&running) != CURLM_OK) { failure="Cannot poll recording stream"; goto failed; }
    if (t-last_data > 5) { failure="Recording stream stalled"; goto failed; }
    CURLMsg *message;
    while ((message=curl_multi_info_read(multi,&messages))) {
        if (message->easy_handle==snapshot) snapshot_stop(message->data.result==CURLE_OK);
        else { stream_result=message->data.result; goto failed; }
    }
    return;
failed: {
    long http_status=0; int saved_errno=errno,had_file=fd>=0;
    if (stream) curl_easy_getinfo(stream,CURLINFO_RESPONSE_CODE,&http_status);
    syslog(LOG_WARNING,"%s (HTTP %ld, curl %d, errno %d)",failure,http_status,stream_result,saved_errno);
    broken=1; record_stop(); if (!had_file) ++errors;
    last_error=failure; status = "Recording unavailable; retrying"; retry = t+5;
}
}
J *record_state(void)
{
    J *o = json_object_new_object();
    json_object_object_add(o,"status",json_object_new_string(status));
    json_object_object_add(o,"active",json_object_new_boolean(stream != NULL));
    json_object_object_add(o,"clips",json_object_new_int64(clips));
    json_object_object_add(o,"errors",json_object_new_int64(errors));
    json_object_object_add(o,"lastError",json_object_new_string(last_error));
    return o;
}
