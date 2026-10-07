#define _GNU_SOURCE
#include "models.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct json_object J;
#ifndef C120_MODEL_MOUNTS
#define C120_MODEL_MOUNTS "/proc/mounts"
#endif
#ifndef C120_MODEL_LOCK
#define C120_MODEL_LOCK "/run/c120-models.lock"
#endif
#define MODEL_LIMIT (8u*1024*1024)
#define LIBRARY_LIMIT 16
#ifndef C120_MODEL_PREFIX
#define C120_MODEL_PREFIX "/mnt/"
#endif
static J *field(J *o,const char *key) { return json_object_object_get(o,key); }
static const char *string(J *o,const char *key) { return json_object_get_string(field(o,key)); }
static J *error(const char *message) { J *o=json_object_new_object(); json_object_object_add(o,"error",json_object_new_string(message)); return o; }

int model_id_valid(const char *s)
{
    if (!s || !*s || strlen(s)>48 || !islower((unsigned char)*s)) return 0;
    for (;*s;++s) if (!(*s>='a' && *s<='z') && !isdigit((unsigned char)*s) && *s!='-' && *s!='_') return 0;
    return 1;
}
static int plain(J *o,const char *key,size_t limit)
{
    J *v=field(o,key); const char *s=json_object_get_string(v);
    if (!json_object_is_type(v,json_type_string) || !s || !*s || strlen(s)>limit || strlen(s)!=(size_t)json_object_get_string_len(v)) return 0;
    for (;*s;++s) if ((unsigned char)*s<32 || (unsigned char)*s==127) return 0;
    return 1;
}
int model_profile(J *o,struct detector_model *m)
{
    if (!json_object_is_type(o,json_type_object) || json_object_object_length(o)!=9 ||
        !json_object_is_type(field(o,"version"),json_type_int) || json_object_get_int(field(o,"version"))!=1 ||
        !plain(o,"id",48) || !model_id_valid(string(o,"id")) || !strcmp(string(o,"id"),"stock") ||
        !plain(o,"name",80) || !plain(o,"target",32) || strcmp(string(o,"target"),"ssc377-ipu-v6") ||
        !plain(o,"decoder",40) || strcmp(string(o,"decoder"),"yolov5n-coco-bird-v1") ||
        !plain(o,"sha256",64) || strlen(string(o,"sha256"))!=64 ||
        !json_object_is_type(field(o,"modelBytes"),json_type_int) ||
        json_object_get_int64(field(o,"modelBytes"))<1024 || json_object_get_int64(field(o,"modelBytes"))>MODEL_LIMIT) return 0;
    const char *hash=string(o,"sha256");
    for (int i=0;i<64;++i) if (!isdigit((unsigned char)hash[i]) && !(hash[i]>='a' && hash[i]<='f')) return 0;
    const char *keys[]={"confidence","nms"};
    for (int i=0;i<2;++i) {
        J *v=field(o,keys[i]); double n=json_object_get_double(v);
        if ((!json_object_is_type(v,json_type_double) && !json_object_is_type(v,json_type_int)) ||
            !isfinite(n) || n<(i?.05:.25) || n>(i?.95:.99)) return 0;
    }
    memset(m,0,sizeof(*m)); m->fd=-1; m->bird=1;
    snprintf(m->id,sizeof(m->id),"%s",string(o,"id")); snprintf(m->name,sizeof(m->name),"%s",string(o,"name"));
    snprintf(m->sha256,sizeof(m->sha256),"%s",hash); m->bytes=json_object_get_int(field(o,"modelBytes"));
    m->confidence=json_object_get_double(field(o,"confidence")); m->nms=json_object_get_double(field(o,"nms"));
    return 1;
}
static J *read_at(int dir,const char *name)
{
    char buf[4097]; int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    struct stat st;
    if (fd<0) return NULL;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<2 || st.st_size>4096) { close(fd); return NULL; }
    ssize_t n=read(fd,buf,sizeof(buf)-1); close(fd);
    if (n!=st.st_size) return NULL;
    buf[n]=0; struct json_tokener *t=json_tokener_new(); if (!t) return NULL;
    json_tokener_set_flags(t,JSON_TOKENER_STRICT); J *o=json_tokener_parse_ex(t,buf,n);
    size_t end=json_tokener_get_parse_end(t); while (end<(size_t)n && isspace((unsigned char)buf[end])) ++end;
    if (json_tokener_get_error(t)!=json_tokener_success || end!=(size_t)n) { json_object_put(o); o=NULL; }
    json_tokener_free(t); return o;
}
static int sd_device(const char *s)
{
    if (strncmp(s,"/dev/mmcblk",11)) return 0;
    s+=11; if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) ++s;
    if (*s=='p') { ++s; if (!isdigit((unsigned char)*s)) return 0; while (isdigit((unsigned char)*s)) ++s; }
    return !*s;
}
static int directory_at(int dir,const char *name,int create)
{
    if (create && mkdirat(dir,name,0700) && errno!=EEXIST) return -1;
    return openat(dir,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static int library(char location[256],int *writable,int create)
{
    *writable=0; location[0]=0;
    FILE *f=setmntent(C120_MODEL_MOUNTS,"r"); if (!f) return -1;
    struct mntent *m; int fd=-1;
    while ((m=getmntent(f))) {
        if (!sd_device(m->mnt_fsname) || strncmp(m->mnt_dir,C120_MODEL_PREFIX,strlen(C120_MODEL_PREFIX)) || strlen(m->mnt_dir)>200 ||
            (strcmp(m->mnt_type,"vfat") && strcmp(m->mnt_type,"exfat") && strcmp(m->mnt_type,"ext4") &&
             strcmp(m->mnt_type,"ext3") && strcmp(m->mnt_type,"ext2") && strcmp(m->mnt_type,"f2fs"))) continue;
        int mount=open(m->mnt_dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        struct stat st,root; struct statvfs fs;
        if (mount<0) continue;
        if (fstat(mount,&st) || stat("/",&root) || st.st_dev==root.st_dev || fstatvfs(mount,&fs)) { close(mount); continue; }
        *writable=!(fs.f_flag&ST_RDONLY);
        /* The target libc's hasmntopt matches "ro" inside "errors=remount-ro". */
        char *save=NULL;
        for (char *opt=strtok_r(m->mnt_opts,",",&save); opt; opt=strtok_r(NULL,",",&save))
            if (!strcmp(opt,"ro")) *writable=0;
        snprintf(location,256,"%s/c120-ai/models",m->mnt_dir);
        int parent=directory_at(mount,"c120-ai",create && *writable); close(mount);
        if (parent>=0) { fd=directory_at(parent,"models",create && *writable); close(parent); }
        break;
    }
    endmntent(f); return fd;
}
static int hash_matches(int fd,const char *expected)
{
    int p[2]; if (lseek(fd,0,SEEK_SET)<0 || pipe(p)) return 0;
    pid_t pid=fork();
    if (!pid) {
        if (dup2(fd,STDIN_FILENO)<0 || dup2(p[1],STDOUT_FILENO)<0) _exit(127);
        close(p[0]); close(p[1]); execlp("sha256sum","sha256sum",(char *)NULL); _exit(127);
    }
    close(p[1]); char hash[65]={0}; size_t n=0;
    while (n<64) { ssize_t k=read(p[0],hash+n,64-n); if (k<=0) break; n+=k; }
    /* Drain stdout so a failed hash process cannot block on its pipe. */
    char tail[256]; while (read(p[0],tail,sizeof(tail))>0) {}
    close(p[0]); int status=0;
    if (pid<0 || waitpid(pid,&status,0)!=pid || !WIFEXITED(status) || WEXITSTATUS(status)) return 0;
    return n==64 && !strcmp(hash,expected) && lseek(fd,0,SEEK_SET)==0;
}
static int open_entry(int lib,const char *id,struct detector_model *m,int verify)
{
    int dir=directory_at(lib,id,0); if (dir<0) return -1;
    J *o=read_at(dir,"profile.json"); int valid=model_profile(o,m) && !strcmp(m->id,id); json_object_put(o);
    if (!valid) { close(dir); return -1; }
    int fd=openat(dir,"model.img",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); close(dir); struct stat st;
    if (fd<0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=m->bytes || (verify && !hash_matches(fd,m->sha256))) { close(fd); return -1; }
    m->fd=fd; return 0;
}
int model_open(const char *id,struct detector_model *m,char err[160])
{
    if (!model_id_valid(id)) { snprintf(err,160,"Invalid model ID"); return -1; }
    if (!strcmp(id,"stock")) {
        memset(m,0,sizeof(*m)); m->fd=-1; strcpy(m->id,"stock"); strcpy(m->name,"Stock detector");
        m->bytes=4501504; m->confidence=.6; m->nms=.45; return 0;
    }
    char path[256]; int writable,lib=library(path,&writable,0);
    if (lib<0) { snprintf(err,160,"SD model library unavailable"); return -1; }
    int rc=open_entry(lib,id,m,1); close(lib);
    if (rc) snprintf(err,160,"Model missing, incomplete, incompatible or checksum failed");
    return rc;
}
int model_available(const char *id)
{
    if (!strcmp(id,"stock")) return 1;
    char path[256]; int writable,lib=library(path,&writable,0); if (lib<0) return 0;
    struct detector_model m; int rc=open_entry(lib,id,&m,0); if (!rc) close(m.fd); close(lib); return !rc;
}
static J *entry(const struct detector_model *m,const char *location,const char *selected,const char *active)
{
    J *o=json_object_new_object(),*categories=json_object_new_array();
    json_object_object_add(o,"id",json_object_new_string(m->id)); json_object_object_add(o,"name",json_object_new_string(m->name));
    json_object_object_add(o,"location",json_object_new_string(location));
    json_object_object_add(o,"selected",json_object_new_boolean(!strcmp(m->id,selected)));
    json_object_object_add(o,"active",json_object_new_boolean(!strcmp(m->id,active)));
    json_object_object_add(o,"confidence",json_object_new_double(m->confidence)); json_object_object_add(o,"nms",json_object_new_double(m->nms));
    const char *names[]={"person","pet","vehicle"};
    for (int i=0;i<(m->bird?1:3);++i) json_object_array_add(categories,json_object_new_string(m->bird?"bird":names[i]));
    json_object_object_add(o,"categories",categories); return o;
}
J *models_state(const char *selected,const char *active)
{
    J *o=json_object_new_object(),*list=json_object_new_array(),*uploads=json_object_new_array(); struct detector_model stock;
    char err[160],path[256]; int writable=0;
    model_open("stock",&stock,err); json_object_array_add(list,entry(&stock,"Bundled",selected,active));
    int lib=library(path,&writable,0);
    json_object_object_add(o,"storage",json_object_new_string(*path?path:"No mounted SD card"));
    json_object_object_add(o,"writable",json_object_new_boolean(*path && writable));
    if (lib>=0) {
        DIR *d=fdopendir(dup(lib)); struct dirent *e; unsigned count=0;
        while (d && (e=readdir(d)) && count<LIBRARY_LIMIT) {
            if (!strncmp(e->d_name,".upload-",8) && model_id_valid(e->d_name+8)) { json_object_array_add(uploads,json_object_new_string(e->d_name+8)); ++count; continue; }
            if (!model_id_valid(e->d_name) || !strcmp(e->d_name,"stock")) continue;
            struct detector_model m;
            if (!open_entry(lib,e->d_name,&m,0)) { close(m.fd); json_object_array_add(list,entry(&m,"SD card",selected,active)); ++count; }
        }
        if (d) closedir(d);
        close(lib);
    }
    json_object_object_add(o,"items",list); json_object_object_add(o,"uploads",uploads); return o;
}
static int write_all(int fd,const void *buf,size_t n)
{
    const char *p=buf;
    while (n) { ssize_t k=write(fd,p,n); if (k<0 && errno==EINTR) continue; if (k<=0) return -1; p+=k; n-=k; }
    return 0;
}
static int decode_chunk(const char *s,unsigned char out[3072])
{
    const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n=s?strlen(s):0; if (!n || n>4096 || n%4) return -1;
    int bytes=0;
    for (size_t i=0;i<n;i+=4) {
        unsigned v=0; int padding=0;
        for (int j=0;j<4;++j) {
            if (s[i+j]=='=') { if (i+4!=n || j<2) return -1; ++padding; v<<=6; }
            else { const char *p=strchr(alphabet,s[i+j]); if (!p || padding) return -1; v=(v<<6)|(p-alphabet); }
        }
        if (padding>2 || (padding==2 && (v&0xffff)) || (padding==1 && (v&0xff))) return -1;
        out[bytes++]=v>>16; if (padding<2) out[bytes++]=v>>8; if (!padding) out[bytes++]=v;
    }
    return bytes;
}
static int count_entries(int lib)
{
    DIR *d=fdopendir(dup(lib)); if (!d) return LIBRARY_LIMIT;
    int count=0; struct dirent *e;
    while ((e=readdir(d))) if (strcmp(e->d_name,".") && strcmp(e->d_name,"..")) ++count;
    closedir(d); return count;
}
J *models_request(J *o,const char *selected,const char *active,const char *previous)
{
    const char *action=string(o,"modelAction"),*id=string(o,"id");
    if (!plain(o,"modelAction",16) || !plain(o,"id",48) || !model_id_valid(id) || !strcmp(id,"stock")) return error("Invalid model request");
    int lock=open(C120_MODEL_LOCK,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lock<0 || flock(lock,LOCK_EX|LOCK_NB)) { if (lock>=0) close(lock); return error("Model library busy; try again"); }
    char path[256],upload[64]; int writable=0,lib=library(path,&writable,1),dir=-1;
    J *result=NULL; snprintf(upload,sizeof(upload),".upload-%s",id);
    if (lib<0 || !writable) { result=error("A writable mounted SD card is required"); goto done; }
    if (!strcmp(action,"start")) {
        struct detector_model m;
        if (!model_profile(field(o,"profile"),&m) || strcmp(id,m.id)) { result=error("Unsupported model profile"); goto done; }
        struct stat st; struct statvfs space;
        if (!fstatat(lib,id,&st,AT_SYMLINK_NOFOLLOW)) { result=error("Model already exists; choose another ID"); goto done; }
        if (count_entries(lib)>=LIBRARY_LIMIT || fstatvfs(lib,&space) || (double)space.f_bavail*space.f_frsize<m.bytes+1024*1024) { result=error("SD card full or model library limit reached"); goto done; }
        if (mkdirat(lib,upload,0700)) { result=error("An unfinished upload exists; cancel it first"); goto done; }
        dir=directory_at(lib,upload,0);
        int fd=dir<0?-1:openat(dir,"profile.json",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        const char *json=json_object_to_json_string_ext(field(o,"profile"),JSON_C_TO_STRING_PLAIN);
        int bad=fd<0; if (fd>=0) { bad=write_all(fd,json,strlen(json)) || fsync(fd); if (close(fd)) bad=1; }
        fd=dir<0?-1:openat(dir,"model.img",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) bad=1; else close(fd);
        if (bad) result=error("Could not begin SD upload; cancel to clean up");
    } else {
        const char *directory=!strcmp(action,"remove")?id:upload;
        dir=directory_at(lib,directory,0);
        if (dir<0) { result=error("Model or upload not found"); goto done; }
        if (!strcmp(action,"remove") || !strcmp(action,"cancel")) {
            if (!strcmp(action,"remove") && (!strcmp(id,selected) || !strcmp(id,active) || !strcmp(id,previous))) { result=error("Select another model before removing an active, selected or rollback model"); goto done; }
            if ((unlinkat(dir,"model.img",0) && errno!=ENOENT) || (unlinkat(dir,"profile.json",0) && errno!=ENOENT) || unlinkat(lib,directory,AT_REMOVEDIR)) result=error("Could not remove model files");
        } else {
            J *profile=read_at(dir,"profile.json"); struct detector_model m;
            int valid=model_profile(profile,&m) && !strcmp(m.id,id); json_object_put(profile);
            if (!valid) { result=error("Upload profile is invalid"); goto done; }
            if (!strcmp(action,"chunk")) {
                unsigned char data[3072]; int n=plain(o,"data",4096)?decode_chunk(string(o,"data"),data):-1;
                int64_t offset=json_object_get_int64(field(o,"offset"));
                if (n<1 || !json_object_is_type(field(o,"offset"),json_type_int) || offset<0 || offset>m.bytes || n>m.bytes-offset) { result=error("Invalid upload chunk"); goto done; }
                int fd=openat(dir,"model.img",O_WRONLY|O_APPEND|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); struct stat st;
                int bad=fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=offset;
                if (!bad) bad=write_all(fd,data,n);
                if (fd>=0) close(fd);
                if (bad) result=error("Upload offset mismatch or SD write failed");
            } else if (!strcmp(action,"finish")) {
                int fd=openat(dir,"model.img",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC); struct stat st;
                int bad=fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size!=m.bytes || fsync(fd) || !hash_matches(fd,m.sha256);
                if (fd>=0) close(fd);
                struct stat final;
                if (bad || !fstatat(lib,id,&final,AT_SYMLINK_NOFOLLOW) || fsync(dir) || renameat(lib,upload,lib,id) || fsync(lib)) result=error("Incomplete upload, checksum failure or SD commit failed");
            } else result=error("Unknown model action");
        }
    }
done:
    if (dir>=0) close(dir);
    if (lib>=0) close(lib);
    close(lock);
    return result?result:models_state(selected,active);
}
