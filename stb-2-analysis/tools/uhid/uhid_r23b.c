/*
 * uhid_r23b.c -- Phase 16 Round 23B: SAFE descriptor-signature fuzz for
 * hid-sei-rcon binding. Creates ONE well-formed virtual HID device per run
 * with caller-selected bus/VID/PID/name/report-descriptor, logs every UHID
 * event (including GET_REPORT with id/rnum/rtype), and optionally holds the
 * fd open so a second shell can diff the per-driver dirs under
 * /sys/bus/hid/drivers/ (their device symlinks) to discover which driver
 * bound the device.
 *
 * SAFETY BY CONSTRUCTION:
 *  - Only the fixed, well-formed descriptors below (or an explicit hex
 *    descriptor supplied by the orchestrator) are ever sent.
 *  - NO input events are ever injected (no UHID_INPUT2 from this harness).
 *  - GET_REPORT / SET_REPORT are answered with a minimal *error* reply
 *    (err=1, size=0) that echoes the correct correlation id -- never an
 *    oversized, wrong-id, or malformed report.
 *  - On exit a clean UHID_DESTROY is written and the fd is closed.
 *
 * Bare-metal armv7a static ELF (no libc headers, raw svc 0 syscalls).
 *
 * Usage:
 *   uhid_r23b <bus> <vid> <pid> <desc> [hold] [name]
 *     bus  : hex (3 USB, 5 BT, 0x18 I2C, 0x14 SPI, 0)
 *     vid  : hex vendor id
 *     pid  : hex product id
 *     desc : built-in name (mouse|digi|kbd|cc|cc_vendor|rcon_ff1c|
 *            rcon_ff00|rcon_ff1c_rid|rcon_ff00_rid|cc_rid|vendor_ff01)
 *            OR "hex:<even-length hex string>" for a custom descriptor.
 *     hold : literal "hold" keeps device alive for sysfs inspection.
 *     name : optional device name string (default "vhid-r23b").
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_r23b uhid_r23b.c
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

enum {
    UHID_DESTROY          = 1,
    UHID_START            = 2,
    UHID_STOP             = 3,
    UHID_OPEN             = 4,
    UHID_CLOSE            = 5,
    UHID_OUTPUT           = 6,
    UHID_GET_REPORT       = 9,
    UHID_GET_REPORT_REPLY = 10,
    UHID_CREATE2          = 11,
    UHID_INPUT2           = 12,
    UHID_SET_REPORT       = 13,
    UHID_SET_REPORT_REPLY = 14,
};

struct uhid_start_req { u64 dev_flags; } __attribute__((packed));
struct uhid_create2_req {
    u8  name[128]; u8 phys[64]; u8 uniq[64];
    u16 rd_size; u16 bus; u32 vendor; u32 product; u32 version; u32 country;
    u8  rd_data[4096];
} __attribute__((packed));
struct uhid_get_report_req { u32 id; u8 rnum; u8 rtype; } __attribute__((packed));
struct uhid_get_report_reply_req {
    u32 id; u16 err; u16 size; u8 data[4096];
} __attribute__((packed));
struct uhid_set_report_req {
    u32 id; u8 rnum; u8 rtype; u16 size; u8 data[4096];
} __attribute__((packed));
struct uhid_set_report_reply_req { u32 id; u16 err; } __attribute__((packed));
struct uhid_event {
    u32 type;
    union {
        struct uhid_create2_req       create2;
        struct uhid_start_req         start;
        struct uhid_get_report_req    get_report;
        struct uhid_get_report_reply_req get_report_reply;
        struct uhid_set_report_req    set_report;
        struct uhid_set_report_reply_req set_report_reply;
    } u;
} __attribute__((packed));
struct pollfd { int fd; short events; short revents; };

/* ---- minimal ARM EABI runtime helpers (-nostdlib) ---- */
void __aeabi_memcpy(void *dst, const void *src, unsigned n) {
    volatile u8 *d = (volatile u8 *)dst; volatile const u8 *s = (volatile const u8 *)src;
    while (n--) *d++ = *s++;
}
void __aeabi_memmove(void *dst, const void *src, unsigned n) { __aeabi_memcpy(dst, src, n); }
void __aeabi_memset(void *dst, int c, unsigned n) {
    volatile u8 *d = (volatile u8 *)dst; while (n--) *d++ = (u8)c;
}
void __aeabi_memclr(void *dst, unsigned n) { __aeabi_memset(dst, 0, n); }

static long sys3(long n, long a, long b, long c) {
    register long r0 asm("r0") = a; register long r1 asm("r1") = b;
    register long r2 asm("r2") = c; register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}
