/*
 * dev_open.c -- Phase 9 Round 17: generic single-node open tester.
 * Bare-metal armv7, raw syscalls. Build with -DDEV_PATH='"/dev/hidraw0"'.
 */
#define __NR_exit   1
#define __NR_write  4
#define __NR_openat 322
#define __NR_close  6
#define AT_FDCWD (-100)
#define O_RDWR   2

#ifndef DEV_PATH
#define DEV_PATH "/dev/hidraw0"
#endif

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

void _start(void)
{
    long fd = sys3(__NR_openat, AT_FDCWD, (long)DEV_PATH, O_RDWR);
    puts_("open(" DEV_PATH ",O_RDWR) fd="); putint(fd);
    puts_(" errno="); putint(fd < 0 ? -fd : 0);
    puts_(" (13=EACCES 1=EPERM 19=ENODEV)\n");
    if (fd >= 0) sys3(__NR_close, fd, 0, 0);
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
