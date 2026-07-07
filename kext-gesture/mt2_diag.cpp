#include "mt2_diag.h"
#include "mt2_log.h"                   /* MT2_DLOG (runtime debug.mt2_log) */
#include <kern/clock.h>                /* clock_get_system_microtime */

/* Resume marker only after a >=60s lull (the long-idle wake edge); the frame-by-frame level-2 edge is
 * what captures a short-window bug (screensaver engages within ~1-2s of the desktop). */
#define MT2_DIAG_GAP_MS  60000ULL

static const char *const kDiagName[MT2_DIAG_NXPORT] = { "BT", "USB" };

/* Per-transport stream state. Distinct slots per transport (see header: single-transport-at-a-time). */
static struct {
    uint8_t  seen[32];       /* report ids logged-once bitmap */
    uint64_t last_ms;        /* last activity time; 0 = fresh stream (no resume line yet) */
    bool     first_logged;   /* first-frame marker gate */
} gDiag[MT2_DIAG_NXPORT];

static uint64_t diag_now_ms(void) {
    clock_sec_t s; clock_usec_t u;
    clock_get_system_microtime(&s, &u);
    return (uint64_t)s * 1000 + u / 1000;   /* full monotonic ms so a multi-minute gap measures right */
}

void mt2_diag_reset(mt2_diag_xport_t x) {
    if ((unsigned)x >= MT2_DIAG_NXPORT) return;
    for (unsigned i = 0; i < sizeof(gDiag[x].seen); i++) gDiag[x].seen[i] = 0;
    gDiag[x].last_ms = 0;
    gDiag[x].first_logged = false;
}

void mt2_diag_saw_id(mt2_diag_xport_t x, uint8_t id) {
    if ((unsigned)x >= MT2_DIAG_NXPORT) return;
    if (gDiag[x].seen[id >> 3] & (uint8_t)(1u << (id & 7))) return;
    gDiag[x].seen[id >> 3] |= (uint8_t)(1u << (id & 7));
    MT2_DLOG(1, "%s shim saw report id 0x%02x", kDiagName[x], (unsigned)id);
}

void mt2_diag_raw(mt2_diag_xport_t x, uint8_t id) {
    if ((unsigned)x >= MT2_DIAG_NXPORT) return;
    mt2_diag_saw_id(x, id);
    uint64_t now = diag_now_ms();
    if (gDiag[x].last_ms != 0 && now - gDiag[x].last_ms >= MT2_DIAG_GAP_MS)
        MT2_DLOG(1, "%s frames resumed after %llu ms gap (id 0x%02x)",
                 kDiagName[x], (unsigned long long)(now - gDiag[x].last_ms), (unsigned)id);
    gDiag[x].last_ms = now;
}

void mt2_diag_frame(mt2_diag_xport_t x, const VoodooInputEvent *frame, bool want_first) {
    if ((unsigned)x >= MT2_DIAG_NXPORT || !frame) return;
    const char *name = kDiagName[x];
    if (want_first && !gDiag[x].first_logged) { gDiag[x].first_logged = true;
        MT2_DLOG(1, "%s steady: first frame streamed end-to-end", name); }
    uint32_t ts = (uint32_t)(diag_now_ms() & 0x3FFFFF);   /* 22-bit, matches the readers' packet clock */
    if (frame->contact_count > 0)
        MT2_DLOG(2, "%s edge n=%d x=%d y=%d ts=%u", name, frame->contact_count,
                 frame->transducers[0].currentCoordinates.x,
                 frame->transducers[0].currentCoordinates.y, ts);
    else
        MT2_DLOG(2, "%s edge n=0 (lift) ts=%u", name, ts);
}
