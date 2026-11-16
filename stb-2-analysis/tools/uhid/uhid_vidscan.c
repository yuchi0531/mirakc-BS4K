/*
 * uhid_vidscan.c -- Phase 16 Round 23B: fast, SAFE VID sweep for the
 * hid-sei-rcon ID table. Creates ONE well-formed device at a time
 * (UHID_CREATE2 + UHID_DESTROY), then checks /sys/bus/hid/drivers/sei-rcon/
 * directly with getdents64 for a new bound-device symlink.
 *
 * SAFETY: only a fixed boot-keyboard or vendor descriptor is ever sent; no
 * input events; GET_REPORT/SET_REPORT (if any) are answered with a minimal
 * well-formed error reply that echoes the correct id; clean DESTROY each
 * iteration. No system/vendor/block writes, no reboot, no malformed reports.
 *
 * Usage: uhid_vidscan <vid_start> <vid_end> <pid> <bus> [kbd|rcon_ff1c]
 *   (hex args). Stops at first VID that binds hid-sei-rcon.
 *
 * Build: clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *        -o uhid_vidscan uhid_vidscan.c
 */

#define __NR_exit     1
#define __NR_read     3
#define __NR_write    4
#define __NR_close    6
#define __NR_poll     168
#define __NR_openat   322
#define __NR_getdents64 217

#define AT_FDCWD (-100)
#define O_RDWR   2
#define O_RDONLY 0
#define O_DIRECTORY 0x10000
#define POLLIN   1

typedef unsigned char u8; typedef unsigned short u16;
typedef unsigned int u32; typedef unsigned long long u64;
typedef long long s64;

enum { UHID_DESTROY=1, UHID_START=2, UHID_STOP=3, UHID_OPEN=4, UHID_CLOSE=5,
       UHID_GET_REPORT=9, UHID_GET_REPORT_REPLY=10, UHID_CREATE2=11,
       UHID_INPUT2=12, UHID_SET_REPORT=13, UHID_SET_REPORT_REPLY=14 };

struct uhid_create2_req {
    u8 name[128]; u8 phys[64]; u8 uniq[64];
    u16 rd_size; u16 bus; u32 vendor; u32 product; u32 version; u32 country;
    u8 rd_data[4096];
} __attribute__((packed));
struct uhid_get_report_req { u32 id; u8 rnum; u8 rtype; } __attribute__((packed));
struct uhid_get_report_reply_req { u32 id; u16 err; u16 size; u8 data[4096]; } __attribute__((packed));
struct uhid_set_report_req { u32 id; u8 rnum; u8 rtype; u16 size; u8 data[4096]; } __attribute__((packed));
struct uhid_set_report_reply_req { u32 id; u16 err; } __attribute__((packed));
struct uhid_event {
    u32 type;
    union {
        struct uhid_create2_req create2;
        struct uhid_get_report_req get_report;
        struct uhid_get_report_reply_req get_report_reply;
        struct uhid_set_report_req set_report;
        struct uhid_set_report_reply_req set_report_reply;
    } u;
} __attribute__((packed));
struct pollfd { int fd; short events; short revents; };
struct linux_dirent64 { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[]; } __attribute__((packed));

void __aeabi_memcpy(void *dst,const void *src,unsigned n){volatile u8*d=dst;volatile const u8*s=src;while(n--)*d++=*s++;}
void __aeabi_memmove(void *d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void *d,int c,unsigned n){volatile u8*p=d;while(n--)*p++=(u8)c;}
void __aeabi_memclr(void *d,unsigned n){__aeabi_memset(d,0,n);}

static long sys3(long n,long a,long b,long c){
    register long r0 asm("r0")=a; register long r1 asm("r1")=b;
    register long r2 asm("r2")=c; register long r7 asm("r7")=n;
    asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");
    return r0;
}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static void putint(long v){char b[24];int i=0,neg=0;if(v<0){neg=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+v%10;v/=10;}if(neg)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}}
static void puthex(u32 v){char b[8];const char*h="0123456789abcdef";for(int i=7;i>=0;i--){b[i]=h[v&0xf];v>>=4;}sys3(__NR_write,1,(long)b,8);}
static void copy(u8*d,const char*s){volatile const char*p=s;while(*p)*d++=(u8)*p++;}
static int eq_(const char*a,const char*b){volatile const char*x=a,*y=b;while(*x&&*y){if(*x!=*y)return 0;x++;y++;}return *x==*y;}
static unsigned long parse_hex(const char*s){unsigned long v=0;if(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))s+=2;while(*s){char c=*s++;unsigned d;if(c>='0'&&c<='9')d=c-'0';else if(c>='a'&&c<='f')d=c-'a'+10;else if(c>='A'&&c<='F')d=c-'A'+10;else break;v=(v<<4)|d;}return v;}
static int is_digit(char c){return c>='0'&&c<='9';}

/* well-formed boot keyboard */
static const u8 rd_kbd[]={
    0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,0x19,0xE0,0x29,0xE7,
    0x15,0x00,0x25,0x01,0x95,0x08,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x08,0x81,0x01,0x95,0x05,0x75,0x01,0x05,0x08,
    0x19,0x01,0x29,0x05,0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,
    0x95,0x06,0x75,0x08,0x15,0x00,0x26,0xFF,0x00,0x05,0x07,
    0x19,0x00,0x29,0xFF,0x81,0x00,0xC0};
