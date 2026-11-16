/*
 * uhid_bind.c -- Phase 10 Round 18: create ONE well-formed virtual HID device
 * with caller-selected bus/vendor/product/report-descriptor, log all emitted
 * UHID events (including GET_REPORT), and optionally hold the fd open so a
 * second shell can inspect which hid driver bound via
 * /sys/bus/hid/drivers/<drv>/ (the per-driver dir lists bound devices).
 *
 * SAFE BY CONSTRUCTION:
 *  - Only fixed, valid boot mouse / digitizer / keyboard descriptors are used.
 *  - GET_REPORT / SET_REPORT requests are answered with a well-formed
 *    *error* reply (err=1, size=0) -- never an oversized or malformed report.
 *  - No exploit primitive, no malformed descriptors, no OOB writes.
 *
 * Bare-metal armv7a static ELF (no libc headers, raw svc 0 syscalls).
 *
 * Usage:
 *   uhid_bind <bus> <vendor> <product> <rd:mouse|digi|kbd> [hold]
 *   e.g. uhid_bind 3 046d c05a mouse hold
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_bind uhid_bind.c
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

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

/* enum uhid_event_type (Linux 5.4 uapi/linux/uhid.h) */
enum {
    UHID_DESTROY         = 1,
    UHID_START           = 2,
    UHID_STOP            = 3,
    UHID_OPEN            = 4,
    UHID_CLOSE           = 5,
    UHID_OUTPUT          = 6,
    UHID_GET_REPORT      = 9,
    UHID_GET_REPORT_REPLY= 10,
    UHID_CREATE2         = 11,
    UHID_INPUT2          = 12,
    UHID_SET_REPORT      = 13,
    UHID_SET_REPORT_REPLY= 14,
};

struct uhid_start_req {
    u64 dev_flags;
} __attribute__((packed));

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

struct uhid_get_report_req {
    u32 id;
    u8  rnum;
    u8  rtype;
} __attribute__((packed));

struct uhid_get_report_reply_req {
    u32 id;
    u16 err;
    u16 size;
    u8  data[4096];
} __attribute__((packed));

struct uhid_set_report_req {
    u32 id;
    u8  rnum;
    u8  rtype;
    u16 size;
    u8  data[4096];
} __attribute__((packed));

struct uhid_set_report_reply_req {
    u32 id;
    u16 err;
} __attribute__((packed));

struct uhid_event {
    u32 type;
    union {
        struct uhid_create2_req create2;
        struct uhid_start_req start;
        struct uhid_get_report_req get_report;
        struct uhid_get_report_reply_req get_report_reply;
        struct uhid_set_report_req set_report;
        struct uhid_set_report_reply_req set_report_reply;
    } u;
} __attribute__((packed));

struct pollfd { int fd; short events; short revents; };

/* ---- minimal ARM EABI runtime helpers (we link with -nostdlib) ---- */
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

/* ---- printing helpers (volatile to avoid libc folding) ---- */
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
static unsigned long parse_hex(const char *s)
{
    unsigned long v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++; unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

/* ---- well-formed report descriptors ---- */
/* Standards-complete boot mouse (3 buttons + pad + X/Y rel 8-bit), 50 bytes. */
static const u8 rd_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
    0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x02, 0x81,0x06,
    0xC0, 0xC0
};
/* Digitizer / touch screen with ContactID (page 0x0D usage 0x51), X, Y. */
static const u8 rd_digi[] = {
    0x05,0x0D, 0x09,0x04, 0xA1,0x01,
    0x09,0x22, 0xA1,0x02,
    0x09,0x51, 0x09,0x30, 0x09,0x31,
    0x15,0x00, 0x26,0xFF,0x00,
    0x75,0x08, 0x95,0x03, 0x81,0x02,
    0xC0, 0xC0
};
/* Standards-complete boot keyboard, 63 bytes. */
static const u8 rd_kbd[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
    0x95,0x08, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xFF,0x00,
    0x05,0x07, 0x19,0x00, 0x29,0xFF, 0x81,0x00,
    0xC0
};

static void print_event(struct uhid_event *ev, long n)
{
    puts_("  EVENT type="); putint(ev->type);
    switch (ev->type) {
    case UHID_START:
        puts_(" (UHID_START) dev_flags.l0=0x");
        puthex((u32)ev->u.start.dev_flags);
        break;
    case UHID_STOP:  puts_(" (UHID_STOP)"); break;
    case UHID_OPEN:  puts_(" (UHID_OPEN)"); break;
    case UHID_CLOSE: puts_(" (UHID_CLOSE)"); break;
    case UHID_OUTPUT: puts_(" (UHID_OUTPUT)"); break;
    case UHID_GET_REPORT:
        puts_(" (UHID_GET_REPORT) id="); putint(ev->u.get_report.id);
        puts_(" rnum="); putint(ev->u.get_report.rnum);
        puts_(" rtype="); putint(ev->u.get_report.rtype);
        break;
    case UHID_SET_REPORT:
        puts_(" (UHID_SET_REPORT) id="); putint(ev->u.set_report.id);
        puts_(" rtype="); putint(ev->u.set_report.rtype);
        puts_(" size="); putint(ev->u.set_report.size);
        break;
    case UHID_CREATE2: puts_(" (UHID_CREATE2)"); break;
    case UHID_INPUT2:  puts_(" (UHID_INPUT2)"); break;
    default: puts_(" (other)");
    }
    puts_(" rawlen="); putint(n);
    puts_("\n");
}

