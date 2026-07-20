#ifndef MAVERICKS_AMD_TERMINAL_H
#define MAVERICKS_AMD_TERMINAL_H
#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include "amd_shim.h"           // AppleMultitouchDevice, AMDDeviceReportStruct
#include "../src/mavericks_frame.h"   // MavericksTouchFrame

/* One independent fabricated AppleMultitouchDevice (+ its MavericksHIDShell + echo-register state),
 * built under `nub` and adopted by hidd. Multiple instances coexist (each mux owns one). Opaque. */
typedef struct mavericks_amd_terminal_ctx mavericks_amd_terminal_ctx;

/* Which transport the fabricated AMD advertises — the pane / MultitouchSupport read the "Transport"
 * property and show transport-matching chrome (e.g. battery is BT-only). USB must NOT claim Bluetooth. */
typedef enum { MAVERICKS_AMD_TERMINAL_XPORT_BT = 0, MAVERICKS_AMD_TERMINAL_XPORT_USB = 1 } mavericks_amd_terminal_transport_t;

mavericks_amd_terminal_ctx     *mavericks_amd_terminal_build(IOService *nub, mavericks_amd_terminal_transport_t transport);  /* NULL on failure */
AppleMultitouchDevice *mavericks_amd_terminal_amd(mavericks_amd_terminal_ctx *ctx);       /* the started AMD, or NULL */
void                   mavericks_amd_terminal_teardown(IOService *nub, mavericks_amd_terminal_ctx *ctx);

/* The fabricated-AMD "terminal" feed half (build+teardown are above). Each resolves the AMD via
 * mavericks_amd_terminal_amd(ctx) (NULL until built / during teardown -> self-fencing no-op). One home for the
 * mavericks_amd_construct_report -> handleTouchFrame logic that MT2BTReader/MT2USBReader/VoodooInputMux all used to copy.
 * timestamp: caller-supplied frame clock (BT: uptime ns->ms; USB: system_microtime 22-bit ms);
 * keeping it as a parameter preserves byte-identical timestamps across transports. */
void     mavericks_amd_terminal_feed(mavericks_amd_terminal_ctx *ctx, const MavericksTouchFrame *frame, uint32_t timestamp);
void     mavericks_amd_terminal_button(mavericks_amd_terminal_ctx *ctx, unsigned mask);
IOReturn mavericks_amd_terminal_inject(mavericks_amd_terminal_ctx *ctx, const unsigned char *bytes, unsigned int len);
#endif
