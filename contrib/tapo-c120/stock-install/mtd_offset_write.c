#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <mtd/mtd-user.h>

static int stock_verify_fd = -1;
static uint32_t stock_verify_base;

/* Stock 1.4.4's MTD reads replace the rootfs header with a decrypted cache.
 * Its vendor read ioctl calls the physical driver's _read directly. */
static void stock_raw_read(int fd, uint8_t *data, size_t size, uint32_t offset) {
    if (sizeof(void *) != 4 || size > 65536 || offset > 0x1000000 ||
        size > 0x1000000 - offset) {
        fprintf(stderr, "invalid stock raw read range or ABI\n");
        exit(2);
    }
    uint32_t request[4] = {offset, (uint32_t)(uintptr_t)data, (uint32_t)size, 0};
    if (ioctl(fd, 0x8001df00UL, request) != 0) {
        perror("stock raw read");
        exit(1);
    }
}

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void enable_stock_verification(uint32_t base) {
    stock_verify_fd = open("/dev/slp_flash_chrdev", O_RDONLY);
    if (stock_verify_fd < 0) die("open stock flash verifier");
    stock_verify_base = base;
}

static uint64_t parse_u64(const char *text) {
    char *end = NULL;
    errno = 0;
    uint64_t value = strtoull(text, &end, 0);
    if (!text[0] || text[0] == '-' || text[0] == '+' || errno || (end && *end)) {
        fprintf(stderr, "invalid number: %s\n", text);
        exit(2);
    }
    return value;
}

static void read_file(const char *path, uint8_t **data, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open image");

    struct stat st;
    if (fstat(fd, &st) < 0) die("stat image");
    if (st.st_size <= 0) {
        fprintf(stderr, "image is empty: %s\n", path);
        exit(2);
    }

    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) die("malloc image");

    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t got = read(fd, buf + done, (size_t)st.st_size - done);
        if (got < 0) die("read image");
        if (got == 0) break;
        done += (size_t)got;
    }
    close(fd);

    if (done != (size_t)st.st_size) {
        fprintf(stderr, "short read from %s\n", path);
        exit(1);
    }
    *data = buf;
    *size = done;
}

static uint64_t align_down(uint64_t value, uint64_t unit) {
    return value - (value % unit);
}

static uint64_t align_up(uint64_t value, uint64_t unit) {
    uint64_t rem = value % unit;
    return rem ? value + unit - rem : value;
}

static void erase_range(int fd, const struct mtd_info_user *info, uint64_t offset, uint64_t length) {
    if (length == 0) return;
    if (offset > info->size || length > info->size - offset) {
        fprintf(stderr, "erase range exceeds MTD size: off=0x%llx len=0x%llx size=0x%x\n",
                (unsigned long long)offset, (unsigned long long)length, info->size);
        exit(2);
    }

    uint64_t start = align_down(offset, info->erasesize);
    uint64_t end = align_up(offset + length, info->erasesize);
    for (uint64_t pos = start; pos < end; pos += info->erasesize) {
        struct erase_info_user erase;
        memset(&erase, 0, sizeof(erase));
        erase.start = (uint32_t)pos;
        erase.length = info->erasesize;
        fprintf(stderr, "erase 0x%08llx +0x%08x\n", (unsigned long long)pos, info->erasesize);
        if (ioctl(fd, MEMERASE, &erase) < 0) die("MEMERASE");
    }
}

static void seek_to(int fd, uint64_t offset, const char *context) {
    off_t got = lseek(fd, (off_t)offset, SEEK_SET);
    if (got < 0) die(context);
    if ((uint64_t)got != offset) {
        fprintf(stderr, "short seek for %s: got=0x%llx want=0x%llx\n",
                context, (unsigned long long)got, (unsigned long long)offset);
        exit(1);
    }
}

static void write_all_at(int fd, const uint8_t *data, size_t size, uint64_t offset) {
    seek_to(fd, offset, "lseek write");
    size_t done = 0;
    while (done < size) {
        ssize_t wrote = write(fd, data + done, size - done);
        if (wrote < 0) die("write");
        if (wrote == 0) {
            fprintf(stderr, "zero-length write\n");
            exit(1);
        }
        done += (size_t)wrote;
    }
}

