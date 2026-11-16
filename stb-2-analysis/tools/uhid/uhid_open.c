/*
 * uhid_open.c -- Phase 9 Round 17, Task A.
 * Bare-metal armv7 static ELF (raw syscalls, no libc) that opens /dev/uhid
 * with O_RDWR and prints fd / errno.  SELinux is Enforcing; DAC permits
 * (root:uhid 0660, shell is in GID uhid=3011) but SELinux may still deny.
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_open uhid_open.c
 *
 * ARM EABI syscalls: write=4 open=5 openat=322 close=6 exit=1
 */
#define __NR_exit   1
#define __NR_write  4
#define __NR_open   5
#define __NR_openat 322
#define __NR_close  6

#define AT_FDCWD  (-100)
#define O_RDONLY  0
#define O_RDWR    2

static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0"
                 : "+r" (r0)
                 : "r" (r1), "r" (r2), "r" (r7)
                 : "memory");
    return r0;
}

static void puts_(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    sys3(__NR_write, 1, (long)s, len);
}

static void putint(long v)
{
    char buf[24];
    int i = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[i++] = '0';
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    if (neg) sys3(__NR_write, 1, (long)"-", 1);
    for (int j = i - 1; j >= 0; j--) {
        char c = buf[j];
        sys3(__NR_write, 1, (long)&c, 1);
    }
}

static void try_open(const char *path, long flags)
{
    long fd = sys3(__NR_openat, AT_FDCWD, (long)path, flags);
    long err = fd < 0 ? -fd : 0;
    puts_("open(");
    puts_(path);
    puts_(flags == O_RDWR ? ",O_RDWR" : ",O_RDONLY");
    puts_(") fd=");
    putint(fd);
    puts_(" errno=");
    putint(err);
    puts_("\n");
    if (fd >= 0) sys3(__NR_close, fd, 0, 0);
}

void _start(void)
{
    puts_("=== Task A: /dev/uhid open test ===\n");
    try_open("/dev/uhid", O_RDONLY);
    try_open("/dev/uhid", O_RDWR);
    puts_("(errno 13=EACCES 1=EPERM 2=ENOENT 19=ENODEV)\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
