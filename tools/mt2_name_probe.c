// mt2_name_probe — READ-ONLY probe for the on-device name of an Apple Bluetooth
// HID accessory (Magic Trackpad 2 & friends). It reassembles the name the exact
// way -[AppleBluetoothHIDDevice deviceNameFromHardware] does, so we can SEE the
// real per-report framing (header/terminator bytes) before writing anything.
//
// What it does, per matched HID device:
//   1. dumps the HID report descriptor,
//   2. finds every Feature report ID in that descriptor,
//   3. GET_REPORTs each Feature report and prints raw hex + ASCII,
//   4. prints the decoded-rule payload  buf[1 .. len-2]  per report, and a
//      concatenation of the printable ones — the candidate device name.
//
// It never issues SET_REPORT. Nothing on the device changes.
//
// Build (on the 10.9 target):
//   clang -o tools/mt2_name_probe tools/mt2_name_probe.c -framework IOKit -framework CoreFoundation
//   (or just:  tools/re mt2-name )
// Run:
//   tools/mt2_name_probe            # auto-match Apple / trackpad devices
//   tools/mt2_name_probe -a         # probe every HID device
//   tools/mt2_name_probe 05ac 0265  # match one VendorID ProductID (hex)
// If a report reads back empty/errors unprivileged, retry with sudo.

#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int prop_int(IOHIDDeviceRef d, CFStringRef key) {
  CFTypeRef v = IOHIDDeviceGetProperty(d, key);
  int out = 0;
  if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberIntType, &out);
  return out;
}

static void prop_str(IOHIDDeviceRef d, CFStringRef key, char *buf, size_t n) {
  buf[0] = 0;
  CFTypeRef v = IOHIDDeviceGetProperty(d, key);
  if (v && CFGetTypeID(v) == CFStringGetTypeID())
    CFStringGetCString((CFStringRef)v, buf, (CFIndex)n, kCFStringEncodingUTF8);
}

// Print a byte window as "hex  |ascii|", marking non-printables as '.'.
static void dump_bytes(const uint8_t *b, CFIndex n) {
  printf("    hex :");
  for (CFIndex i = 0; i < n; i++) printf(" %02x", b[i]);
  printf("\n    ascii: \"");
  for (CFIndex i = 0; i < n; i++) putchar(isprint(b[i]) ? b[i] : '.');
  printf("\"\n");
}

// Count printable bytes in a window (to guess "this report holds text").
static int printable_run(const uint8_t *b, CFIndex n) {
  int p = 0;
  for (CFIndex i = 0; i < n; i++) if (isprint(b[i])) p++;
  return p;
}

// Walk a HID report descriptor and collect distinct report IDs that carry a
// Feature main item. Minimal item parser: prefix byte encodes bTag/bType/bSize.
static int collect_feature_report_ids(const uint8_t *d, CFIndex n, uint8_t *ids, int max) {
  int count = 0, curReport = 0;
  CFIndex i = 0;
  while (i < n) {
    uint8_t p = d[i++];
    if (p == 0xFE) {                       // long item: [0xFE][dataSize][tag][data...]
      if (i < n) { uint8_t dsz = d[i]; i += (CFIndex)2 + dsz; }
      continue;
    }
    int bSize = p & 0x3; int len = (bSize == 3) ? 4 : bSize;
    int bType = (p >> 2) & 0x3;
    int bTag  = (p >> 4) & 0xF;
    uint32_t data = 0;
    for (int k = 0; k < len && (i + k) < n; k++) data |= (uint32_t)d[i + k] << (8 * k);
    if (bType == 1 && bTag == 0x8) curReport = data & 0xff;   // Global: Report ID
    if (bType == 0 && bTag == 0xB) {                          // Main: Feature
      int seen = 0;
      for (int j = 0; j < count; j++) if (ids[j] == (uint8_t)curReport) seen = 1;
      if (!seen && count < max) ids[count++] = (uint8_t)curReport;
    }
    i += len;
  }
  return count;
}

