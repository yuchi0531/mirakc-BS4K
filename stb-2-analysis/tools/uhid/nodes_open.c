/*
 * nodes_open.c -- Phase 9 Round 17, Task C.
 * Opens (open-only; NO ioctl/read/write) a curated list of /dev nodes and
 * prints fd/errno for each, so DAC-vs-SELinux openness can be distinguished.
 * Bare-metal armv7, raw syscalls.
 */
#define __NR_exit   1
#define __NR_write  4
#define __NR_openat 322
#define __NR_close  6
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_RDWR   2

static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}
static int slen_(const char *s){ volatile const char*p=s; int n=0; while(*p){p++;n++;} return n; }
static void puts_(const char*s){ sys3(__NR_write,1,(long)s,slen_(s)); }
static void putint(long v){ char b[24];int i=0;int ng=0;if(v<0){ng=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+(v%10);v/=10;}if(ng)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);} }

static const char *paths[] = {
    "/dev/uhid",
    "/dev/uinput",
    "/dev/input/event0",
    "/dev/input/event6",
    "/dev/dri/card0",
    "/dev/dri/renderD128",
    "/dev/nexusmem",
    "/dev/nxmw",
    "/dev/nxwk",
    "/dev/bcmpip",
    "/dev/ion",
    "/dev/hddidentify",
    "/dev/binder",
    "/dev/ashmem",
};

void _start(void)
{
    puts_("=== Task C: open-only probe (no ioctl/read/write) ===\n");
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        long fd = sys3(__NR_openat, AT_FDCWD, (long)paths[i], O_RDWR);
        long er = fd < 0 ? -fd : 0;
        /* Also try O_RDONLY when O_RDWR failed (e.g. hddidentify 0664). */
        long fd2 = -1000, er2 = -1000;
        if (fd < 0) {
            fd2 = sys3(__NR_openat, AT_FDCWD, (long)paths[i], O_RDONLY);
            er2 = fd2 < 0 ? -fd2 : 0;
        }
        puts_(paths[i]);
        puts_(" RDWR:fd="); putint(fd); puts_(" err="); putint(er);
        if (fd < 0) { puts_(" RDONLY:fd="); putint(fd2); puts_(" err="); putint(er2); }
        puts_("\n");
        if (fd >= 0) sys3(__NR_close, fd, 0, 0);
        if (fd2 >= 0) sys3(__NR_close, fd2, 0, 0);
    }
    puts_("(0=OK 1=EPERM 2=ENOENT 13=EACCES 19=ENODEV 25=ENOTTY)\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
