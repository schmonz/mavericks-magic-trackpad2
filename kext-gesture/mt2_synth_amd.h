#ifndef MT2_SYNTH_AMD_H
#define MT2_SYNTH_AMD_H
#include <IOKit/IOService.h>
#include "amd_shim.h"   // AppleMultitouchDevice, AMDDeviceReportStruct

/* One independent fabricated AppleMultitouchDevice (+ its MT2HIDShell + echo-register state),
 * built under `nub` and adopted by hidd. Multiple instances coexist (each mux owns one). Opaque. */
typedef struct mt2_synth_amd_ctx mt2_synth_amd_ctx;

mt2_synth_amd_ctx     *mt2_synth_amd_build(IOService *nub);              /* NULL on failure */
AppleMultitouchDevice *mt2_synth_amd_amd(mt2_synth_amd_ctx *ctx);       /* the started AMD, or NULL */
void                   mt2_synth_amd_teardown(IOService *nub, mt2_synth_amd_ctx *ctx);
#endif
