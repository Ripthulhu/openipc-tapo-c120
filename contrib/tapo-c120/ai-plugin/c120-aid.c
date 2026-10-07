#define _GNU_SOURCE
#include <assert.h>
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
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
#include <zlib.h>
#include "../ai-probe/frame-abi.h"
#include "sound.h"
#include "opus-input.h"
#include "notify.h"
#include "record.h"
#include "models.h"
#include "bird.h"

#define CONFIG "/etc/c120-ai.json"
#define STATE "/run/c120-ai-state.json"
#define PIDFILE "/run/c120-ai.pid"
#define TOKEN "/run/c120-ai.token"
#define AP "/run/c120-setup-ap.active"
#define MODEL "/usr/lib/c120-ai/objects.img.gz"
#define MODEL_BYTES 4501504U
#define SOUND_MODEL "/usr/lib/c120-ai/sound.img.gz"
#define SOUND_BYTES 480640U
#define SCL_PROC "/proc/mi_modules/mi_scl/mi_scl0"
#define LIGHT_SIGNAL "/run/c120-ai-light.signal"

typedef struct json_object J;
static volatile sig_atomic_t stopping, reloading, testing_notification;
static J *settings, *events, *regions;
static unsigned frames;
static unsigned long long sequence;
static char boot_id[40] = "unknown";
static double last_seen[9], last_event[9];
static double inference_ms;
static unsigned main_width, main_height;
static const char *reason = "Starting";
static const char *sound_reason = "Disabled";
static unsigned audio_rate, sound_rate, sound_frames;
static double sound_ms, sound_last_event[4], sound_last_seen[4], sound_last_data, sound_retry, audio_started;
static float sound_scores[7];
static unsigned sound_history[10], sound_history_pos;
static int sound_current = -1;
static const char *sound_labels[] = {"bark", "meow", "cry", "glass"};
static struct detector_model active_model;
static char model_error[160], previous_model[49]="stock";
static struct bird_tensor bird_input, bird_heads[3];
static double visual_warmup;
static int missing_model;
static unsigned mma_kb(void);

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static J *field(J *o, const char *name) { return json_object_object_get(o, name); }
static void add(J *o, const char *k, J *v) { json_object_object_add(o, k, v); }
static int boolean(J *o, const char *k) { return json_object_get_boolean(field(o, k)); }
static double number(J *o, const char *k) { return json_object_get_double(field(o, k)); }
static void text(J *o, const char *k, const char *v) { add(o, k, json_object_new_string(v)); }

static J *parse(const char *s, size_t n)
{
    struct json_tokener *t = json_tokener_new();
    if (!t || n > 65536) { if (t) json_tokener_free(t); return NULL; }
    json_tokener_set_flags(t, JSON_TOKENER_STRICT);
    J *o = json_tokener_parse_ex(t, s, (int)n);
    size_t end = json_tokener_get_parse_end(t);
    while (end < n && isspace((unsigned char)s[end])) ++end;
    if (json_tokener_get_error(t) != json_tokener_success || end != n) {
        json_object_put(o); o = NULL;
    }
    json_tokener_free(t);
    return o;
}

static J *read_json(const char *path)
{
    char buf[65537];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf), f);
    int bad = ferror(f) || n == sizeof(buf);
    fclose(f);
    return bad ? NULL : parse(buf, n);
}

static int atomic_text(const char *path, const char *value, int durable)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    int bad = fprintf(f, "%s\n", value) < 0;
    if (fflush(f) || (durable && fsync(fd))) bad = 1;
    if (fclose(f)) bad = 1;
    if (!bad && rename(tmp, path)) bad = 1;
    if (bad) unlink(tmp);
    return bad ? -1 : 0;
}

static int atomic_json(const char *path, J *o, int durable)
{
    return atomic_text(path,json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN),durable);
}

static J *defaults(void)
{
    J *o = json_tokener_parse("{\"enabled\":false,\"confidence\":0.6,\"intervalMs\":500,\"motionRegions\":true,"
        "\"soundEnabled\":false,\"soundSensitivity\":1,\"soundGainDb\":12,\"soundClasses\":[\"bark\",\"meow\",\"cry\",\"glass\"]}");
    add(o,"notifications",notify_defaults());
    add(o,"recording",record_defaults());
    text(o,"model","stock"); add(o,"nms",json_object_new_double(.45));
    return o;
}

static int valid_settings(J *o)
{
    J *model=field(o,"model"),*nms=field(o,"nms");
    if (!o || !json_object_is_type(o, json_type_object) || json_object_object_length(o) != 12 ||
        !json_object_is_type(model,json_type_string) ||
        json_object_get_string_len(model)!=(int)strlen(json_object_get_string(model)) || !model_id_valid(json_object_get_string(model)) ||
        (!json_object_is_type(nms,json_type_double) && !json_object_is_type(nms,json_type_int)) ||
        !isfinite(number(o,"nms")) || number(o,"nms")<.05 || number(o,"nms")>.95 ||
        !notify_valid(field(o,"notifications")) || !record_valid(field(o,"recording"))) return 0;
    if (!json_object_is_type(field(o, "enabled"), json_type_boolean) ||
        !json_object_is_type(field(o, "motionRegions"), json_type_boolean) ||
        !json_object_is_type(field(o, "intervalMs"), json_type_int) ||
        !json_object_is_type(field(o, "soundEnabled"), json_type_boolean) ||
        !json_object_is_type(field(o, "soundSensitivity"), json_type_int) ||
        !json_object_is_type(field(o, "soundGainDb"), json_type_int) ||
        number(o, "soundGainDb") < 0 || number(o, "soundGainDb") > 24 ||
        number(o, "soundSensitivity") < 0 || number(o, "soundSensitivity") > 2) return 0;
    J *classes = field(o, "soundClasses"); unsigned mask = 0;
    if (!json_object_is_type(classes, json_type_array) || json_object_array_length(classes) > 4 ||
        (boolean(o, "soundEnabled") && !json_object_array_length(classes))) return 0;
    for (size_t n = 0; n < json_object_array_length(classes); ++n) {
        J *item = json_object_array_get_idx(classes, n);
        if (!json_object_is_type(item, json_type_string)) return 0;
        const char *s = json_object_get_string(item); unsigned bit = 0;
        for (int i = 0; i < 4; ++i)
            if (json_object_get_string_len(item) == (int)strlen(sound_labels[i]) && !strcmp(s, sound_labels[i])) bit = 1u << i;
        if (!bit || (mask & bit)) return 0;
        mask |= bit;
    }
    J *c = field(o, "confidence");
    if (!json_object_is_type(c, json_type_double) && !json_object_is_type(c, json_type_int)) return 0;
    return isfinite(number(o, "confidence")) && number(o, "confidence") >= .25 &&
        number(o, "confidence") <= .99 && number(o, "intervalMs") >= 500 && number(o, "intervalMs") <= 5000;
}

