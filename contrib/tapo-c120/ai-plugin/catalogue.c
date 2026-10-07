#if defined(__arm__)
#define _FILE_OFFSET_BITS 64
#endif
#define _GNU_SOURCE
#include "catalogue.h"
#include "notify.h"
#include <ctype.h>
#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "mp4.h"

typedef struct json_object J;
static J *public_clip(int dir,J *o);
#ifndef C120_RECORDING_LATEST
#define C120_RECORDING_LATEST "/run/c120-recording-latest.json"
#endif
extern char **environ;
static char root[512];
static J *config;
static pid_t worker;
static time_t next_work;
static int recording_enabled, native_enabled, max_usage=95;
static J *get(J *o,const char *k) { return json_object_object_get(o,k); }
static const char *str(J *o,const char *k) { const char *s=json_object_get_string(get(o,k)); return s?s:""; }
static void text(J *o,const char *k,const char *v) { json_object_object_add(o,k,json_object_new_string(v)); }
static void integer(J *o,const char *k,int64_t v) { json_object_object_add(o,k,json_object_new_int64(v)); }
static uint64_t hash(const char *s) { uint64_t h=UINT64_C(14695981039346656037); for (;*s;++s) h=(h^(unsigned char)*s)*UINT64_C(1099511628211); return h; }
static void stamp(J *o,const char *key,double value)
{
    if (value<=0) { json_object_object_add(o,key,NULL); return; }
    time_t t=(time_t)value; struct tm tm; char s[32]; gmtime_r(&t,&tm);
    strftime(s,sizeof(s),"%Y-%m-%dT%H:%M:%SZ",&tm); text(o,key,s);
}

J *catalogue_defaults(void) { return json_tokener_parse("{\"enabled\":false,\"url\":\"\",\"token\":\"\"}"); }
int catalogue_valid(J *o)
{
    if (!json_object_is_type(o,json_type_object) || json_object_object_length(o)!=3) return 0;
    /* Reuse the existing notification URL/header validation, including disabled settings. */
    J *n=notify_defaults();
    const char *keys[]={"enabled","url","token"};
    for (unsigned i=0;i<3;++i) json_object_object_add(n,keys[i],json_object_get(get(o,keys[i])));
    int valid=notify_valid(n); json_object_put(n); return valid;
}
void catalogue_configure(J *o) { json_object_put(config); config=json_object_get(o); }
void catalogue_recording_enabled(int enabled) { recording_enabled=enabled; }
void catalogue_storage(J *records)
{
    native_enabled=json_object_get_boolean(get(records,"enabled"));
    max_usage=json_object_get_int(get(records,"maxUsage")); if (max_usage<1 || max_usage>99) max_usage=95;
    const char *p=str(records,"path"); root[0]=0;
    if (*p!='/' || strlen(p)>=sizeof(root)) return;
    snprintf(root,sizeof(root),"%s",p);
    char *percent=strchr(root,'%');
    if (percent) { *percent=0; char *slash=strrchr(root,'/'); if (slash) *slash=0; }
    size_t n=strlen(root); while (n && root[n-1]=='/') root[--n]=0;
}

