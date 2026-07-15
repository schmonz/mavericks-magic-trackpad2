#ifndef MT2_SYNTH_AMD_H
#define MT2_SYNTH_AMD_H
#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include "amd_shim.h"           // AppleMultitouchDevice, AMDDeviceReportStruct
#include "../src/mt2_frame.h"   // mt2_frame

/* One independent fabricated AppleMultitouchDevice (+ its MT2HIDShell + echo-register state),
 * built under `nub` and adopted by hidd. Multiple instances coexist (each mux owns one). Opaque. */
typedef struct mt2_synth_amd_ctx mt2_synth_amd_ctx;

mt2_synth_amd_ctx     *mt2_synth_amd_build(IOService *nub);              /* NULL on failure */
AppleMultitouchDevice *mt2_synth_amd_amd(mt2_synth_amd_ctx *ctx);       /* the started AMD, or NULL */
void                   mt2_synth_amd_teardown(IOService *nub, mt2_synth_amd_ctx *ctx);

/* The fabricated-AMD "terminal" feed half (build+teardown are above). Each resolves the AMD via
 * mt2_synth_amd_amd(ctx) (NULL until built / during teardown -> self-fencing no-op). One home for the
 * mt1_encode -> handleTouchFrame logic that MT2BTReader/MT2USBReader/VoodooInputMux all used to copy.
 * timestamp: caller-supplied frame clock (BT: uptime ns->ms; USB: system_microtime 22-bit ms);
 * keeping it as a parameter preserves byte-identical timestamps across transports. */
void     mt2_synth_amd_feed(mt2_synth_amd_ctx *ctx, const mt2_frame *frame, uint32_t timestamp);
void     mt2_synth_amd_button(mt2_synth_amd_ctx *ctx, unsigned mask);
IOReturn mt2_synth_amd_inject(mt2_synth_amd_ctx *ctx, const unsigned char *bytes, unsigned int len);
#endif
