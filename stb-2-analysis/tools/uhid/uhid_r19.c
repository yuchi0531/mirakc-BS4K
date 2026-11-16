/*
 * uhid_r19.c -- Phase 11 Round 19: NON-DESTRUCTIVE descriptor reconnaissance.
 *
 * Creates ONE virtual HID digitizer carrying the CVE-2026-43047 *trigger-path*
 * descriptor: an input report (ID 1, ContactID/X/Y) that makes hid-multitouch
 * bind, a FEATURE report ID 3 carrying HID_DG_CONTACTMAX (0x000d0055) which
 * makes hid-multitouch's mt_feature_mapping()->mt_get_feature() issue a
 * UHID_GET_REPORT at probe time, and a LARGE FEATURE report ID 4 (64 bytes)
 * that is present only to confirm the second report exists.
 *
 * ABSOLUTELY NO EXPLOITATION:
 *  - We NEVER write a UHID_GET_REPORT_REPLY with a mismatched/wrong report ID.
 *  - Modes: observe (no reply), errreply (err=1,size=0), okreply (data[0]==
 *    requested id, matching size).  The large report ID 4 is never answered
 *    with data that could select it from a smaller buffer.
 *  - Only fixed, structurally-valid descriptors; no OOB, no crash, no panic.
 *
 * Bare-metal armv7a static ELF (no libc headers, raw svc 0 syscalls).
 * Usage: uhid_r19 <bus> <vendor> <product> [observe|errreply|okreply]
 * Build: clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *            -o uhid_r19 uhid_r19.c
 */

#define __NR_exit   1
#define __NR_read   3
#define __NR_write  4
#define __NR_close  6
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

struct uhid_start_req { u64 dev_flags; } __attribute__((packed));

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

struct uhid_get_report_req { u32 id; u8 rnum; u8 rtype; } __attribute__((packed));

struct uhid_get_report_reply_req {
    u32 id;
    u16 err;
    u16 size;
    u8  data[4096];
} __attribute__((packed));

struct uhid_set_report_req {
    u32 id; u8 rnum; u8 rtype; u16 size; u8 data[4096];
} __attribute__((packed));

struct uhid_set_report_reply_req { u32 id; u16 err; } __attribute__((packed));

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

/* ---- minimal ARM EABI runtime helpers (-nostdlib) ---- */
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

static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}