static int slen_(const char *s) { volatile const char *p = s; int n = 0; while (*p) { p++; n++; } return n; }
static void puts_(const char *s) { sys3(__NR_write, 1, (long)s, slen_(s)); }
static void zero_(void *p, unsigned n) { volatile u8 *q = (volatile u8 *)p; while (n--) *q++ = 0; }
static void putint(long v) {
    char b[24]; int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) b[i++] = '0';
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    if (neg) sys3(__NR_write, 1, (long)"-", 1);
    for (int j = i - 1; j >= 0; j--) { char c = b[j]; sys3(__NR_write, 1, (long)&c, 1); }
}
static void puthex(u32 v) {
    char b[8]; const char *h = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) { b[i] = h[v & 0xf]; v >>= 4; }
    sys3(__NR_write, 1, (long)b, 8);
}
static void copy(u8 *dst, const char *src) { volatile const char *s = src; while (*s) *dst++ = (u8)*s++; }
static int eq_(const char *a, const char *b) {
    volatile const char *x = a, *y = b;
    while (*x && *y) { if (*x != *y) return 0; x++; y++; }
    return *x == *y;
}
static unsigned long parse_hex(const char *s) {
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
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- well-formed report descriptors ---- */
static const u8 rd_mouse[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F,
    0x75,0x08, 0x95,0x02, 0x81,0x06, 0xC0, 0xC0
};
static const u8 rd_digi[] = {
    0x05,0x0D, 0x09,0x04, 0xA1,0x01, 0x09,0x22, 0xA1,0x02,
    0x09,0x51, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xFF,0x00,
    0x75,0x08, 0x95,0x03, 0x81,0x02, 0xC0, 0xC0
};
static const u8 rd_kbd[] = {
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x05,0x07, 0x19,0xE0, 0x29,0xE7,
    0x15,0x00, 0x25,0x01, 0x95,0x08, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01, 0x95,0x05, 0x75,0x01, 0x05,0x08,
    0x19,0x01, 0x29,0x05, 0x91,0x02, 0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x26,0xFF,0x00, 0x05,0x07,
    0x19,0x00, 0x29,0xFF, 0x81,0x00, 0xC0
};
/* Consumer Control: Play/Pause/Vol+/Vol-/Next/Prev (1-bit each). */
static const u8 rd_cc[] = {
    0x05,0x0C, 0x09,0x01, 0xA1,0x01,
    0x09,0xB0, 0x09,0xB1, 0x09,0xE9, 0x09,0xEA, 0x09,0xB5, 0x09,0xB6,
    0x15,0x00, 0x25,0x01, 0x95,0x06, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x02, 0x81,0x01, 0xC0
};
/* Consumer Control + vendor page 0xFF00 (8 input bytes). */
static const u8 rd_cc_vendor[] = {
    0x05,0x0C, 0x09,0x01, 0xA1,0x01,
    0x09,0xB0, 0x09,0xE9, 0x09,0xEA,
    0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x05, 0x81,0x01,
    0x06,0x00,0xFF, 0x09,0x01, 0x15,0x00, 0x26,0xFF,0x00,
    0x75,0x08, 0x95,0x08, 0x81,0x02, 0xC0
};
/* Vendor page 0xFF1C, 8 x 8-bit inputs, no report id. */
static const u8 rd_rcon_ff1c[] = {
    0x06,0x1C,0xFF, 0x09,0x01, 0xA1,0x01,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x08, 0x81,0x02, 0xC0
};
/* Vendor page 0xFF00, 8 x 8-bit inputs, no report id. */
static const u8 rd_rcon_ff00[] = {
    0x06,0x00,0xFF, 0x09,0x01, 0xA1,0x01,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x08, 0x81,0x02, 0xC0
};
/* Vendor page 0xFF1C with report id 1, 8 x 8-bit inputs. */
static const u8 rd_rcon_ff1c_rid[] = {
    0x06,0x1C,0xFF, 0x09,0x01, 0xA1,0x01, 0x85,0x01,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x08, 0x81,0x02, 0xC0
};
/* Vendor page 0xFF00 with report id 1, 8 x 8-bit inputs. */
static const u8 rd_rcon_ff00_rid[] = {
    0x06,0x00,0xFF, 0x09,0x01, 0xA1,0x01, 0x85,0x01,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x08, 0x81,0x02, 0xC0
};
/* Consumer Control with report id 2 (some RC drivers expect id 2). */
static const u8 rd_cc_rid[] = {
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x02,
    0x09,0xB0, 0x09,0xE9, 0x09,0xEA, 0x09,0xB5, 0x09,0xB6,
    0x15,0x00, 0x25,0x01, 0x95,0x06, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x02, 0x81,0x01, 0xC0
};
/* Vendor page 0xFF01, 16 x 8-bit inputs, report id 3. */
static const u8 rd_vendor_ff01[] = {
    0x06,0x01,0xFF, 0x09,0x01, 0xA1,0x01, 0x85,0x03,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x10, 0x81,0x02, 0xC0
};

static void print_event(struct uhid_event *ev, long n) {
    puts_("  EVENT type="); putint(ev->type);
    switch (ev->type) {
    case UHID_START:
        puts_(" (UHID_START) dev_flags.l0=0x"); puthex((u32)ev->u.start.dev_flags); break;
    case UHID_STOP:  puts_(" (UHID_STOP)"); break;
    case UHID_OPEN:  puts_(" (UHID_OPEN)"); break;
    case UHID_CLOSE: puts_(" (UHID_CLOSE)"); break;
    case UHID_OUTPUT: puts_(" (UHID_OUTPUT) rtype="); putint(ev->u.get_report.rtype); break;
    case UHID_GET_REPORT:
        puts_(" (UHID_GET_REPORT) id="); putint(ev->u.get_report.id);
        puts_(" rnum="); putint(ev->u.get_report.rnum);
        puts_(" rtype="); putint(ev->u.get_report.rtype);
        break;
    case UHID_SET_REPORT:
        puts_(" (UHID_SET_REPORT) id="); putint(ev->u.set_report.id);
        puts_(" rnum="); putint(ev->u.set_report.rnum);
        puts_(" rtype="); putint(ev->u.set_report.rtype);
        puts_(" size="); putint(ev->u.set_report.size);
        break;
    case UHID_CREATE2: puts_(" (UHID_CREATE2)"); break;
    case UHID_INPUT2:  puts_(" (UHID_INPUT2)"); break;
    default: puts_(" (other)");
    }
    puts_(" rawlen="); putint(n); puts_("\n");
}

/* Answer GET_REPORT/SET_REPORT with a minimal well-formed ERROR reply. */
static void reply_error(int fd, struct uhid_event *in) {
    struct uhid_event out;
    zero_(&out, sizeof(out));
    if (in->type == UHID_GET_REPORT) {
        out.type = UHID_GET_REPORT_REPLY;
        out.u.get_report_reply.id = in->u.get_report.id; /* correct correlation id */
        out.u.get_report_reply.err = 1;
        out.u.get_report_reply.size = 0;                 /* no data, never oversized */
    } else {
        out.type = UHID_SET_REPORT_REPLY;
        out.u.set_report_reply.id = in->u.set_report.id;
        out.u.set_report_reply.err = 1;
    }
    long w = sys3(__NR_write, fd, (long)&out, sizeof(out));
    puts_("  -> error reply write ret="); putint(w); puts_("\n");
}

static int pump(int fd, int rounds, int hold) {
    for (int i = 0; i < rounds; i++) {
        struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        long pr = sys3(__NR_poll, (long)&pfd, 1, 1200);
        if (pr <= 0) { if (!hold) { puts_("[pump] poll ret="); putint(pr); puts_(" -> idle\n"); } continue; }
        struct uhid_event ev;
        long n = sys3(__NR_read, fd, (long)&ev, sizeof(ev));
        if (n <= 0) { puts_("[pump] read ret="); putint(n); puts_("\n"); return 1; }
        print_event(&ev, n);
        if (ev.type == UHID_GET_REPORT || ev.type == UHID_SET_REPORT) reply_error(fd, &ev);
    }
    return 0;
}

__attribute__((naked)) void _start(void) { __asm__ volatile("mov r0, sp\n\tb uhid_main\n"); }

void uhid_main(unsigned long *stackp) {
    int argc = (int)stackp[0];
    char **argv = (char **)&stackp[1];

    puts_("=== UHID r23b fuzz probe ===\n");
    if (argc < 5) {
        puts_("usage: uhid_r23b <bus> <vid> <pid> <desc|hex:...> [hold] [name]\n");
        sys3(__NR_exit, 2, 0, 0); for (;;) {}
    }
    u32 bus = (u32)parse_hex(argv[1]);
    u32 vendor = (u32)parse_hex(argv[2]);
    u32 product = (u32)parse_hex(argv[3]);
    const char *rds = argv[4];
    int hold = (argc >= 6 && eq_(argv[5], "hold"));
    const char *nm = (argc >= 7) ? argv[6] : "vhid-r23b";

    const u8 *rd = rd_mouse; int rd_len = sizeof(rd_mouse); const char *rd_name = "mouse";
    if      (eq_(rds,"digi"))           { rd=rd_digi;         rd_len=sizeof(rd_digi);         rd_name="digi"; }
    else if (eq_(rds,"kbd"))            { rd=rd_kbd;          rd_len=sizeof(rd_kbd);          rd_name="kbd"; }
    else if (eq_(rds,"cc"))             { rd=rd_cc;           rd_len=sizeof(rd_cc);           rd_name="cc"; }
    else if (eq_(rds,"cc_vendor"))      { rd=rd_cc_vendor;    rd_len=sizeof(rd_cc_vendor);    rd_name="cc_vendor"; }
    else if (eq_(rds,"rcon_ff1c"))      { rd=rd_rcon_ff1c;    rd_len=sizeof(rd_rcon_ff1c);    rd_name="rcon_ff1c"; }
    else if (eq_(rds,"rcon_ff00"))      { rd=rd_rcon_ff00;    rd_len=sizeof(rd_rcon_ff00);    rd_name="rcon_ff00"; }
    else if (eq_(rds,"rcon_ff1c_rid")) { rd=rd_rcon_ff1c_rid;rd_len=sizeof(rd_rcon_ff1c_rid);rd_name="rcon_ff1c_rid"; }
    else if (eq_(rds,"rcon_ff00_rid")) { rd=rd_rcon_ff00_rid;rd_len=sizeof(rd_rcon_ff00_rid);rd_name="rcon_ff00_rid"; }
    else if (eq_(rds,"cc_rid"))         { rd=rd_cc_rid;       rd_len=sizeof(rd_cc_rid);       rd_name="cc_rid"; }
    else if (eq_(rds,"vendor_ff01"))    { rd=rd_vendor_ff01;  rd_len=sizeof(rd_vendor_ff01);  rd_name="vendor_ff01"; }
    else if (rds[0]=='h' && rds[1]=='e' && rds[2]=='x' && rds[3]==':') {
        static u8 custom[4096]; int n = 0; const char *p = rds + 4;
        while (p[0] && p[1]) {
            int a = hexval(p[0]), b = hexval(p[1]);
            if (a < 0 || b < 0) break;
            custom[n++] = (u8)((a<<4)|b); p += 2;
        }
        rd = custom; rd_len = n; rd_name = "hex";
    }

    puts_("[cfg] bus=0x"); puthex(bus);
    puts_(" vendor=0x"); puthex(vendor);
    puts_(" product=0x"); puthex(product);
    puts_(" rd="); puts_(rd_name); puts_(" len="); putint(rd_len);
    puts_(" name="); puts_(nm); puts_("\n");

    long fd = sys3(__NR_openat, AT_FDCWD, (long)"/dev/uhid", O_RDWR);
    puts_("open(/dev/uhid,O_RDWR) fd="); putint(fd); puts_("\n");
    if (fd < 0) { sys3(__NR_exit, 1, 0, 0); for (;;) {} }

    struct uhid_event ev;
    zero_(&ev, sizeof(ev));
    ev.type = UHID_CREATE2;
    copy(ev.u.create2.name, nm);
    copy(ev.u.create2.phys, "uhid/r23b");
    copy(ev.u.create2.uniq, "R23B");
    ev.u.create2.rd_size = (u16)rd_len;
    ev.u.create2.bus = (u16)bus;
    ev.u.create2.vendor = vendor;
    ev.u.create2.product = product;
    ev.u.create2.version = 0x0001;
    ev.u.create2.country = 0x00;
    for (int i = 0; i < rd_len; i++) ev.u.create2.rd_data[i] = rd[i];

    puts_("sizeof(uhid_event)="); putint(sizeof(ev)); puts_("\n");
    long wr = sys3(__NR_write, fd, (long)&ev, sizeof(ev));
    puts_("write(CREATE2) ret="); putint(wr); puts_("\n");

    pump(fd, 3, hold);
    if (hold) {
        puts_("=== HOLDING (diff /sys/bus/hid/drivers/* from another shell) ===\n");
        for (;;) pump(fd, 25, hold);
    }
    struct uhid_event de;
    zero_(&de, sizeof(de)); de.type = UHID_DESTROY;
    long dw = sys3(__NR_write, fd, (long)&de, sizeof(de));
    puts_("write(DESTROY) ret="); putint(dw); puts_("\n");
    sys3(__NR_close, fd, 0, 0);
    puts_("=== done ===\n");
    sys3(__NR_exit, 0, 0, 0);
    for (;;) {}
}
