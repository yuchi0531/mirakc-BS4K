/* PHASE27 harness: bipc cmd enumeration context + AuthenticatedJoin auth matrix.
 * JNI entry Java_Nx_load(long variant):
 *   v = 0..11   : authMode=v%3 (at +0x30), pwfile=(v/3)%2, env=(v/6)
 *   v = 200..211: same matrix but authMode written at +0x88 instead of +0x30
 *   v = 100     : dump libnxclient .bss static auth template @ +0xb4f0, AuthJoin(template)
 *   v = 101     : AuthJoin with zeroed NEXUS_Platform_GetDefaultClientAuthenticationSettings buffer
 * Build: clang --target=armv7a-linux-androideabi21 -shared -nostdlib -fPIC -O1 \
 *          -Wl,--allow-shlib-undefined -L libs -l:libdl.so -l:libc.so -o libnxjni.so nxphase27.c
 */
typedef void *JNIEnv_t;
typedef void *jclass_t;
typedef long jlong_t;
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern int printf(const char *, ...);
extern long write(int, const void *, unsigned long);
static void mark(const char *s) { int n = 0; while (s[n]) n++; write(1, s, n); write(1, "\n", 1); }
extern int open(const char *, int, ...);
extern int ioctl(int, int, ...);
extern int close(int);
extern int *__errno(void);
extern int setenv(const char *, const char *, int);
extern int unsetenv(const char *);
extern char *getenv(const char *);
extern void *fopen(const char *, const char *);
extern unsigned long fwrite(const void *, unsigned long, unsigned long, void *);
extern int fclose(void *);
extern void *memset(void *, int, unsigned int);
extern void *memcpy(void *, const void *, unsigned int);
#define JNIEXPORT __attribute__((visibility("default")))
#define JNICALL

static char settings[4096];
static char cgbuf[4096];
static char cfg[65536];
static char auth[2048];

