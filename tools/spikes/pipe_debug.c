/* throwaway: verbose end-to-end pipeline. Confirms frames arrive, decode
 * succeeds, and IOHIDUserDeviceHandleReport accepts the MT1 reports. */
#include "../src/mt2_reader.h"
#include "../src/mt2_usb_decode.h"
#include "../src/mt1_encode.h"
#include "../src/vhid_mt1.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <signal.h>

static vhid_t *v;
static long frames=0, decoded=0, sent_ok=0, sent_fail=0;

static void on_frame(const uint8_t *f, size_t len, void *ctx){
    (void)ctx; frames++;
    touch_frame_t tf={0};
    if (mt2_usb_decode(f,len,&tf)!=0) return;
    decoded++;
    uint8_t out[256];
    int n=mt1_encode(&tf,out,sizeof(out));
    if(n<=0) return;
    int rc=vhid_send(v,out,(size_t)n);
    if(rc==0) sent_ok++; else sent_fail++;
    if(frames%30==1){
        fprintf(stderr,"frames=%ld decoded=%ld send_ok=%ld send_fail=%ld | "
                "nt=%d t0(id=%d x=%d y=%d st=%d) MT1[%d]: ",
                frames,decoded,sent_ok,sent_fail,tf.ntouches,
                tf.ntouches?tf.touches[0].id:-1,
                tf.ntouches?tf.touches[0].x:0, tf.ntouches?tf.touches[0].y:0,
                tf.ntouches?tf.touches[0].state:-1, n);
        for(int i=0;i<n && i<16;i++) fprintf(stderr,"%02x ",out[i]);
        fprintf(stderr,"\n");
    }
}
static void sig(int s){(void)s; mt2_reader_stop(); _exit(0);}
int main(void){
    signal(SIGINT,sig); signal(SIGTERM,sig);
    v=vhid_create(); if(!v){fprintf(stderr,"vhid_create failed\n");return 1;}
    if(mt2_reader_start(on_frame,NULL)!=0){fprintf(stderr,"reader start failed\n");return 1;}
    fprintf(stderr,"pipe_debug running; touch the pad\n");
    CFRunLoopRun(); return 0;
}