/* Answer GET_REPORT/SET_REPORT with a minimal well-formed error reply. */
static void reply_error(int fd, struct uhid_event *in)
{
    struct uhid_event out;
    zero_(&out, sizeof(out));
    if (in->type == UHID_GET_REPORT) {
        out.type = UHID_GET_REPORT_REPLY;
        out.u.get_report_reply.id = in->u.get_report.id;
        out.u.get_report_reply.err = 1;   /* unsupported -> graceful failure */
        out.u.get_report_reply.size = 0;  /* NO data, never oversized */
    } else {
        out.type = UHID_SET_REPORT_REPLY;
        out.u.set_report_reply.id = in->u.set_report.id;
        out.u.set_report_reply.err = 1;
    }
    long w = sys3(__NR_write, fd, (long)&out, sizeof(out));
    puts_("  -> error reply write ret="); putint(w); puts_("\n");
}

/* Pump events for `rounds` poll iterations (timeout 1200ms each).
   Returns 1 if told to stop. */
static int pump(int fd, int rounds, int hold)
{
    for (int i = 0; i < rounds; i++) {
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        long pr = sys3(__NR_poll, (long)&pfd, 1, 1200);
        if (pr <= 0) {
            if (!hold) { puts_("[pump] poll ret="); putint(pr); puts_(" -> idle\n"); }
            continue;
        }
        struct uhid_event ev;
        long n = sys3(__NR_read, fd, (long)&ev, sizeof(ev));
        if (n <= 0) { puts_("[pump] read ret="); putint(n); puts_("\n"); return 1; }
        print_event(&ev, n);
        if (ev.type == UHID_GET_REPORT || ev.type == UHID_SET_REPORT)
            reply_error(fd, &ev);
    }
    return 0;
}

/* Naked entry: hand the initial stack pointer (points at argc) to C. */
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("mov r0, sp\n\tb uhid_main\n");
}

void uhid_main(unsigned long *stackp)
{
    int argc = (int)stackp[0];
    char **argv = (char **)&stackp[1];

    puts_("=== UHID bind probe (Phase 10 Round 18) ===\n");
    if (argc < 5) {
        puts_("usage: uhid_bind <bus> <vendor> <product> <mouse|digi|kbd> [hold]\n");
        sys3(__NR_exit, 2, 0, 0); for (;;) {}
    }

    u32 bus     = (u32)parse_hex(argv[1]);
    u32 vendor  = (u32)parse_hex(argv[2]);
    u32 product = (u32)parse_hex(argv[3]);
    const char *rds = argv[4];
    int hold = (argc >= 6 && eq_(argv[5], "hold"));

    const u8 *rd; int rd_len; const char *rd_name;
    if (eq_(rds, "digi")) { rd = rd_digi;  rd_len = sizeof(rd_digi);  rd_name = "digitizer/ContactID"; }
    else if (eq_(rds, "kbd")) { rd = rd_kbd; rd_len = sizeof(rd_kbd); rd_name = "boot keyboard"; }
    else { rd = rd_mouse; rd_len = sizeof(rd_mouse); rd_name = "boot mouse"; }

    puts_("[cfg] bus=0x"); puthex(bus);
    puts_(" vendor=0x"); puthex(vendor);
    puts_(" product=0x"); puthex(product);
    puts_(" rd="); puts_(rd_name); puts_("\n");

    long fd = sys3(__NR_openat, AT_FDCWD, (long)"/dev/uhid", O_RDWR);
    puts_("open(/dev/uhid,O_RDWR) fd="); putint(fd);
    puts_("\n");
    if (fd < 0) { sys3(__NR_exit, 1, 0, 0); for (;;) {} }

    struct uhid_event ev;
    zero_(&ev, sizeof(ev));
    ev.type = UHID_CREATE2;
    copy(ev.u.create2.name, "vhid-r18");
    copy(ev.u.create2.phys, "uhid/round18");
    copy(ev.u.create2.uniq, "R18");
    ev.u.create2.rd_size = (u16)rd_len;
    ev.u.create2.bus     = (u16)bus;
    ev.u.create2.vendor  = vendor;
    ev.u.create2.product = product;
    ev.u.create2.version = 0x0001;
    ev.u.create2.country = 0x00;
    for (int i = 0; i < rd_len; i++) ev.u.create2.rd_data[i] = rd[i];

    puts_("sizeof(uhid_event)="); putint(sizeof(ev)); puts_("\n");
    long wr = sys3(__NR_write, fd, (long)&ev, sizeof(ev));
    puts_("write(CREATE2) ret="); putint(wr); puts_("\n");

    pump(fd, 3, hold);

    if (hold) {
        puts_("=== HOLDING (inspect /sys/bus/hid/drivers/* from another shell) ===\n");
        for (;;) pump(fd, 25, hold);
    }

    struct uhid_event de;
    zero_(&de, sizeof(de));
    de.type = UHID_DESTROY;
    long dw = sys3(__NR_write, fd, (long)&de, sizeof(de));
    puts_("write(DESTROY) ret="); putint(dw); puts_("\n");
    sys3(__NR_close, fd, 0, 0);
    puts_("=== done ===\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
