#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/watchdog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void request_stop(int sig) { (void)sig; stop = 1; }

static void close_inherited(int keep) {
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) maxfd = 1024;
    for (int fd = 3; fd < maxfd; fd++) if (fd != keep) close(fd);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "exec") == 0) {
        close_inherited(-1);
        execvp(argv[2], argv + 2);
        perror("exec");
        return 1;
    }
    if ((argc != 3 && argc != 4) || strcmp(argv[1], "watchdog") != 0) {
        fprintf(stderr, "usage: %s exec <program> [args...] | watchdog <new-pid-file> [monitor-pid]\n", argv[0]);
        return 2;
    }

    /* A fresh stock boot may spawn main before monitor opens the watchdog.
     * Duplicate monitor's open description; reopening /dev/watchdog is not dup. */
    if (argc == 4) {
        char *end, path[64], executable[64];
        long owner = strtol(argv[3], &end, 10);
        if (*end || owner <= 1 || owner > INT_MAX) return 2;
        snprintf(path, sizeof(path), "/proc/%ld/exe", owner);
        ssize_t length = readlink(path, executable, sizeof(executable) - 1);
        if (length < 0) { perror("monitor executable"); return 1; }
        executable[length] = 0;
        if (strcmp(executable, "/bin/monitor") != 0) {
            fprintf(stderr, "not the stock monitor; no changes made\n");
            return 1;
        }
        int ownerfd = syscall(SYS_pidfd_open, (pid_t)owner, 0);
        if (ownerfd < 0) { perror("pidfd_open"); return 1; }
        /* Descriptor numbers can differ between stock releases and boots. */
        struct stat device, candidate;
        if (stat("/dev/watchdog", &device)) { perror("watchdog device"); close(ownerfd); return 1; }
        snprintf(path, sizeof(path), "/proc/%ld/fd", owner);
        DIR *fds = opendir(path);
        if (!fds) { perror("monitor descriptors"); close(ownerfd); return 1; }
        int watchdog = -1;
        struct dirent *entry;
        while ((entry = readdir(fds))) {
            long number = strtol(entry->d_name, &end, 10);
            if (end == entry->d_name || *end || number < 0 || number > INT_MAX) continue;
            snprintf(path, sizeof(path), "/proc/%ld/fd/%ld", owner, number);
            if (stat(path, &candidate) || !S_ISCHR(candidate.st_mode) ||
                candidate.st_rdev != device.st_rdev) continue;
            watchdog = syscall(SYS_pidfd_getfd, ownerfd, (int)number, 0);
            break;
        }
        closedir(fds);
        close(ownerfd);
        if (watchdog < 0) { perror("pidfd_getfd watchdog"); return 1; }
        if (watchdog != 6) {
            if (dup2(watchdog, 6) < 0) { perror("dup watchdog"); close(watchdog); return 1; }
            close(watchdog);
        }
    }

    /* Never stop monitor until this independent feeder has been verified. */
    struct stat dev, fdstat;
    int fd = 6, timeout = 0;
    if (stat("/dev/watchdog", &dev) || fstat(fd, &fdstat) ||
        !S_ISCHR(fdstat.st_mode) || fdstat.st_rdev != dev.st_rdev) {
        fprintf(stderr, "fd 6 is not the inherited stock watchdog; no changes made\n");
        return 1;
    }
    if (ioctl(fd, WDIOC_GETTIMEOUT, &timeout) || ioctl(fd, WDIOC_KEEPALIVE, 0)) {
        perror("watchdog probe");
        return 1;
    }
    int pidfile = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (pidfile < 0) { perror("pidfile"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(pidfile); unlink(argv[2]); return 1; }
    if (pid > 0) {
        dprintf(pidfile, "%ld\n", (long)pid);
        fsync(pidfile);
        close(pidfile);
        printf("watchdog feeder pid=%ld timeout=%d seconds\n", (long)pid, timeout);
        return 0;
    }
    setsid();
    close_inherited(fd);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0) _exit(1);
    for (int i = 0; i < 3; i++) dup2(nullfd, i);
    if (nullfd > 2) close(nullfd);
    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    for (;;) {
        if (stop) {
            int options = WDIOS_DISABLECARD;
            if (ioctl(fd, WDIOC_SETOPTIONS, &options) == 0) break;
            stop = 0;
        }
        if (ioctl(fd, WDIOC_KEEPALIVE, 0)) _exit(1);
        sleep(1);
    }
    close(fd);
    return 0;
}