static void read_all_at(int fd, uint8_t *data, size_t size, uint64_t offset) {
    if (stock_verify_fd >= 0) {
        uint64_t absolute = stock_verify_base + offset;
        if (absolute > 0x1000000 || size > 0x1000000 - absolute) {
            fprintf(stderr, "stock verify exceeds flash size\n");
            exit(2);
        }
        for (size_t done = 0; done < size;) {
            size_t length = size - done < 4096 ? size - done : 4096;
            stock_raw_read(stock_verify_fd, data + done, length, (uint32_t)(absolute + done));
            done += length;
        }
        return;
    }
    seek_to(fd, offset, "lseek read");
    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, data + done, size - done);
        if (got < 0) die("read verify");
        if (got == 0) {
            fprintf(stderr, "short verify read\n");
            exit(1);
        }
        done += (size_t)got;
    }
}

static void verify_all(int fd, const uint8_t *data, size_t size, uint64_t offset) {
    uint8_t *buf = malloc(size);
    if (!buf) die("malloc verify");

    read_all_at(fd, buf, size, offset);

    if (memcmp(data, buf, size) != 0) {
        fprintf(stderr, "verify mismatch at offset 0x%llx\n", (unsigned long long)offset);
        exit(1);
    }
    free(buf);
}

static void verify_chunk(int fd, const uint8_t *data, uint8_t *buf, size_t size, uint64_t offset) {
    read_all_at(fd, buf, size, offset);
    if (memcmp(data, buf, size) != 0) {
        size_t first = 0;
        while (first < size && data[first] == buf[first]) first++;
        fprintf(stderr, "verify mismatch at offset 0x%llx: expected %02x, read %02x\n",
                (unsigned long long)(offset + first), data[first], buf[first]);
        exit(1);
    }
}

static size_t read_exact_or_die(int fd, uint8_t *buf, size_t size) {
    size_t done = 0;
    while (done < size) {
        ssize_t got = read(fd, buf + done, size - done);
        if (got < 0) die("read stream");
        if (got == 0) break;
        done += (size_t)got;
    }
    return done;
}

static void stream_write_exact(int input_fd, int mtd_fd, const struct mtd_info_user *info,
                               const char *label, uint64_t offset, uint64_t size,
                               uint64_t erase_len, int do_erase) {
    if (offset > info->size || size > info->size - offset) {
        fprintf(stderr, "stream exceeds MTD size: %s off=0x%llx size=0x%llx mtd=0x%x\n",
                label, (unsigned long long)offset, (unsigned long long)size, info->size);
        exit(2);
    }
    if (do_erase) {
        if (!info->erasesize || offset % info->erasesize) {
            fprintf(stderr, "write offset must be erase-block aligned\n");
            exit(2);
        }
        if (erase_len < size) erase_len = size;
        fprintf(stderr, "stream write %s size=0x%llx at 0x%llx erase_len=0x%llx\n",
                label, (unsigned long long)size, (unsigned long long)offset,
                (unsigned long long)erase_len);
        erase_range(mtd_fd, info, offset, erase_len);
    } else {
        fprintf(stderr, "stream write %s size=0x%llx at 0x%llx no_erase\n",
                label, (unsigned long long)size, (unsigned long long)offset);
    }

    const size_t chunk_size = 4096;
    uint8_t *chunk = malloc(chunk_size);
    uint8_t *verify = malloc(chunk_size);
    if (!chunk || !verify) die("malloc stream");

    uint64_t done = 0;
    while (done < size) {
        size_t want = (size - done) < chunk_size ? (size_t)(size - done) : chunk_size;
        size_t got = read_exact_or_die(input_fd, chunk, want);
        if (got != want) {
            fprintf(stderr, "short stream for %s: got=0x%llx want=0x%llx\n",
                    label, (unsigned long long)(done + got), (unsigned long long)size);
            exit(1);
        }
        write_all_at(mtd_fd, chunk, got, offset + done);
        verify_chunk(mtd_fd, chunk, verify, got, offset + done);
        done += got;
        fprintf(stderr, ".");
    }
    fprintf(stderr, "\nverified %s\n", label);
    fsync(mtd_fd);
    free(chunk);
    free(verify);
}

