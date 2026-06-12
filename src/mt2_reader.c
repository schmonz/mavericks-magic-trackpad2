#include "mt2_reader.h"
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>

#define MT2_VID 0x05ac
#define MT2_PID 0x0265
#define MT2_MULTITOUCH_INTERFACE 1   /* bInterfaceNumber carrying touch frames */

static IOUSBDeviceInterface500 **g_dev;
static IOUSBInterfaceInterface500 **g_intf;
static UInt8 g_pipe;            /* interrupt-IN pipe ref */
static UInt16 g_mps;            /* max packet size */
static pthread_t g_thread;
static volatile int g_run;
static mt2_frame_cb g_cb;
static void *g_ctx;

static IOUSBDeviceInterface500 **open_device(void) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = MT2_VID, pid = MT2_PID;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(0, kCFNumberIntType, &vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(0, kCFNumberIntType, &pid));
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, m);
    if (!svc) return NULL;
    IOCFPlugInInterface **plug; SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (!plug) return NULL;
    IOUSBDeviceInterface500 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
    (*plug)->Release(plug);
    return dev;
}

/* Find interface #1 and the interrupt-IN pipe on it. */
static IOUSBInterfaceInterface500 **open_interface(void) {
    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    io_iterator_t it;
    if ((*g_dev)->CreateInterfaceIterator(g_dev, &req, &it) != kIOReturnSuccess) return NULL;

    io_service_t usbIf;
    IOUSBInterfaceInterface500 **found = NULL;
    while ((usbIf = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug; SInt32 score;
        IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID,
            kIOCFPlugInInterfaceID, &plug, &score);
        IOObjectRelease(usbIf);
        if (!plug) continue;
        IOUSBInterfaceInterface500 **intf = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
        (*plug)->Release(plug);
        if (!intf) continue;

        UInt8 ifnum = 255;
        (*intf)->GetInterfaceNumber(intf, &ifnum);
        if (ifnum != MT2_MULTITOUCH_INTERFACE) { (*intf)->Release(intf); continue; }

        if ((*intf)->USBInterfaceOpenSeize(intf) != kIOReturnSuccess) {
            (*intf)->Release(intf); found = NULL; break;
        }
        UInt8 nep = 0; (*intf)->GetNumEndpoints(intf, &nep);
        for (UInt8 e = 1; e <= nep; e++) {
            UInt8 dir, num, tt, interval; UInt16 mps;
            (*intf)->GetPipeProperties(intf, e, &dir, &num, &tt, &mps, &interval);
            if (dir == kUSBIn && tt == kUSBInterrupt) { g_pipe = e; g_mps = mps; }
        }
        found = intf;
        break;
    }
    IOObjectRelease(it);
    return found;
}

static void *read_loop(void *arg) {
    (void)arg;
    uint8_t buf[256];
    while (g_run) {
        UInt32 n = g_mps;                       /* one interrupt transfer */
        IOReturn r = (*g_intf)->ReadPipe(g_intf, g_pipe, buf, &n);
        if (r == kIOReturnSuccess) {
            if (n > 0 && g_cb) g_cb(buf, (size_t)n, g_ctx);
        } else if (r == kIOReturnAborted) {
            break;                              /* stop() aborted the pipe */
        } else {
            /* transient (e.g. stall) -> clear and continue; fatal -> bail */
            (*g_intf)->ClearPipeStallBothEnds(g_intf, g_pipe);
        }
    }
    return NULL;
}

int mt2_reader_start(mt2_frame_cb cb, void *ctx) {
    g_cb = cb; g_ctx = ctx;
    g_dev = open_device();
    if (!g_dev) { fprintf(stderr, "mt2_reader: device not found\n"); return -1; }
    if ((*g_dev)->USBDeviceOpenSeize(g_dev) != kIOReturnSuccess) {
        fprintf(stderr, "mt2_reader: cannot open device\n"); return -1;
    }
    g_intf = open_interface();
    if (!g_intf) {
        fprintf(stderr, "mt2_reader: cannot claim interface %d "
                "(is the MT2Claim kext loaded?)\n", MT2_MULTITOUCH_INTERFACE);
        return -1;
    }
    /* enable multitouch: SET_REPORT feature, report id 0x02, payload {0x02,0x01} */
    IOUSBDevRequest en;
    uint8_t payload[] = {0x02, 0x01};
    en.bmRequestType = 0x21; en.bRequest = 0x09;
    en.wValue = 0x0302; en.wIndex = MT2_MULTITOUCH_INTERFACE;
    en.wLength = sizeof(payload); en.pData = payload;
    (*g_intf)->ControlRequest(g_intf, 0, &en);

    g_run = 1;
    if (pthread_create(&g_thread, NULL, read_loop, NULL) != 0) {
        fprintf(stderr, "mt2_reader: pthread_create failed\n");
        g_run = 0; return -1;
    }
    return 0;
}

void mt2_reader_stop(void) {
    if (!g_run) return;
    g_run = 0;
    if (g_intf) (*g_intf)->AbortPipe(g_intf, g_pipe);   /* unblock ReadPipe */
    pthread_join(g_thread, NULL);
    if (g_intf) { (*g_intf)->USBInterfaceClose(g_intf); (*g_intf)->Release(g_intf); g_intf = NULL; }
    if (g_dev)  { (*g_dev)->USBDeviceClose(g_dev); (*g_dev)->Release(g_dev); g_dev = NULL; }
}
