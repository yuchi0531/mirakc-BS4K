#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <signal.h>

static const char *entries[] = {
 "astm","audio","audio_dap","audio_graph","audio_output","audio_resources","audio_volume",
 "bhdm_edid","bhdm_packet_acr","bhdm_packet_audio","bhdm_packet_avi","bhdm_packet_drm","bhdm_packet_vsi",
 "cec","config","core","debug","display","dtu","gpio","graphics2d","hdmi_output","kni","mma",
 "platform","sage","simple_audio_playback","simple_decoder","status","stream_mux","surface",
 "surface_compositor","sync_channel","tee","transport","video_decoder","video_encoder"
};
#define NENTRIES (sizeof(entries)/sizeof(entries[0]))
#define OUT "/data/local/tmp/phase24b"

static long now_ms(void){
  struct timeval tv; gettimeofday(&tv,0);
  return tv.tv_sec*1000L + tv.tv_usec/1000;
}

int main(void){
  char cmd[512];
  snprintf(cmd,sizeof cmd,"mkdir -p %s",OUT); system(cmd);
  snprintf(cmd,sizeof cmd,"rm -f %s/*",OUT); system(cmd);

  for(int it=1; it<=10; it++){
    long t0 = now_ms();
    for(unsigned e=0; e<NENTRIES; e++){
      char path[256], opath[320];
      snprintf(path,sizeof path,"/proc/brcm/%s",entries[e]);
      snprintf(opath,sizeof opath,"%s/s%d-%s",OUT,it,entries[e]);
      pid_t pid = fork();
      if(pid==0){
        struct itimerval itv;
        itv.it_value.tv_sec=0; itv.it_value.tv_usec=300000;
        itv.it_interval.tv_sec=0; itv.it_interval.tv_usec=0;
        setitimer(ITIMER_REAL,&itv,0); /* 300ms safety: kill blocking reads */
        int fd = open(path,O_RDONLY);
        if(fd<0) _exit(1);
        int ofd = open(opath,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(ofd<0){ close(fd); _exit(2); }
        static char buf[65536];
        ssize_t n;
        while((n=read(fd,buf,sizeof buf))>0) write(ofd,buf,n);
        close(fd); close(ofd); _exit(0);
      }
      int st; waitpid(pid,&st,0);
    }
    /* timestamp after capture */
    char tpath[64]; snprintf(tpath,sizeof tpath,"%s/s%d-TIME",OUT,it);
    FILE *tf=fopen(tpath,"w"); if(tf){ fprintf(tf,"%ld\n",now_ms()); fclose(tf); }
    long elapsed = now_ms()-t0;
    fprintf(stderr,"iter %d capture=%ldms\n",it,elapsed);
    long remain = 500 - elapsed;
    if(remain>0) usleep(remain*1000);
  }
  fprintf(stderr,"DONE\n");
  return 0;
}