static void stream_compare_exact(int input_fd, int mtd_fd, const struct mtd_info_user *info,
                                 const char *label, uint64_t offset, uint64_t size) {
    if (offset > info->size || size > info->size - offset) {
        fprintf(stderr, "stream compare exceeds MTD size: %s off=0x%llx size=0x%llx mtd=0x%x\n",
                label, (unsigned long long)offset, (unsigned long long)size, info->size);
        exit(2);
    }

    const size_t chunk_size = 4096;
    uint8_t *chunk = malloc(chunk_size);
    uint8_t *verify = malloc(chunk_size);
    if (!chunk || !verify) die("malloc compare stream");

    uint64_t done = 0;
    while (done < size) {
        size_t want = (size - done) < chunk_size ? (size_t)(size - done) : chunk_size;
        size_t got = read_exact_or_die(input_fd, chunk, want);
        if (got != want) {
            fprintf(stderr, "short compare stream for %s: got=0x%llx want=0x%llx\n",
                    label, (unsigned long long)(done + got), (unsigned long long)size);
            exit(1);
        }
        verify_chunk(mtd_fd, chunk, verify, got, offset + done);
        done += got;
        fprintf(stderr, ".");
    }
    fprintf(stderr, "\ncompared %s\n", label);
    free(chunk);
    free(verify);
}

static void compare_image(int fd, const struct mtd_info_user *info, const char *image,
                          uint64_t offset) {
    int input_fd = open(image, O_RDONLY);
    if (input_fd < 0) die("open image");
    struct stat st;
    if (fstat(input_fd, &st) < 0) die("stat image");
    if (st.st_size <= 0) {
        fprintf(stderr, "image is empty: %s\n", image);
        exit(2);
    }
    stream_compare_exact(input_fd, fd, info, image, offset, (uint64_t)st.st_size);
    close(input_fd);
}

static int open_mtd_flags(const char *mtd, struct mtd_info_user *info, int flags) {
    int fd = open(mtd, flags);
    if (fd < 0) die("open mtd");
    if (ioctl(fd, MEMGETINFO, info) < 0) die("MEMGETINFO");
    fprintf(stderr, "%s: size=0x%x erasesize=0x%x writesize=0x%x\n",
            mtd, info->size, info->erasesize, info->writesize);
    return fd;
}

static int open_mtd_rw(const char *mtd, struct mtd_info_user *info) {
    return open_mtd_flags(mtd, info, O_SYNC | O_RDWR);
}

static int open_mtd_ro(const char *mtd, struct mtd_info_user *info) {
    return open_mtd_flags(mtd, info, O_RDONLY);
}

static void write_buffer(int fd, const struct mtd_info_user *info, const char *label,
                         const uint8_t *data, size_t size, uint64_t offset,
                         uint64_t erase_len) {
    if (offset + size > info->size) {
        fprintf(stderr, "image exceeds MTD size: %s off=0x%llx size=0x%zx mtd=0x%x\n",
                label, (unsigned long long)offset, size, info->size);
        exit(2);
    }
    if (erase_len < size) erase_len = size;
    fprintf(stderr, "write %s size=0x%zx at 0x%llx erase_len=0x%llx\n",
            label, size, (unsigned long long)offset, (unsigned long long)erase_len);
    erase_range(fd, info, offset, erase_len);
    write_all_at(fd, data, size, offset);
    fsync(fd);
    verify_all(fd, data, size, offset);
    fprintf(stderr, "verified %s\n", label);
}

