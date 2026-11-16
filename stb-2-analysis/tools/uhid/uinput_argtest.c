/* tiny arg-style test for UI_SET_EVBIT: pointer vs direct value */
#define __NR_exit 1
#define __NR_write 4
#define __NR_close 6
#define __NR_ioctl 54
#define __NR_openat 322
#define AT_FDCWD (-100)
#define O_RDWR 2
typedef unsigned char u8; typedef unsigned int u32;
static long sys3(long n,long a,long b,long c){
  register long r0 asm("r0")=a; register long r1 asm("r1")=b;
  register long r2 asm("r2")=c; register long r7 asm("r7")=n;
  asm volatile("svc 0":"+r"(r0):"r"(r1),"r"(r2),"r"(r7):"memory"); return r0;
}
static void puts_(const char*s){volatile const char*p=s;int n=0;while(*p){p++;n++;}sys3(__NR_write,1,(long)s,n);}
static void putint(long v){char b[24];int i=0;if(!v)b[i++]='0';while(v>0){b[i++]='0'+(v%10);v/=10;}for(int j=i-1;j>=0;j--){char c=b[j];sys3(__NR_write,1,(long)&c,1);}}
void __aeabi_memcpy(void*d,const void*s,unsigned n){volatile u8*x=d;volatile const u8*y=s;while(n--)*x++=*y++;}
void __aeabi_memmove(void*d,const void*s,unsigned n){__aeabi_memcpy(d,s,n);}
void __aeabi_memset(void*d,int c,unsigned n){volatile u8*x=d;while(n--)*x++=(u8)c;}
void __aeabi_memclr(void*d,unsigned n){__aeabi_memset(d,0,n);}
__attribute__((naked)) void _start(void){__asm__ volatile("mov r0,sp\n\tb argtest_main\n");}
void argtest_main(unsigned long*sp){
  (void)sp;
  long fd=sys3(__NR_openat,AT_FDCWD,(long)"/dev/uinput",O_RDWR);
  puts_("fd=");putint(fd);puts_("\n");
  int ev=1;
  long a=sys3(__NR_ioctl,fd,(long)0x40045564,(long)&ev);
  puts_("EVBIT via POINTER ret=");putint(a);puts_("\n");
  long b=sys3(__NR_ioctl,fd,(long)0x40045564,(long)1);
  puts_("EVBIT via DIRECT val ret=");putint(b);puts_("\n");
  int kb=30;
  long c=sys3(__NR_ioctl,fd,(long)0x40045565,(long)&kb);
  puts_("KEYBIT via POINTER ret=");putint(c);puts_("\n");
  sys3(__NR_close,fd,0,0);
  sys3(__NR_exit,0,0,0);for(;;){}
}
