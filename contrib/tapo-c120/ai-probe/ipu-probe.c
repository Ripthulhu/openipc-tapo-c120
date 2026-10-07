#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "frame-abi.h"

/* Infinity6C ABI, checked against the stock library's exported functions. */
static uint64_t desc[8192];
struct tensor { void *data[2]; uint64_t physical[2]; };
struct tensors { uint32_t count; struct tensor items[60]; };
static unsigned live_frames = 10;
_Static_assert(sizeof(struct tensor) == 24, "ARM tensor ABI");
_Static_assert(sizeof(struct tensors) == 1448, "ARM tensor vector ABI");

static double milliseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}

static void memory(void)
{
    const char *paths[] = {"/proc/meminfo", "/proc/self/status", "/proc/mi_modules/mi_sys_mma/mma_heap_name0"};
    char line[256];
    for (unsigned i = 0; i < 3; ++i) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "MemAvailable:", 13) || !strncmp(line, "VmRSS:", 6) ||
                !strncmp(line, "Threads:", 8) || strstr(line, "mma_heap_name0"))
                printf("resource %s", line);
        fclose(f);
    }
}

/* The vendor wrapper's optional crash trace has no musl implementation. */
int backtrace(void **frames, int size) { (void)frames; (void)size; return 0; }
char **backtrace_symbols(void *const *frames, int size)
{ (void)frames; (void)size; return NULL; }

static void load(const char *name)
{
    if (!dlopen(name, RTLD_NOW | RTLD_GLOBAL)) {
        fprintf(stderr, "%s: %s\n", name, dlerror());
        exit(1);
    }
}

static void *sym(const char *name)
{
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        fprintf(stderr, "%s: %s\n", name, dlerror());
        exit(1);
    }
    return p;
}

static int live_frame(void *dest)
{
    int (*get)(struct port *, struct frame_buffer *, int *) = sym("MI_SYS_ChnOutputPortGetBuf");
    int (*put)(int) = sym("MI_SYS_ChnOutputPortPutBuf");
    struct port port = {34, 0, 0, 2};
    double end = milliseconds() + 2000;
    while (milliseconds() < end) {
        struct frame_buffer b = {0};
        int handle;
        if (get(&port, &b, &handle)) { usleep(10000); continue; }
        struct frame *f = &b.frame;
        int rc = -1;
        if (b.type == 1 && f->format == 11 && f->width == 800 && f->height == 448 &&
            f->virtual[0] && f->virtual[1] && f->stride[0] >= 800 && f->stride[1] >= 800) {
            for (unsigned y = 0; y < 448; ++y)
                memcpy((char *)dest + y * 800, (char *)f->virtual[0] + y * f->stride[0], 800);
            for (unsigned y = 0; y < 224; ++y)
                memcpy((char *)dest + 800 * 448 + y * 800, (char *)f->virtual[1] + y * f->stride[1], 800);
            rc = 0;
        } else printf("unexpected_frame type=%d format=%d size=%ux%u mapped=%d,%d\n", b.type, f->format, f->width, f->height, !!f->virtual[0], !!f->virtual[1]);
        int released = put(handle);
        return rc ? rc : released;
    }
    return -1;
}

