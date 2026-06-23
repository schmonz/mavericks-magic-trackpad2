#ifndef MT2_STACK_H
#define MT2_STACK_H
/* Canonical RE facts for driving the 10.9 multitouch stack (Magic Trackpad 2).
 *
 * SINGLE SOURCE OF TRUTH. The kext builds with the constants tagged [BUILD]; reference-only facts
 * [REF] (the geometry GET call chain, struct sub-fields) are documented here too so every number
 * lives in exactly one place. Each carries the `re/` command that re-derives it from the live
 * binaries — run it to verify a fact after a point-release. Prose lives in docs/mt-stack/
 * (explanation = the model, reference = these facts in tables, how-to = recipes).
 *
 * All values RE'd from the 10.9 binaries; verified on-device 2026-06-22 unless noted UNVERIFIED. */

/* ---- transport vtable slots (BluetoothMultitouchTransport / BNBTrackpadDevice) ---------------- *
 * re/vtable AppleBluetoothMultitouch BNBTrackpadDevice <slot>                                     */
#define MT2_VT_getMultitouchReport       0xcc8  /* [BUILD] geometry DATA fetch                     */
#define MT2_VT_getMultitouchReportInfo   0xcd8  /* [BUILD] geometry LENGTH probe — runs FIRST in   */
                                                /*   _deviceGetReportWithLookUp; if it fails the   */
                                                /*   data fetch (0xcc8) is never reached.          */
#define MT2_VT_createMultitouchHandler   0xd08  /* [REF] spawns the AMD. UNVERIFIED this session   */
                                                /*   (from prior RE); re-confirm before relying.   */

/* ---- AMD vtable slots (AppleMultitouchDevice) — [REF], the geometry GET call chain ------------ *
 * re/vtable AppleMultitouchDriver AppleMultitouchDevice <slot>                                    *
 *   _cacheDeviceProperties -> getReport(0x880) -> _deviceGetReportWithLookUp(0x8e8)               *
 *   -> _getFeatureReportInfo(0x8f8) -> handler obj (AMD+0xa8) -> transport 0xcd8 then 0xcc8       */
#define MT2_AMD_setGetReportHandler      0x858
#define MT2_AMD_setReportInfoHandler     0x868
#define MT2_AMD_getReport                0x880
#define MT2_AMD_deviceGetReportWithLookUp 0x8e8
#define MT2_AMD_getFeatureReportInfo     0x8f8

/* ---- struct field offsets -------------------------------------------------------------------- */
#define MT2_OFF_BNB_AMD                  0x1b0  /* [BUILD] BNBDevice multitouch handler            */
                                               /*   (AppleMultitouchDevice*), set by              */
                                               /*   startMultitouchThreaded. re/xref-offset.      */
#define MT2_OFF_BNB_INTERRUPT_CHANNEL    0xf0   /* [BUILD] BNBDevice::_interruptChannel            */
#define MT2_OFF_L2CAP_DELEGATE_CB        0x110  /* [BUILD] IOBluetoothL2CAPChannel delegate cb     */
                                               /*   fn-ptr; target is at +0x118. re/xref-offset   */
                                               /*   on newDataIn.                                 */
#define MT2_OFF_AMD_HANDLER_OBJ          0xa8   /* [REF] AMD getReport handler object; layout      */
                                               /*   {report fn @+0x0, refcon +0x8,                */
                                               /*    reportinfo fn @+0x20, refcon +0x28}.         */
/* [REF] AMD device-button gate ("S+9"): this+0xb0 struct, byte +9. AMD::start sets it when it     */
/*       reads getProperty(MT2_PROP_EXTRACT_BUTTON)==true; without it handlePointerEventFromDevice */
/*       posts are dropped. re/disasm AppleMultitouchDriver <AMD::start>.                          */

/* ---- report ids ------------------------------------------------------------------------------ */
#define MT2_REPORT_ID_MT2                0x31   /* MT2 multitouch report (see src/mt2_decode.c)    */
#define MT2_REPORT_ID_MT1                0x28   /* MT1 multitouch report (see src/mt1_encode.c)    */
/* geometry D-report ids answered from src/mt2_geometry.c: 0xd1 0xd3 0xd0 0xa1 0xd9 0x7f           */

/* ---- properties ------------------------------------------------------------------------------ */
/* [BUILD] set in DefaultMultitouchProperties -> copied to the AMD by createMultitouchHandler ->   */
/*         AMD::start sets the S+9 device-button gate (physical + two-finger right click).         */
#define MT2_PROP_EXTRACT_BUTTON          "ExtractAndPostDeviceButtonState"

#endif /* MT2_STACK_H */
