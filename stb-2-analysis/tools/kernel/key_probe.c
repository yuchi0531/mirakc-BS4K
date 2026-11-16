/*
 * key_probe.c -- Phase 14 Round 22 Task 0 follow-up.
 * Test whether shell can obtain a writable keyring (add_key spray primitive).
 * Bare-metal armv7a static ELF, raw syscalls. add_key=309 keyctl=311.
 */
#define __NR_exit 1
#define __NR_write 4
#define __NR_getpid 20
#define __NR_add_key 309
#define __NR_keyctl 311

#define KEY_SPEC_THREAD_KEYRING   (-1)
#define KEY_SPEC_PROCESS_KEYRING  (-2)
#define KEY_SPEC_SESSION_KEYRING  (-3)
#define KEY_SPEC_USER_KEYRING     (-4)
#define KEY_SPEC_USER_SESSION_KEYRING (-5)
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_DESCRIBE 6
#define KEYCTL_READ 11

typedef unsigned char u8;
typedef unsigned int u32;
static void zero_(void*p,unsigned n){volatile u8*q=p;while(n--)*q++=0;}
static int slen_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}return n;}
static long sys3(long n,long a,long b,long c)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory");return r0;}
static long sys5(long n,long a,long b,long c,long d,long e)
{register long r0 asm("r0")=a,r1 asm("r1")=b,r2 asm("r2")=c,r3 asm("r3")=d,r4 asm("r4")=e,r7 asm("r7")=n;asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r3),"r"(r4),"r"(r7):"memory");return r0;}
static void puts_(const char*s){sys3(__NR_write,1,(long)s,slen_(s));}
static void putint(long v)
{char b[24];int i=0,neg=0;if(v<0){neg=1;v=-v;}if(!v)b[i++]='0';while(v>0){b[i++]='0'+(v%10);v/=10;}
 if(neg)sys3(__NR_write,1,(long)"-",1);for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}}

static long keyctl(long cmd,long a,long b,long c,long d)
{return sys5(__NR_keyctl,cmd,a,b,c,d);}
static long add_key(const char*t,const char*desc,const void*pl,u32 plen,long ring)
{return sys5(__NR_add_key,(long)t,(long)desc,(long)pl,plen,ring);}

static u8 payload[128];
static u8 rb[256];
static char desc[160];

static void try_add(const char*label,long ring)
{
    zero_(desc,sizeof(desc));
    for(int i=0;i<120;i++)desc[i]=(char)('a'+(i%26));
    for(int i=0;i<128;i++)payload[i]=(u8)(0x41+(i%26));
    puts_("  add_key(");puts_(label);puts_(") ring=");putint(ring);puts_(" = ");
    long kid=add_key("user",desc,payload,128,ring);
    putint(kid);puts_("\n");
    if(kid>=0){
        zero_(rb,sizeof(rb));
        long rr=keyctl(KEYCTL_READ,kid,(long)rb,128,0);
        puts_("    READ=");putint(rr);puts_(" byte0=");putint(rb[0]);puts_("\n");
        zero_(rb,sizeof(rb));
        long dr=keyctl(KEYCTL_DESCRIBE,kid,(long)rb,255,0);
        puts_("    DESCRIBE=");putint(dr);puts_("\n");
    }
}

__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb kmain\n");}
void kmain(void)
{
    puts_("=== key_probe: can shell get a writable keyring? ===\n");
    puts_("pid=");putint(sys3(__NR_getpid,0,0,0));puts_("\n");

    puts_("JOIN_SESSION_KEYRING('r22fresh') = ");
    long j=keyctl(KEYCTL_JOIN_SESSION_KEYRING,(long)"r22fresh",0,0,0);
    putint(j);puts_("\n");

    long rings[5];
    rings[0]=KEY_SPEC_PROCESS_KEYRING;
    rings[1]=KEY_SPEC_SESSION_KEYRING;
    rings[2]=KEY_SPEC_USER_KEYRING;
    rings[3]=KEY_SPEC_USER_SESSION_KEYRING;
    rings[4]=KEY_SPEC_THREAD_KEYRING;
    const char*labels[5]={"PROCESS","SESSION","USER","USER_SESSION","THREAD"};
    for(int i=0;i<5;i++){
        puts_("  GET_KEYRING_ID(");puts_(labels[i]);puts_(")=");
        long id=keyctl(KEYCTL_GET_KEYRING_ID,rings[i],0,0,0);
        putint(id);puts_("\n");
    }
    for(int i=0;i<5;i++) try_add(labels[i],rings[i]);

    puts_("TRY add_key(keyring, 'r22own') = ");
    long kr=add_key("keyring","r22own",0,0,KEY_SPEC_SESSION_KEYRING);
    putint(kr);puts_("\n");
    if(kr>=0) try_add("OWN_KEYRING",kr);

    puts_("=== done ===\n");sys3(__NR_exit,0,0,0);for(;;){}
}