/* Walk every component with openat: a symlink on removable media must not expose flash. */
static int walk(int base,const char *path,int flags)
{
    if (!path || !*path || strlen(path)>=640 || *path=='/') return -1;
    char copy[640]; snprintf(copy,sizeof(copy),"%s",path);
    int fd=dup(base); if (fd<0) return -1;
    char *save=NULL,*p=strtok_r(copy,"/",&save);
    while (p) {
        if (!strcmp(p,".") || !strcmp(p,"..") || !*p) { close(fd); return -1; }
        char *next=strtok_r(NULL,"/",&save);
        int n=openat(fd,p,(next?O_RDONLY|O_DIRECTORY:flags)|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        close(fd); if (n<0) return -1; fd=n; p=next;
    }
    return fd;
}
static int storage(void)
{
    if (*root!='/' || !root[1]) return -1;
    int slash=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC); if (slash<0) return -1;
    int fd=walk(slash,root+1,O_RDONLY|O_DIRECTORY); close(slash); if (fd<0) return -1;
    struct statfs fs,r;
    if (fstatfs(fd,&fs) || statfs("/",&r) ||
        (fs.f_type==r.f_type && !memcmp(&fs.f_fsid,&r.f_fsid,sizeof(fs.f_fsid))) ||
        (fs.f_type!=0x4d44 && fs.f_type!=0x2011bab0 && fs.f_type!=0xef53 && fs.f_type!=0xf2f52010 &&
         fs.f_type!=0x6969 && fs.f_type!=(long)0xff534d42 && fs.f_type!=(long)0xfe534d42)) { close(fd); return -1; }
    return fd;
}
static int write_all(int fd,const char *p,size_t n)
{
    while (n) { ssize_t w=write(fd,p,n); if (w<0 && errno==EINTR) continue; if (w<=0) return -1; p+=w; n-=w; } return 0;
}
static int save(int dir,const char *name,J *o)
{
    char tmp[96]; snprintf(tmp,sizeof(tmp),".%s.%ld.tmp",name,(long)getpid());
    /* Every caller holds the catalogue lock; discard a temp left by a reused PID. */
    if (unlinkat(dir,tmp,0) && errno!=ENOENT) return -1;
    int fd=openat(dir,tmp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600); if (fd<0) return -1;
    const char *s=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN);
    int bad=write_all(fd,s,strlen(s)) || fsync(fd); if (close(fd)) bad=1;
    if (!bad && renameat(dir,tmp,dir,name)) bad=1;
    if (!bad && fsync(dir) && errno!=EINVAL) bad=1;
    if (bad) unlinkat(dir,tmp,0);
    return bad?-1:0;
}
static J *load(int dir,const char *name)
{
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); if (fd<0) return NULL;
    struct stat st; char buf[16385];
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<1 || st.st_size>16384) { close(fd); return NULL; }
    size_t done=0;
    while (done<(size_t)st.st_size) { ssize_t n=read(fd,buf+done,st.st_size-done); if (n<0 && errno==EINTR) continue; if (n<=0) break; done+=n; }
    close(fd); buf[done]=0; if (done!=(size_t)st.st_size) return NULL;
    struct json_tokener *t=json_tokener_new(); json_tokener_set_flags(t,JSON_TOKENER_STRICT);
    J *o=json_tokener_parse_ex(t,buf,done+1);
    if (json_tokener_get_error(t)!=json_tokener_success || json_tokener_get_parse_end(t)!=done) { json_object_put(o); o=NULL; }
    json_tokener_free(t);
    /* Sidecars are untrusted removable-media input, not just files produced by this process. */
    if (o && strlen(name)==25 && strspn(name,"0123456789")==20 && !strcmp(name+20,".json")) {
        J *a=get(o,"detections"); int valid=json_object_is_type(o,json_type_object) &&
            json_object_is_type(a,json_type_array) && json_object_array_length(a)<=16 &&
            json_object_is_type(get(o,"bytes"),json_type_int) && json_object_get_int64(get(o,"bytes"))>0 &&
            strlen(str(o,"path"))<=600 && *str(o,"path") && strlen(str(o,"id"))==37;
        for (size_t i=0;valid && i<json_object_array_length(a);++i) {
            J *d=json_object_array_get_idx(a,i); double score=json_object_get_double(get(d,"maxConfidence"));
            valid=json_object_is_type(d,json_type_object) && *str(d,"category") && strlen(str(d,"category"))<16 &&
                *str(d,"model") && strlen(str(d,"model"))<=64 && isfinite(score) && score>=0 && score<=1;
        }
        if (!valid) { json_object_put(o); o=NULL; }
    }
    return o;
}
struct catalogue { int root,dir,lock; J *state; };
static void release(struct catalogue *c)
{
    json_object_put(c->state); if (c->lock>=0) close(c->lock); if (c->dir>=0) close(c->dir); if (c->root>=0) close(c->root);
}
static int acquire(struct catalogue *c,int create)
{
    *c=(struct catalogue){.root=-1,.dir=-1,.lock=-1}; c->root=storage(); if (c->root<0) return -1;
    if (create && mkdirat(c->root,".c120-recordings",0700) && errno!=EEXIST) goto fail;
    c->dir=openat(c->root,".c120-recordings",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (c->dir<0) goto fail;
    c->lock=openat(c->dir,"lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (c->lock<0 || flock(c->lock,LOCK_EX)) goto fail;
    c->state=load(c->dir,"state.json");
    if (!c->state && create) {
        struct stat st; if (!fstatat(c->dir,"state.json",&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT) goto fail;
        unsigned char bytes[8]; int r=open("/dev/urandom",O_RDONLY|O_CLOEXEC); if (r<0) goto fail;
        ssize_t n=read(r,bytes,sizeof(bytes)); close(r); if (n!=sizeof(bytes)) goto fail;
        char gen[17]; for (unsigned i=0;i<8;++i) sprintf(gen+2*i,"%02x",bytes[i]);
        char camera[80]="",mac[64]=""; FILE *f=fopen("/sys/class/net/wlan0/address","r");
        if (f) { if (!fgets(mac,sizeof(mac),f)) mac[0]=0; fclose(f); }
        if (*mac) { mac[strcspn(mac,"\r\n")]=0; snprintf(camera,sizeof(camera),"camera-%016" PRIx64,hash(mac)); }
        else { gethostname(camera,sizeof(camera)-1); }
        c->state=json_object_new_object(); text(c->state,"generation",gen); text(c->state,"cameraId",camera);
        integer(c->state,"sequence",0); integer(c->state,"indexed",0);
        if (save(c->dir,"state.json",c->state)) goto fail;
    }
    if (!c->state || strlen(str(c->state,"generation"))!=16 ||
        strspn(str(c->state,"generation"),"0123456789abcdef")!=16 || !*str(c->state,"cameraId") ||
        !json_object_is_type(get(c->state,"sequence"),json_type_int) ||
        !json_object_is_type(get(c->state,"indexed"),json_type_int) ||
        json_object_get_int64(get(c->state,"indexed"))<0 ||
        json_object_get_int64(get(c->state,"sequence"))<json_object_get_int64(get(c->state,"indexed"))) goto fail;
    return 0;
fail: release(c); *c=(struct catalogue){.root=-1,.dir=-1,.lock=-1}; return -1;
}
static void name_for(char out[32],uint64_t seq) { snprintf(out,32,"%020" PRIu64 ".json",seq); }
static const char *relative(const char *path)
{
    size_t n=strlen(root);
    return n && !strncmp(path,root,n) && path[n]=='/' && path[n+1]?path+n+1:NULL;
}
static int media_stat(int dir,const char *path,struct stat *st)
{
    int fd=walk(dir,path,O_RDONLY); if (fd<0) return -1;
    int bad=fstat(fd,st) || !S_ISREG(st->st_mode) || !st->st_size; close(fd); return bad?-1:0;
}
static int same_file(int dir,J *o)
{
    struct stat st;
    return !media_stat(dir,str(o,"path"),&st) && st.st_size==json_object_get_int64(get(o,"bytes")) &&
        st.st_mtime==json_object_get_int64(get(o,"mtime"));
}
static void url(J *o,const char *key,const char *path)
{
    char input[1200],out[3600]; snprintf(input,sizeof(input),"%s/%s",root,path);
    char *p=out; for (const unsigned char *s=(unsigned char *)input;*s;++s) {
        if (isalnum(*s) || strchr("/-._~",*s)) *p++=*s; else { sprintf(p,"%%%02X",*s); p+=3; }
    } *p=0; text(o,key,out);
}
static int repair_refs(struct catalogue *c)
{
    uint64_t at=json_object_get_int64(get(c->state,"indexed")), end=json_object_get_int64(get(c->state,"sequence"));
    if (at>=end) return 0;
    for (;at<end;++at) {
        char name[32]; name_for(name,at+1); J *o=load(c->dir,name);
        if (o) {
            /* A crash after metadata can leave the path reference missing. */
            char refname[40]; snprintf(refname,sizeof(refname),"p-%016" PRIx64 ".json",hash(str(o,"path")));
            J *ref=json_object_new_int64(at+1); int bad=save(c->dir,refname,ref);
            json_object_put(ref); json_object_put(o);
            if (bad) return -1;
        }
    }
    integer(c->state,"indexed",end);
    return save(c->dir,"state.json",c->state);
}
static uint64_t destination(void)
{
    if (!json_object_get_boolean(get(config,"enabled"))) return 0;
    return hash(str(config,"url")) ^ hash(str(config,"token"));
}
static void ref_name(char out[40],const char *path) { snprintf(out,40,"p-%016" PRIx64 ".json",hash(path)); }
int catalogue_complete(const char *path,const char *source,double start,double duration,J *detections,const char *snapshot,double captured,int notify)
{
    const char *rel=relative(path); if (!rel || strlen(rel)>600) return -1;
    struct catalogue c; if (acquire(&c,1)) return -1;
    struct stat st; int rc=-1; J *o=NULL,*ref=NULL;
    if (media_stat(c.root,rel,&st) || repair_refs(&c)) goto out;
    char refname[40],name[32]; ref_name(refname,rel); ref=load(c.dir,refname);
    if (ref) {
        name_for(name,json_object_get_int64(ref)); o=load(c.dir,name);
        if (o && strcmp(str(o,"path"),rel)) goto out; /* Ref hash collision: fail closed. */
        if (o && same_file(c.root,o)) { rc=0; goto out; }
        json_object_put(o); o=NULL;
    }
    uint64_t seq=json_object_get_int64(get(c.state,"sequence"))+1;
    if (seq>INT64_MAX) goto out;
    integer(c.state,"sequence",seq); if (save(c.dir,"state.json",c.state)) goto out;
    char id[48]; snprintf(id,sizeof(id),"%s-%020" PRIu64,str(c.state,"generation"),seq);
    o=json_object_new_object(); integer(o,"schemaVersion",1); text(o,"id",id); text(o,"cameraId",str(c.state,"cameraId"));
    text(o,"source",source); text(o,"state","complete"); text(o,"path",rel); integer(o,"sequence",seq);
    integer(o,"bytes",st.st_size); integer(o,"mtime",st.st_mtime); stamp(o,"startedAt",start);
    stamp(o,"endedAt",st.st_mtime); stamp(o,"completedAt",time(NULL)); integer(o,"completedEpoch",time(NULL));
    if (duration>=0) json_object_object_add(o,"durationSeconds",json_object_new_double(duration));
    else json_object_object_add(o,"durationSeconds",NULL);
    text(o,"durationAccuracy",duration>=0?"estimated":"unknown");
    text(o,"classification",detections && json_object_array_length(detections)?"ai":"unclassified");
    json_object_object_add(o,"detections",detections?json_object_get(detections):json_object_new_array());
    const char *snap=snapshot?relative(snapshot):NULL; struct stat ss;
    if (snap && !media_stat(c.root,snap,&ss)) { text(o,"snapshotPath",snap); stamp(o,"snapshotCapturedAt",captured); }
    else { json_object_object_add(o,"snapshotPath",NULL); json_object_object_add(o,"snapshotCapturedAt",NULL); }
    char dest[24]; snprintf(dest,sizeof(dest),"%016" PRIx64,destination());
    text(o,"destination",dest); integer(o,"attempts",0); integer(o,"nextAttempt",0);
    text(o,"delivery",notify && destination()?"pending":"disabled");
    name_for(name,seq); if (save(c.dir,name,o)) goto out;
    json_object_put(ref); ref=json_object_new_int64(seq);
    if (save(c.dir,refname,ref) || repair_refs(&c)) goto out;
    rc=0;
    if (notify) {
        /* A bounded live signal; the SD catalogue remains the durable event feed. */
        J *latest=public_clip(c.root,o);
        char temporary[]=C120_RECORDING_LATEST ".XXXXXX";
        int fd=mkstemp(temporary);
        if (fd>=0) {
            const char *s=json_object_to_json_string_ext(latest,JSON_C_TO_STRING_PLAIN);
            size_t n=strlen(s); int bad=write(fd,s,n)!=(ssize_t)n;
            if (close(fd)) bad=1;
            if (bad || rename(temporary,C120_RECORDING_LATEST)) unlink(temporary);
        }
        json_object_put(latest);
    }
out: json_object_put(o); json_object_put(ref); release(&c); return rc;
}
static J *public_clip(int dir,J *o)
{
    J *out=json_object_new_object();
    const char *keys[]={"id","cameraId","source","state","bytes","startedAt","endedAt","completedAt",
        "durationSeconds","durationAccuracy","classification","detections","snapshotCapturedAt"};
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i) json_object_object_add(out,keys[i],json_object_get(get(o,keys[i])));
    url(out,"videoUrl",str(o,"path")); struct stat st;
    if (*str(o,"snapshotPath") && !media_stat(dir,str(o,"snapshotPath"),&st)) url(out,"snapshotUrl",str(o,"snapshotPath"));
    else { json_object_object_add(out,"snapshotUrl",NULL); json_object_object_add(out,"snapshotCapturedAt",NULL); }
    return out;
}
static J *error(int *status,int code,const char *s)
{ *status=code; J *o=json_object_new_object(); integer(o,"schemaVersion",1); text(o,"error",s); return o; }
static int decimal(const char *s,uint64_t *n)
{
    if (!*s || strlen(s)>20) return 0;
    for (const char *p=s;*p;++p) if (!isdigit((unsigned char)*p)) return 0;
    errno=0; char *end; *n=strtoull(s,&end,10); return !errno && !*end && *n<=INT64_MAX;
}
static int identity(const char *s,const char *generation,char separator,uint64_t *seq)
{ return strlen(s)>17 && !strncmp(s,generation,16) && s[16]==separator && decimal(s+17,seq); }
static int unescape(char *s)
{
    char *out=s;
    while (*s) {
        unsigned value;
        if (*s=='%') { if (!s[1] || !s[2] || !isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2]) || sscanf(s+1,"%2x",&value)!=1 || value<32 || value==127) return 0; *out++=value; s+=3; }
        else { if ((unsigned char)*s<32 || *s==127) return 0; *out++=*s=='+'?' ':*s; ++s; }
    } *out=0; return 1;
}
struct query { char *id,*cursor,*category,*source,*after,*before; uint64_t limit; };
static int parse_query(char *s,struct query *q)
{
    *q=(struct query){.limit=50}; unsigned seen=0; char *saveptr=NULL;
    for (char *p=strtok_r(s,"&",&saveptr);p;p=strtok_r(NULL,"&",&saveptr)) {
        char *eq=strchr(p,'='); if (!eq) return 0; *eq++=0;
        if (!unescape(p) || !unescape(eq)) return 0;
        const char *keys[]={"id","cursor","category","source","after","before","limit"};
        int i=0; while (i<7 && strcmp(p,keys[i])) ++i;
        if (i==7 || seen&(1u<<i) || !*eq) return 0;
        seen|=1u<<i;
        switch(i) { case 0:q->id=eq;break;case 1:q->cursor=eq;break;case 2:q->category=eq;break;
        case 3:q->source=eq;break;case 4:q->after=eq;break;case 5:q->before=eq;break;
        default:if (!decimal(eq,&q->limit) || q->limit<1 || q->limit>200) return 0; }
    }
    if (q->id && seen!=1) return 0;
    if (q->source && strcmp(q->source,"ai") && strcmp(q->source,"manual") && strcmp(q->source,"native") && strcmp(q->source,"imported")) return 0;
    const char *times[]={q->after,q->before};
    for (int i=0;i<2;++i) if (times[i]) {
        struct tm tm={0}; char *end=strptime(times[i],"%Y-%m-%dT%H:%M:%SZ",&tm); char check[32];
        if (!end || *end) return 0;
        timegm(&tm); strftime(check,sizeof(check),"%Y-%m-%dT%H:%M:%SZ",&tm);
        if (strcmp(check,times[i])) return 0;
    }
    if (q->category) {
        const char *labels[]={"person","pet","vehicle","bird","bark","meow","cry","glass"}; int found=0;
        for (unsigned i=0;i<8;++i) if (!strcmp(q->category,labels[i])) found=1;
        if (!found) return 0;
    }
    return 1;
}
static int matches(J *o,struct query *q)
{
    if (q->source && strcmp(q->source,str(o,"source"))) return 0;
    if (q->after && strcmp(str(o,"completedAt"),q->after)<=0) return 0;
    if (q->before && strcmp(str(o,"completedAt"),q->before)>=0) return 0;
    if (q->category) {
        J *a=get(o,"detections"); int found=0;
        for (size_t i=0;i<json_object_array_length(a);++i) if (!strcmp(q->category,str(json_object_array_get_idx(a,i),"category"))) found=1;
        if (!found) return 0;
    }
    return 1;
}
J *catalogue_query(const char *query,int *status)
{
    char args[2049]; if (!query) query="";
    if (strlen(query)>2048) return error(status,400,"Invalid query");
    strcpy(args,query); struct query q;
    if (!parse_query(args,&q)) return error(status,400,"Invalid query");
    struct catalogue c; if (acquire(&c,0)) return error(status,503,"Recording storage/catalogue unavailable");
    const char *gen=str(c.state,"generation"); uint64_t after=0;
    J *out=NULL;
    if (q.cursor && !identity(q.cursor,gen,':',&after)) { out=error(status,410,"Cursor expired or belongs to different storage"); goto done; }
    if (after>(uint64_t)json_object_get_int64(get(c.state,"sequence"))) { out=error(status,400,"Cursor is ahead of catalogue"); goto done; }
    if (q.id) {
        uint64_t seq; if (!identity(q.id,gen,'-',&seq) || !seq || strlen(q.id)!=37) { out=error(status,400,"Invalid recording ID"); goto done; }
        char name[32]; name_for(name,seq); J *o=load(c.dir,name);
        if (!o || !same_file(c.root,o)) out=error(status,404,"Recording not found");
        else { out=public_clip(c.root,o); integer(out,"schemaVersion",1); *status=200; }
        json_object_put(o); goto done;
    }
    /* ponytail: linear scan; restore an index only if catalogue growth makes paging slow. */
    uint64_t chosen[201]={0},maximum=json_object_get_int64(get(c.state,"sequence")); unsigned count=0;
    int fd=openat(c.dir,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    DIR *dir=fd>=0?fdopendir(fd):NULL; if (fd>=0 && !dir) close(fd);
    if (!dir) { out=error(status,503,"Catalogue scan failed"); goto done; }
    int damaged=0;
    for (;;) {
        errno=0; struct dirent *entry=readdir(dir);
        if (!entry) { damaged=errno!=0; break; }
        const char *name=entry->d_name;
        if (strlen(name)!=25 || strspn(name,"0123456789")!=20 || strcmp(name+20,".json")) continue;
        char number[21]; memcpy(number,name,20); number[20]=0; uint64_t seq;
        if (!decimal(number,&seq)) continue;
        if (seq<=after) continue;
        if (seq>maximum) continue;
        unsigned pos=0; while (pos<count && chosen[pos]<seq) ++pos;
        if ((pos<count && chosen[pos]==seq) || pos>q.limit) continue;
        J *o=load(c.dir,name);
        int ok=o && same_file(c.root,o) && matches(o,&q); json_object_put(o); if (!ok) continue;
        if (count<q.limit+1) ++count;
        memmove(chosen+pos+1,chosen+pos,(count-pos-1)*sizeof(*chosen)); chosen[pos]=seq;
    }
    closedir(dir);
    if (damaged) { out=error(status,503,"Catalogue scan failed"); goto done; }
    out=json_object_new_object(); integer(out,"schemaVersion",1); text(out,"cameraId",str(c.state,"cameraId"));
    text(out,"storageGeneration",gen); J *items=json_object_new_array();
    int more=count>q.limit; if (more) count=q.limit;
    for (unsigned i=0;i<count;++i) { char name[32]; name_for(name,chosen[i]); J *o=load(c.dir,name); if (o) json_object_array_add(items,public_clip(c.root,o)); json_object_put(o); }
    json_object_object_add(out,"recordings",items); json_object_object_add(out,"hasMore",json_object_new_boolean(more));
    char cursor[48]; snprintf(cursor,sizeof(cursor),"%s:%020" PRIu64,gen,more?chosen[count-1]:maximum); text(out,"nextCursor",cursor); *status=200;
done: release(&c); return out;
}

static int open_by_writer(const struct stat *target)
{
    DIR *proc=opendir("/proc"); if (!proc) return 1; struct dirent *e; int found=0;
    while (!found && (e=readdir(proc))) {
        if (!isdigit((unsigned char)*e->d_name)) continue;
        char path[320]; snprintf(path,sizeof(path),"/proc/%s/fd",e->d_name); DIR *fds=opendir(path); if (!fds) continue;
        struct dirent *entry;
        while (!found && (entry=readdir(fds))) { struct stat st; if (fstatat(dirfd(fds),entry->d_name,&st,0)) continue; if (st.st_dev==target->st_dev && st.st_ino==target->st_ino) found=1; }
        closedir(fds);
    } closedir(proc); return found;
}
static int unlink_media(int dir,const char *path)
{
    if (!*path || strlen(path)>=640) return -1;
    char copy[640]; strcpy(copy,path); char *slash=strrchr(copy,'/'); int parent;
    const char *name=copy;
    if (slash) { *slash=0; name=slash+1; parent=walk(dir,copy,O_RDONLY|O_DIRECTORY); }
    else parent=dup(dir);
    if (parent<0) return -1;
    struct stat st; int rc=-1;
    if (strcmp(name,".") && strcmp(name,"..") && !fstatat(parent,name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode) && !open_by_writer(&st))
        rc=unlinkat(parent,name,0);
    close(parent); return rc;
}
static void forget(struct catalogue *c,const char *name,J *o)
{
    char snapshot[680]; snprintf(snapshot,sizeof(snapshot),"%s.jpg",str(o,"path"));
    /* Only remove the companion this plugin created, never arbitrary metadata-supplied paths. */
    if (!strcmp(snapshot,str(o,"snapshotPath"))) unlink_media(c->root,snapshot);
    char refname[40]; ref_name(refname,str(o,"path")); J *ref=load(c->dir,refname);
    if (ref && json_object_get_int64(ref)==json_object_get_int64(get(o,"sequence"))) unlinkat(c->dir,refname,0);
    json_object_put(ref); unlinkat(c->dir,name,0);
}
static int pressure(int fd,int margin)
{
    struct statvfs fs; if (fstatvfs(fd,&fs) || !fs.f_blocks || !fs.f_frsize) return -1;
    double used=(double)(fs.f_blocks-fs.f_bavail)*100/fs.f_blocks;
    int threshold=max_usage-margin; if (threshold<1) threshold=1;
    return used>=threshold || (double)fs.f_bavail*fs.f_frsize<(margin?32:16)*1024*1024;
}
struct oldest { int64_t mtime,seq; };
static int compare_oldest(const void *a,const void *b)
{
    const struct oldest *x=a,*y=b;
    if (x->mtime!=y->mtime) return x->mtime<y->mtime?-1:1;
    return x->seq<y->seq?-1:x->seq>y->seq;
}
static void prune(void)
{
    struct catalogue c; if (acquire(&c,0)) return;
    int full=recording_enabled && !native_enabled && pressure(c.root,0)==1;
    struct oldest oldest[32]; unsigned count=0;
    DIR *dir=fdopendir(dup(c.dir)); struct dirent *e;
    while (dir && (e=readdir(dir))) {
        if (strlen(e->d_name)!=25 || !isdigit((unsigned char)*e->d_name)) continue;
        J *o=load(c.dir,e->d_name); if (!o) continue;
        if (!same_file(c.root,o)) forget(&c,e->d_name,o);
        else if (full) {
            struct oldest value={json_object_get_int64(get(o,"mtime")),json_object_get_int64(get(o,"sequence"))};
            if (count<32) oldest[count++]=value;
            else if (compare_oldest(&value,&oldest[31])<0) oldest[31]=value;
            qsort(oldest,count,sizeof(*oldest),compare_oldest);
        }
        json_object_put(o);
    }
    if (dir) closedir(dir);
    for (unsigned i=0;i<count && pressure(c.root,2)==1;++i) {
        char name[32]; name_for(name,oldest[i].seq); J *o=load(c.dir,name);
        if (o && same_file(c.root,o) && !unlink_media(c.root,str(o,"path"))) forget(&c,name,o);
        json_object_put(o);
    }
    release(&c);
}
int catalogue_native(const char *path,const char *reason,const char *seconds)
{
    if (!strcmp(reason,"error")) {
        const char *rel=relative(path); struct catalogue c;
        if (rel && !acquire(&c,1)) {
            struct stat st;
            if (!media_stat(c.root,rel,&st)) {
                J *o=json_object_new_object(); text(o,"path",rel); integer(o,"bytes",st.st_size); integer(o,"mtime",st.st_mtime);
                json_object_object_add(o,"rejected",json_object_new_boolean(1));
                char name[40]; ref_name(name,rel); save(c.dir,name,o); json_object_put(o);
            }
            release(&c);
        }
        return -1;
    }
    if (strcmp(reason,"motion") && strcmp(reason,"split") && strcmp(reason,"stop")) return -1;
    char *end; double duration=strtod(seconds,&end); if (!*seconds || *end || !isfinite(duration) || duration<0 || duration>86400) return -1;
    const char *rel=relative(path); int fd=storage(); struct stat st;
    int good=rel && fd>=0 && !media_stat(fd,rel,&st); if (fd>=0) close(fd);
    return good?catalogue_complete(path,"native",st.st_mtime-duration,duration,NULL,NULL,0,1):-1;
}
static void reconcile(int fd,const char *prefix,unsigned depth,unsigned *budget)
{
    if (depth>8 || !*budget) return;
    DIR *d=fdopendir(dup(fd)); if (!d) return; struct dirent *e;
    while (*budget && (e=readdir(d))) {
        if (*e->d_name=='.') continue;
        struct stat st; if (fstatat(fd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)) continue;
        char rel[640]; int n=snprintf(rel,sizeof(rel),"%s%s%s",prefix,*prefix?"/":"",e->d_name); if (n<0 || n>600) continue;
        if (S_ISDIR(st.st_mode)) {
            int sub=openat(fd,e->d_name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
            if (sub>=0) { reconcile(sub,rel,depth+1,budget); close(sub); } continue;
        }
        size_t len=strlen(e->d_name);
        if (!S_ISREG(st.st_mode) || !st.st_size || len<5 || strcmp(e->d_name+len-4,".mp4") || time(NULL)-st.st_mtime<10) continue;
        struct catalogue c; if (acquire(&c,1)) break;
        char refname[40],name[32]; ref_name(refname,rel); J *ref=load(c.dir,refname),*o=NULL;
        if (json_object_is_type(ref,json_type_int)) { name_for(name,json_object_get_int64(ref)); o=load(c.dir,name); }
        int known=(o && !strcmp(str(o,"path"),rel) && same_file(c.root,o)) ||
            (json_object_get_boolean(get(ref,"rejected")) && !strcmp(str(ref,"path"),rel) && same_file(c.root,ref));
        json_object_put(ref); json_object_put(o); release(&c); if (known || open_by_writer(&st)) continue;
        int media=openat(fd,e->d_name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        off_t complete=media>=0?mp4_complete_bytes(media,1):0; if (media>=0) close(media);
        if (!complete) continue;
        --*budget; char full[1200]; snprintf(full,sizeof(full),"%s/%s",root,rel);
        catalogue_complete(full,"imported",0,-1,NULL,NULL,0,0);
    } closedir(d);
}
static size_t discard(char *p,size_t size,size_t n,void *unused) { (void)p; (void)unused; return size*n; }
static void deliver(void)
{
    struct catalogue c; if (acquire(&c,1)) return;
    DIR *dir=fdopendir(dup(c.dir)); if (!dir) { release(&c); return; }
    J *item=NULL,*payload=NULL; char name[32]=""; struct dirent *e; time_t now=time(NULL);
    char dest[24]; snprintf(dest,sizeof(dest),"%016" PRIx64,destination());
    while ((e=readdir(dir))) {
        if (strlen(e->d_name)!=25 || !isdigit((unsigned char)*e->d_name)) continue;
        J *o=load(c.dir,e->d_name); if (!o || strcmp(str(o,"delivery"),"pending")) { json_object_put(o); continue; }
        const char *cancel=NULL;
        if (!destination() || strcmp(dest,str(o,"destination"))) cancel="cancelled";
        else if (now-json_object_get_int64(get(o,"completedEpoch"))>=86400) cancel="expired";
        else if (!same_file(c.root,o)) cancel="deleted";
        if (cancel) { text(o,"delivery",cancel); save(c.dir,e->d_name,o); json_object_put(o); continue; }
        if (now<json_object_get_int64(get(o,"nextAttempt"))) { json_object_put(o); continue; }
        item=o; memcpy(name,e->d_name,26); payload=json_object_new_object();
        integer(payload,"schemaVersion",1); text(payload,"type","recording.ready");
        text(payload,"cameraId",str(o,"cameraId")); text(payload,"recordingId",str(o,"id"));
        json_object_object_add(payload,"recording",public_clip(c.root,o)); break;
    }
    closedir(dir); release(&c); if (!item) return;
    /* No catalogue lock during network I/O. This runs in a short-lived worker, never the inference loop. */
    CURL *curl=curl_easy_init(); struct curl_slist *headers=NULL; long code=0; CURLcode rc=CURLE_FAILED_INIT;
    headers=curl_slist_append(headers,"Content-Type: application/json"); char auth[544];
    if (*str(config,"token")) { snprintf(auth,sizeof(auth),"Authorization: Bearer %s",str(config,"token")); headers=curl_slist_append(headers,auth); }
    if (curl) {
        curl_easy_setopt(curl,CURLOPT_URL,str(config,"url")); curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN));
        curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,3000L); curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,1000L);
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard); rc=curl_easy_perform(curl); curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&code);
    }
    curl_easy_cleanup(curl); curl_slist_free_all(headers); json_object_put(payload);
    if (!acquire(&c,0)) {
        J *current=load(c.dir,name);
        if (current && !strcmp(str(current,"id"),str(item,"id")) && !strcmp(str(current,"delivery"),"pending")) {
            int attempts=json_object_get_int(get(current,"attempts"))+1;
            integer(current,"attempts",attempts); integer(current,"nextAttempt",now+(attempts<6?(5<<attempts):300));
            if (rc==CURLE_OK && code>=200 && code<300) text(current,"delivery","delivered");
            save(c.dir,name,current);
        }
        json_object_put(current); release(&c);
    } json_object_put(item);
}
void catalogue_work(void)
{
    struct catalogue c; if (acquire(&c,1)) return;
    int lock=openat(c.dir,"worker.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { if (lock>=0) close(lock); release(&c); return; }
    if (repair_refs(&c)) { close(lock); release(&c); return; }
    time_t now=time(NULL); int scan=now-json_object_get_int64(get(c.state,"lastScan"))>=60;
    if (scan) { integer(c.state,"lastScan",now); save(c.dir,"state.json",c.state); }
    release(&c);
    prune();
    int fd=storage(); if (fd<0) { close(lock); return; }
    /* Bound imports per pass. Existing refs make each subsequent pass incremental. */
    unsigned budget=64; if (scan) reconcile(fd,"",0,&budget);
    close(fd); deliver(); close(lock);
}
void catalogue_poll(void)
{
    if (worker>0) { if (!waitpid(worker,NULL,WNOHANG)) return; worker=0; }
    time_t now=time(NULL); if (now<next_work || !*root) return; next_work=now+10;
    char *args[]={"/usr/bin/c120-ai","catalogue-worker",NULL};
    if (posix_spawn(&worker,args[0],NULL,NULL,args,environ)) worker=0;
}
void catalogue_stop(void)
{
    if (worker>0) { kill(worker,SIGTERM); waitpid(worker,NULL,0); worker=0; }
}