static void probe_device(IOHIDDeviceRef dev) {
  char product[256]; prop_str(dev, CFSTR(kIOHIDProductKey), product, sizeof product);
  int vid = prop_int(dev, CFSTR(kIOHIDVendorIDKey));
  int pid = prop_int(dev, CFSTR(kIOHIDProductIDKey));
  char transport[64]; prop_str(dev, CFSTR(kIOHIDTransportKey), transport, sizeof transport);
  printf("\n=== %s  (VID 0x%04x  PID 0x%04x  %s) ===\n",
         product[0] ? product : "(unnamed)", vid, pid, transport[0] ? transport : "?");

  if (IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
    printf("  ! could not open device (try: sudo tools/mt2_name_probe). skipping.\n");
    return;
  }

  CFTypeRef dd = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDReportDescriptorKey));
  uint8_t feat[64]; int nfeat = 0;
  if (dd && CFGetTypeID(dd) == CFDataGetTypeID()) {
    const uint8_t *db = CFDataGetBytePtr((CFDataRef)dd);
    CFIndex dn = CFDataGetLength((CFDataRef)dd);
    printf("  report descriptor (%ld bytes):\n   ", (long)dn);
    for (CFIndex i = 0; i < dn; i++) { printf(" %02x", db[i]); if ((i & 15) == 15) printf("\n   "); }
    printf("\n");
    nfeat = collect_feature_report_ids(db, dn, feat, (int)sizeof feat);
  } else {
    printf("  ! no report descriptor property; will scan Feature IDs 1..63\n");
    for (int r = 1; r < 64; r++) feat[nfeat++] = (uint8_t)r;
  }

  printf("  Feature report IDs to probe: ");
  for (int j = 0; j < nfeat; j++) printf("%d ", feat[j]);
  printf("\n");

  // Buffer sized to the device's max feature report (fallback 256).
  int maxlen = prop_int(dev, CFSTR(kIOHIDMaxFeatureReportSizeKey));
  if (maxlen <= 0 || maxlen > 4096) maxlen = 256;

  // Accumulate the decoded-rule payloads of the text-looking reports, in the
  // order probed, to show the reassembled name candidate.
  char name[1024]; size_t namelen = 0;

  for (int j = 0; j < nfeat; j++) {
    uint8_t buf[4096];
    CFIndex len = maxlen;
    memset(buf, 0, sizeof buf);
    IOReturn r = IOHIDDeviceGetReport(dev, kIOHIDReportTypeFeature, (CFIndex)feat[j], buf, &len);
    if (r != kIOReturnSuccess) {
      // Only note failures for descriptor-declared reports; scan noise stays quiet.
      if (dd) printf("  report 0x%02x: GET_REPORT error 0x%08x\n", feat[j], r);
      continue;
    }
    printf("  Feature report 0x%02x  len=%ld\n", feat[j], (long)len);
    dump_bytes(buf, len);
    if (len >= 3) {
      // The decoded read rule: skip 1-byte header buf[0], take len-2 bytes.
      CFIndex plen = len - 2;
      printf("    payload[1..len-2] (%ld): \"", (long)plen);
      for (CFIndex i = 1; i <= plen; i++) putchar(isprint(buf[i]) ? buf[i] : '.');
      printf("\"\n");
      // If it looks like text, append to the reassembly candidate.
      if (printable_run(buf + 1, plen) >= (plen + 1) / 2) {
        for (CFIndex i = 1; i <= plen && namelen < sizeof name - 1; i++)
          if (buf[i]) name[namelen++] = buf[i];
      }
    }
  }
  name[namelen] = 0;
  if (namelen) printf("  >> reassembled name candidate: \"%s\"\n", name);
  else         printf("  >> no text-looking Feature reports found\n");

  IOHIDDeviceClose(dev, kIOHIDOptionsTypeNone);
}

static int matches(IOHIDDeviceRef d, int want_all, int want_vid, int want_pid) {
  if (want_all) return 1;
  int vid = prop_int(d, CFSTR(kIOHIDVendorIDKey));
  int pid = prop_int(d, CFSTR(kIOHIDProductIDKey));
  if (want_vid) return (vid == want_vid) && (want_pid < 0 || pid == want_pid);
  if (vid == 0x05ac) return 1;                       // Apple
  char product[256]; prop_str(d, CFSTR(kIOHIDProductKey), product, sizeof product);
  return strcasestr(product, "trackpad") || strcasestr(product, "magic");
}

int main(int argc, char **argv) {
  int want_all = 0, want_vid = 0, want_pid = -1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) want_all = 1;
    else if (!want_vid)        want_vid = (int)strtol(argv[i], NULL, 16);
    else                       want_pid = (int)strtol(argv[i], NULL, 16);
  }

  IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
  IOHIDManagerSetDeviceMatching(mgr, NULL);          // all HID devices
  if (IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
    fprintf(stderr, "could not open IOHIDManager\n");
    return 1;
  }
  CFSetRef set = IOHIDManagerCopyDevices(mgr);
  if (!set) { fprintf(stderr, "no HID devices\n"); return 1; }

  CFIndex n = CFSetGetCount(set);
  IOHIDDeviceRef *devs = calloc(n, sizeof *devs);
  CFSetGetValues(set, (const void **)devs);

  int probed = 0;
  for (CFIndex i = 0; i < n; i++) {
    if (matches(devs[i], want_all, want_vid, want_pid)) { probe_device(devs[i]); probed++; }
  }
  if (!probed)
    fprintf(stderr, "\nno matching HID device (try -a, or pass VID PID in hex).\n");

  free(devs);
  CFRelease(set);
  CFRelease(mgr);
  return 0;
}
