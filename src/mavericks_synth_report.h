#ifndef MAVERICKS_SYNTH_REPORT_H
#define MAVERICKS_SYNTH_REPORT_H
/* Pure decision logic for the fabricated AMD's get-report handler: answer geometry from
 * mt2_fill_geometry_report, else echo hidd's last SET value per reportID. No IOKit. */
typedef enum { MAVERICKS_SYNTH_OK = 0, MAVERICKS_SYNTH_SKIP = 1 } mavericks_synth_rc_t;

typedef struct { unsigned char reg[256]; } mavericks_synth_regs_t;

/* C linkage: the kext compiles this .c as C++ (-x c++) but MavericksAMDTerminal.cpp references these
 * under extern "C" — without this guard the kext defines mangled names and kxld fails to resolve
 * the unmangled references at LOAD time (host tests, compiled as plain C, don't expose it). */
#ifdef __cplusplus
extern "C" {
#endif
void mavericks_synth_regs_init(mavericks_synth_regs_t *r);              /* zero the echo table */
void mavericks_synth_note_set(mavericks_synth_regs_t *r, unsigned char reportId, unsigned char value);
/* Fill `out` with the report answer for `reportId`, length in *outLen. Returns MAVERICKS_SYNTH_SKIP
 * for reports the driver must skip (0xDB), MAVERICKS_SYNTH_OK otherwise. */
mavericks_synth_rc_t mavericks_synth_answer_report(const mavericks_synth_regs_t *r, unsigned char reportId,
                                       unsigned char *out, unsigned int *outLen);
#ifdef __cplusplus
}
#endif
#endif