static void read_settings(void)
{
    J *o = read_json(CONFIG);
    /* Preserve existing visual settings when upgrading the four-field plugin. */
    if (o && json_object_is_type(o, json_type_object) && json_object_object_length(o) == 4 &&
        field(o, "enabled") && field(o, "confidence") && field(o, "intervalMs") && field(o, "motionRegions")) {
        J *d = defaults();
        add(o, "soundEnabled", json_object_get(field(d, "soundEnabled")));
        add(o, "soundSensitivity", json_object_get(field(d, "soundSensitivity")));
        add(o, "soundClasses", json_object_get(field(d, "soundClasses")));
        json_object_put(d);
    }
    if (o && json_object_is_type(o, json_type_object) && json_object_object_length(o) == 7 && !field(o, "soundGainDb"))
        add(o, "soundGainDb", json_object_new_int(12));
    if (o && json_object_is_type(o,json_type_object) && json_object_object_length(o) == 8 && !field(o,"notifications"))
        add(o,"notifications",notify_defaults());
    if (o && json_object_is_type(o,json_type_object) && json_object_object_length(o) == 9 && !field(o,"recording"))
        add(o,"recording",record_defaults());
    if (o && json_object_is_type(o,json_type_object) && json_object_object_length(o)==10 && !field(o,"model") && !field(o,"nms")) {
        text(o,"model","stock"); add(o,"nms",json_object_new_double(.45));
    }
    if (!valid_settings(o)) { json_object_put(o); o = defaults(); }
    json_object_put(settings); settings = o;
}

static pid_t camera_pid(void)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;
    pid_t result = 0;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300], comm[40];
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f) && !strcmp(comm, "majestic\n")) result = atoi(e->d_name);
        fclose(f);
        if (result) break;
    }
    closedir(d);
    return result;
}

static pid_t daemon_pid(void)
{
    FILE *f = fopen(PIDFILE, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid <= 1) return 0;
    char path[64], comm[40] = "";
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (f) { if (!fgets(comm, sizeof(comm), f)) comm[0] = 0; fclose(f); }
    return !strcmp(comm, "c120-aid\n") ? pid : 0;
}

static int token(char out[65], int create)
{
    int fd = open(TOKEN, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        ssize_t n = read(fd, out, 65); close(fd);
        if (n == 64) { out[64] = 0; return 0; }
        return -1;
    }
    if (!create || errno != ENOENT) return -1;
    unsigned char bytes[32];
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, bytes, sizeof(bytes)); close(fd);
    if (n != sizeof(bytes)) return -1;
    for (int i = 0; i < 32; ++i) sprintf(out + 2*i, "%02x", bytes[i]);
    fd = open(TOKEN, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return errno == EEXIST ? token(out, 0) : -1;
    n = write(fd, out, 64); close(fd);
    return n == 64 ? 0 : -1;
}

static void reply(int code, J *o)
{
    const char *message = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" :
        code == 405 ? "Method Not Allowed" : code == 503 ? "Service Unavailable" : "Internal Server Error";
    /* Majestic's CGI bridge requires HTTP/1.1 and LF-only header separators. */
    printf("HTTP/1.1 %d %s\nContent-Type: application/json\nCache-Control: no-store\nX-Content-Type-Options: nosniff\n\n%s\n",
        code, message, json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN));
    json_object_put(o);
}

static int error_reply(int code, const char *message)
{
    J *o = json_object_new_object(); text(o, "error", message); reply(code, o); return 0;
}

