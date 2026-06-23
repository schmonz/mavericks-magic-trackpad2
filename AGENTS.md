# Agent guide

## Multitouch stack knowledge — read before working on the driver

`docs/mt-stack/` is the **canonical, living knowledge base** for driving the 10.9 multitouch stack
with the Magic Trackpad 2. Consult it before changing driver code; extend it when you learn something
durable.

- `docs/mt-stack/explanation.md` — the mental model: the load-bearing principle, class hierarchy,
  data flow, the two injected side-channels (geometry, device-button gate), connect lifecycle.
- `docs/mt-stack/reference.md` — fact tables (vtable slots, field offsets, report formats,
  properties), each pointing at its source + `re/` re-derivation command.
- `docs/mt-stack/how-to.md` — recipes: drive a new behavior, the dev-loop, runtime diagnostics,
  safety, verify-a-fact.
- `docs/mt-stack/decisions.md` — **big-ticket roads not taken. Check this before proposing an
  approach** so you don't re-tread a dead end (functioning vs. desirable distinction).
- `docs/mt-stack/open-questions.md` — things we've observed but don't yet understand, and unmeasured
  scenarios.

**`src/mt2_stack.h`** is the single build-consumed source of the load-bearing RE constants
(offsets/slots/properties) — the kext compiles with them, so the facts and the build can't drift.
Fix a fact there, not inline.

## Working norms (project-specific)

- Do RE only through the in-tree `re/` wrappers, never raw otool/nm/ioreg ad hoc.
- Before any kext load/reload: assess panic risk and get go/no-go; pad off before unload.
- Always confirm the loaded kext binary contains your edit (`strings | grep`) before trusting a test.
- Host-green is necessary but not sufficient — verify behavior in the real system.
- Transient working docs live in `docs/superpowers/` (gitignored, deleted on completion); durable
  knowledge graduates into `docs/mt-stack/`.
