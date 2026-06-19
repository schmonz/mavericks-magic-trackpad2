# Oracles

The instruments we use to decide whether a change worked. **Each oracle measures at a different
altitude and each one can lie in a specific way.** Know which altitude you're at, and validate a
proxy against ground truth *before* you trust it to bless a fix.

> The rule this file exists to enforce (learned 2026-06-19, the hard way): we committed a "fix"
> (non-thumb fingerID) because a *proxy* oracle (recognizer decisions) said it worked — it was
> **moot at the user-visible level**. A fix commit must cite the **ground-truth** oracle that proved
> it, not a proxy. (Make it cheap to be wrong: build/validate the oracle before betting on a change.)

## The altitude ladder (lowest → highest fidelity to "what the user experiences")

| Oracle | Measures | How it lies |
|---|---|---|
| `make test` (`tests/test_*`) | our algorithm + stage integration, kext-free | only as good as our RE'd model — **host-green has lied** (round-6 liftoff passed every test, real tap still broke). Passing ≠ real behavior. |
| `re/*` (`disasm`/`xref-offset`/`syms`/`consts`) | the binary's **code** (static truth) | static, not runtime. **`xref-offset` finds only literal-displacement accesses** — it MISSES computed-offset writes (we never found the `this+0x408` write site this way). |
| `iter_tap.sh` + `trace_tapclick.d` | recognizer **decisions** in `hidd` (selectTapChord, handleChordTaps, handleTapsForDrag, queueButtonClick, handleChordLiftoff, state `0xc`, `this+0x408`) | **proxy, not the click.** `queueButtonClick` fired with **0** CGEvents; fingerID flipped this oracle but changed **nothing** the user sees. Synth injection reproduces the recognizer signature faithfully, but its queued button events don't reach CGEvents on their own. |
| `tap_clicks.sh` + `click_monitor` | **CGEvent clicks** (user-visible), hands-free via streamed synth taps | **flush artifact**: the recognizer flushes a tap's queued click only on the *next* tap's frames, so the last tap drops and the clean-count caps (~7/16). Phantom *presence* is reliable; exact *rate* is noisy. (Fix: feed trailing pump-frames per tap.) |
| `trace_button.d` / `trace_btnstack.d` | the actual poster `MTAppendMouseButtonEvent` (2×/click = down+up) + caller stacks | ground truth of *which recognizer path* posts each button — this is how we found the phantom = `handleTapsForDrag` on top of `chk4dragCycling`. |
| **real finger + `click_monitor`** | the real thing, no synth artifacts | needs a human → can't iterate fast. Use to validate the synth oracle, then iterate hands-free. |
| `re/mt-devices`, `re/ioreg-props` | live device identity (transport/family/parser/builtIn) | reflects what we publish, not what the recognizer *does* with it. |

## How to use them well

- **Pick the altitude that matches your claim.** "The encoder is right" → host test. "The recognizer
  takes branch X" → dtrace. "The user gets one click" → CGEvent / real finger. Don't bless a
  user-facing claim with a proxy.
- **Validate a new/changed oracle against ground truth once**, then iterate on it cheaply. (We only
  trusted the synth CGEvent oracle after it reproduced a real tap's signature — and we found its
  flush artifact by *comparing to the recognizer trace*.)
- **One variable per run.** Parameterize experiments via env/args (`SYNTH_TAPS`, `SYNTH_GAP_MS`, the
  `tap_clicks.sh` duration args, `TAPGAP`) — never hand-edit source per trial; that's where
  attribution dies (changed builtIn+prefs+hidd together once and lost the thread).
- New oracles live here in `tools/` (live) or `re/` (static, see `re/README.md`), Unix-filter style,
  and get a row in this table with their lie.

These oracles are the "see" half of the see / manipulate / test-the-effect architecture the codebase
is converging toward; pair an actuator (`synth_tap`, the `src/` pipeline) with an oracle here and an
assertion to get an effect-test.
