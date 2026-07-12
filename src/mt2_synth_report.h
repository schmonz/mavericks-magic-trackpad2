#ifndef MT2_SYNTH_REPORT_H
#define MT2_SYNTH_REPORT_H
/* Pure decision logic for the fabricated AMD's get-report handler: answer geometry from
 * mt2_fill_geometry_report, else echo hidd's last SET value per reportID. No IOKit. */
typedef enum { MT2_SYNTH_OK = 0, MT2_SYNTH_SKIP = 1 } mt2_synth_rc_t;

typedef struct { unsigned char reg[256]; } mt2_synth_regs_t;

void mt2_synth_regs_init(mt2_synth_regs_t *r);              /* zero the echo table */
void mt2_synth_note_set(mt2_synth_regs_t *r, unsigned char reportId, unsigned char value);
/* Fill `out` with the report answer for `reportId`, length in *outLen. Returns MT2_SYNTH_SKIP
 * for reports the driver must skip (0xDB), MT2_SYNTH_OK otherwise. */
mt2_synth_rc_t mt2_synth_answer_report(const mt2_synth_regs_t *r, unsigned char reportId,
                                       unsigned char *out, unsigned int *outLen);
#endif
