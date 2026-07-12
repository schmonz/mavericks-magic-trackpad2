#include "mt2_synth_report.h"
#include "mt2_geometry.h"
#include <string.h>

void mt2_synth_regs_init(mt2_synth_regs_t *r) { memset(r->reg, 0, sizeof(r->reg)); }

void mt2_synth_note_set(mt2_synth_regs_t *r, unsigned char reportId, unsigned char value) {
    r->reg[reportId] = value;
}

mt2_synth_rc_t mt2_synth_answer_report(const mt2_synth_regs_t *r, unsigned char reportId,
                                       unsigned char *out, unsigned int *outLen) {
    unsigned int n = 0;
    switch (mt2_fill_geometry_report(reportId, out, &n)) {
    case MT2_GEO_OK:
        *outLen = n;
        return MT2_SYNTH_OK;
    case MT2_GEO_UNSUPPORTED:            /* 0xDB Multitouch ID: driver skips */
        *outLen = 0;
        return MT2_SYNTH_SKIP;
    default:                             /* not geometry: echo the last SET value */
        out[0] = r->reg[reportId];
        *outLen = 1;
        return MT2_SYNTH_OK;
    }
}
