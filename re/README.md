# RE scripts

Reusable wrappers for the reverse-engineering we do against the 10.9 multitouch stack.

**Convention** (so these stay allowlist-friendly and readable):
- One job per script, **Unix-filter style**: take args (or stdin), write plain text to stdout,
  compose with pipes. No side effects beyond reading the system / building a local tool.
- All **read-only** against system binaries and the live IORegistry (safe to run anytime).
- Self-documenting: run with no args for a usage line.
- Kept in-tree under `re/` for now.

When doing RE work: **use one of these, adjust one, or add one** — don't run raw `otool`/`nm`/`ioreg`
ad hoc. New scripts go here with the same shape.

## Scripts

| script | what | example |
|---|---|---|
| `re/syms` | list (demangled) symbols of a Mach-O, filtered | `re/syms <bin> 'Tap\|Chord'` |
| `re/disasm` | print a function/range disassembly (x86_64), bounded | `re/disasm <bin> 227d0 22932` |
| `re/consts` | dump a Mach-O section (resolve constants/thresholds) | `re/consts <bin> __TEXT __const` |
| `re/ioreg-props` | dump a live IORegistry class's properties | `re/ioreg-props AppleMultitouchDevice` |
| `re/mt-devices` | MultitouchSupport-enumerated devices + parser/transport/family | `re/mt-devices` |
| `re/mt-contacts` | live per-frame contacts the recognizer receives (state/size/density) | `re/mt-contacts` |

## Common targets

- Recognizer: `/System/Library/Extensions/AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin/Contents/MacOS/MultitouchHID`
- Contact decode: `/System/Library/PrivateFrameworks/MultitouchSupport.framework/Versions/A/MultitouchSupport`
- BT device construction: `/System/Library/Extensions/AppleBluetoothMultitouch.kext/Contents/MacOS/AppleBluetoothMultitouch`
</content>
