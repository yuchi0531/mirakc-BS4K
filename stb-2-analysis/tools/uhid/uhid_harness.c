/*
 * uhid_harness.c -- Phase 9 Round 17, Task B.
 * Bare-metal armv7 static ELF (no libc headers, raw syscalls) that:
 *   1. opens /dev/uhid O_RDWR
 *   2. tries the LEGACY UHID_CREATE2 ioctl (_IOW('U',0x11,create2)) and
 *      prints the result (Linux 5.4 uhid.c has no .ioctl fops -> ENOTTY)
 *   3. creates a virtual HID device the CORRECT 5.4 way: write() a
 *      struct uhid_event with type=UHID_CREATE2 and u.create2 filled
 *   4. reads UHID_START / UHID_OPEN events (struct uhid_event from kernel)
 *   5. submits an UHID_INPUT2 event (write) and drains any follow-up events
 *
 * Build (default = 13-byte minimal mouse from the task):
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_harness uhid_harness.c
 * Add -DRD_FULL to use a 74-byte standards-complete mouse descriptor.
 *
 * ARM EABI syscalls: exit=1 read=3 write=4 openat=322 close=6 ioctl=54 poll=168
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
#define POLLIN   1

/* ---- minimal integer types (no libc headers) ---- */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* ---- ioctl numbers (legacy; computed with host _IOW, 14-bit size) ---- */
#define UHID_CREATE2_IOCTL 0x51145511u  /* _IOW('U',0x11,struct uhid_create2_req) */
#define UHID_INPUT2_IOCTL  0x50025512u  /* _IOW('U',0x12,struct uhid_input2_req)  */

/* ---- enum uhid_event_type (Linux 5.4 include/uapi/linux/uhid.h) ---- */
enum {
    UHID_DESTROY         = 1,
    UHID_START           = 2,
    UHID_STOP            = 3,
    UHID_OPEN            = 4,
    UHID_CLOSE           = 5,
    UHID_OUTPUT          = 6,
    UHID_CREATE2         = 11,
    UHID_INPUT2          = 12,
};

/* ---- uapi structs, packed, verified against Linux 5.4 header ---- */
struct uhid_create2_req {
    u8  name[128];
    u8  phys[64];
    u8  uniq[64];
    u16 rd_size;
    u16 bus;
    u32 vendor;
    u32 product;
    u32 version;
    u32 country;
    u8  rd_data[4096];
} __attribute__((packed));

struct uhid_input2_req {
    u16 size;
    u8  data[4096];
} __attribute__((packed));

struct uhid_start_req {
    u64 dev_flags;
} __attribute__((packed));

struct uhid_output_req {
    u8  data[4096];
    u16 size;
    u8  rtype;
} __attribute__((packed));

struct uhid_event {
    u32 type;
    union {
        struct uhid_create2_req create2;
        struct uhid_input2_req  input2;
        struct uhid_start_req   start;
        struct uhid_output_req  output;
    } u;
} __attribute__((packed));

struct pollfd { int fd; short events; short revents; };

/* ---- raw syscall wrappers ---- */
static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}

/* ---- printing helpers ---- */
/* volatile loads so clang cannot fold this into libc strlen() */
static int slen_(const char *s)
{
    volatile const char *p = s;
    int n = 0;
    while (*p) { p++; n++; }
    return n;
}

static void puts_(const char *s)
{
    sys3(__NR_write, 1, (long)s, slen_(s));
}

/* volatile stores so clang cannot fold this into memset/__aeabi_memclr */
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
    for (int j = i - 1; j >= 0; j--) {
        char c = b[j];
        sys3(__NR_write, 1, (long)&c, 1);
    }
}

static void puthex(u32 v)
{
    char b[8];
    const char *h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) { b[i] = h[v & 0xf]; v >>= 4; }
    sys3(__NR_write, 1, (long)b, 8);
}

static void putdump(const u8 *p, int n)
{
    const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        char b[2] = { h[p[i] >> 4], h[p[i] & 0xf] };
        sys3(__NR_write, 1, (long)b, 2);
    }
}

/* ---- report descriptors ---- */
/* Task's minimal descriptor: UsagePage(1),Usage(2),Collection(1),
 * ReportCount(3),ReportSize(8),Input(0x06),EndCollection -> 13 bytes. */
static const u8 rd_simple[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x95,0x03, 0x75,0x08, 0x81,0x06, 0xC0
};

/* Standards-complete boot mouse, 50 bytes (known-good with hid-generic). */
static const u8 rd_full[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
    0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0, 0xC0
};

static void print_event(struct uhid_event *ev, long n)
{
    puts_("  EVENT type="); putint(ev->type);
    switch (ev->type) {
    case UHID_START:
        puts_(" (UHID_START) dev_flags=0x");
        puthex((u32)ev->u.start.dev_flags);
        break;
    case UHID_STOP:  puts_(" (UHID_STOP)"); break;
    case UHID_OPEN:  puts_(" (UHID_OPEN)"); break;
    case UHID_CLOSE: puts_(" (UHID_CLOSE)"); break;
    case UHID_OUTPUT:
        puts_(" (UHID_OUTPUT) rtype="); putint(ev->u.output.rtype);
        puts_(" size="); putint(ev->u.output.size);
        puts_(" data="); putdump(ev->u.output.data, ev->u.output.size > 16 ? 16 : ev->u.output.size);
        break;
    case UHID_CREATE2: puts_ (" (UHID_CREATE2)"); break;
    case UHID_INPUT2:  puts_ (" (UHID_INPUT2)"); break;
    default:
        puts_(" (other)");
    }
    puts_(" rawlen="); putint(n);
    puts_("\n");
}