static void dump(const char *tag, void *p, int n) {
    unsigned char *b = (unsigned char *)p;
    printf("%s (%d bytes):\n", tag, n);
    for (int i = 0; i < n; i++) {
        printf("%02x ", b[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n");
}
static int nonzero(void *p, int n) {
    unsigned char *b = (unsigned char *)p;
    int c = 0;
    for (int i = 0; i < n; i++) if (b[i]) c++;
    return c;
}

JNIEXPORT jlong_t JNICALL Java_Nx_load(JNIEnv_t *env, jclass_t c, jlong_t v) {
    printf("=== nxphase27 variant=%ld ===\n", v);
    setenv("NEXUS_DEVICE_NODE", "/dev/nxmw", 1);
    void *h = dlopen("/vendor/lib/libnxclient.so", 2);
    printf("dlopen libnxclient=%p err=%s\n", h, dlerror() ? dlerror() : "(none)");
    if (!h) return 0;

    long (*bkinit)(void *) = (long (*)(void *))dlsym(h, "BKNI_Init");
    long (*gdef)(void *) = (long (*)(void *))dlsym(h, "NxClient_GetDefaultJoinSettings");
    long (*join)(void *) = (long (*)(void *))dlsym(h, "NxClient_Join");
    long (*cgjs)(void *, void *) = (long (*)(void *, void *))dlsym(h, "NxClient_Config_GetJoinSettings");
    long (*gc)(void *) = (long (*)(void *))dlsym(h, "NEXUS_Platform_GetConfiguration");
    printf("syms init=%p gdef=%p join=%p cgjs=%p gc=%p\n", bkinit, gdef, join, cgjs, gc);
    if (bkinit) printf("BKNI_Init rc=0x%lx\n", bkinit(0));

    void *hn = dlopen("/vendor/lib/libnexus.so", 2);
    long (*gdefn)(void *) = hn ? (long (*)(void *))dlsym(hn, "NEXUS_Platform_GetDefaultClientAuthenticationSettings") : 0;
    long (*ajoin)(void *) = hn ? (long (*)(void *))dlsym(hn, "NEXUS_Platform_AuthenticatedJoin") : 0;
    printf("libnexus=%p gdefn=%p ajoin=%p\n", hn, gdefn, ajoin);

    /* --- v=400: proxy ioctl forwarding probe (read-only getter + one known transport ioctl) --- */
    if (v == 400 || v == 401) {
        mark("P27-M1 enter v400/401");
        static char pbuf[8192];
        int fd = open("/dev/nxmw", 0x802);            /* O_RDWR|O_NONBLOCK */
        mark("P27-M2 opened");
        char name[32];
        memset(name, 0, 32);
        const char *nm = "nxclient";
        for (int i = 0; i < 8; i++) name[i + 4] = nm[i];
        int cr = ioctl(fd, 0x40014202, name);          /* CONNECT */
        mark("P27-M3 connect done");
        printf("proxy probe: fd=%d CONNECT rc=%d errno=%d\n", fd, cr, *__errno());
        unsigned char *nb = (unsigned char *)gdefn - 0x641b10;
        int *pproxyfd = (int *)(nb + 0x78e4e4);
        int *pglobalfd = (int *)(nb + 0x78e830);
        printf("libnexus base=%p proxy_fd_before=%d global_fd_before=%d\n", nb, *pproxyfd, *pglobalfd);
        *pproxyfd = fd;
        if (v == 400) { printf("v400 stop before proxy calls\n"); close(fd); *pproxyfd = -1; printf("=== nxphase27 done ===\n"); return (jlong_t)(long)h; }
        long (*pgc)(void *, void *) = hn ? (long (*)(void *, void *))dlsym(hn, "NEXUS_GetPlatformCapabilities_tagged_proxy") : 0;
        printf("pgc=%p\n", pgc);
        mark("P27-M4 before pgc");
        if (pgc) {
            memset(pbuf, 0, sizeof(pbuf));
            long r = pgc(pbuf, 0);
            printf("GetPlatformCapabilities_proxy rc=0x%lx nonzero8192=%d first64:\n", r, nonzero(pbuf, 64));
            for (int i = 0; i < 64; i++) { printf("%02x ", (unsigned char)pbuf[i]); if ((i & 15) == 15) printf("\n"); }
            printf("\n");
        }
        mark("P27-M5 after pgc");
        /* single known transport ioctl via same fd (probes whether 0x65xxxxxx is forwarded) */
        static char treq[64];
        memset(treq, 0, sizeof(treq));
        int rr = ioctl(fd, 0x6538a0, treq);
        printf("ioctl(fd,0x6538a0) rc=%d errno=%d\n", rr, *__errno());
        int rr2 = ioctl(fd, 0x650002, treq);
        printf("ioctl(fd,0x650002) rc=%d errno=%d\n", rr2, *__errno());
        close(fd);
        *pproxyfd = -1;
        printf("=== nxphase27 done ===\n");
        return (jlong_t)(long)h;
    }

    /* --- special probes 100/101: auth struct only, no join --- */
    if (v == 100 || v == 101) {
        memset(auth, 0, sizeof(auth));
        if (gdefn) { long r = gdefn(auth); printf("GetDefaultClientAuth rc=0x%lx\n", r); }
        dump("GETTER_AUTH_260", auth, 260);
        if (v == 101) {
            printf("AuthJoin(getter_zeroed) rc=0x%lx\n", ajoin ? ajoin(auth) : -1);
        } else {
            /* static template NxClient_Join uses: libnxclient vaddr 0xb4f0, 260B.
             * base from dlsym address of NxClient_Join (vaddr 0x65f4). */
            unsigned char *base = (unsigned char *)join - 0x65f4;
            printf("libnxclient base=%p template=%p\n", base, base + 0xb4f0);
            memcpy(auth, base + 0xb4f0, 260);
            dump("STATIC_TEMPLATE_260", auth, 260);
            printf("AuthJoin(static_template) rc=0x%lx\n", ajoin ? ajoin(auth) : -1);
        }
        printf("=== nxphase27 done ===\n");
        return (jlong_t)(long)h;
    }

    /* --- matrix variants --- */
    long idx = v;
    int mode = 0, pw = 0, envset = 0, off = 0x30, setname = 0;
    if (v >= 200) { idx = v - 200; off = 0x88; }
    if (v >= 300) { idx = v - 300; off = 0x30; setname = 1; }
    mode = (int)(idx % 3);
    pw = (int)((idx / 3) % 2);
    envset = (int)(idx / 6);
    printf("matrix: authMode=%d off=0x%x pwfile=%d envset=%d\n", mode, off, pw, envset);

    if (pw) {
        void *fp = fopen("/data/local/tmp/nxp27pw", "wb");
        if (fp) { const char *p = "trusted:testpw\n"; fwrite(p, 1, 15, fp); fclose(fp); }
        printf("pwfile written\n");
    }
    if (envset) setenv("NXCLIENT_PASSWORD", "/data/local/tmp/nxp27pw", 1);
    else unsetenv("NXCLIENT_PASSWORD");
    printf("env NXCLIENT_PASSWORD=%s\n", getenv("NXCLIENT_PASSWORD") ? getenv("NXCLIENT_PASSWORD") : "(unset)");

    memset(settings, 0, sizeof(settings));
    printf("GetDefaultJoinSettings rc=0x%lx\n", gdef ? gdef(settings) : -1);
    printf("defaults +0x30=%d +0x34=[%s] +0x88=%d\n",
           *(int *)(settings + 0x30), settings + 0x34, *(int *)(settings + 0x88));
    *(int *)(settings + off) = mode;   /* force requested authMode at chosen offset */
    printf("forced settings[0x%x]=%d\n", off, mode);
    if (setname) { const char *nm = "nxprobe"; for (int i = 0; i < 8; i++) settings[i] = nm[i]; }

    printf("NxClient_Join rc=0x%lx\n", join ? join(settings) : -1);

    /* internal config object: pointer at libnxclient base+0xb5fc; word1 is bipc target handle */
    unsigned char *base = (unsigned char *)join - 0x65f4;
    void **cfgobj = (void **)(base + 0xb5fc);
    printf("cfgobj@0xb5fc=%p\n", (void *)*cfgobj);
    if (*cfgobj) dump("CFGLINK_OBJ64", *cfgobj, 64);

    /* Config_GetJoinSettings: sweep first-arg values (request payload word0). */
    for (int fa = 0; fa < 8; fa++) {
        memset(cgbuf, 0, sizeof(cgbuf));
        long cr = cgjs ? cgjs((void *)(long)fa, cgbuf) : -1;
        printf("CGJS firstArg=%d rc=0x%lx nonzero312=%d name=[%.32s] auth@0x30=%d auth@0x88=%d\n",
               fa, cr, nonzero(cgbuf, 312), cgbuf,
               *(int *)(cgbuf + 0x30), *(int *)(cgbuf + 0x88));
        if (nonzero(cgbuf, 312) > 2 || cr != 0) dump("CGJS_BUF_312", cgbuf, 312);
        if (cr == 0 && cgbuf[0] != 0) {
            printf("*** CGJS rc=0 + sane name with firstArg=%d ***\n", fa);
            memset(cfg, 0, sizeof(cfg));
            if (gc) gc(cfg);
            printf("Platform_GetConfiguration cfg nonzero=%d first16:\n", nonzero(cfg, 128));
            for (int i = 0; i < 16; i++) printf("%02x ", (unsigned char)cfg[i]);
            printf("\n");
            void *(*recopen)(long, void *) = (void *(*)(long, void *))dlsym(h, "NEXUS_Recpump_Open");
            void (*recclose)(void *) = (void (*)(void *))dlsym(h, "NEXUS_Recpump_Close");
            if (recopen && recclose) {
                void *rec = recopen(0, 0);
                printf("Recpump_Open(0)=%p (probe only)\n", rec);
                if (rec) recclose(rec);
            }
            break;
        }
    }

    /* direct AuthenticatedJoin with zeroed getter struct, for reference */
    if (gdefn && ajoin) {
        memset(auth, 0, sizeof(auth));
        gdefn(auth);
        printf("AuthJoin(getter_zeroed) rc=0x%lx\n", ajoin(auth));
    }
    printf("=== nxphase27 done ===\n");
    return (jlong_t)(long)h;
}
JNIEXPORT jlong_t JNICALL Java_Nx_capture(JNIEnv_t *e, jclass_t c, jlong_t p, jlong_t s) { return 0; }