static int slen_(const char *s)
{
    volatile const char *p = s; int n = 0;
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
    char b[8]; const char *h = "0123456789abcdef";
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

/* ---- trigger-path descriptor (report IDs 1 / 3 / 4) ---- */
static const u8 rd_r19[] = {
    /* Digitizer / Touch Screen application collection */
    0x05,0x0D,              /* Usage Page (Digitizer) */
    0x09,0x04,              /* Usage (Touch Screen) */
    0xA1,0x01,              /* Collection (Application) */

    /* Input report ID 1: Finger with ContactID(0x51), X(0x30), Y(0x31).
       HID_DG_CONTACTID makes hid group == HID_GROUP_MULTITOUCH. */
    0x85,0x01,              /*   Report ID (1) */
    0x09,0x22,              /*   Usage (Finger) */
    0xA1,0x02,              /*   Collection (Logical) */
    0x09,0x51,              /*     Usage (ContactID) */
    0x09,0x30,              /*     Usage (X) */
    0x09,0x31,              /*     Usage (Y) */
    0x15,0x00,              /*     Logical Minimum (0) */
    0x26,0xFF,0x00,         /*     Logical Maximum (255) */
    0x75,0x08,              /*     Report Size (8) */
    0x95,0x03,              /*     Report Count (3) */
    0x81,0x02,              /*     Input (Data,Var,Abs) */
    0xC0,                   /*   End Collection (Logical) */

    /* FEATURE report ID 3: HID_DG_CONTACTMAX (0x000d0055).
       report_features() -> mt_feature_mapping(CONTACTMAX) -> mt_get_feature()
       -> hid_hw_raw_request(GET_REPORT, id=3) at probe time. */
    0x85,0x03,              /*   Report ID (3) */
    0x09,0x55,              /*   Usage (Contact Count Maximum) = 0x000d0055 */
    0x15,0x00,              /*   Logical Minimum (0) */
    0x26,0xFF,0x00,         /*   Logical Maximum (255) */
    0x75,0x08,              /*   Report Size (8) */
    0x95,0x01,              /*   Report Count (1) */
    0xB1,0x02,              /*   Feature (Data,Var,Abs) */

    /* LARGE FEATURE report ID 4: 64-byte vendor blob. Present only to prove
       the second (large) feature report exists. Never selected by a reply. */
    0x85,0x04,              /*   Report ID (4) */
    0x06,0x00,0xFF,         /*   Usage Page (Vendor Defined 0xFF00) */
    0x09,0x01,              /*   Usage (Vendor Usage 1) */
    0x15,0x00,              /*   Logical Minimum (0) */
    0x26,0xFF,0x00,         /*   Logical Maximum (255) */
    0x75,0x08,              /*   Report Size (8) */
    0x95,0x40,              /*   Report Count (64) */
    0xB1,0x02,              /*   Feature (Data,Var,Abs) */

    0xC0                    /* End Collection (Application) */
};

static void print_event(struct uhid_event *ev, long n)
{
    puts_("  EVENT type="); putint(ev->type);
    switch (ev->type) {
    case UHID_START:
        puts_(" (UHID_START) flags=0x"); puthex((u32)ev->u.start.dev_flags); break;
    case UHID_STOP:  puts_(" (UHID_STOP)"); break;
    case UHID_OPEN:  puts_(" (UHID_OPEN)"); break;
    case UHID_CLOSE: puts_(" (UHID_CLOSE)"); break;
    case UHID_OUTPUT:puts_(" (UHID_OUTPUT)"); break;
    case UHID_GET_REPORT:
        puts_(" (UHID_GET_REPORT) id="); putint(ev->u.get_report.id);
        puts_(" rnum="); putint(ev->u.get_report.rnum);
        puts_(" rtype="); putint(ev->u.get_report.rtype); break;
    case UHID_SET_REPORT:
        puts_(" (UHID_SET_REPORT) id="); putint(ev->u.set_report.id);
        puts_(" rtype="); putint(ev->u.set_report.rtype);
        puts_(" size="); putint(ev->u.set_report.size); break;
    case UHID_CREATE2: puts_(" (UHID_CREATE2)"); break;
    case UHID_INPUT2:  puts_(" (UHID_INPUT2)"); break;
    default: puts_(" (other)");
    }
    puts_(" rawlen="); putint(n); puts_("\n");
}

/* Safe replies only. mode: 0=observe(no reply), 1=errreply, 2=okreply. */
static void maybe_reply(int fd, struct uhid_event *in, int mode)
{
    struct uhid_event out;
    long w;
    if (mode == 0) return;               /* observe: no reply at all */
    zero_(&out, sizeof(out));
    if (in->type == UHID_GET_REPORT) {
        u32 id = in->u.get_report.id;    /* correlation token (echoed in reply) */
        u8  rnum = in->u.get_report.rnum;/* actual report ID requested */
        out.type = UHID_GET_REPORT_REPLY;
        out.u.get_report_reply.id = id;  /* MUST echo correlation id */
        if (mode == 1) {                 /* errreply: graceful failure */
            out.u.get_report_reply.err = 1;
            out.u.get_report_reply.size = 0;
            puts_("  -> GET_REPORT_REPLY err=1 size=0 id="); putint(id); puts_("\n");
        } else {                         /* okreply: ONLY for rnum==3 (matching) */
            if (rnum == 3) {
                out.u.get_report_reply.err = 0;
                out.u.get_report_reply.size = 2;
                out.u.get_report_reply.data[0] = rnum; /* == requested report ID: safe */
                out.u.get_report_reply.data[1] = 0;
                puts_("  -> GET_REPORT_REPLY ok matching rnum=3 size=2\n");
            } else {
                /* Never craft a reply that could name the large report. */
                out.u.get_report_reply.err = 1;
                out.u.get_report_reply.size = 0;
                puts_("  -> GET_REPORT_REPLY err=1 (rnum!=3, refused) rnum=");
                putint(rnum); puts_("\n");
            }
        }
    } else {
        out.type = UHID_SET_REPORT_REPLY;
        out.u.set_report_reply.id = in->u.set_report.id;
        out.u.set_report_reply.err = 1;
    }
    w = sys3(__NR_write, fd, (long)&out, sizeof(out));
    puts_("  reply write ret="); putint(w); puts_("\n");
}

/* Pump events for `rounds` iterations of 500ms each. Returns 1 on read error. */
static int pump(int fd, int rounds, int mode)
{
    for (int i = 0; i < rounds; i++) {
        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        long pr = sys3(__NR_poll, (long)&pfd, 1, 500);
        if (pr <= 0) continue;
        struct uhid_event ev;
        long n = sys3(__NR_read, fd, (long)&ev, sizeof(ev));
        if (n <= 0) { puts_("[pump] read ret="); putint(n); puts_("\n"); return 1; }
        print_event(&ev, n);
        if (ev.type == UHID_GET_REPORT || ev.type == UHID_SET_REPORT)
            maybe_reply(fd, &ev, mode);
    }
    return 0;
}

__attribute__((naked)) void _start(void)
{
    __asm__ volatile("mov r0, sp\n\tb uhid_main\n");
}

void uhid_main(unsigned long *stackp)
{
    int argc = (int)stackp[0];
    char **argv = (char **)&stackp[1];

    puts_("=== UHID r19 recon (Phase 11 Round 19) ===\n");
    if (argc < 4) {
        puts_("usage: uhid_r19 <bus> <vendor> <product> [observe|errreply|okreply]\n");
        sys3(__NR_exit, 2, 0, 0); for (;;) {}
    }

    u32 bus     = (u32)parse_hex(argv[1]);
    u32 vendor  = (u32)parse_hex(argv[2]);
    u32 product = (u32)parse_hex(argv[3]);
    int mode = 0;
    if (argc >= 5) {
        if (eq_(argv[4], "errreply")) mode = 1;
        else if (eq_(argv[4], "okreply")) mode = 2;
        else mode = 0;  /* default: observe */
    }
    const char *modes[] = { "observe", "errreply", "okreply" };
    int longrun = (argc >= 6 && eq_(argv[5], "long"));

    puts_("[cfg] bus=0x"); puthex(bus);
    puts_(" vendor=0x"); puthex(vendor);
    puts_(" product=0x"); puthex(product);
    puts_(" mode="); puts_(modes[mode]); puts_("\n");
    puts_("[cfg] rd_size="); putint(sizeof(rd_r19)); puts_("\n");

    long fd = sys3(__NR_openat, AT_FDCWD, (long)"/dev/uhid", O_RDWR);
    puts_("open(/dev/uhid,O_RDWR) fd="); putint(fd); puts_("\n");
    if (fd < 0) { sys3(__NR_exit, 1, 0, 0); for (;;) {} }

    struct uhid_event ev;
    zero_(&ev, sizeof(ev));
    ev.type = UHID_CREATE2;
    copy(ev.u.create2.name, "vhid-r19");
    copy(ev.u.create2.phys, "uhid/round19");
    copy(ev.u.create2.uniq, "R19");
    ev.u.create2.rd_size = (u16)sizeof(rd_r19);
    ev.u.create2.bus     = (u16)bus;
    ev.u.create2.vendor  = vendor;
    ev.u.create2.product = product;
    ev.u.create2.version = 0x0001;
    ev.u.create2.country = 0x00;
    for (unsigned i = 0; i < sizeof(rd_r19); i++) ev.u.create2.rd_data[i] = rd_r19[i];

    puts_("sizeof(uhid_event)="); putint(sizeof(ev)); puts_("\n");
    long wr = sys3(__NR_write, fd, (long)&ev, sizeof(ev));
    puts_("write(CREATE2) ret="); putint(wr); puts_("\n");

    puts_("[pump] observing for "); putint(longrun ? 30 : 5);
    puts_("s\n");
    pump(fd, longrun ? 60 : 10, mode);

    struct uhid_event de;
    zero_(&de, sizeof(de));
    de.type = UHID_DESTROY;
    long dw = sys3(__NR_write, fd, (long)&de, sizeof(de));
    puts_("write(DESTROY) ret="); putint(dw); puts_("\n");
    /* drain a few post-destroy events */
    pump(fd, 2, mode);
    sys3(__NR_close, fd, 0, 0);
    puts_("=== done ===\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