static int api(void)
{
    if (!access(AP, F_OK)) return error_reply(503, "AI unavailable in setup AP mode");
    char csrf[65];
    if (token(csrf, 1)) return error_reply(503, "Cannot create request token");
    const char *method = getenv("REQUEST_METHOD");
    if (!method) return 2;
    if (!strcmp(method, "POST")) {
        const char *type = getenv("CONTENT_TYPE"), *len = getenv("CONTENT_LENGTH");
        char *end = NULL;
        long n = len ? strtol(len, &end, 10) : 0;
        if (!type || (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8")) ||
            !len || !*len || !end || *end || n < 2 || n > 8192) return error_reply(400, "Expected a small JSON request");
        char buf[8193];
        if (fread(buf, 1, n, stdin) != (size_t)n) return error_reply(400, "Incomplete request");
        J *o = parse(buf, n);
        J *key = field(o, "csrf");
        const char *provided = json_object_get_string(key);
        if (!json_object_is_type(key, json_type_string) || json_object_get_string_len(key) != 64 ||
            !provided || strcmp(provided, csrf)) {
            json_object_put(o); return error_reply(403, "Invalid request token");
        }
        json_object_object_del(o, "csrf");
        if (field(o,"modelAction")) {
            read_settings(); J *s=read_json(STATE);
            const char *selected=json_object_get_string(field(settings,"model"));
            const char *active=json_object_get_string(field(s,"activeModel"));
            const char *previous=json_object_get_string(field(s,"previousModel"));
            J *result=models_request(o,selected,active?active:"",previous?previous:"stock");
            json_object_put(s); json_object_put(o); json_object_put(settings);
            reply(field(result,"error")?400:200,result); return 0;
        }
        if (json_object_object_length(o) == 1 && json_object_is_type(field(o,"testNotification"),json_type_boolean) && boolean(o,"testNotification")) {
            read_settings(); pid_t pid = daemon_pid();
            json_object_put(o);
            if (!pid || !boolean(field(settings,"notifications"),"enabled")) {
                json_object_put(settings); return error_reply(503,"Enable notifications and start the AI service first");
            }
            if (kill(pid,SIGUSR1)) { json_object_put(settings); return error_reply(503,"AI service unavailable"); }
            o = json_object_new_object(); text(o,"status","Test requested"); reply(200,o); json_object_put(settings); return 0;
        }
        /* Old clients must preserve action settings rather than disable them on upgrade. */
        if (o && json_object_object_length(o) == 8 && !field(o,"notifications")) {
            read_settings(); add(o,"notifications",json_object_get(field(settings,"notifications")));
        }
        if (o && json_object_object_length(o) == 9 && !field(o,"recording")) {
            read_settings(); add(o,"recording",json_object_get(field(settings,"recording")));
        }
        if (o && json_object_object_length(o)==10 && !field(o,"model") && !field(o,"nms")) {
            read_settings(); add(o,"model",json_object_get(field(settings,"model"))); add(o,"nms",json_object_get(field(settings,"nms")));
        }
        if (!valid_settings(o)) { json_object_put(o); return error_reply(400, "Invalid AI settings"); }
        read_settings();
        if (strcmp(json_object_get_string(field(o,"model")),json_object_get_string(field(settings,"model")))) {
            struct detector_model m; char err[160];
            if (model_open(json_object_get_string(field(o,"model")),&m,err)) { json_object_put(o); json_object_put(settings); return error_reply(400,err); }
            if (m.fd>=0) close(m.fd);
        }
        int rc = atomic_json(CONFIG, o, 1);
        json_object_put(o);
        if (rc) return error_reply(500, "Could not save settings");
        pid_t pid = daemon_pid(); if (pid) kill(pid, SIGHUP);
    } else if (strcmp(method, "GET")) return error_reply(405, "Use GET or POST");
    read_settings();
    J *o = read_json(STATE);
    if (!o || !json_object_is_type(o, json_type_object)) { json_object_put(o); o = json_object_new_object(); }
    if (!daemon_pid() || now() - number(o, "monotonic") > 10) {
        text(o, "status", "Service stopped"); add(o, "objects", json_object_new_array());
        add(o, "running", json_object_new_boolean(0));
        text(o, "soundStatus", "Service stopped"); add(o, "soundRunning", json_object_new_boolean(0));
        add(o, "sounds", json_object_new_array());
    }
    add(o, "config", json_object_get(settings));
    const char *active=json_object_get_string(field(o,"activeModel"));
    add(o,"models",models_state(json_object_get_string(field(settings,"model")),active?active:""));
    add(o,"schemaVersion",json_object_new_int(2));
    text(o, "csrf", csrf);
    reply(200, o);
    json_object_put(settings);
    return 0;
}

static int roi_rect(const char *s, unsigned *v)
{
    if (!s || !*s) return 0;
    for (int i = 0; i < 4; ++i) {
        if (!isdigit((unsigned char)*s)) return 0;
        char *end;
        unsigned long n = strtoul(s, &end, 10);
        if (n > 16384 || (i < 3 ? *end != 'x' : *end != 0)) return 0;
        v[i] = n; s = end + (i < 3);
    }
    return v[2] && v[3];
}

static int in_regions(float x, float y)
{
    if (!boolean(settings, "motionRegions") || !json_object_array_length(regions)) return 1;
    for (size_t i = 0; i < json_object_array_length(regions); ++i) {
        unsigned r[4];
        if (!roi_rect(json_object_get_string(json_object_array_get_idx(regions, i)), r)) return 0;
        if (x * main_width >= r[0] && x * main_width < r[0] + r[2] &&
            y * main_height >= r[1] && y * main_height < r[1] + r[3]) return 1;
    }
    return 0;
}

struct body { char buf[16384]; size_t len; };
static size_t receive(char *p, size_t size, size_t count, void *user)
{
    struct body *b = user;
    size_t n = size * count;
    if (n > sizeof(b->buf) - b->len) return 0;
    memcpy(b->buf + b->len, p, n); b->len += n;
    return n;
}

static int pipeline(long timeout_ms)
{
    struct body b = {0};
    CURL *c = curl_easy_init();
    if (!c) return 0;
    curl_easy_setopt(c, CURLOPT_URL, "http://127.0.0.1/api/v1/config.json");
    curl_easy_setopt(c, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 250L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    long status = 0;
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    J *o = rc == CURLE_OK && status == 200 ? parse(b.buf, b.len) : NULL;
    int ok = 0;
    reason = "Waiting for camera";
    if (!o) {
        syslog(LOG_WARNING,"Camera configuration unavailable: curl=%d HTTP=%ld bytes=%zu",rc,status,b.len);
        return 0;
    }
    J *a = field(o, "audio");
    record_storage(field(o,"records"));
    const char *rate = json_object_get_string(field(a, "srate")); char *end = NULL;
    unsigned long hz = rate ? strtoul(rate, &end, 10) : 0;
    audio_rate = boolean(a, "enabled") && rate && *rate && end && !*end && hz >= 8000 && hz <= 48000 ? hz : 0;
    if (!boolean(settings, "enabled")) { reason = "Microphone disabled or unsupported sample rate"; ok = camera_pid() && audio_rate; goto done; }
    J *v = field(o, "video0"), *v1 = field(o, "video1");
    if (!boolean(v, "enabled") || !camera_pid()) goto done;
    reason = "Paused: substream uses analysis resources";
    if (boolean(v1, "enabled")) goto done;
    reason = "Paused: invalid motion regions";
    unsigned dims[4]; char extra;
    const char *size = json_object_get_string(field(v, "size"));
    if (!size || sscanf(size, "%ux%u%c", &main_width, &main_height, &extra) != 2 ||
        !main_width || !main_height || main_width > 8192 || main_height > 8192) goto done;
    J *r = field(field(o, "motionDetect"), "roi");
    if (!json_object_is_type(r, json_type_array) || json_object_array_length(r) > 32) goto done;
    for (size_t i = 0; i < json_object_array_length(r); ++i) {
        J *item = json_object_array_get_idx(r, i);
        if (!json_object_is_type(item, json_type_string) || !roi_rect(json_object_get_string(item), dims) ||
            dims[0] + dims[2] > main_width || dims[1] + dims[3] > main_height) goto done;
    }
    json_object_put(regions); regions = json_object_get(r);
    ok = 1;
done:
    json_object_put(o);
    return ok;
}

/* Optional vendor crash reporting is unavailable in musl. */
int backtrace(void **frames_, int size) { (void)frames_; (void)size; return 0; }
char **backtrace_symbols(void *const *frames_, int size)
{ (void)frames_; (void)size; return NULL; }

struct tensor { void *data[2]; uint64_t physical[2]; };
struct tensors { uint32_t count; struct tensor items[60]; };
#ifndef C120_HOST_TEST
_Static_assert(sizeof(struct tensors) == 1448, "IPU tensor ABI");
#endif
static struct tensors input, output;
static uint64_t descriptor[5401];
static unsigned channel;
static int sys_ready, device_ready, channel_ready, input_ready, output_ready, port_ready;
static pid_t owner_pid;
static struct port_config port_cfg;
static struct port port = {34, 0, 0, 2};
static gzFile model;
static unsigned model_bytes = MODEL_BYTES;
static unsigned sound_channel;
static struct tensors sound_input, sound_output;
static int sound_channel_ready, sound_input_ready, sound_output_ready;
static struct sound_dsp *dsp;
static CURLM *audio_multi;
static CURL *audio_stream;
static unsigned audio_samples, audio_reconnects;
static struct opus_input *audio_decoder;

typedef int (*reader)(void *, uint32_t, uint32_t, const char *);
static int (*info)(reader, const char *, void *);
static int (*sys_init)(unsigned), (*sys_exit)(unsigned);
static int (*create_dev)(void *, void *, void *, unsigned), (*destroy_dev)(void);
static int (*create_chn)(unsigned *, void *, reader, const char *), (*destroy_chn)(unsigned);
static int (*get_desc)(unsigned, void *);
static int (*get_in)(unsigned, struct tensors *), (*get_out)(unsigned, struct tensors *);
static int (*put_in)(unsigned, struct tensors *), (*put_out)(unsigned, struct tensors *);
static int (*invoke)(unsigned, struct tensors *, struct tensors *), (*flush)(void *, unsigned);
static int (*get_port)(int, int, int, struct port_config *), (*set_port)(int, int, int, struct port_config *);
static int (*enable_port)(int, int, int), (*disable_port)(int, int, int);
static int (*depth)(unsigned, struct port *, unsigned, unsigned);
static int (*get_buf)(struct port *, struct frame_buffer *, int *), (*put_buf)(int);

static int libraries(void)
{
    const char *libs[] = {"libgcc_s.so.1", "libuclibc-compat.so", "libmi_common.so", "libcam_os_wrapper.so",
        "libcam_fs_wrapper.so", "libmi_sys.so", "libmi_ipu.so", "libmi_scl.so"};
    for (unsigned i = 0; i < sizeof(libs)/sizeof(libs[0]); ++i)
        if (!dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL)) { syslog(LOG_ERR, "%s: %s", libs[i], dlerror()); return -1; }
#define SYMBOL(var, name) do { *(void **)(&(var)) = dlsym(RTLD_DEFAULT, name); if (!(var)) return -1; } while (0)
    SYMBOL(info, "MI_IPU_GetOfflineModeStaticInfo");
    SYMBOL(sys_init, "MI_SYS_Init"); SYMBOL(sys_exit, "MI_SYS_Exit");
    SYMBOL(create_dev, "MI_IPU_CreateDevice"); SYMBOL(destroy_dev, "MI_IPU_DestroyDevice");
    SYMBOL(create_chn, "MI_IPU_CreateCHN"); SYMBOL(destroy_chn, "MI_IPU_DestroyCHN");
    SYMBOL(get_desc, "MI_IPU_GetInOutTensorDesc");
    SYMBOL(get_in, "MI_IPU_GetInputTensors"); SYMBOL(get_out, "MI_IPU_GetOutputTensors");
    SYMBOL(put_in, "MI_IPU_PutInputTensors"); SYMBOL(put_out, "MI_IPU_PutOutputTensors");
    SYMBOL(invoke, "MI_IPU_Invoke"); SYMBOL(flush, "MI_SYS_FlushInvCache");
    SYMBOL(get_port, "MI_SCL_GetOutputPortParam"); SYMBOL(set_port, "MI_SCL_SetOutputPortParam");
    SYMBOL(enable_port, "MI_SCL_EnableOutputPort"); SYMBOL(disable_port, "MI_SCL_DisableOutputPort");
    SYMBOL(depth, "MI_SYS_SetChnOutputPortDepth"); SYMBOL(get_buf, "MI_SYS_ChnOutputPortGetBuf");
    SYMBOL(put_buf, "MI_SYS_ChnOutputPortPutBuf");
#undef SYMBOL
    return 0;
}

static int model_read(void *dest, uint32_t offset, uint32_t length, const char *path)
{
    (void)path;
    if (!model || offset > model_bytes || length > model_bytes - offset || stopping) return -1;
    if (gzseek(model, offset, SEEK_SET) != (z_off_t)offset) return -1;
    return gzread(model, dest, length) == (int)length ? 0 : -1;
}

static int port_used(int *bound)
{
    FILE *f = fopen(SCL_PROC, "r");
    if (!f) return -1;
    char line[512]; int section = 0, seen = 0, used = 0;
    *bound = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "BindPeerInputPortList")) section = 1;
        if (strstr(line, "start dump SCL module")) section = 0;
        if (strstr(line, "start dump scl OUTPUT PORT info")) { section = 2; seen = 1; }
        unsigned d, c, p;
        if (sscanf(line, "%u %u %u", &d, &c, &p) == 3 && d == 0 && c == 0 && p == 2) {
            if (section == 1) *bound = 1;
            if (section == 2) used = 1;
        }
    }
    fclose(f);
    return seen ? used : -1;
}