static int invoke(unsigned chn, uint32_t *d, int live, int input)
{
    int (*get_in)(unsigned, struct tensors *) = sym("MI_IPU_GetInputTensors");
    int (*get_out)(unsigned, struct tensors *) = sym("MI_IPU_GetOutputTensors");
    int (*put_in)(unsigned, struct tensors *) = sym("MI_IPU_PutInputTensors");
    int (*put_out)(unsigned, struct tensors *) = sym("MI_IPU_PutOutputTensors");
    int (*run)(unsigned, struct tensors *, struct tensors *) = sym("MI_IPU_Invoke");
    int (*flush)(void *, unsigned) = sym("MI_SYS_FlushInvCache");
    struct tensors in = {0}, out = {0};
    int rc = get_in(chn, &in);
    printf("get_input=%d count=%u\n", rc, in.count);
    if (rc) return rc;
    rc = get_out(chn, &out);
    printf("get_output=%d count=%u\n", rc, out.count);
    if (!rc) {
        if (in.count != d[0] || out.count != d[1]) rc = -1;
        for (unsigned i = 0; !rc && i < in.count; ++i) {
            uint32_t *t = d + 2 + i * 90;
            if (!in.items[i].data[0] || t[80] > 8 * 1024 * 1024) { rc = -1; break; }
            memset(in.items[i].data[0], 0, t[80]);
            if (input == 1 && (in.count != 1 || fread(in.items[i].data[0], 1, t[80], stdin) != t[80] || getchar() != EOF)) {
                fprintf(stderr, "Input must match the model's aligned input buffer exactly\n");
                rc = -1;
                break;
            }
            rc = flush(in.items[i].data[0], t[80]);
            printf("flush_input=%d\n", rc);
        }
        memory();
        for (unsigned repeat = 0; !rc && repeat < (live || input == 2 ? live_frames : 3u); ++repeat) {
            if (input == 2) {
                if (in.count != 1 || fread(in.items[0].data[0], 1, d[82], stdin) != d[82]) { rc=-1; break; }
                rc = flush(in.items[0].data[0], d[82]);
                if (rc) break;
            }
            if (live) {
                if (in.count != 1 || d[3] != 1 || d[82] != 537600) { rc = -1; break; }
                rc = live_frame(in.items[0].data[0]);
                if (rc) { printf("live_frame=%d\n", rc); break; }
                rc = flush(in.items[0].data[0], 537600);
                if (rc) break;
            }
            double start = milliseconds();
            rc = run(chn, &in, &out);
            printf("invoke=%d elapsed_ms=%.3f\n", rc, milliseconds() - start);
            for (unsigned i = 0; !rc && i < out.count; ++i) {
                uint32_t *t = d + 2 + (60 + i) * 90;
                rc = flush(out.items[i].data[0], t[80]);
                printf("output%u=", i);
                unsigned n = t[80] / (t[1] == 5 ? 4 : 2);
                unsigned count = 1;
                for (unsigned j = 0; j < t[0] && j < 10; ++j) count *= t[2 + j];
                if (n > count) n = count;
                if (n > 16) n = 16;
                for (unsigned j = 0; j < n; ++j) {
                    if (t[1] == 5) printf("%.6g,", ((float *)out.items[i].data[0])[j]);
                    else printf("%d,", ((int16_t *)out.items[i].data[0])[j]);
                }
                printf("\n");
            }
            if (live) usleep(200000);
        }
        memory();
        printf("put_output=%d\n", put_out(chn, &out));
    }
    printf("put_input=%d\n", put_in(chn, &in));
    return rc;
}