static void write_image(int fd, const struct mtd_info_user *info, const char *image,
                        uint64_t offset, uint64_t erase_len) {
    int input_fd = open(image, O_RDONLY);
    if (input_fd < 0) die("open image");
    struct stat st;
    if (fstat(input_fd, &st) < 0) die("stat image");
    if (st.st_size <= 0) {
        fprintf(stderr, "image is empty: %s\n", image);
        exit(2);
    }
    stream_write_exact(input_fd, fd, info, image, offset, (uint64_t)st.st_size,
                       erase_len, 1);
    close(input_fd);
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s write <mtd> <offset> <image> [erase_len]\n"
            "  %s stock-write <mtd> <absolute-base> <offset> <image> [erase_len]\n"
            "  %s stock-compare <absolute-offset> <image>\n"
            "  %s write-stream <mtd> <offset> <size> [erase_len]\n"
            "  %s stock-write-stream <mtd> <absolute-base> <offset> <size> [erase_len]\n"
            "  %s compare <mtd> <offset> <image>\n"
            "  %s compare-stream <mtd> <offset> <size>\n"
            "  %s islocked <mtd> <offset> <length>\n"
            "  %s c120-raw-read <absolute-offset> <length> <new-output-file>\n"
            "  %s erase <mtd> <offset> <length>\n"
            "  %s c120-openipc <mtd15> <uImage> <rootfs.squashfs>\n"
            "  %s c120-openipc-boot <mtd0> <mtd1> <u-boot.bin>\n"
            "  %s c120-openipc-boot-stream <mtd0> <mtd1> <u-boot-size>\n"
            "  %s c120-restore-stock-critical <mtd15> <mtd3_config> <mtd4_normal_boot> <mtd5_kernel> <mtd6_rootfs>\n"
            "  %s c120-restore-stock-critical-stream <mtd15>\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0,
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage(argv[0]);

    if (strcmp(argv[1], "c120-raw-read") == 0) {
        if (argc != 5) return usage(argv[0]);
        uint64_t offset = parse_u64(argv[2]);
        uint64_t size = parse_u64(argv[3]);
        if (!size || offset >= 0x1000000 || size > 0x1000000 - offset) {
            fprintf(stderr, "read must stay inside the C120 16 MiB flash\n");
            return 2;
        }
        int fd = open("/dev/slp_flash_chrdev", O_RDONLY);
        if (fd < 0) die("open stock flash reader");
        int out = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (out < 0) die("create raw backup");
        uint8_t chunk[4096];
        for (uint64_t done = 0; done < size;) {
            size_t length = size - done < sizeof(chunk) ? size - done : sizeof(chunk);
            stock_raw_read(fd, chunk, length, (uint32_t)(offset + done));
            write_all_at(out, chunk, length, done);
            done += length;
        }
        if (fsync(out) < 0) die("sync raw backup");
        close(out);
        close(fd);
        fprintf(stderr, "raw backup complete: 0x%llx +0x%llx\n",
                (unsigned long long)offset, (unsigned long long)size);
        return 0;
    }

    if (strcmp(argv[1], "stock-compare") == 0) {
        if (argc != 4) return usage(argv[0]);
        struct mtd_info_user info = {.size = 0x1000000};
        enable_stock_verification(0);
        compare_image(stock_verify_fd, &info, argv[3], parse_u64(argv[2]));
        close(stock_verify_fd);
        return 0;
    }

    if (strcmp(argv[1], "write") == 0 || strcmp(argv[1], "stock-write") == 0) {
        int stock = strcmp(argv[1], "stock-write") == 0;
        if (argc < 5 + stock || argc > 6 + stock) return usage(argv[0]);
        if (stock) {
            uint64_t base = parse_u64(argv[3]);
            if (base >= 0x1000000) return 2;
            enable_stock_verification((uint32_t)base);
        }
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        if (stock && info.size > 0x1000000 - stock_verify_base) {
            fprintf(stderr, "stock MTD base and size exceed the physical flash\n");
            close(fd);
            return 2;
        }
        uint64_t offset = parse_u64(argv[3 + stock]);
        uint64_t erase_len = argc == 6 + stock ? parse_u64(argv[5 + stock]) : 0;
        write_image(fd, &info, argv[4 + stock], offset, erase_len);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "compare") == 0) {
        if (argc != 5) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_ro(argv[2], &info);
        compare_image(fd, &info, argv[4], parse_u64(argv[3]));
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "write-stream") == 0 || strcmp(argv[1], "stock-write-stream") == 0) {
        int stock = strcmp(argv[1], "stock-write-stream") == 0;
        if (argc < 5 + stock || argc > 6 + stock) return usage(argv[0]);
        if (stock) {
            uint64_t base = parse_u64(argv[3]);
            if (base >= 0x1000000) return 2;
            enable_stock_verification((uint32_t)base);
        }
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        if (stock && info.size > 0x1000000 - stock_verify_base) {
            fprintf(stderr, "stock MTD base and size exceed the physical flash\n");
            close(fd);
            return 2;
        }
        uint64_t offset = parse_u64(argv[3 + stock]);
        uint64_t size = parse_u64(argv[4 + stock]);
        if (!size) return 2;
        uint64_t erase_len = argc == 6 + stock ? parse_u64(argv[5 + stock]) : 0;
        stream_write_exact(STDIN_FILENO, fd, &info, "stdin", offset, size, erase_len, 1);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "compare-stream") == 0) {
        if (argc != 5) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_ro(argv[2], &info);
        stream_compare_exact(STDIN_FILENO, fd, &info, "stdin", parse_u64(argv[3]), parse_u64(argv[4]));
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "erase") == 0) {
        if (argc != 5) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        erase_range(fd, &info, parse_u64(argv[3]), parse_u64(argv[4]));
        fsync(fd);
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "islocked") == 0) {
        if (argc != 5) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_ro(argv[2], &info);
        uint64_t offset = parse_u64(argv[3]);
        uint64_t length = parse_u64(argv[4]);
        if (length == 0 || offset + length > info.size) {
            fprintf(stderr, "lock query exceeds MTD size\n");
            exit(2);
        }
        struct erase_info_user region = {
            .start = (uint32_t)offset,
            .length = (uint32_t)length,
        };
        int locked = ioctl(fd, MEMISLOCKED, &region);
        if (locked < 0) die("MEMISLOCKED");
        printf("%s 0x%llx+0x%llx: %s\n", argv[2],
               (unsigned long long)offset, (unsigned long long)length,
               locked ? "locked" : "unlocked");
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "c120-openipc") == 0) {
        if (argc != 5) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        if (info.size < 0x00fc0000) {
            fprintf(stderr, "expected mtd15-like partition of at least 0xfc0000 bytes\n");
            exit(2);
        }
        write_image(fd, &info, argv[3], 0x00010000, 0x00200000);
        write_image(fd, &info, argv[4], 0x00210000, 0x00500000);
        erase_range(fd, &info, 0x00710000, 0x008b0000);
        fsync(fd);
        close(fd);
        fprintf(stderr, "c120-openipc flash plan complete\n");
        return 0;
    }

    if (strcmp(argv[1], "c120-openipc-boot") == 0) {
        if (argc != 5) return usage(argv[0]);
        uint8_t *data = NULL;
        size_t size = 0;
        read_file(argv[4], &data, &size);

        struct mtd_info_user info0;
        struct mtd_info_user info1;
        int fd0 = open_mtd_rw(argv[2], &info0);
        int fd1 = open_mtd_rw(argv[3], &info1);
        if (info0.size != 0x00030000 || info1.size != 0x00010000) {
            fprintf(stderr, "expected C120 stock mtd0=0x30000 and mtd1=0x10000\n");
            exit(2);
        }
        if (size > (size_t)info0.size + (size_t)info1.size) {
            fprintf(stderr, "u-boot image too large for stock mtd0+mtd1: size=0x%zx\n", size);
            exit(2);
        }

        size_t first = size < info0.size ? size : info0.size;
        write_buffer(fd0, &info0, "openipc boot part 0", data, first, 0, info0.size);
        if (size > first) {
            write_buffer(fd1, &info1, "openipc boot part 1", data + first, size - first, 0, info1.size);
        } else {
            erase_range(fd1, &info1, 0, info1.size);
            fsync(fd1);
        }
        close(fd0);
        close(fd1);
        free(data);
        fprintf(stderr, "c120-openipc bootloader flash plan complete\n");
        return 0;
    }

    if (strcmp(argv[1], "c120-openipc-boot-stream") == 0) {
        if (argc != 5) return usage(argv[0]);
        uint64_t size = parse_u64(argv[4]);

        struct mtd_info_user info0;
        struct mtd_info_user info1;
        int fd0 = open_mtd_rw(argv[2], &info0);
        int fd1 = open_mtd_rw(argv[3], &info1);
        if (info0.size != 0x00030000 || info1.size != 0x00010000) {
            fprintf(stderr, "expected C120 stock mtd0=0x30000 and mtd1=0x10000\n");
            exit(2);
        }
        if (size > (uint64_t)info0.size + (uint64_t)info1.size) {
            fprintf(stderr, "u-boot stream too large for stock mtd0+mtd1: size=0x%llx\n",
                    (unsigned long long)size);
            exit(2);
        }

        uint64_t first = size < info0.size ? size : info0.size;
        if (first) {
            stream_write_exact(STDIN_FILENO, fd0, &info0, "openipc boot part 0", 0, first, info0.size, 1);
        } else {
            erase_range(fd0, &info0, 0, info0.size);
            fsync(fd0);
        }
        if (size > first) {
            stream_write_exact(STDIN_FILENO, fd1, &info1, "openipc boot part 1", 0, size - first, info1.size, 1);
        } else {
            erase_range(fd1, &info1, 0, info1.size);
            fsync(fd1);
        }
        close(fd0);
        close(fd1);
        fprintf(stderr, "c120-openipc bootloader stream flash complete\n");
        return 0;
    }

    if (strcmp(argv[1], "c120-restore-stock-critical") == 0) {
        if (argc != 7) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        if (info.size != 0x00fc0000) {
            fprintf(stderr, "expected stock C120 /dev/mtd15 af partition size 0xfc0000\n");
            exit(2);
        }

        const uint64_t offsets[] = {0x00010000, 0x00020000, 0x00030200, 0x001f0000};
        const uint64_t sizes[] = {0x10000, 0x10000, 0x1bfe00, 0x1d0000};
        const char *labels[] = {"stock config", "stock normal_boot", "stock kernel", "stock rootfs"};
        int inputs[4];
        for (int i = 0; i < 4; i++) {
            inputs[i] = open(argv[i + 3], O_RDONLY);
            if (inputs[i] < 0) die("open stock backup");
            struct stat st;
            if (fstat(inputs[i], &st) < 0) die("stat stock backup");
            if (st.st_size != (off_t)sizes[i]) {
                fprintf(stderr, "unexpected stock backup size: %s got=0x%llx want=0x%llx\n",
                        argv[i + 3], (unsigned long long)st.st_size,
                        (unsigned long long)sizes[i]);
                exit(2);
            }
        }

        erase_range(fd, &info, 0x00010000, 0x003b0000);
        for (int i = 0; i < 4; i++) {
            stream_write_exact(inputs[i], fd, &info, labels[i], offsets[i], sizes[i], 0, 0);
            close(inputs[i]);
        }
        fsync(fd);
        close(fd);
        fprintf(stderr, "c120 stock critical restore complete\n");
        return 0;
    }

    if (strcmp(argv[1], "c120-restore-stock-critical-stream") == 0) {
        if (argc != 3) return usage(argv[0]);
        struct mtd_info_user info;
        int fd = open_mtd_rw(argv[2], &info);
        if (info.size != 0x00fc0000) {
            fprintf(stderr, "expected stock C120 /dev/mtd15 af partition size 0xfc0000\n");
            exit(2);
        }

        erase_range(fd, &info, 0x00010000, 0x003b0000);
        stream_write_exact(STDIN_FILENO, fd, &info, "stock config", 0x00010000, 0x10000, 0, 0);
        stream_write_exact(STDIN_FILENO, fd, &info, "stock normal_boot", 0x00020000, 0x10000, 0, 0);
        stream_write_exact(STDIN_FILENO, fd, &info, "stock kernel", 0x00030200, 0x1bfe00, 0, 0);
        stream_write_exact(STDIN_FILENO, fd, &info, "stock rootfs", 0x001f0000, 0x1d0000, 0, 0);
        fsync(fd);
        close(fd);
        fprintf(stderr, "c120 stock critical stream restore complete\n");
        return 0;
    }

    return usage(argv[0]);
}
