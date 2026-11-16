/*
 * uinput_probe.c -- Phase 10 Round 18, Task C: minimal /dev/uinput probe.
 * Opens O_RDWR, sets EV_KEY + one key bit, creates a tiny virtual input
 * device via UI_DEV_SETUP/UI_DEV_CREATE, and holds it open for inspection.
 * NO event injection. Bare-metal armv7a static ELF, raw syscalls.
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uinput_probe uinput_probe.c
 * Usage: uinput_probe [hold]
 */

#define __NR_exit   1
#define __NR_read   3
#define __NR_write  4
#define __NR_close  6
#define __NR_ioctl  54
#define __NR_poll   168
#define __NR_openat 322

#define AT_FDCWD (-100)
#define O_RDWR   2

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* Linux 5.4 uapi/linux/uinput.h ioctls */
#define UI_DEV_CREATE   0x00005501u  /* _IO('U',1)  */
#define UI_DEV_DESTROY  0x00005502u  /* _IO('U',2)  */
#define UI_DEV_SETUP    0x405c5503u  /* _IOW('U',3,struct uinput_setup=92B) */
#define UI_SET_EVBIT    0x40045564u  /* _IOW('U',100,int) */
#define UI_SET_KEYBIT   0x40045565u  /* _IOW('U',101,int) */
#define UI_GET_VERSION  0x8004552du  /* _IOR('U',45,unsigned int) */

struct input_id { u16 bustype; u16 vendor; u16 product; u16 version; };
struct uinput_setup {
    struct input_id id;
    char name[80];
    u32 ff_effects_max;
};
struct pollfd { int fd; short events; short revents; };

static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}

void __aeabi_memcpy(void *dst, const void *src, unsigned n)
{
    volatile u8 *d = (volatile u8 *)dst;
    volatile const u8 *s = (volatile const u8 *)src;
    while (n--) *d++ = *s++;
}
void __aeabi_memmove(void *dst, const void *src, unsigned n) { __aeabi_memcpy(dst, src, n); }
void __aeabi_memset(void *dst, int c, unsigned n)
{
    volatile u8 *d = (volatile u8 *)dst;
    while (n--) *d++ = (u8)c;
}
void __aeabi_memclr(void *dst, unsigned n) { __aeabi_memset(dst, 0, n); }

static int slen_(const char *s)
{
    volatile const char *p = s;
    int n = 0;
    while (*p) { p++; n++; }
    return n;
}
static void puts_(const char *s) { sys3(__NR_write, 1, (long)s, slen_(s)); }
static void zero_(void *p, unsigned n)
{
    volatile u8 *q = (volatile u8 *)p;
    while (n--) *q++ = 0;
}
static void putint(long v)
{
    char b[24]; int i = 0; int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) b[i++] = '0';
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    if (neg) sys3(__NR_write, 1, (long)"-", 1);
    for (int j = i - 1; j >= 0; j--) { char c = b[j]; sys3(__NR_write, 1, (long)&c, 1); }
}
static void copy(u8 *dst, const char *src)
{
    volatile const char *s = src;
    while (*s) *dst++ = (u8)*s++;
}
static int eq_(const char *a, const char *b)
{
    volatile const char *x = a; volatile const char *y = b;
    while (*x && *y) { if (*x != *y) return 0; x++; y++; }
    return *x == *y;
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile("mov r0, sp\n\tb uinput_main\n");
}

void uinput_main(unsigned long *stackp)
{
    int argc = (int)stackp[0];
    char **argv = (char **)&stackp[1];
    int hold = (argc >= 2 && eq_(argv[1], "hold"));

    puts_("=== uinput probe (Phase 10 Round 18, Task C) ===\n");

    long fd = sys3(__NR_openat, AT_FDCWD, (long)"/dev/uinput", O_RDWR);
    puts_("open(/dev/uinput,O_RDWR) fd="); putint(fd); puts_("\n");
    if (fd < 0) { sys3(__NR_exit, 1, 0, 0); for (;;) {} }

    unsigned int version = 0;
    long rv = sys3(__NR_ioctl, fd, (long)UI_GET_VERSION, (long)&version);
    puts_("ioctl(UI_GET_VERSION) ret="); putint(rv);
    puts_(" version="); putint(version); puts_("\n");

    int ev_key = 0x01;   /* EV_KEY */
    int key_a  = 0x1e;   /* KEY_A */
    /* NOTE: this vendor kernel expects the bit value DIRECTLY as the ioctl
       arg (mainline uses a pointer); the pointer form returns -EINVAL. */
    long r1 = sys3(__NR_ioctl, fd, (long)UI_SET_EVBIT, (long)ev_key);
    puts_("ioctl(UI_SET_EVBIT,EV_KEY) ret="); putint(r1); puts_("\n");
    long r2 = sys3(__NR_ioctl, fd, (long)UI_SET_KEYBIT, (long)key_a);
    puts_("ioctl(UI_SET_KEYBIT,KEY_A) ret="); putint(r2); puts_("\n");

    struct uinput_setup us;
    zero_(&us, sizeof(us));
    us.id.bustype = 0x03;      /* BUS_USB */
    us.id.vendor  = 0x1234;
    us.id.product = 0x0002;
    us.id.version = 0x0001;
    copy((u8 *)us.name, "r18-uinput");
    long r3 = sys3(__NR_ioctl, fd, (long)UI_DEV_SETUP, (long)&us);
    puts_("ioctl(UI_DEV_SETUP) ret="); putint(r3); puts_("\n");

    long r4 = sys3(__NR_ioctl, fd, (long)UI_DEV_CREATE, 0);
    puts_("ioctl(UI_DEV_CREATE) ret="); putint(r4); puts_("\n");

    if (hold) {
        puts_("=== HOLDING uinput device (inspect /dev/input + getevent) ===\n");
        for (;;) {
            struct pollfd pfd;
            pfd.fd = fd; pfd.events = 0; pfd.revents = 0;
            sys3(__NR_poll, (long)&pfd, 1, 30000); /* sleep ~30s, no event injection */
        }
    }

    long r5 = sys3(__NR_ioctl, fd, (long)UI_DEV_DESTROY, 0);
    puts_("ioctl(UI_DEV_DESTROY) ret="); putint(r5); puts_("\n");
    sys3(__NR_close, fd, 0, 0);
    puts_("=== done ===\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
