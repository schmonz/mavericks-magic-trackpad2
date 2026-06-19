# RE scripts

Reusable wrappers for the reverse-engineering we do against the 10.9 multitouch stack.

**Convention** (so these stay allowlist-friendly and readable):
- One job per script, **Unix-filter style**: take args (or stdin), write plain text to stdout,
  compose with pipes. No side effects beyond reading the system / building a local tool.
- **Call things by their symbolic name, not raw addresses/flags.** Name a function; the wrapper
  finds where it lives. The cryptic `otool`/`nm` address math stays hidden.
- **Output is for humans AND pipes:** a `#`-prefixed header naming what you asked for, then regular
  whitespace columns. Skip metadata with `grep -v '^#'`; split the body with `awk`. C++ names are
  demangled; `nm` type letters become words (text/data/undefined/…).
- All **read-only** against system binaries and the live IORegistry (safe to run anytime).
- Self-documenting: run with no args for a usage line.
- Kept in-tree under `re/` for now.

Working principle (Kent Beck): **make the change easy (build/extend the tool that does what you
want), then make the easy change (call it).** When RE feels cryptic, that's the signal to improve a
wrapper, not to hand-run `otool`. Use one of these, adjust one, or add one — never raw
`otool`/`nm`/`ioreg` ad hoc. New scripts go here with the same shape.

## Scripts

| script | what | example |
|---|---|---|
| `re/syms` | symbols by readable name → `address  kind  name` | `re/syms <bin> "Tap\|Chord"` |
| `re/disasm` | a function's code BY NAME → `offset  address  instruction` | `re/disasm <bin> "tapHasValidTimingAndStrength"` |
| `re/consts` | dump a Mach-O section (resolve constants/thresholds) | `re/consts <bin> __TEXT __const` |
| `re/xref-offset` | BINARY-WIDE: every instruction touching a struct field offset, with enclosing fn, marked read/write | `re/xref-offset MultitouchHID 0x18 W` |
| `re/ioreg-props` | dump a live IORegistry class's properties | `re/ioreg-props AppleMultitouchDevice` |
| `re/mt-devices` | MultitouchSupport-enumerated devices + parser/transport/family | `re/mt-devices` |
| `re/mt-contacts` | live per-frame contacts the recognizer receives (state/size/density) | `re/mt-contacts` |

`re/disasm` also takes a raw `<start-hex> [end-hex]` range for code with no symbol. Arch defaults to
x86_64 (`RE_ARCH=i386` to override).

## Common targets

- Recognizer: `/System/Library/Extensions/AppleMultitouchDriver.kext/Contents/PlugIns/MultitouchHID.plugin/Contents/MacOS/MultitouchHID`
- Contact decode: `/System/Library/PrivateFrameworks/MultitouchSupport.framework/Versions/A/MultitouchSupport`
- BT device construction: `/System/Library/Extensions/AppleBluetoothMultitouch.kext/Contents/MacOS/AppleBluetoothMultitouch`
</content>
