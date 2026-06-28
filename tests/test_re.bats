#!/usr/bin/env bats
# Tests for tools/re — the reverse-engineering toolkit.
#
# Two levels:
#   - unit:        source `re` as a library (RE_LIB=1 suppresses dispatch) and call helpers directly.
#   - integration: run `re <subcommand>` as a subprocess against stable 10.9 system binaries / fixtures.
#
# Run:  bats tests/test_re.bats     (or `make test`, which includes it)

RE="${BATS_TEST_DIRNAME}/../tools/re"
FIX="${BATS_TEST_DIRNAME}/fixtures"

# Source the script as a library for unit tests of its shell helpers.
load_re() { RE_LIB=1 source "$RE"; }

# --- sourcing guard: helpers usable without running the dispatch ---------------

@test "re can be sourced as a library without running the dispatch" {
  run bash -c "RE_LIB=1 source '$RE'; echo SOURCED_OK"
  [ "$status" -eq 0 ]
  [ "${lines[0]}" = "SOURCED_OK" ]
}

# --- calls: must find sites that otool renders by SYMBOL NAME, not just by addr ---
# Regression for the "false no-callers on dylibs" bug: intra-binary calls in a
# dylib are disassembled as `callq _symbolname`, never as the numeric target, so
# matching only the address found zero callers. _MTDeviceCreateListForDriverType
# lives at 0xd25 in MultitouchSupport and has 5 real call sites.

@test "calls resolves an address whose call sites are rendered by symbol name" {
  run "$RE" calls MultitouchSupport 0xd25
  [ "$status" -eq 0 ]
  # 5 caller lines, each naming the target symbol
  n=$(printf '%s\n' "$output" | grep -c 'MTDeviceCreateListForDriverType')
  [ "$n" -ge 5 ]
}

# --- re_hexnum: shared hex/decimal -> decimal normalizer ----------------------

@test "re_hexnum parses hex, decimal, and empty" {
  load_re
  [ "$(re_hexnum 0x10)" = "16" ]
  [ "$(re_hexnum 32)" = "32" ]
  [ "$(re_hexnum)" = "0" ]
  [ "$(re_hexnum 0X1F)" = "31" ]
}

@test "calls accepts a symbol name directly, tolerant of leading underscores" {
  # No address to resolve from (the stripped-symbol case); name must still match
  # even though otool spells it with extra leading underscores.
  run "$RE" calls MultitouchSupport _MTDeviceCreateListForDriverType
  [ "$status" -eq 0 ]
  n=$(printf '%s\n' "$output" | grep -c 'MTDeviceCreateListForDriverType')
  [ "$n" -ge 5 ]
  # header should not show an empty "(0x)" when no address was given
  printf '%s\n' "$output" | grep -q '(0x)' && return 1 || true
}

# --- resolvers (unit) ---------------------------------------------------------

@test "re_resolve maps an alias and passes a raw path through" {
  load_re
  [ "$(re_resolve MultitouchSupport)" = "/System/Library/PrivateFrameworks/MultitouchSupport.framework/Versions/A/MultitouchSupport" ]
  [ "$(re_resolve /some/raw/path)" = "/some/raw/path" ]
}

@test "re_sym_at_addr resolves a known symbol, accepts 0x and zero-padded forms" {
  load_re
  [ "$(re_sym_at_addr "$(re_resolve MultitouchSupport)" 0xd25)" = "___MTDeviceCreateListForDriverType" ]
  [ "$(re_sym_at_addr "$(re_resolve MultitouchSupport)" 0000000000000d25)" = "___MTDeviceCreateListForDriverType" ]
}

# --- disasm / syms characterization (the otool/nm pipeline, post-refactor) ----

@test "disasm by-name finds a C function and shows its first instruction" {
  run "$RE" disasm MultitouchSupport "_MTCompactV4HeaderUnpack"
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q '# function: _MTCompactV4HeaderUnpack'
  printf '%s\n' "$output" | grep -q 'pushq %rbp'
}

@test "syms filters by name" {
  run "$RE" syms MultitouchSupport MTDeviceCreateList
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'MTDeviceCreateListForDriverType'
}

# --- hex characterization (re_hexnum normalization, post-refactor) ------------

@test "hex honors hex-or-decimal offset/length and reports the window" {
  printf 'ABCDEFGHIJKLMNOP' > "$BATS_TEST_TMPDIR/blob"
  run "$RE" hex "$BATS_TEST_TMPDIR/blob" 0x4 8
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q '# window: offset 4 length 8'
  printf '%s\n' "$output" | grep -q 'EFGHIJKL'   # the 8-byte window starting at offset 4
}

# --- amd-actuation verdict (fixture-driven) -----------------------------------

@test "amd-actuation reports STEADY when all three classes present" {
  cat > "$BATS_TEST_TMPDIR/steady" <<'EOF'
  +-o foo  <class AppleMultitouchDevice, ...>
  +-o bar  <class AppleMultitouchHIDEventDriver, ...>
  +-o baz  <class IOHIDPointing, ...>
EOF
  run "$RE" amd-actuation "$BATS_TEST_TMPDIR/steady"
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'VERDICT: STEADY'
}

@test "amd-actuation reports BROKEN when actuation classes missing" {
  cat > "$BATS_TEST_TMPDIR/broken" <<'EOF'
  +-o foo  <class AppleMultitouchDevice, ...>
EOF
  run "$RE" amd-actuation "$BATS_TEST_TMPDIR/broken"
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'VERDICT: BROKEN'
}

# --- dispatch -----------------------------------------------------------------

@test "unknown subcommand exits 2 with a hint" {
  run "$RE" bogus-subcommand
  [ "$status" -eq 2 ]
  printf '%s\n' "$output" | grep -q "unknown subcommand"
}

@test "help exits 0 and lists subcommands" {
  run "$RE" help
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'disasm'
}
