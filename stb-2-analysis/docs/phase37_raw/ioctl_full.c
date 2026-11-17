/* PHASE37: probe exact full ioctl command values given as hex argv.
 * Prints rc/errno and dumps first 64 bytes of the returned buffer.
 * Usage: ioctl_full <dev> <size> <cmd_hex> [cmd_hex ...]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <dev> <size> <cmd_hex>...\n", argv[0]); return 2; }
    int sz = atoi(argv[2]);
    if (sz < 16) sz = 16;
    unsigned char *buf = calloc(1, (size_t)sz);
    int fd = open(argv[1], O_RDWR);
    printf("dev=%s fd=%d size=%d\n", argv[1], fd, sz);
    if (fd < 0) return 1;
    for (int i = 3; i < argc; i++) {
        unsigned long cmd = strtoul(argv[i], NULL, 0);
        memset(buf, 0, (size_t)sz);
        errno = 0;
        long r = ioctl(fd, cmd, buf);
        printf("cmd=0x%08lx xperm=0x%04lx rc=%4ld errno=%2d",
               cmd, cmd & 0xffffUL, r, errno);
        if (r >= 0) {
            printf(" buf=");
            int n = sz < 64 ? sz : 64;
            for (int j = 0; j < n; j++) printf("%02x", buf[j]);
        }
        printf("\n");
    }
    close(fd);
    free(buf);
    return 0;
}
