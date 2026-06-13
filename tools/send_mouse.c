/* throwaway: create the virtual device and inject relative-mouse reports (0x02)
 * to prove HandleReport reaches the cursor via IOHIDPointing. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <unistd.h>
#include <stdio.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef, CFDictionaryRef);
extern IOReturn IOHIDUserDeviceHandleReport(IOHIDUserDeviceRef, const uint8_t *, CFIndex);

static const uint8_t kMT1Desc[] = {
    0x05,0x01,0x09,0x02,0xa1,0x01, 0x85,0x02, 0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x03, 0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x05,0x81,0x03, 0x05,0x01,0x09,0x30,0x09,0x31,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x02,0x81,0x06, 0xc0,
    0x06,0x00,0xff, 0x85,0x28, 0x09,0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0xff,0x81,0x02, 0xc0
};
static void pnum(CFMutableDictionaryRef d, CFStringRef k, int v){
    CFNumberRef n=CFNumberCreate(0,kCFNumberIntType,&v); CFDictionarySetValue(d,k,n); CFRelease(n);
}
int main(void){
    CFMutableDictionaryRef p=CFDictionaryCreateMutable(0,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFDataRef desc=CFDataCreate(0,kMT1Desc,sizeof(kMT1Desc));
    CFDictionarySetValue(p,CFSTR("ReportDescriptor"),desc); CFRelease(desc);
    pnum(p,CFSTR("VendorID"),1452); pnum(p,CFSTR("ProductID"),782); pnum(p,CFSTR("VendorIDSource"),2);
    pnum(p,CFSTR("PrimaryUsagePage"),1); pnum(p,CFSTR("PrimaryUsage"),2);
    CFDictionarySetValue(p,CFSTR("Transport"),CFSTR("Bluetooth"));
    CFDictionarySetValue(p,CFSTR("Product"),CFSTR("Magic Trackpad"));
    IOHIDUserDeviceRef d=IOHIDUserDeviceCreate(0,p); CFRelease(p);
    if(!d){fprintf(stderr,"create failed\n");return 1;}
    fprintf(stderr,"injecting 60 relative-mouse reports (cursor should drift down-right)...\n");
    sleep(1);  /* let the driver attach */
    for(int i=0;i<60;i++){
        uint8_t rep[4] = { 0x02, 0x00, 0x06, 0x06 };  /* id, buttons, dX=+6, dY=+6 */
        IOReturn r = IOHIDUserDeviceHandleReport(d, rep, sizeof(rep));
        if(i==0) fprintf(stderr,"first HandleReport = 0x%08x\n", r);
        usleep(50000);
    }
    fprintf(stderr,"done\n");
    return 0;
}
