/* throwaway: stand up the virtual Magic Trackpad with get/set-report callbacks
 * and log what AppleMultitouchHIDEventDriver asks for during start(). */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>

typedef struct __IOHIDUserDevice *IOHIDUserDeviceRef;
typedef enum { kReportInput = 0, kReportOutput, kReportFeature } ReportType;
typedef IOReturn (*ReportCB)(void *refcon, ReportType type, uint32_t reportID,
                             uint8_t *report, CFIndex reportLength);

extern IOHIDUserDeviceRef IOHIDUserDeviceCreate(CFAllocatorRef, CFDictionaryRef);
extern void IOHIDUserDeviceScheduleWithRunLoop(IOHIDUserDeviceRef, CFRunLoopRef, CFStringRef);
extern void IOHIDUserDeviceRegisterGetReportCallback(IOHIDUserDeviceRef, ReportCB, void *);
extern void IOHIDUserDeviceRegisterSetReportCallback(IOHIDUserDeviceRef, ReportCB, void *);

static const char *tn(ReportType t){ return t==kReportInput?"Input":t==kReportOutput?"Output":"Feature"; }

static IOReturn on_get(void *r, ReportType type, uint32_t rid, uint8_t *report, CFIndex len) {
    (void)r;
    fprintf(stderr, "GET_REPORT  type=%s id=0x%02x maxlen=%ld\n", tn(type), rid, (long)len);
    if (report && len > 0) for (CFIndex i=0;i<len;i++) report[i]=0;  /* answer with zeros */
    return kIOReturnSuccess;
}
static IOReturn on_set(void *r, ReportType type, uint32_t rid, uint8_t *report, CFIndex len) {
    (void)r;
    fprintf(stderr, "SET_REPORT  type=%s id=0x%02x len=%ld: ", tn(type), rid, (long)len);
    for (CFIndex i=0;i<len && i<32;i++) fprintf(stderr,"%02x ", report[i]);
    fprintf(stderr, "\n");
    return kIOReturnSuccess;
}

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
    CFDictionarySetValue(p,CFSTR("Manufacturer"),CFSTR("Apple Inc."));

    IOHIDUserDeviceRef d=IOHIDUserDeviceCreate(0,p); CFRelease(p);
    if(!d){fprintf(stderr,"create failed (root?)\n");return 1;}
    IOHIDUserDeviceRegisterGetReportCallback(d,on_get,0);
    IOHIDUserDeviceRegisterSetReportCallback(d,on_set,0);
    IOHIDUserDeviceScheduleWithRunLoop(d,CFRunLoopGetCurrent(),kCFRunLoopDefaultMode);
    fprintf(stderr,"virtual device up; logging driver queries. Ctrl-C to stop.\n");
    CFRunLoopRun();
    return 0;
}
