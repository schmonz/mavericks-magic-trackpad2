#ifndef MT2_SYNTH_AMD_H
#define MT2_SYNTH_AMD_H
#include <IOKit/IOService.h>
#include "amd_shim.h"   // AppleMultitouchDevice, AMDDeviceReportStruct

/* Build a fabricated AppleMultitouchDevice (+ MT2HIDShell) attached under `nub`, adopted by
 * hidd, ready to receive MT1 0x28 frames via handleTouchFrame. Returns the started AMD (retained;
 * caller stops+releases via mt2_synth_amd_teardown) or NULL on failure. */
AppleMultitouchDevice *mt2_synth_amd_build(IOService *nub);
void mt2_synth_amd_teardown(IOService *nub, AppleMultitouchDevice *amd);
#endif