static int same_port(const struct port_config *a, const struct port_config *b)
{
    /* Bytes 14-15 are ABI padding, not driver settings or evidence of ownership. */
    return a->crop.x == b->crop.x && a->crop.y == b->crop.y &&
        a->crop.w == b->crop.w && a->crop.h == b->crop.h &&
        a->width == b->width && a->height == b->height &&
        a->mirror == b->mirror && a->flip == b->flip &&
        a->format == b->format && a->compression == b->compression;
}

static int owns_port(void)
{
    struct port_config current = {0}; int bound = 0;
    return port_ready && owner_pid == camera_pid() && port_used(&bound) == 1 && !bound &&
        !get_port(0, 0, 2, &current) && same_port(&current, &port_cfg);
}

static void audio_stop(void)
{
    if (audio_stream && audio_multi) curl_multi_remove_handle(audio_multi, audio_stream);
    if (audio_stream) curl_easy_cleanup(audio_stream);
    if (audio_multi) curl_multi_cleanup(audio_multi);
    audio_stream = NULL; audio_multi = NULL;
    opus_input_destroy(audio_decoder); audio_decoder = NULL;
    sound_destroy(dsp); dsp = NULL;
    sound_last_data = 0; sound_current = -1;
    memset(sound_history, 0, sizeof(sound_history));
    memset(sound_scores, 0, sizeof(sound_scores));
}

static void visual_stop(void)
{
    unlink(LIGHT_SIGNAL);
    if (model) { gzclose(model); model = NULL; }
    if (owns_port()) { depth(0, &port, 0, 0); disable_port(0, 0, 2); }
    port_ready = 0;
    if (output_ready) put_out(channel, &output);
    if (input_ready) put_in(channel, &input);
    if (channel_ready) destroy_chn(channel);
    output_ready = input_ready = channel_ready = 0;
    active_model.id[0]=0;
    memset(last_seen,0,sizeof(last_seen));
}

static void engine_stop(void)
{
    visual_stop(); audio_stop();
    if (sound_output_ready) put_out(sound_channel, &sound_output);
    if (sound_input_ready) put_in(sound_channel, &sound_input);
    if (sound_channel_ready) destroy_chn(sound_channel);
    if (device_ready) destroy_dev();
    if (sys_ready) sys_exit(0);
    device_ready = sys_ready = 0;
    sound_output_ready = sound_input_ready = sound_channel_ready = 0;
    memset(last_seen, 0, sizeof(last_seen));
    memset(sound_last_seen, 0, sizeof(sound_last_seen));
}

static int visual_start(const char *id)
{
    struct detector_model candidate; int bound=0;
    if (model_open(id,&candidate,model_error)) return -1;
    model_bytes=candidate.bytes;
    model=candidate.bird?gzdopen(candidate.fd,"rb"):gzopen(MODEL,"rb");
    if (!model && candidate.fd>=0) close(candidate.fd);
    uint32_t meta[32]={0},attr[16]={0,1,1,1};
    snprintf(model_error,sizeof(model_error),"Model metadata or channel could not be loaded");
    if (!model || info(model_read,MODEL,meta) || meta[1]!=candidate.bytes || !meta[0] || meta[0]>1687552 ||
        (!candidate.bird && meta[0]!=1687552)) goto fail;
    /* The shared device already owns scratch. Admit weights, I/O and feed queues separately. */
    unsigned needed=candidate.bytes+(candidate.bird?1685504:538944)+1080000+512*1024;
    if (mma_kb()*1024u<needed) { snprintf(model_error,sizeof(model_error),"Insufficient media memory for this model"); goto fail; }
    if (create_chn(&channel,attr,model_read,MODEL)) goto fail;
    channel_ready = 1;
    gzclose(model); model = NULL;
    if (get_desc(channel, descriptor)) goto fail;
    uint32_t *d = (uint32_t *)descriptor;
    if (candidate.bird) {
        snprintf(model_error,sizeof(model_error),"Unsupported raw YOLO input or output tensors");
        if (d[0]!=1 || d[1]!=3 || bird_tensor_desc(d+2,320,320,3,&bird_input)) goto fail;
        for (unsigned i=0;i<3;++i) {
            uint32_t *head=d+2+(60+i)*90; char name[12]; snprintf(name,sizeof(name),"head%u",8u<<i);
            if (strncmp((char *)(head+12),name,256) || bird_tensor_desc(head,40u>>i,40u>>i,255,&bird_heads[i])) goto fail;
        }
    } else {
        if (d[0] != 1 || d[1] != 4 || d[3] != 1 || d[82] != 537600) goto fail;
        const unsigned sizes[] = {832, 256, 256, 64};
        for (unsigned i = 0; i < 4; ++i)
            if (d[2 + (60+i)*90 + 1] != 5 || d[2 + (60+i)*90 + 80] != sizes[i]) goto fail;
    }
    if (get_in(channel, &input)) goto fail;
    input_ready = 1;
    if (get_out(channel, &output)) goto fail;
    output_ready = 1;
    if (input.count != 1 || output.count != (candidate.bird?3u:4u) || !input.items[0].data[0]) goto fail;
    for (unsigned i=0;i<output.count;++i) if (!output.items[i].data[0]) goto fail;
    reason = "Analysis feed unavailable";
    snprintf(model_error,sizeof(model_error),"Camera stopped or setup AP active during model load");
    if (stopping || !access(AP, F_OK) || camera_pid() != owner_pid) goto fail;
    /* Model hashing/loading can briefly contend with the camera's HTTP worker. */
    if (!pipeline(2500L)) { snprintf(model_error,sizeof(model_error),"Camera not ready during model load: %s",reason); goto fail; }
    snprintf(model_error,sizeof(model_error),"Analysis port in use during model load");
    if (port_used(&bound) != 0 || bound) goto fail;
    memset(&port_cfg, 0, sizeof(port_cfg));
    snprintf(model_error,sizeof(model_error),"Cannot read camera scaler settings");
    if (get_port(0, 0, 0, &port_cfg)) goto fail;
    port_cfg.width = 800; port_cfg.height = candidate.bird?450:448; port_cfg.format = 11; port_cfg.compression = 0;
    snprintf(model_error,sizeof(model_error),"Cannot configure or enable analysis port");
    if (set_port(0, 0, 2, &port_cfg) || enable_port(0, 0, 2)) goto fail;
    port_ready = 1;
    snprintf(model_error,sizeof(model_error),"Cannot configure analysis frame queue");
    if (depth(0, &port, 1, 2)) goto fail;
    candidate.fd=-1; active_model=candidate; visual_warmup=now()+3;
    return 0;
fail:
    visual_stop(); return -1;
}