/* Read up to 'max' queued events; poll() with 1.5s timeout per event. */
static void drain_events(int fd, const char *tag, int max)
{
    for (int i = 0; i < max; i++) {
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        long pr = sys3(__NR_poll, (long)&pfd, 1, 1500);
        if (pr <= 0) {
            puts_("["); puts_(tag); puts_("] poll ret="); putint(pr);
            puts_(" -> no more events\n");
            break;
        }
        struct uhid_event ev;
        long n = sys3(__NR_read, fd, (long)&ev, sizeof(ev));
        puts_("["); puts_(tag); puts_("] read ret="); putint(n); puts_("\n");
        if (n <= 0) break;
        print_event(&ev, n);
    }
}

static void copy(u8 *dst, const char *src)
{
    volatile const char *s = src;
    while (*s) *dst++ = (u8)*s++;
}

void _start(void)
{
    puts_("=== UhID harness (Linux 5.4 protocol: write/read struct uhid_event) ===\n");

    long fd = sys3(__NR_openat, AT_FDCWD, (long)"/dev/uhid", O_RDWR);
    puts_("open(/dev/uhid,O_RDWR) fd="); putint(fd);
    puts_(" errno="); putint(fd < 0 ? -fd : 0); puts_("\n");
    if (fd < 0) { sys3(__NR_exit, 1, 0, 0); for (;;) {} }

    /* Legacy ioctl probe: 5.4 uhid_fops has no .ioctl -> expect ENOTTY(25). */
    struct uhid_event probe;
    zero_(&probe, sizeof(probe));
    long ir = sys3(__NR_ioctl, fd, (long)UHID_CREATE2_IOCTL, (long)&probe);
    puts_("ioctl(fd, UHID_CREATE2=0x51145511, ...) ret="); putint(ir);
    puts_(" errno="); putint(ir < 0 ? -ir : 0);
    puts_(" (25=ENOTTY expected: ioctl interface removed in modern uhid)\n");

    /* Build CREATE2 event on the stack (4376 bytes). */
    struct uhid_event ev;
    zero_(&ev, sizeof(ev));

#ifdef RD_FULL
    const u8 *rd = rd_full;  int rd_len = (int)sizeof(rd_full);
    puts_("[descriptor] full 74-byte boot mouse\n");
#else
    const u8 *rd = rd_simple; int rd_len = (int)sizeof(rd_simple);
    puts_("[descriptor] simple 13-byte mouse\n");
#endif
    ev.type = UHID_CREATE2;
    copy(ev.u.create2.name, "vhid-round17");
    copy(ev.u.create2.phys, "uhid/round17");
    copy(ev.u.create2.uniq, "R17");
    ev.u.create2.rd_size = (u16)rd_len;
    ev.u.create2.bus     = 0x03;      /* BUS_USB */
    ev.u.create2.vendor  = 0x1d6b;    /* Linux Foundation */
    ev.u.create2.product = 0x0001;
    ev.u.create2.version = 0x0001;
    ev.u.create2.country = 0x00;
    for (int i = 0; i < rd_len; i++) ev.u.create2.rd_data[i] = rd[i];

    puts_("sizeof(uhid_event)="); putint(sizeof(ev));
    puts_(" rd_size="); putint(rd_len); puts_("\n");

    long wr = sys3(__NR_write, fd, (long)&ev, sizeof(ev));
    puts_("write(CREATE2) ret="); putint(wr);
    puts_(" errno="); putint(wr < 0 ? -wr : 0); puts_("\n");

    drain_events(fd, "post-CREATE2", 6);

    /* Submit INPUT2: 3-byte mouse report (buttons,dx,dy). */
    struct uhid_event in;
    zero_(&in, sizeof(in));
    in.type = UHID_INPUT2;
    in.u.input2.size = 3;
    in.u.input2.data[0] = 0x01;
    in.u.input2.data[1] = 0x02;
    in.u.input2.data[2] = 0x03;
    long iw = sys3(__NR_write, fd, (long)&in, sizeof(in));
    puts_("write(INPUT2 size=3) ret="); putint(iw);
    puts_(" errno="); putint(iw < 0 ? -iw : 0); puts_("\n");

    drain_events(fd, "post-INPUT2", 4);

#ifdef HOLD
    /* Keep the virtual device alive so a second adb shell can inspect it. */
    puts_("=== HOLDING device open (inspect from another shell) ===\n");
    for (;;) {
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        long pr = sys3(__NR_poll, (long)&pfd, 1, 30000);
        if (pr > 0) {
            struct uhid_event hx;
            long hn = sys3(__NR_read, fd, (long)&hx, sizeof(hx));
            if (hn > 0) print_event(&hx, hn);
        }
    }
#endif

    /* Destroy is UHID_DESTROY event with no payload; close() also destroys. */
    struct uhid_event de;
    zero_(&de, sizeof(de));
    de.type = UHID_DESTROY;
    long dw = sys3(__NR_write, fd, (long)&de, sizeof(de));
    puts_("write(DESTROY) ret="); putint(dw);
    puts_(" errno="); putint(dw < 0 ? -dw : 0); puts_("\n");

    sys3(__NR_close, fd, 0, 0);
    puts_("=== done ===\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
