/*
 * uhid_descfuzz.c -- Phase 16 Round 23B: fast SAFE descriptor fuzz for
 * hid-sei-rcon. Iterates a usage-page x usage grid, creating ONE well-formed
 * virtual HID device per combination and checking /sys/bus/hid/drivers/
 * sei-rcon/ with getdents64 for a new bound-device symlink.
 *
 * SAFETY: fixed well-formed descriptors only; no input events; report requests
 * answered with a correct-id error reply; clean UHID_DESTROY every iteration.
 *
 * Usage: uhid_descfuzz <page_lo> <page_hi> <usage_lo> <usage_hi> <rid> <count> <bus> <vid> <pid>
 *   all hex. Stops at first page/usage that binds hid-sei-rcon.
 *
 * Build: clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *        -o uhid_descfuzz uhid_descfuzz.c
 */

#define __NR_exit 1
#define __NR_read 3
#define __NR_write 4
#define __NR_close 6
#define __NR_poll 168
#define __NR_openat 322
#define __NR_getdents64 217
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_RDONLY 0
#define O_DIRECTORY 0x10000
#define POLLIN 1

typedef unsigned char u8; typedef unsigned short u16;
typedef unsigned int u32; typedef unsigned long long u64; typedef long long s64;
enum { UHID_DESTROY=1, UHID_GET_REPORT=9, UHID_GET_REPORT_REPLY=10,
       UHID_CREATE2=11, UHID_SET_REPORT=13, UHID_SET_REPORT_REPLY=14 };
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

