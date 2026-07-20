#include "mavericks_synth_report.h"
#include "mt2_geometry.h"
#include <string.h>

void mavericks_synth_regs_init(mavericks_synth_regs_t *r) { memset(r->reg, 0, sizeof(r->reg)); }

void mavericks_synth_note_set(mavericks_synth_regs_t *r, unsigned char reportId, unsigned char value) {
    r->reg[reportId] = value;
}

mavericks_synth_rc_t mavericks_synth_answer_report(const mavericks_synth_regs_t *r, unsigned char reportId,
                                       unsigned char *out, unsigned int *outLen) {
    unsigned int n = 0;
    switch (mt2_fill_geometry_report(reportId, out, &n)) {
    case MT2_GEO_OK:
        *outLen = n;
        return MAVERICKS_SYNTH_OK;
    case MT2_GEO_UNSUPPORTED:            /* 0xDB Multitouch ID: driver skips */
        *outLen = 0;
        return MAVERICKS_SYNTH_SKIP;
    default:                             /* not geometry: echo the last SET value */
        out[0] = r->reg[reportId];
        *outLen = 1;
        return MAVERICKS_SYNTH_OK;
    }
}
