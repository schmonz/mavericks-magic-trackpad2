#include "mt2_name_report.h"

size_t mt2_build_name_report(uint8_t report_id, const uint8_t *name, size_t nl,
                             uint8_t *out, size_t out_cap) {
    size_t i;
    if (!out || out_cap == 0) return 0;
    /* Zero the WHOLE report first: the tail past the name must be defined zeros, not whatever the caller's
     * report buffer happened to hold. This is the leak fix -- see the header. */
    for (i = 0; i < out_cap; i++) out[i] = 0;
    out[0] = report_id;
    if (name && nl > 0) {
        if (nl > out_cap - 1) nl = out_cap - 1;   /* clamp so [id][name] fits */
        for (i = 0; i < nl; i++) out[1 + i] = name[i];
    }
    return out_cap;   /* the entire report is defined -- emit all of it */
}
