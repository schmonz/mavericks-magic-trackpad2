#ifndef MT2_AMD_PROBE_H
#define MT2_AMD_PROBE_H
/* Debug oracle: build/teardown a fabricated AppleMultitouchDevice on demand and count live AMDs,
 * so a userspace check can assert the count returns to baseline (no orphan). Debug-gated. */
void mt2_amd_probe_register(void);
void mt2_amd_probe_unregister(void);
#endif
