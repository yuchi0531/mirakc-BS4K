/* PHASE37 Task B: sweep all appdomain-allowed gpu_device xperms.
 * For each xperm X, issue ioctl(fd, 0x00060000|X, zeroed 64B buf).
 * Report any result that is not ENOTTY(25)/EFAULT(14)/EINVAL(22)/ENOSPC(28).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "allowed_xperms.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <dev>\n", argv[0]); return 2; }
    unsigned char buf[64];
    int fd = open(argv[1], O_RDWR);
    printf("dev=%s fd=%d count=%zu\n", argv[1], fd, sizeof(ALLOWED)/sizeof(ALLOWED[0]));
    if (fd < 0) return 1;
    for (size_t i = 0; i < sizeof(ALLOWED)/sizeof(ALLOWED[0]); i++) {
        unsigned long x = ALLOWED[i];
        unsigned long cmd = 0x00060000UL | x;
        memset(buf, 0, sizeof(buf));
        errno = 0;
        long r = ioctl(fd, cmd, buf);
        if (errno != 25 && errno != 14 && errno != 22 && errno != 28) {
            printf("INTERESTING x=0x%04x cmd=0x%08lx rc=%ld errno=%d", x, cmd, r, errno);
            if (r >= 0) { printf(" buf="); for (int j=0;j<32;j++) printf("%02x",buf[j]); }
            printf("\n");
        }
    }
    printf("--- sweep done ---\n");
    close(fd);
    return 0;
}