static int live_invoke(unsigned chn, uint32_t *d)
{
    load("libmi_scl.so");
    int (*get)(int, int, int, struct port_config *) = sym("MI_SCL_GetOutputPortParam");
    int (*set)(int, int, int, struct port_config *) = sym("MI_SCL_SetOutputPortParam");
    int (*enable)(int, int, int) = sym("MI_SCL_EnableOutputPort");
    int (*disable)(int, int, int) = sym("MI_SCL_DisableOutputPort");
    int (*depth)(unsigned, struct port *, unsigned, unsigned) = sym("MI_SYS_SetChnOutputPortDepth");
    struct port_config old = {0}, cfg = {0};
    struct port port = {34, 0, 0, 2};
    int rc = get(0, 0, 2, &old);
    printf("get_unused_port=%d\n", rc);
    if (rc) return rc;
    rc = get(0, 0, 0, &cfg);
    if (rc) return rc;
    cfg.width = 800; cfg.height = 448; cfg.format = 11; cfg.compression = 0;
    rc = set(0, 0, 2, &cfg);
    printf("set_unused_port=%d\n", rc);
    if (rc) return rc;
    rc = enable(0, 0, 2);
    printf("enable_unused_port=%d\n", rc);
    if (!rc) {
        rc = depth(0, &port, 1, 2);
        printf("port_depth=%d\n", rc);
        if (!rc) rc = invoke(chn, d, 1, 0);
        printf("reset_depth=%d\n", depth(0, &port, 0, 0));
        printf("disable_unused_port=%d\n", disable(0, 0, 2));
    }
    /* An unconfigured disabled port has zero dimensions, which Set rejects. */
    if (old.width && old.height) printf("restore_unused_port=%d\n", set(0, 0, 2, &old));
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) return 2;
    if (argc == 4) {
        char *end;
        unsigned long frames = strtoul(argv[3], &end, 10);
        if ((strcmp(argv[2], "--live") && strcmp(argv[2], "--batch")) || !*argv[3] || *end || frames < 1 || frames > 1000) return 2;
        live_frames = frames;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(40);
    load("libgcc_s.so.1");
    load("libuclibc-compat.so");
    load("libmi_common.so");
    load("libcam_os_wrapper.so");
    load("libcam_fs_wrapper.so");
    load("libmi_sys.so");
    load("libmi_ipu.so");
    int (*info)(void *, const char *, void *) = sym("MI_IPU_GetOfflineModeStaticInfo");
    int (*sys_init)(unsigned) = sym("MI_SYS_Init");
    int (*sys_exit)(unsigned) = sym("MI_SYS_Exit");
    int (*create_dev)(void *, void *, void *, unsigned) = sym("MI_IPU_CreateDevice");
    int (*destroy_dev)(void) = sym("MI_IPU_DestroyDevice");
    int (*create_chn)(unsigned *, void *, void *, const char *) = sym("MI_IPU_CreateCHN");
    int (*destroy_chn)(unsigned) = sym("MI_IPU_DestroyCHN");
    int (*get_desc)(unsigned, void *) = sym("MI_IPU_GetInOutTensorDesc");
    uint32_t metadata[32] = {0}, dev[16] = {0}, attr[16] = {0, 1, 1, 1};
    unsigned chn = 0;
    int rc = info(NULL, argv[1], metadata);
    printf("static_info=%d variable_bytes=%u model_bytes=%u\n", rc, metadata[0], metadata[1]);
    if (rc || argc == 2) return rc ? 1 : 0;
    if (strcmp(argv[2], "--load") && strcmp(argv[2], "--invoke") && strcmp(argv[2], "--live") && strcmp(argv[2], "--input") && strcmp(argv[2], "--batch")) return 2;
    if (metadata[0] > 8 * 1024 * 1024 || metadata[1] > 8 * 1024 * 1024) return 3;
    rc = sys_init(0);
    printf("sys_init=%d\n", rc);
    if (rc) return 1;
    dev[0] = metadata[0];
    dev[1] = 16; dev[2] = 2; dev[3] = 16;
    rc = create_dev(dev, NULL, NULL, 0);
    printf("create_device=%d\n", rc);
    if (!rc) {
        rc = create_chn(&chn, attr, NULL, argv[1]);
        printf("create_channel=%d channel=%u\n", rc, chn);
        if (!rc) {
            rc = get_desc(chn, desc);
            uint32_t *d = (uint32_t *)desc;
            printf("tensor_desc=%d inputs=%u outputs=%u\n", rc, d[0], d[1]);
            if (!rc && d[0] <= 60 && d[1] <= 60) {
                for (unsigned k = 0; k < 2; ++k)
                    for (unsigned i = 0; i < d[k]; ++i) {
                        uint32_t *t = d + 2 + (k * 60 + i) * 90;
                        printf("%s%u dims=%u format=%u shape=", k ? "out" : "in", i, t[0], t[1]);
                        for (unsigned j = 0; j < t[0] && j < 10; ++j) printf("%u,", t[2 + j]);
                        float scale;
                        memcpy(&scale, t + 77, sizeof(scale));
                        printf(" name=%.256s stride=%u buffer=%u scale=%g zero=%lld\n", (char *)(t + 12), t[76], t[80], scale, (long long)*(int64_t *)(t + 78));
                    }
                if (!strcmp(argv[2], "--invoke")) rc = invoke(chn, d, 0, 0);
                if (!strcmp(argv[2], "--input")) rc = invoke(chn, d, 0, 1);
                if (!strcmp(argv[2], "--batch")) rc = invoke(chn, d, 0, 2);
                if (!strcmp(argv[2], "--live")) rc = live_invoke(chn, d);
            }
            printf("destroy_channel=%d\n", destroy_chn(chn));
        }
        printf("destroy_device=%d\n", destroy_dev());
    }
    printf("sys_exit=%d\n", sys_exit(0));
    return rc ? 1 : 0;
}
