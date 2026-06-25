#include "../src/genuine_host.h"
#include <string.h>
#include <stdio.h>

/* Mock adapter: appends one letter per op to g_log; failure of each op is forced via g_force.
 * letters: a=alloc i=init_attach c=class_ok p=interpose s=start  R=restore D=detach T=terminate L=release */
static char g_log[64]; static int g_n;
static struct { bool class_ok, init_attach, start; int interpose; } g_force;
static void rec(char c){ g_log[g_n++]=c; g_log[g_n]=0; }
static void *m_alloc(void*c,const char*s){(void)c;(void)s;rec('a');return (void*)0x1;}
static bool  m_class_ok(void*c,void*o,const char*e){(void)c;(void)o;(void)e;rec('c');return g_force.class_ok;}
static bool  m_init_attach(void*c,void*o){(void)c;(void)o;rec('i');return g_force.init_attach;}
static int   m_interpose(void*c,void*o){(void)c;(void)o;rec('p');return g_force.interpose;}
static bool  m_start(void*c,void*o){(void)c;(void)o;rec('s');return g_force.start;}
static void  m_restore(void*c,void*o){(void)c;(void)o;rec('R');}
static void  m_detach(void*c,void*o){(void)c;(void)o;rec('D');}
static void  m_terminate(void*c,void*o){(void)c;(void)o;rec('T');}
static void  m_release(void*c,void*o){(void)c;(void)o;rec('L');}
static const gh_adapter_t MOCK = { m_alloc,m_class_ok,m_init_attach,m_interpose,m_start,
                                   m_restore,m_detach,m_terminate,m_release };
static const gh_config_t CFG = { "DriverClass", "DriverClass" };
static void reset(void){ g_n=0; g_log[0]=0; g_force.class_ok=true; g_force.init_attach=true;
                         g_force.start=true; g_force.interpose=0; }
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); return 1; } }while(0)

static int test_happy(void){
    reset(); gh_host_t h={GH_IDLE,0};
    CHECK(gh_start(&h,&CFG,&MOCK,0)==0); CHECK(h.state==GH_STARTED);
    CHECK(strcmp(g_log,"aicps")==0);
    reset(); gh_stop(&h,&MOCK,0); CHECK(h.state==GH_IDLE); CHECK(strcmp(g_log,"RTL")==0);
    reset(); gh_stop(&h,&MOCK,0); CHECK(strcmp(g_log,"")==0);          /* idempotent */
    return 0;
}
static int test_init_fail(void){
    reset(); g_force.init_attach=false; gh_host_t h={GH_IDLE,0};
    CHECK(gh_start(&h,&CFG,&MOCK,0)!=0); CHECK(h.state==GH_IDLE); CHECK(strcmp(g_log,"aiL")==0);
    return 0;
}
static int test_class_fail(void){
    reset(); g_force.class_ok=false; gh_host_t h={GH_IDLE,0};
    CHECK(gh_start(&h,&CFG,&MOCK,0)!=0); CHECK(h.state==GH_IDLE); CHECK(strcmp(g_log,"aicDL")==0);
    return 0;
}
static int test_interpose_fail(void){
    reset(); g_force.interpose=-1; gh_host_t h={GH_IDLE,0};
    CHECK(gh_start(&h,&CFG,&MOCK,0)!=0); CHECK(h.state==GH_IDLE); CHECK(strcmp(g_log,"aicpDL")==0);
    return 0;
}
static int test_start_fail(void){
    reset(); g_force.start=false; gh_host_t h={GH_IDLE,0};
    CHECK(gh_start(&h,&CFG,&MOCK,0)!=0); CHECK(h.state==GH_IDLE); CHECK(strcmp(g_log,"aicpsRDL")==0);
    return 0;
}
int main(void){
    if(test_happy()||test_init_fail()||test_class_fail()||test_interpose_fail()||test_start_fail()) return 1;
    printf("OK\n"); return 0;
}
