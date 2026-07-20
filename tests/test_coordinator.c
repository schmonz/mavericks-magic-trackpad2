#include "../src/mavericks_coordinator.h"
#include <stdio.h>

int main(void){
    if(!mavericks_coordinator_activate(MT2_XPORT_BT, 1)){ printf("FAIL bt\n"); return 1; }
    if(!mavericks_coordinator_activate(MT2_XPORT_USB, 1)){ printf("FAIL usb\n"); return 1; }
    printf("OK\n"); return 0;
}