static int visual_switch(void)
{
    const char *requested=json_object_get_string(field(settings,"model"));
    struct detector_model rollback_profile=active_model;
    char rollback[49]; snprintf(rollback,sizeof(rollback),"%s",*active_model.id?active_model.id:previous_model);
    visual_stop();
    if (!visual_start(requested)) {
        snprintf(previous_model,sizeof(previous_model),"%s",requested);
        model_error[0]=0; missing_model=0; return 0;
    }
    char err[160]; snprintf(err,sizeof(err),"%s",model_error);
    missing_model=!model_available(requested);
    if (strcmp(rollback,requested) && !visual_start(rollback)) {}
    else if (strcmp(rollback,"stock") && !visual_start("stock")) {}
    else { snprintf(model_error,sizeof(model_error),"%s",err); reason="Visual model unavailable"; return -1; }
    if (!strcmp(active_model.id,rollback_profile.id)) {
        active_model.confidence=rollback_profile.confidence; active_model.nms=rollback_profile.nms;
    }
    snprintf(previous_model,sizeof(previous_model),"%s",active_model.id);
    snprintf(model_error,sizeof(model_error),"%s",err); return 0;
}

static int engine_start(void)
{
    int bound=0,visual=boolean(settings,"enabled");
    reason="Paused: analysis port is in use";
    if (visual && (port_used(&bound)!=0 || bound)) return -1;
    owner_pid=camera_pid();
    uint32_t meta[32]={0},dev[16]={visual?1687552u:81920u,16,2,16},attr[16]={0,1,1,1};
    reason="Model could not be loaded";
    if (sys_init(0)) goto fail;
    sys_ready=1;
    if (create_dev(dev,NULL,NULL,0)) goto fail;
    device_ready=1;
    if (boolean(settings,"soundEnabled") && audio_rate) {
        model_bytes=SOUND_BYTES; model=gzopen(SOUND_MODEL,"rb");
        if (!model || info(model_read,SOUND_MODEL,meta) || meta[0]!=81920 || meta[1]!=SOUND_BYTES) goto fail;
        if (create_chn(&sound_channel,attr,model_read,SOUND_MODEL)) goto fail;
        sound_channel_ready=1;
        gzclose(model); model=NULL;
        if (get_desc(sound_channel,descriptor)) goto fail;
        uint32_t *sd=(uint32_t *)descriptor;
        if (sd[0]!=1 || sd[1]!=1 || sd[3]!=2 || sd[4]!=1 || sd[5]!=64 || sd[6]!=101 ||
            sd[82]!=SOUND_VALUES*2 || sd[2+60*90+1]!=5 || sd[2+60*90+80]!=64) goto fail;
        if (get_in(sound_channel,&sound_input)) goto fail;
        sound_input_ready=1;
        if (get_out(sound_channel,&sound_output)) goto fail;
        sound_output_ready=1;
        if (sound_input.count!=1 || sound_output.count!=1 || !sound_input.items[0].data[0] || !sound_output.items[0].data[0]) goto fail;
    }
    sound_rate=audio_rate;
    if (visual) visual_switch();
    return 0;
fail:
    engine_stop(); return -1;
}

static int live_frame(void)
{
    double until = now() + .4;
    while (!stopping && access(AP, F_OK) && now() < until) {
        if (camera_pid() != owner_pid) return -1;
        struct frame_buffer b = {0}; int handle;
        if (get_buf(&port, &b, &handle)) { usleep(10000); continue; }
        struct frame *f = &b.frame;
        int rc = -1;
        if (b.type == 1 && f->format == 11 && f->width == 800 && f->height == port_cfg.height &&
            f->virtual[0] && f->virtual[1] && f->stride[0] >= 800 && f->stride[1] >= 800) {
            if (active_model.bird) {
                struct bird_image im={.y=f->virtual[0],.uv=f->virtual[1],.width=800,.height=f->height,.y_stride=f->stride[0],.uv_stride=f->stride[1]};
                rc=bird_prepare(&im,&bird_input,input.items[0].data[0]);
            } else {
                char *dest = input.items[0].data[0];
                for (unsigned y = 0; y < 448; ++y) memcpy(dest+y*800, (char *)f->virtual[0]+y*f->stride[0], 800);
                for (unsigned y = 0; y < 224; ++y) memcpy(dest+800*448+y*800, (char *)f->virtual[1]+y*f->stride[1], 800);
                rc = 0;
            }
        }
        int released = put_buf(handle);
        return rc ? rc : released;
    }
    return -1;
}

static const char *label(int id)
{
    /* Stock .dsc groups 2/3/7 together. Cat and dog fixtures both return 4. */
    switch (id) {
        case 0: return "person";
        case 2: case 3: case 7: return "vehicle";
        case 4: return "pet";
        default: return NULL;
    }
}

static void event(const char *name, const char *kind, double confidence)
{
    J *e = json_object_new_object();
    text(e, "label", name); text(e, "type", kind);
    add(e, "confidence", json_object_new_double(confidence));
    add(e, "time", json_object_new_int64(time(NULL))); add(e, "id", json_object_new_int64(++sequence));
    text(e,"bootId",boot_id);
    notify_event(e);
    json_object_array_add(events, e);
    if (json_object_array_length(events) > 12) json_object_array_del_idx(events, 0, 1);
}

static void sound_accept(int winner)
{
    /* Stock 8 kHz thresholds: low, normal, high sensitivity; two hits in ten windows. */
    static const float thresholds[4][3] = {{.80,.72,.67},{.75,.65,.60},{.80,.72,.67},{.99,.98,.97}};
    unsigned hit = 0;
    if (winner >= 0 && winner < 4 && sound_scores[winner] >= thresholds[winner][(int)number(settings,"soundSensitivity")]) {
        J *classes = field(settings, "soundClasses");
        for (size_t i = 0; i < json_object_array_length(classes); ++i)
            if (!strcmp(json_object_get_string(json_object_array_get_idx(classes,i)), sound_labels[winner])) hit = 1u << winner;
    }
    sound_history[sound_history_pos++ % 10] = hit;
    sound_current = -1;
    if (hit) {
        unsigned matches = 0;
        for (int i = 0; i < 10; ++i) if (sound_history[i] == hit) ++matches;
        if (matches >= 2) {
            sound_current = winner;
            record_observe(sound_labels[winner]);
            double t = now();
            if (t-sound_last_seen[winner] > 3 && t-sound_last_event[winner] > 10) {
                event(sound_labels[winner], "sound", sound_scores[winner]); sound_last_event[winner] = t;
            }
            sound_last_seen[winner] = t;
        }
    }
}

