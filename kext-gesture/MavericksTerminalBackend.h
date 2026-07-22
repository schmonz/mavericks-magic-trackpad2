#ifndef MAVERICKS_TERMINAL_BACKEND_H
#define MAVERICKS_TERMINAL_BACKEND_H
#include <libkern/c++/OSObject.h>
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include "mavericks_session.h"        // mavericks_session_t
#include "MavericksAMDTerminal.h"      // mavericks_amd_terminal_ctx + transport enum + feed/button
#include "voodoo_wire.h"              // VoodooInputEvent
#include "../third_party/VoodooInput/VoodooInputTerminal.hpp"
class IOWorkLoop; class IOTimerEventSource;

/* The 10.9 terminal backend: everything that turns VoodooInputEvents into device output on Mavericks —
 * the shared session/conditioning engine + the fabricated-AMD terminal + their workloop/timer/lock/sink
 * wiring. Owned by the mux (a thin router) with an explicit start/stop lifecycle the mux drives. An
 * OSObject so it can be the owner of its own IOTimerEventSource (and mirrors upstream, whose terminal is
 * also an OSObject). */
class MavericksTerminalBackend : public VoodooInputTerminal {
    OSDeclareDefaultStructors(MavericksTerminalBackend)
public:
    virtual bool start(IOService *mux, IOService *provider) override;
    virtual void handleEvent(const VoodooInputEvent *ev) override;
    virtual void updateDimensions(uint32_t logicalMaxX, uint32_t logicalMaxY) override;
    virtual void stop(IOService *nub) override;

    /* Publish a battery capacity (0-100) on our fabricated AMD node as "BatteryPercent" — the terminal
     * owns that node, so the reader hands us a parsed value instead of reaching the node itself. Self-
     * fences (no-op until the AMD is built / once teardown starts) and dedups repeat values. Honors the
     * debug.mt2_batt override. */
    void publishBattery(uint8_t pct);

    uint32_t logicalMaxX() const { return fLogicalMaxX; }   // mux reads these as the update-message default
    uint32_t logicalMaxY() const { return fLogicalMaxY; }
    mavericks_amd_terminal_ctx *synthCtx() const { return fSynth; }   // sink-trampoline glue
    void armIdle(uint32_t ms);                                        // sink-trampoline glue
private:
    static void idleTick(OSObject *owner, IOTimerEventSource *sender);
    IOService          *fProvider;   /* the satellite; we self-register on it for the battery bridge */
    uint32_t            fLogicalMaxX;
    uint32_t            fLogicalMaxY;
    mavericks_amd_terminal_ctx *fSynth;
    mavericks_session_t fSession;
    IOWorkLoop         *fWL;
    IOTimerEventSource *fIdle;
    IOLock             *fLock;
    int                 fLastBattPct;   /* publishBattery dedup (-1 = none published yet) */
};
#endif
