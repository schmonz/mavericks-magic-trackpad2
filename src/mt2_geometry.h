#ifndef MT2_GEOMETRY_H
#define MT2_GEOMETRY_H
/* Pure, transport-neutral source of the Magic Trackpad 2 sensor-geometry D-report
 * payloads. Both the fDevice get-report handler (kext) and the full-BNB transport
 * override answer the recognizer's geometry queries from here, so the constants live
 * in exactly one place. Byte layouts reversed from
 * AppleMultitouchDevice::decodeDeviceProperty (see the RE findings). */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MT2_GEO_OK            = 0,  /* filled out[0..*outLen) for a geometry report id   */
    MT2_GEO_UNSUPPORTED   = 1,  /* a known id the driver must SKIP (0xDB Multitouch ID) */
    MT2_GEO_NOT_GEOMETRY  = 2   /* not a geometry id; caller decides what to do      */
};

/* reportId: the MT D-report id. out: caller buffer (>= 16 bytes). outLen: set to the
 * payload length on MT2_GEO_OK. Returns one of the enum values above. */
int mt2_fill_geometry_report(unsigned char reportId, unsigned char *out, unsigned int *outLen);

#ifdef __cplusplus
}
#endif
#endif
