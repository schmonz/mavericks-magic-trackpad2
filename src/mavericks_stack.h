#ifndef MAVERICKS_STACK_H
#define MAVERICKS_STACK_H
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
#define MT2_VT_newTransportString        0x868  /* [BUILD] IOBluetoothHIDDriver::newTransportString;*/
                                                /*   multitouchProperties() sets it as the AMD's    */
                                                /*   "Transport" property. Override -> non-BT to    */
                                                /*   skip the isBlocked edge-reserve (transport==4). */
                                                /*   NOTE slot-collides with AMD setReportInfoHandler*/
                                                /*   (0x868 on the AMD vtable) — different object.   */

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
                                               /*   fn-ptr; the target is the ADJACENT word       */
                                               /*   +0x118 (= cb + sizeof(void*)) — which the      */
                                               /*   mavericks_splice MEM_SLOT engine saves/restores as  */
                                               /*   "slot+8". re/xref-offset on newDataIn.        */
#define MT2_OFF_AMD_HANDLER_OBJ          0xa8   /* [REF] AMD getReport handler object; layout      */
                                               /*   {report fn @+0x0, refcon +0x8,                */
                                               /*    reportinfo fn @+0x20, refcon +0x28}.         */
/* [REF] AMD device-button gate ("S+9"): this+0xb0 struct, byte +9. AMD::start sets it when it     */
/*       reads getProperty(MT2_PROP_EXTRACT_BUTTON)==true; without it handlePointerEventFromDevice */
/*       posts are dropped. re/disasm AppleMultitouchDriver <AMD::start>.                          */

/* ---- BT connect handshake (genuine IOBluetoothHIDDriver, 10.9) — [REF] until the flap fix ----- *
 * The genuine driver accepts the control channel in a specific order; PSM 19 is DEVICE-INITIATED    *
 * (host sends nothing to provoke it). Reproducing this order on OUR PSM-17 reader is the eventual    *
 * flap fix (see docs/mt-stack/how-to.md "fix the connect flap"). Values from 2026-06-21 RE;          *
 * re-verify before building the fix. re/vtable IOBluetoothFamily IOBluetoothL2CAPChannel <slot>.     */
#define MT2_PSM_INTERRUPT                0x13   /* PSM 19 interrupt channel (control = PSM 17 = 0x11) */
#define MT2_L2CAP_VT_listenAt            0xa50  /* IOBluetoothL2CAPChannel::listenAt(IOService*, cb)  */
#define MT2_L2CAP_VT_waitForChannelState 0xa20  /* ::waitForChannelState(state); arg 4 = OPEN         */
#define MT2_L2CAP_STATE_OPEN             4
#define MT2_HIDP_SET_PROTOCOL            0x70   /* SET_PROTOCOL transaction; |bit (0x70 boot/0x71     */
                                               /*   report); subclass INVERTS the bit for 05AC:0309. */
                                               /*   Sent in deviceReady AFTER both channels OPEN.    */

/* ---- BT device enable / mode / handler-create trigger ---------------------------------------- */
#define MT2_HIDP_SET_REPORT_FEATURE      0x53   /* [BUILD] (SET_REPORT<<4)|FEATURE — raw-L2CAP HIDP */
#define MT2_HIDP_GET_REPORT_INPUT        0x41   /* [BUILD] (GET_REPORT<<4)|INPUT — poll report over  */
                                               /*   the control channel; response returns as        */
                                               /*   0xA1(DATA|INPUT) id .. on the same channel.      */
#define MT2_BATTERY_REPORT_ID            0x90   /* [BUILD] Apple Power-Device INPUT report:           */
                                               /*   [0x90][flags][capacity 0-100]. See               */
                                               /*   docs/mt-stack/battery-reporting.md.              */
#define MT2_ENABLE_REPORT_ID             0xf1   /* [BUILD] MT2 enable feature report: 0xF1 0x02 0x01;*/
                                               /*   BNB bring-up resets to mouse mode after, so we  */
                                               /*   re-send it ~8x once both channels are OPEN.     */
#define MT2_REPORT_ID_MOUSE              0x02   /* [REF] device mode mouse (vs MT2 0x31 multitouch)  */
#define MT2_TRIGGER_REPORT_ID            0x60   /* [BUILD] 0xA1 0x60 0x02 -> createMultitouchHandler */
#define MT2_BNB_WATCHDOG_MS              5000   /* [REF] BNBDevice handleStart 5s (0x1388) "Forcing  */
                                               /*   MT restart" kickstart; cancelled by real MT data*/

/* ---- report ids ------------------------------------------------------------------------------ */
#define MT2_REPORT_ID_MT2                0x31   /* MT2 multitouch report (see src/mt2_decode.c)    */
#define MT2_REPORT_ID_MT1                0x28   /* MT1 multitouch report (see src/mt1_encode.c)    */
/* geometry D-report ids answered from src/mt2_geometry.c: 0xd1 0xd3 0xd0 0xa1 0xd9 0x7f           */

/* ---- multitouch property seeds --------------------------------------------------------------- *
 * Seeded into the genuine Apple driver by the readers (usb_build_init_props / bt_build_bnb_props). *
 * The paths drive DIFFERENT Apple drivers, so some divergence is genuine, not drift — see each     *
 * builder head + decisions.md "genuine-reuse tax". SHARED values live here so they can't drift;    *
 * GENUINE per-transport values are named separately and must NOT be unified.                       */
#define MT2_PARSER_TYPE                  1000   /* [BUILD] selects the CompactV4 contact parser (both)*/
/* [BUILD] parser-options is GENUINE PER-TRANSPORT — each matches Apple's OWN value for that driver;  *
 * do NOT unify. Only bit 0x2 (clicky gate: 2-finger secondary + 3FD) is feature-relevant, and it is  *
 * set in BOTH. reference.md "parser-options bit 0x2".                                                */
#define MT2_PARSER_OPTIONS_BT            0x2F   /* 47: BT's genuine value                             */
#define MT2_PARSER_OPTIONS_USB           0x27  /* 39: Apple's genuine USB-personality value          */
#define MT2_MTHID_PLUGIN_GUID            "0516B563-B15B-11DA-96EB-0014519758EF" /* IOCFPlugInTypes key */
#define MT2_MTHID_PLUGIN_PATH            "AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin"
/* [BUILD] shared property keys, seeded identically on both transports (one spelling, no drift).    */
#define MT2_PROP_MTHID_DEVICE            "MTHIDDevice"
#define MT2_PROP_HID_SERVICE_SUPPORT     "HIDServiceSupport"
#define MT2_PROP_MOMENTUM_SCROLL         "TrackpadMomentumScroll"
#define MT2_PROP_SECONDARY_CLICK_CORNERS "TrackpadSecondaryClickCorners"
#define MT2_PROP_FOUR_FINGER_GESTURES    "TrackpadFourFingerGestures"
/* [BUILD] set in DefaultMultitouchProperties -> copied to the AMD by createMultitouchHandler ->    *
 *         AMD::start sets the S+9 device-button gate (physical + two-finger right click).          */
#define MT2_PROP_EXTRACT_BUTTON          "ExtractAndPostDeviceButtonState"

#endif /* MAVERICKS_STACK_H */