void __aeabi_memcpy(void*d,const void*s,unsigned n){volatile u8*x=d;volatile const u8*y=s;while(n--)*x++=*y++;}
void __aeabi_memmove(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void*d,int c,unsigned n){volatile u8*p=d;while(n--)*p++=(u8)c;}
void __aeabi_memclr(void*d,unsigned n){__aeabi_memset(d,0,n);}
void __aeabi_memcpy8(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memmove8(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset8(void*d,int c,unsigned n){__aeabi_memset(d,c,n);}
void __aeabi_memclr8(void*d,unsigned n){__aeabi_memset(d,0,n);}
static long sys3(long n,long a,long b,long c){
    register long r0 asm("r0")=a;register long r1 asm("r1")=b;
    register long r2 asm("r2")=c;register long r7 asm("r7")=n;
    asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static void putint(long v){char b[24];int i=0,neg=0;if(v<0){neg=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+v%10;v/=10;}if(neg)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}}
static void puthex(u32 v){char b[8];const char*h="0123456789abcdef";for(int i=7;i>=0;i--){b[i]=h[v&0xf];v>>=4;}sys3(__NR_write,1,(long)b,8);}
static void copy(u8*d,const char*s){volatile const char*p=s;while(*p)*d++=(u8)*p++;}
static unsigned long parse_hex(const char*s){unsigned long v=0;if(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))s+=2;while(*s){char c=*s++;unsigned d;if(c>='0'&&c<='9')d=c-'0';else if(c>='a'&&c<='f')d=c-'a'+10;else if(c>='A'&&c<='F')d=c-'A'+10;else break;v=(v<<4)|d;}return v;}
static int is_digit(char c){return c>='0'&&c<='9';}

static int sei_rcon_has_device(void){
    int fd=(int)sys3(__NR_openat,AT_FDCWD,(long)"/sys/bus/hid/drivers/sei-rcon",O_RDONLY|O_DIRECTORY);
    if(fd<0)return 0;
    char buf[4096]; long n=sys3(__NR_getdents64,fd,(long)buf,sizeof(buf));
    sys3(__NR_close,fd,0,0);
    if(n<=0)return 0;
    for(long off=0;off<n;){
        struct linux_dirent64*d=(struct linux_dirent64*)(buf+off);char*nm=d->d_name;
        if(is_digit(nm[0])){int colon=0;for(char*p=nm;*p;p++)if(*p==':'){colon=1;break;}if(colon)return 1;}
        off+=d->d_reclen;
    }
    return 0;
}
static void drain(int fd){
    for(int i=0;i<2;i++){
        struct pollfd p;p.fd=fd;p.events=POLLIN;p.revents=0;
        if(sys3(__NR_poll,(long)&p,1,1)<=0)return;
        struct uhid_event ev;if(sys3(__NR_read,fd,(long)&ev,sizeof(ev))<=0)return;
        if(ev.type==UHID_GET_REPORT||ev.type==UHID_SET_REPORT){
            struct uhid_event out;zero_(&out,sizeof(out));
            if(ev.type==UHID_GET_REPORT){out.type=UHID_GET_REPORT_REPLY;out.u.get_report_reply.id=ev.u.get_report.id;out.u.get_report_reply.err=1;}
            else{out.type=UHID_SET_REPORT_REPLY;out.u.set_report_reply.id=ev.u.set_report.id;out.u.set_report_reply.err=1;}
            sys3(__NR_write,fd,(long)&out,sizeof(out));
        }
    }
}
__attribute__((naked)) void _start(void){__asm__ volatile("mov r0, sp\n\tb fuzz_main\n");}

void fuzz_main(unsigned long*sp){
    int argc=(int)sp[0];char**argv=(char**)&sp[1];
    puts_("=== UHID descriptor fuzz (sei-rcon) ===\n");
    if(argc<10){puts_("usage: uhid_descfuzz <page_lo> <page_hi> <usage_lo> <usage_hi> <rid> <count> <bus> <vid> <pid>\n");sys3(__NR_exit,2,0,0);for(;;){}}
    u32 pl=(u32)parse_hex(argv[1]),ph=(u32)parse_hex(argv[2]);
    u32 ul=(u32)parse_hex(argv[3]),uh=(u32)parse_hex(argv[4]);
    u32 rid=(u32)parse_hex(argv[5]),cnt=(u32)parse_hex(argv[6]);
    u32 bus=(u32)parse_hex(argv[7]),vid=(u32)parse_hex(argv[8]),pid=(u32)parse_hex(argv[9]);
    puts_("[cfg] pages ");puthex(pl);puts_("-");puthex(ph);
    puts_(" usages ");puthex(ul);puts_("-");puthex(uh);
    puts_(" rid=");puthex(rid);puts_(" cnt=");puthex(cnt);
    puts_(" bus=");puthex(bus);puts_(" vid=");puthex(vid);puts_(" pid=");puthex(pid);puts_("\n");

    for(u32 page=pl;;page++){
        for(u32 usage=ul;;usage++){
            long fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uhid",O_RDWR);
            if(fd<0){puts_("open failed\n");sys3(__NR_exit,1,0,0);for(;;){}}
            u8 rd[32];int n=0;
            u8 hi=(u8)(page>>8),lo=(u8)page;
            if(hi){rd[n++]=0x06;rd[n++]=lo;rd[n++]=hi;}else{rd[n++]=0x05;rd[n++]=lo;}
            rd[n++]=0x09;rd[n++]=(u8)usage;rd[n++]=0xA1;rd[n++]=0x01;
            if(rid){rd[n++]=0x85;rd[n++]=(u8)rid;}
            rd[n++]=0x15;rd[n++]=0x00;rd[n++]=0x26;rd[n++]=0xFF;rd[n++]=0x00;
            rd[n++]=0x75;rd[n++]=0x08;rd[n++]=0x95;rd[n++]=(u8)cnt;rd[n++]=0x81;rd[n++]=0x02;rd[n++]=0xC0;
            struct uhid_event ev;zero_(&ev,sizeof(ev));ev.type=UHID_CREATE2;
            copy(ev.u.create2.name,"vhid-dfuzz");copy(ev.u.create2.phys,"uhid/dfuzz");copy(ev.u.create2.uniq,"DF");
            ev.u.create2.rd_size=(u16)n;ev.u.create2.bus=(u16)bus;ev.u.create2.vendor=vid;ev.u.create2.product=pid;
            ev.u.create2.version=1;ev.u.create2.country=0;
            for(int i=0;i<n;i++)ev.u.create2.rd_data[i]=rd[i];
            sys3(__NR_write,fd,(long)&ev,sizeof(ev));
            if(sei_rcon_has_device()){
                puts_("*** HIT page=0x");puthex(page);puts_(" usage=0x");puthex(usage);
                puts_(" rid=");puthex(rid);puts_(" cnt=");puthex(cnt);
                puts_(" bus=");puthex(bus);puts_(" vid=");puthex(vid);puts_(" pid=");puthex(pid);
                puts_(" — device LEFT BOUND ***\n");
                for(;;)drain(fd);
            }
            drain(fd);
            struct uhid_event de;zero_(&de,sizeof(de));de.type=UHID_DESTROY;
            sys3(__NR_write,fd,(long)&de,sizeof(de));
            sys3(__NR_close,fd,0,0);
            if(usage==uh)break;
        }
        puts_("progress page=0x");puthex(page);puts_("\n");
        if(page==ph)break;
    }
    puts_("=== fuzz complete, no sei-rcon bind ===\n");
    sys3(__NR_exit,0,0,0);for(;;){}
}