static int sound_result(const int16_t *tensor, int active, void *user)
{
    (void)user;
    if (stopping || reloading || !access(AP, F_OK)) return -1;
    ++sound_frames;
    int winner = -1;
    if (active) {
        memcpy(sound_input.items[0].data[0], tensor, SOUND_VALUES*2);
        if (flush(sound_input.items[0].data[0], SOUND_VALUES*2)) return -1;
        double start = now();
        if (invoke(sound_channel, &sound_input, &sound_output) || flush(sound_output.items[0].data[0], 64)) return -1;
        sound_ms = (now()-start)*1000;
        memcpy(sound_scores, sound_output.items[0].data[0], sizeof(sound_scores));
        for (int i = 0; i < 7; ++i) {
            if (!isfinite(sound_scores[i]) || sound_scores[i] < 0 || sound_scores[i] > 1.001) return -1;
            if (winner < 0 || sound_scores[i] > sound_scores[winner]) winner = i;
        }
    } else memset(sound_scores, 0, sizeof(sound_scores));
    sound_accept(winner); sound_reason = "Listening";
    return 0;
}

static int audio_pcm(const int16_t *pcm, size_t count, void *user)
{
    (void)user;
    /* Discard a backlog rather than report seconds-old audio as live. */
    if (stopping || reloading) return -1;
    if (count > OPUS_INPUT_RATE - audio_samples) {
        syslog(LOG_WARNING, "Discarding microphone backlog");
        return -1;
    }
    audio_samples += count;
    if (sound_pcm(dsp, pcm, count)) return -1;
    sound_last_data = now();
    return 0;
}

static size_t audio_receive(char *p, size_t size, size_t count, void *user)
{
    (void)user;
    if (stopping || reloading || (size && count > SIZE_MAX / size)) return 0;
    size_t n = size * count;
    return opus_input_feed(audio_decoder, p, n) ? 0 : n;
}

