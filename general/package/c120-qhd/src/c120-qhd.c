#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Infinity6C MI_SYS ABI, verified against the matching vendor SDK. */
#define BIND_PORTS 0x40386903U
#define SET_DEPTH 0x40186915U
typedef struct { uint32_t module, device, channel, port; } port_t;
typedef struct { uint16_t soc, pad; uint32_t size; uint64_t data; } envelope_t;
typedef struct { port_t port; uint32_t user, queue; } depth_t;
typedef struct { port_t src, dst; uint32_t param, type, reserved[2], src_fps, dst_fps; } bind_t;
_Static_assert(sizeof(port_t) == 16, "MI_SYS port ABI");
_Static_assert(sizeof(envelope_t) == 16, "MI_SYS envelope ABI");
_Static_assert(sizeof(depth_t) == 24, "MI_SYS depth ABI");
_Static_assert(sizeof(bind_t) == 56, "MI_SYS bind ABI");

#ifdef __GLIBC__
typedef unsigned long request_t;
#else
typedef int request_t;
#endif
static int (*next_ioctl)(int, request_t, ...);

static int main_port(const port_t *p, uint32_t module) {
    return p->module == module && !p->device && !p->channel && !p->port;
}

static int sys_device(int fd) {
    char path[48], target[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) return 0;
    target[n] = 0;
    return !strcmp(target, "/dev/mi_sys");
}

__attribute__((constructor)) static void init(void) {
    next_ioctl = dlsym(RTLD_NEXT, "ioctl");
}

int ioctl(int fd, request_t request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!next_ioctl) { errno = ENOSYS; return -1; }
    if (arg && ((unsigned)request == BIND_PORTS || (unsigned)request == SET_DEPTH)
            && sys_device(fd)) {
        envelope_t *e = arg;
        if (!e->soc && e->data) {
            if ((unsigned)request == BIND_PORTS && e->size == sizeof(bind_t)) {
                bind_t *b = (void *)(uintptr_t)e->data;
                if (main_port(&b->src, 34) && main_port(&b->dst, 2) && b->type == 1) {
                    /* Queue=3 allocates too much before VENC can reserve its frames. */
                    depth_t depth = { b->src, 0, 2 };
                    envelope_t wrap = { 0, 0, sizeof(depth), (uintptr_t)&depth };
                    int rc = next_ioctl(fd, SET_DEPTH, &wrap);
                    if (rc) return rc;
                }
            } else if ((unsigned)request == SET_DEPTH && e->size == sizeof(depth_t)) {
                depth_t *d = (void *)(uintptr_t)e->data;
                if (main_port(&d->port, 34) && !d->user && d->queue > 2) d->queue = 2;
            }
        }
    }
    return next_ioctl(fd, request, arg);
}
