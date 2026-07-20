#ifndef MAVERICKS_AMD_PROBE_H
#define MAVERICKS_AMD_PROBE_H
/* Debug oracle: build/teardown a fabricated AppleMultitouchDevice on demand and count live AMDs,
 * so a userspace check can assert the count returns to baseline (no orphan). Debug-gated. */
void mavericks_amd_probe_register(void);
void mavericks_amd_probe_unregister(void);
#endif
