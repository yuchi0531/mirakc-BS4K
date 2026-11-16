/*
 * ipc_probe.c -- Phase 13 Round 21, Task 0.
 *
 * Bare-metal armv7a static ELF (no libc headers, raw svc 0 syscalls).
 * Proves whether SysV IPC message queues and POSIX mqueues are reachable
 * from uid 2000 on kernel 5.4.286-ab0600 (arm64 kernel / armv8l userspace).
 *
 * Exact syscall numbers from 5.4.286 arch/arm/tools/syscall.tbl:
 *   msgget=303 msgsnd=301 msgrcv=302 msgctl=304
 *   mq_open=274 mq_unlink=275 mq_timedsend=276 mq_timedreceive=277
 *
 * /proc/config.gz says CONFIG_SYSVIPC is not set -> all SysV calls must
 * return -ENOSYS (-38) regardless of seccomp.
 *
 * Build:
 *   clang --target=armv7a-linux-androideabi21 -nostdlib -static -Os \
 *         -o ipc_probe ipc_probe.c
 */
#define __NR_exit   1
#define __NR_write  4
#define __NR_getpid 20

#define NR_MSGSND 301
#define NR_MSGRCV 302
#define NR_MSGGET 303
#define NR_MSGCTL 304
#define NR_MQ_OPEN 274
#define NR_MQ_UNLINK 275
#define NR_MQ_TIMEDSEND 276
#define NR_MQ_TIMEDRECV 277

#define IPC_CREAT  01000
#define IPC_EXCL   02000
#define IPC_NOWAIT 04000
#define MSG_COPY   0400000
#define O_RDWR     2

typedef unsigned char u8;
typedef unsigned int  u32;

static long sys3(long n, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r7) : "memory");
    return r0;
}
static long sys4(long n, long a, long b, long c, long d)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r3 asm("r3") = d;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r3), "r" (r7) : "memory");
    return r0;
}
static long sys5(long n, long a, long b, long c, long d, long e)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r3 asm("r3") = d;
    register long r4 asm("r4") = e;
    register long r7 asm("r7") = n;
    asm volatile("svc 0" : "+r" (r0) : "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r7) : "memory");
    return r0;
}
static int slen_(const char *s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void putint(long v)
{
    char b[24]; int i=0,neg=0;
    if(v<0){neg=1;v=-v;}
    if(!v)b[i++]='0';
    while(v>0){b[i++]='0'+(v%10);v/=10;}
    if(neg)sys3(__NR_write,1,(long)"-",1);
    for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}
}

struct msgbuf { long mtype; char mtext[16]; };
struct mq_attr { long mq_flags, mq_maxmsg, mq_msgsize, mq_curmsgs; };

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0, sp\n\tb probe_main\n");}
void probe_main(unsigned long *stackp)
{
    (void)stackp;
    puts_("=== Phase13 Round21 Task0: syscall availability ===\n");
    puts_("pid="); putint(sys3(__NR_getpid,0,0,0)); puts_("\n");

    puts_("[SysV] msgget(IPC_CREAT|0666) = ");
    long id = sys3(NR_MSGGET, 0x52423100, IPC_CREAT|0666, 0);
    putint(id); puts_("\n");
    if (id >= 0) {
        struct msgbuf sb; sb.mtype=1;
        for(int i=0;i<16;i++) sb.mtext[i]=(char)('A'+i);
        puts_("  msgsnd = "); putint(sys4(NR_MSGSND,id,(long)&sb,16,0)); puts_("\n");
        struct msgbuf rb;
        puts_("  msgrcv(MSG_COPY|IPC_NOWAIT) = ");
        putint(sys5(NR_MSGRCV,id,(long)&rb,16,1,MSG_COPY|IPC_NOWAIT)); puts_("\n");
        puts_("  msgctl(IPC_RMID) = "); putint(sys3(NR_MSGCTL,id,0,0)); puts_("\n");
    } else {
        puts_("  -> SysV IPC UNAVAILABLE (kernel compiled without CONFIG_SYSVIPC)\n");
    }

    puts_("[POSIX mq] mq_open(O_CREAT|O_RDWR,0600) = ");
    const char qname[] = "/r21probe";
    long mq = sys4(NR_MQ_OPEN,(long)qname,O_RDWR|IPC_CREAT,0600,0);
    putint(mq); puts_("\n");
    if (mq >= 0) {
        struct mq_attr attr;
        puts_("  mq_getattr via mq_open attr=NULL ok; mq_timedsend = ");
        putint(sys5(NR_MQ_TIMEDSEND,mq,(long)"HELLO",5,0,0)); puts_("\n");
        char rb[32]; long prio=0;
        puts_("  mq_timedreceive = ");
        putint(sys5(NR_MQ_TIMEDRECV,mq,(long)rb,32,(long)&prio,0)); puts_("\n");
        puts_("  mq_unlink = "); putint(sys3(NR_MQ_UNLINK,(long)qname,0,0)); puts_("\n");
    } else {
        puts_("  -> POSIX mqueue unavailable\n");
    }

    puts_("=== done ===\n");
    sys3(__NR_exit,0,0,0);
    for(;;){}
}
