#ifndef MAVERICKS_SINGLE_LOAD_H
#define MAVERICKS_SINGLE_LOAD_H

/* First-loader-wins guard for the injected prefpane payload. The osax route and the SIMBL route can
 * both load our payload into ONE System Preferences process; they are separate Mach-O images, so the
 * per-image static swizzle guards (e.g. `if (gOrigImage) return`) cannot dedupe across them. Call
 * this ONCE at the very top of the load path: it returns 1 for the FIRST caller in the process and 0
 * for every caller after, via a process-global environment marker that spans images. The losing copy
 * bails before swizzling, so exactly one payload activates regardless of SIMBL state (added, removed,
 * present-but-broken, both installed). Host-testable: call twice in one process -> 1 then 0. */

/* The process-global marker env var. Exposed here (not just in the .c) so tests and any teardown
 * can reference the name without duplicating the string literal. Keep this name STABLE across
 * versions/renames (see the .c for why). */
#define MAVERICKS_SINGLE_LOAD_ENV "MT2_PAYLOAD_ACTIVE"

int mavericks_claim_single_load(void);

#endif
