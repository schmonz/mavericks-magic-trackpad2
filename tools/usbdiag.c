/* throwaway: can we claim an MT2 USB interface away from the kernel HID driver,
 * send the multitouch enable, and read raw frames off the interrupt endpoint? */
#include <stdio.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>

#define VID 0x05ac
#define PID 0x0265

static IOUSBDeviceInterface500 **open_device(void) {
    CFMutableDictionaryRef m = IOServiceMatching(kIOUSBDeviceClassName);
    int vid=VID, pid=PID;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(0,kCFNumberIntType,&vid));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(0,kCFNumberIntType,&pid));
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, m);
    if (!svc) { printf("device not found\n"); return NULL; }
    IOCFPlugInInterface **plug; SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
        kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    IOUSBDeviceInterface500 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID500), (LPVOID*)&dev);
    (*plug)->Release(plug);
    return dev;
}

int main(void) {
    IOUSBDeviceInterface500 **dev = open_device();
    if (!dev) return 1;

    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    printf("USBDeviceOpenSeize = 0x%08x\n", r);

    IOUSBFindInterfaceRequest req;
    req.bInterfaceClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    req.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    req.bAlternateSetting = kIOUSBFindInterfaceDontCare;
    io_iterator_t it;
    (*dev)->CreateInterfaceIterator(dev, &req, &it);

    io_service_t usbIf;
    int idx = 0;
    while ((usbIf = IOIteratorNext(it))) {
        IOCFPlugInInterface **plug; SInt32 score;
        IOCreatePlugInInterfaceForService(usbIf, kIOUSBInterfaceUserClientTypeID,
            kIOCFPlugInInterfaceID, &plug, &score);
        IOObjectRelease(usbIf);
        if (!plug) { idx++; continue; }
        IOUSBInterfaceInterface500 **intf = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID500), (LPVOID*)&intf);
        (*plug)->Release(plug);
        if (!intf) { idx++; continue; }

        UInt8 cls=0, sub=0, proto=0, nep=0;
        (*intf)->GetInterfaceClass(intf, &cls);
        (*intf)->GetInterfaceSubClass(intf, &sub);
        (*intf)->GetInterfaceProtocol(intf, &proto);
        (*intf)->GetNumEndpoints(intf, &nep);
        IOReturn op = (*intf)->USBInterfaceOpenSeize(intf);
        printf("if%d class=%u sub=%u proto=%u neps=%u  OpenSeize=0x%08x\n",
               idx, cls, sub, proto, nep, op);

        if (op == kIOReturnSuccess) {
            for (UInt8 e = 1; e <= nep; e++) {
                UInt8 dir, num, tt, interval; UInt16 mps;
                (*intf)->GetPipeProperties(intf, e, &dir, &num, &tt, &mps, &interval);
                printf("    pipe%u dir=%u(%s) type=%u maxpkt=%u\n", e, dir,
                       dir==kUSBIn?"IN":"OUT", tt, mps);
                if (dir == kUSBIn && tt == kUSBInterrupt) {
                    /* send enable to this interface, then read the interrupt pipe */
                    IOUSBDevRequest en;
                    uint8_t payload[] = {0x02, 0x01};
                    en.bmRequestType = 0x21; en.bRequest = 0x09;
                    en.wValue = 0x0302; en.wIndex = idx; en.wLength = 2; en.pData = payload;
                    IOReturn sr = (*intf)->ControlRequest(intf, 0, &en);
                    printf("    enable SET_REPORT = 0x%08x; reading pipe%u (blocking) 40 times...\n", sr, e);
                    for (int k = 0; k < 40; k++) {
                        uint8_t buf[256]; UInt32 n = mps;   /* one interrupt transfer */
                        IOReturn rr = (*intf)->ReadPipe(intf, e, buf, &n);
                        if (rr == kIOReturnSuccess && n > 0) {
                            printf("      [len=%u] ", (unsigned)n);
                            for (UInt32 i=0;i<n && i<48;i++) printf("%02x ", buf[i]);
                            printf("\n");
                        } else {
                            printf("      read=0x%08x n=%u\n", rr, (unsigned)n);
                        }
                    }
                }
            }
            (*intf)->USBInterfaceClose(intf);
        }
        (*intf)->Release(intf);
        idx++;
    }
    IOObjectRelease(it);
    (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return 0;
}
