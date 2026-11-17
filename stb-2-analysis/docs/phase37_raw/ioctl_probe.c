/* PHASE37: minimal aarch64 ioctl probe. argv: <device> [buffer_size]
 * Issues a curated set of raw ioctl cmd values with a zeroed buffer.
 * Reports rc + errno. No side-effectful commands (no CONNECT/LISTEN/etc).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

static const unsigned long CMDS[] = {
    /* appdomain gpu_device allowxperm whitelist samples */
    0x0000UL, 0x0008UL, 0x000eUL, 0x0014UL, 0x0018UL, 0x0036UL,
    0x0a00UL, 0x0a15UL, 0x0a25UL, 0x0a31UL,
    0x1600UL, 0x1611UL, 0x3000UL, 0x3014UL, 0x3a00UL, 0x3a22UL,
    0x4202UL, 0x6400UL, 0x6411UL, 0x641cUL,
    /* forbidden: no allowxperm rule covers 0x5aXX */
    0x5a00UL, 0x5a25UL, 0x5affUL, 0x65a25UL,
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dev> [size]\n", argv[0]); return 2; }
    int sz = argc > 2 ? atoi(argv[2]) : 512;
    if (sz < 16) sz = 16;
    unsigned char *buf = calloc(1, (size_t)sz);
    int fd = open(argv[1], O_RDWR);
    printf("=== dev=%s open=%d errno=%d size=%d ===\n", argv[1], fd, errno, sz);
    if (fd < 0) return 1;
    for (unsigned k = 0; k < sizeof(CMDS)/sizeof(CMDS[0]); k++) {
        memset(buf, 0, (size_t)sz);
        errno = 0;
        long r = ioctl(fd, CMDS[k], buf);
        printf("cmd=0x%06lx rc=%3ld errno=%2d (%s)\n", CMDS[k], r, errno, strerror(errno));
    }
    close(fd);
    free(buf);
    return 0;
}