/* well-formed vendor 0xFF1C, 8 bytes input, report id 1 */
static const u8 rd_rcon[]={
    0x06,0x1C,0xFF,0x09,0x01,0xA1,0x01,0x85,0x01,
    0x15,0x00,0x26,0xFF,0x00,0x75,0x08,0x95,0x08,0x81,0x02,0xC0};

/* Return 1 if any bound-device symlink (digit-leading name containing ':')
   appears in /sys/bus/hid/drivers/sei-rcon/. */
static int sei_rcon_has_device(void){
    int fd=(int)sys3(__NR_openat,AT_FDCWD,(long)"/sys/bus/hid/drivers/sei-rcon",O_RDONLY|O_DIRECTORY);
    if(fd<0) return 0;
    char buf[4096];
    long n=sys3(__NR_getdents64,fd,(long)buf,sizeof(buf));
    sys3(__NR_close,fd,0,0);
    if(n<=0) return 0;
    for(long off=0;off<n;){
        struct linux_dirent64*d=(struct linux_dirent64*)(buf+off);
        char*nm=d->d_name;
        if(is_digit(nm[0])){
            int colon=0; for(char*p=nm;*p;p++) if(*p==':'){colon=1;break;}
            if(colon) return 1;
        }
        off+=d->d_reclen;
    }
    return 0;
}

/* Drain pending events briefly; answer report requests with a safe error. */
static void drain(int fd){
    for(int i=0;i<4;i++){
        struct pollfd p; p.fd=fd; p.events=POLLIN; p.revents=0;
        if(sys3(__NR_poll,(long)&p,1,1)<=0) return;
        struct uhid_event ev; long n=sys3(__NR_read,fd,(long)&ev,sizeof(ev));
        if(n<=0) return;
        if(ev.type==UHID_GET_REPORT||ev.type==UHID_SET_REPORT){
            struct uhid_event out; zero_(&out,sizeof(out));
            if(ev.type==UHID_GET_REPORT){out.type=UHID_GET_REPORT_REPLY;out.u.get_report_reply.id=ev.u.get_report.id;out.u.get_report_reply.err=1;}
            else {out.type=UHID_SET_REPORT_REPLY;out.u.set_report_reply.id=ev.u.set_report.id;out.u.set_report_reply.err=1;}
            sys3(__NR_write,fd,(long)&out,sizeof(out));
        }
    }
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0, sp\n\tb scan_main\n");}

void scan_main(unsigned long*sp){
    int argc=(int)sp[0]; char**argv=(char**)&sp[1];
    puts_("=== UHID sei-rcon VID scan ===\n");
    if(argc<5){puts_("usage: uhid_vidscan <vid_start> <vid_end> <pid> <bus> [kbd|rcon]\n");sys3(__NR_exit,2,0,0);for(;;){}}
    u32 vs=(u32)parse_hex(argv[1]), ve=(u32)parse_hex(argv[2]);
    u32 pid=(u32)parse_hex(argv[3]), bus=(u32)parse_hex(argv[4]);
    const u8*rd=rd_kbd; int rdlen=sizeof(rd_kbd); const char*rn="kbd";
    if(argc>=6&&eq_(argv[5],"rcon")){rd=rd_rcon;rdlen=sizeof(rd_rcon);rn="rcon";}
    puts_("[cfg] vid ");puthex(vs);puts_("-");puthex(ve);
    puts_(" pid ");puthex(pid);puts_(" bus ");puthex(bus);puts_(" rd ");puts_(rn);puts_("\n");

    for(u32 v=vs;;v++){
        long fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
        if(fd<0){puts_("open failed at vid ");puthex(v);puts_("\n");sys3(__NR_exit,1,0,0);for(;;){}}
        struct uhid_event ev; zero_(&ev,sizeof(ev));
        ev.type=UHID_CREATE2;
        copy(ev.u.create2.name,"vhid-vscan");
        copy(ev.u.create2.phys,"uhid/vscan");
        copy(ev.u.create2.uniq,"VS");
        ev.u.create2.rd_size=(u16)rdlen; ev.u.create2.bus=(u16)bus;
        ev.u.create2.vendor=v; ev.u.create2.product=pid;
        ev.u.create2.version=1; ev.u.create2.country=0;
        for(int i=0;i<rdlen;i++) ev.u.create2.rd_data[i]=rd[i];
        long w=sys3(__NR_write,fd,(long)&ev,sizeof(ev));
        if(w!=(long)sizeof(ev)){puts_("create write failed vid ");puthex(v);puts_("\n");}
        if(sei_rcon_has_device()){
            puts_("*** HIT: sei-rcon bound vid=0x");puthex(v);
            puts_(" pid=0x");puthex(pid);puts_(" bus=0x");puthex(bus);
            puts_(" rd=");puts_(rn);puts_(" — device LEFT BOUND for inspection ***\n");
            drain(fd);
            /* leave fd open and device bound */
            for(;;) drain(fd);
        }
        /* no event drain in the hot loop: ID-table binding is decided at
           probe time (synchronous with CREATE2), before any GET_REPORT. */
        struct uhid_event de; zero_(&de,sizeof(de)); de.type=UHID_DESTROY;
        sys3(__NR_write,fd,(long)&de,sizeof(de));
        sys3(__NR_close,fd,0,0);
        if((v&0xff)==0){puts_("progress vid=0x");puthex(v);puts_("\n");}
        if(v==ve) break;
    }
    puts_("=== scan complete, no sei-rcon bind in range ===\n");
    sys3(__NR_exit,0,0,0);for(;;){}
}