static void audio_poll(void)
{
    if (!sound_channel_ready) return;
    if (!audio_stream && now() >= sound_retry) {
        sound_reason = "Connecting microphone";
        dsp = sound_create(OPUS_INPUT_RATE, number(settings, "soundGainDb"), sound_result, NULL);
        audio_decoder = opus_input_create(audio_pcm, NULL);
        audio_multi = curl_multi_init(); audio_stream = curl_easy_init();
        if (!dsp || !audio_decoder || !audio_multi || !audio_stream) goto failed;
        /* PCM can crash Majestic 8dbe2e7 independently of AI. Never fall back to it. */
        curl_easy_setopt(audio_stream, CURLOPT_URL, "http://127.0.0.1/audio.opus");
        curl_easy_setopt(audio_stream, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(audio_stream, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
        curl_easy_setopt(audio_stream, CURLOPT_CONNECTTIMEOUT_MS, 500L);
        curl_easy_setopt(audio_stream, CURLOPT_LOW_SPEED_LIMIT, 100L);
        curl_easy_setopt(audio_stream, CURLOPT_LOW_SPEED_TIME, 3L);
        curl_easy_setopt(audio_stream, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(audio_stream, CURLOPT_WRITEFUNCTION, audio_receive);
        curl_easy_setopt(audio_stream, CURLOPT_BUFFERSIZE, 8192L);
        if (curl_multi_add_handle(audio_multi, audio_stream)) goto failed;
        audio_started = now(); sound_reason = "Warming up";
    }
    if (!audio_stream) return;
    int running = 0, messages = 0;
    audio_samples = 0;
    if (curl_multi_perform(audio_multi, &running) != CURLM_OK) goto failed;
    CURLMsg *message = curl_multi_info_read(audio_multi, &messages);
    if (message) {
        if (!stopping && !reloading)
            syslog(LOG_WARNING, "Microphone connection ended: %d (%s)", message->data.result, curl_easy_strerror(message->data.result));
        goto failed;
    }
    if (now()-(sound_last_data ? sound_last_data : audio_started) > 3) {
        syslog(LOG_WARNING, "Microphone connection stalled"); goto failed;
    }
    return;
failed:
    if (!stopping && !reloading) ++audio_reconnects;
    audio_stop(); sound_reason = "Microphone unavailable; retrying"; sound_retry = now()+3;
}

static void object(J *objects,const char *name,int id,float score,float x1,float y1,float x2,float y2)
{
    if (x2<=x1 || y2<=y1 || !in_regions((x1+x2)/2,(y1+y2)/2)) return;
    J *o=json_object_new_object(),*box=json_object_new_array();
    text(o,"label",name); add(o,"classId",json_object_new_int(id)); add(o,"confidence",json_object_new_double(score));
    float xywh[]={x1,y1,x2-x1,y2-y1};
    for (int j=0;j<4;++j) json_object_array_add(box,json_object_new_double(xywh[j]));
    add(o,"box",box); json_object_array_add(objects,o);
    record_observe(name); notify_observe(name);
    int category=!strcmp(name,"person")?0:!strcmp(name,"pet")?1:!strcmp(name,"vehicle")?2:3;
    double t=now();
    if (t<visual_warmup) notify_prime(name);
    else if (t-last_seen[category]>3 && t-last_event[category]>10) { event(name,"object",score); last_event[category]=t; }
    last_seen[category]=t;
}

static J *detect(void)
{
    if (live_frame() || flush(input.items[0].data[0], active_model.bird?bird_input.bytes:537600)) return NULL;
    double start = now();
    if (invoke(channel, &input, &output)) return NULL;
    inference_ms = (now() - start) * 1000;
    J *objects=json_object_new_array(); ++frames;
    if (!strcmp(active_model.id,json_object_get_string(field(settings,"model")))) {
        active_model.confidence=number(settings,"confidence"); active_model.nms=number(settings,"nms");
    }
    double confidence=active_model.confidence;
    if (active_model.bird) {
        const int16_t *data[3];
        for (unsigned i=0;i<3;++i) { data[i]=output.items[i].data[0]; if (flush(output.items[i].data[0],bird_heads[i].bytes)) { json_object_put(objects); return NULL; } }
        struct bird_box boxes[300];
        int n=bird_decode(bird_heads,data,confidence,active_model.nms,800,port_cfg.height,boxes);
        if (n<0) { json_object_put(objects); return NULL; }
        for (int i=0;i<n;++i) object(objects,"bird",14,boxes[i].score,boxes[i].x1,boxes[i].y1,boxes[i].x2,boxes[i].y2);
    } else {
    const unsigned sizes[] = {832, 256, 256, 64};
    for (int i = 0; i < 4; ++i) if (flush(output.items[i].data[0], sizes[i])) { json_object_put(objects); return NULL; }
    float *boxes = output.items[0].data[0], *classes = output.items[1].data[0];
    float *scores = output.items[2].data[0], count = *(float *)output.items[3].data[0];
    if (!isfinite(count) || count < 0 || count > 50 || count != floorf(count)) { json_object_put(objects); return NULL; }
    for (int i = 0; i < (int)count; ++i) {
        if (!isfinite(scores[i]) || scores[i] < confidence || scores[i] > 1 ||
            !isfinite(classes[i]) || classes[i] < 0 || classes[i] > 8 || floorf(classes[i]) != classes[i]) continue;
        int id = classes[i];
        if (!label(id)) continue;
        float *b = boxes + i*4;
        if (!isfinite(b[0]) || !isfinite(b[1]) || !isfinite(b[2]) || !isfinite(b[3])) continue;
        float x1 = fminf(1, fmaxf(0,b[1])), y1 = fminf(1, fmaxf(0,b[0]));
        float x2 = fminf(1, fmaxf(0,b[3])), y2 = fminf(1, fmaxf(0,b[2]));
        object(objects,label(id),id,scores[i],x1,y1,x2,y2);
    }
    }
    /* Compact, versioned presence signal keeps the GPIO helper independent of the IPU. */
    unsigned mask = 0;
    for (size_t i = 0; i < json_object_array_length(objects); ++i) {
        const char *name = json_object_get_string(field(json_object_array_get_idx(objects,i),"label"));
        mask |= !strcmp(name,"person") ? 1 : !strcmp(name,"pet") ? 2 : !strcmp(name,"vehicle")?4:8;
    }
    char signal[100];
    snprintf(signal,sizeof(signal),"1 %u %llu %u %u",frames,(unsigned long long)(now()*1000),
        (unsigned)number(settings,"intervalMs"),mask);
    if (atomic_text(LIGHT_SIGNAL,signal,0)) syslog(LOG_ERR,"Cannot publish AI light signal");
    return objects;
}

static unsigned memory_kb(void)
{
    FILE *f = fopen("/proc/meminfo", "r"); char line[120]; unsigned n = 0;
    if (f) { while (fgets(line, sizeof(line), f)) if (sscanf(line, "MemAvailable: %u", &n) == 1) break; fclose(f); }
    return n;
}

static unsigned mma_kb(void)
{
    FILE *f = fopen("/proc/mi_modules/mi_sys_mma/mma_heap_name0", "r"); char line[256]; unsigned a, b, free_ = 0;
    if (f) { while (fgets(line, sizeof(line), f)) if (sscanf(line, " mma_heap_name0 %x %x %x", &a, &b, &free_) == 3) break; fclose(f); }
    return free_ / 1024;
}

static void state(J *objects)
{
    J *o = json_object_new_object();
    text(o, "status", reason); add(o, "running", json_object_new_boolean(port_ready));
    text(o,"activeModel",active_model.id); text(o,"activeModelName",active_model.name);
    text(o,"previousModel",previous_model); text(o,"modelError",model_error);
    text(o,"requestedModel",json_object_get_string(field(settings,"model")));
    add(o,"modelFallback",json_object_new_boolean(port_ready && strcmp(active_model.id,json_object_get_string(field(settings,"model")))));
    add(o, "objects", objects ? objects : json_object_new_array()); add(o, "events", json_object_get(events));
    add(o, "frames", json_object_new_int64(frames)); add(o, "inferenceMs", json_object_new_double(inference_ms));
    add(o, "time", json_object_new_int64(time(NULL))); add(o, "monotonic", json_object_new_double(now()));
    text(o,"bootId",boot_id); add(o,"lastEventId",json_object_new_int64(sequence));
    add(o,"notifications",notify_state());
    add(o,"recording",record_state());
    add(o, "availableKiB", json_object_new_int(memory_kb())); add(o, "mediaFreeKiB", json_object_new_int(mma_kb()));
    text(o, "soundStatus", sound_reason);
    text(o, "soundSource", "opus");
    add(o, "soundRunning", json_object_new_boolean(audio_stream && sound_last_data > 0 && now()-sound_last_data < 3));
    add(o, "soundFrames", json_object_new_int64(sound_frames));
    add(o, "soundReconnects", json_object_new_int64(audio_reconnects));
    add(o, "soundInferenceMs", json_object_new_double(sound_ms));
    add(o, "soundLevelDbfs", json_object_new_double(sound_level_dbfs(dsp)));
    J *scores = json_object_new_array();
    for (int i = 0; i < 7; ++i) json_object_array_add(scores, json_object_new_double(sound_scores[i]));
    add(o, "soundScores", scores);
    J *sounds = json_object_new_array();
    if (audio_stream && sound_current >= 0 && now()-sound_last_data < 1) {
        J *s = json_object_new_object(); text(s, "label", sound_labels[sound_current]);
        add(s, "confidence", json_object_new_double(sound_scores[sound_current])); json_object_array_add(sounds, s);
    }
    add(o, "sounds", sounds);
    if (atomic_json(STATE, o, 0)) syslog(LOG_ERR, "Cannot publish AI status");
    json_object_put(o);
}

static void signal_handler(int sig) { if (sig == SIGHUP) reloading = 1; else if (sig == SIGUSR1) testing_notification = 1; else stopping = 1; }

static int selftest(void)
{
    struct port_config a = {{0,0,2688,1520},800,450,0,0,11,0}, b = a;
    ((unsigned char *)&b)[14] = 0x55; ((unsigned char *)&b)[15] = 0xaa;
    assert(same_port(&a, &b));
#define CHANGED_PORT(member) do { ++b.member; assert(!same_port(&a,&b)); --b.member; } while (0)
    CHANGED_PORT(crop.x); CHANGED_PORT(crop.y); CHANGED_PORT(crop.w); CHANGED_PORT(crop.h);
    CHANGED_PORT(width); CHANGED_PORT(height); CHANGED_PORT(mirror); CHANGED_PORT(flip);
    CHANGED_PORT(format); CHANGED_PORT(compression);
#undef CHANGED_PORT
    settings = defaults(); assert(valid_settings(settings));
    J *bad = json_tokener_parse("{\"enabled\":1,\"confidence\":0.6,\"intervalMs\":500,\"motionRegions\":true}");
    assert(!valid_settings(bad)); json_object_put(bad);
    add(settings, "intervalMs", json_object_new_int(499)); assert(!valid_settings(settings));
    add(settings, "intervalMs", json_object_new_int(500)); assert(valid_settings(settings));
    add(settings, "soundSensitivity", json_object_new_int(3)); assert(!valid_settings(settings));
    add(settings, "soundSensitivity", json_object_new_int(1)); assert(valid_settings(settings));
    add(settings, "soundGainDb", json_object_new_int(25)); assert(!valid_settings(settings));
    add(settings, "soundGainDb", json_object_new_int(12)); assert(valid_settings(settings));
    add(settings, "soundClasses", json_tokener_parse("[\"bark\",\"bark\"]")); assert(!valid_settings(settings));
    add(settings, "soundClasses", json_tokener_parse("[\"bird\"]")); assert(!valid_settings(settings));
    add(settings, "soundClasses", json_tokener_parse("[]")); assert(valid_settings(settings));
    add(settings, "soundEnabled", json_object_new_boolean(1)); assert(!valid_settings(settings));
    add(settings, "soundClasses", json_tokener_parse("[\"bark\",\"meow\",\"cry\",\"glass\"]")); assert(valid_settings(settings));
    events = json_object_new_array(); sound_scores[0] = .9;
    sound_last_seen[0] = sound_last_event[0] = -100;
    sound_accept(0); assert(sound_current == -1 && !json_object_array_length(events));
    sound_accept(0); assert(sound_current == 0 && json_object_array_length(events) == 1);
    sound_accept(0); assert(json_object_array_length(events) == 1);
    sound_accept(6); assert(sound_current == -1);
    sound_scores[0] = .5; sound_accept(0); assert(sound_current == -1);
    add(settings, "soundClasses", json_tokener_parse("[\"meow\"]"));
    sound_scores[0] = .9; sound_accept(0); assert(sound_current == -1);
    json_object_put(events);
    assert(!parse("{}garbage", 9));
    unsigned r[4]; assert(roi_rect("0x20x100x200", r));
    assert(!roi_rect("-1x0x10x10", r)); assert(!roi_rect("0x0x0x10", r));
    assert(!roi_rect("0x0x10x10;reboot", r));
    assert(!strcmp(label(4), "pet") && !strcmp(label(7), "vehicle") && !label(8));
    regions = json_tokener_parse("[\"100x100x500x500\"]"); main_width=1000; main_height=1000;
    assert(in_regions(.2,.2)); assert(!in_regions(.05,.2)); assert(!in_regions(.6,.2));
    add(settings,"motionRegions",json_object_new_boolean(0)); assert(in_regions(.05,.2));
    json_object_put(regions); json_object_put(settings);
    puts("AI validation and region tests passed"); return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--self-test")) return selftest();
#ifdef C120_HOST_TEST
    /* A host test build must never open camera devices or write runtime files. */
    fputs("Host test builds only support --self-test\n", stderr);
    return 2;
#endif
    if (argc == 2 && !strcmp(argv[1], "api")) return api();
    unsigned trial_seconds = 0;
    if (argc == 3 && (!strcmp(argv[1], "--trial") || !strcmp(argv[1], "--trial-sound"))) {
        char *end;
        unsigned long n = strtoul(argv[2], &end, 10);
        if (!*argv[2] || *end || n < 10 || n > 600) return 2;
        trial_seconds = n;
    } else if (argc != 1) return 2;
    int fd = open(PIDFILE, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) return 1;
    if (ftruncate(fd, 0) || dprintf(fd, "%d\n", getpid()) < 0) return 1;
    openlog("c120-ai", LOG_PID, LOG_DAEMON);
    signal(SIGTERM, signal_handler); signal(SIGINT, signal_handler); signal(SIGHUP, signal_handler);
    signal(SIGUSR1,signal_handler);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    if (nice(10) == -1 && errno) syslog(LOG_WARNING, "Cannot lower priority: %s", strerror(errno));
    events = json_object_new_array(); regions = json_object_new_array(); read_settings();
    sequence = (unsigned long long)(now()*1000000);
    FILE *boot = fopen("/proc/sys/kernel/random/boot_id","r");
    if (boot) { if (fgets(boot_id,sizeof(boot_id),boot)) boot_id[strcspn(boot_id,"\r\n")] = 0; fclose(boot); }
    if (trial_seconds) add(settings, "enabled", json_object_new_boolean(1));
    if (trial_seconds && !strcmp(argv[1], "--trial-sound")) add(settings, "soundEnabled", json_object_new_boolean(1));
    if (curl_global_init(CURL_GLOBAL_DEFAULT) || libraries()) {
        reason = "AI libraries unavailable"; state(NULL); unlink(PIDFILE); return 1;
    }
    notify_configure(field(settings,"notifications"));
    record_configure(field(settings,"recording"));
    double last_test = -100;
    double next_attempt = 0, next_check = 0, next_frame = 0, next_state = 0;
    int ready = 0;
    J *objects = NULL;
    double deadline = trial_seconds ? now() + trial_seconds : INFINITY;
    while (!stopping && now() < deadline) {
        double start = now();
        if (reloading) {
            reloading=0; J *old=json_object_get(settings); read_settings();
            int full=boolean(old,"enabled")!=boolean(settings,"enabled") ||
                boolean(old,"soundEnabled")!=boolean(settings,"soundEnabled") ||
                number(old,"soundGainDb")!=number(settings,"soundGainDb") ||
                number(old,"soundSensitivity")!=number(settings,"soundSensitivity") ||
                !json_object_equal(field(old,"soundClasses"),field(settings,"soundClasses"));
            int changed=strcmp(json_object_get_string(field(old,"model")),json_object_get_string(field(settings,"model")));
            int switched=!full && device_ready && boolean(settings,"enabled") &&
                (changed || strcmp(active_model.id,json_object_get_string(field(settings,"model"))));
            if (full) engine_stop();
            else if (switched) visual_switch();
            if (!json_object_equal(field(old,"notifications"),field(settings,"notifications"))) notify_configure(field(settings,"notifications"));
            if (!json_object_equal(field(old,"recording"),field(settings,"recording"))) record_configure(field(settings,"recording"));
            if (full || changed || switched) { json_object_put(objects); objects=NULL; }
            if (changed || switched) { json_object_put(events); events=json_object_new_array(); }
            json_object_put(old);
            next_check = next_attempt = next_frame = sound_retry = 0;
        }
        if (start >= next_check || !access(AP, F_OK)) {
            next_check = start+2; ready = 0;
            int visual = boolean(settings, "enabled"), sound = boolean(settings, "soundEnabled");
            if (!visual && !sound) reason = "Disabled";
            else if (!access(AP, F_OK)) reason = "Paused: setup AP";
            else if (memory_kb() < (device_ready ? 3000u : 4096u) ||
                mma_kb() < (device_ready ? 1536u : visual ? 11264u : 2048u)) reason = "Paused: low memory";
            else ready = pipeline(700L);
            if (device_ready && (owner_pid != camera_pid() || sound_rate != audio_rate || (port_ready && !owns_port()))) engine_stop();
            if (!ready) {
                engine_stop(); sound_reason = sound ? reason : "Disabled";
                json_object_put(objects); objects = NULL;
            } else if (!device_ready && start >= next_attempt) {
                if (engine_start()) { next_attempt = now()+15; sound_reason = sound ? reason : "Disabled"; }
            }
            if (ready && device_ready) {
                const char *requested=json_object_get_string(field(settings,"model"));
                if (visual && start>=next_attempt && ((!port_ready) ||
                    (active_model.bird && !model_available(active_model.id)) ||
                    (missing_model && model_available(requested)))) {
                    visual_switch(); next_attempt=now()+15;
                    json_object_put(objects); objects=NULL; json_object_put(events); events=json_object_new_array();
                }
                reason = visual ? "Detecting" : "Disabled";
                if (visual && !port_ready) reason="Visual model unavailable";
                if (!sound) sound_reason = "Disabled";
                else if (!audio_rate) sound_reason = "Microphone disabled or unsupported sample rate";
            }
        }
        if (ready && port_ready && start >= next_frame) {
            json_object_put(objects); objects = detect();
            next_frame = start+number(settings,"intervalMs")/1000;
            if (objects) reason = "Detecting";
            else { engine_stop(); reason = "Waiting for analysis frames"; sound_reason = reason; next_attempt = now()+5; }
        }
        if (ready && sound_channel_ready) audio_poll();
        if (access(AP,F_OK)) {
            if (testing_notification) {
                testing_notification = 0;
                if (now()-last_test >= 10) {
                    J *e = json_object_new_object(); text(e,"label","test"); text(e,"type","test");
                    add(e,"time",json_object_new_int64(time(NULL))); text(e,"bootId",boot_id);
                    add(e,"id",json_object_new_int64(++sequence)); add(e,"confidence",json_object_new_double(1));
                    notify_event(e); json_object_put(e); last_test = now();
                }
            }
            notify_poll(); record_poll();
        } else record_stop();
        if (start >= next_state) { state(json_object_get(objects)); next_state = start+.5; }
        usleep(10000);
    }
    engine_stop(); reason = sound_reason = "Service stopped"; state(NULL);
    notify_stop(); record_stop(); unlink(PIDFILE); close(fd); curl_global_cleanup();
    json_object_put(objects); json_object_put(events); json_object_put(regions); json_object_put(settings);
    return 0;
}
