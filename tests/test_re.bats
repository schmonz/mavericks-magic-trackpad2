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

# Platform gates. The RE targets differ by host: the Mavericks/Intel box exposes
# system binaries on disk (x86_64, fixed addresses, otool/nm); this Apple-Silicon
# box keeps them in the dyld shared cache (arm64e, dyld_info). Tests that pin a
# specific binary/arch run only where that binary lives; shared-logic tests run
# everywhere. Result: `bats tests/test_re.bats` is green on both.
classic_only() { [ "$(uname -m)" = x86_64 ] || skip "x86_64 / on-disk target only"; }
modern_only()  { [ "$(uname -m)" = arm64 ]  || skip "arm64 / dyld-cache target only"; }

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
  classic_only
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
  classic_only
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
  classic_only
  load_re
  [ "$(re_sym_at_addr "$(re_resolve MultitouchSupport)" 0xd25)" = "___MTDeviceCreateListForDriverType" ]
  [ "$(re_sym_at_addr "$(re_resolve MultitouchSupport)" 0000000000000d25)" = "___MTDeviceCreateListForDriverType" ]
}

# --- disasm / syms characterization (the otool/nm pipeline, post-refactor) ----

@test "disasm by-name finds a C function and shows its first instruction" {
  classic_only
  run "$RE" disasm MultitouchSupport "_MTCompactV4HeaderUnpack"
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q '# function: _MTCompactV4HeaderUnpack'
  printf '%s\n' "$output" | grep -q 'pushq %rbp'
}

@test "syms filters by name" {
  classic_only
  run "$RE" syms MultitouchSupport MTDeviceCreateList
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'MTDeviceCreateListForDriverType'
}

# --- objc-methods: ObjC selector -> IMP from runtime metadata (not the symtab) ---
# The Trackpad pane's ObjC methods are NOT in nm's symtab; otool -oV exposes them.

@test "objc-methods resolves a selector to its class + IMP" {
  classic_only
  run "$RE" objc-methods Trackpad awakeFromNib
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -qE '0x[0-9a-f]+\s+[-+]\[BaseTrackPadController awakeFromNib\]'
}

@test "objc-methods --at finds the method containing an address" {
  classic_only
  # loadMainView's IMP is 0x225d; 0x2300 is inside it (the detection routine).
  run "$RE" objc-methods Trackpad --at 0x2300
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'loadMainView'
}

# Regression: a NON-STRIPPED system framework (IOBluetoothUI) has a symbol table, so
# `otool -oV` SYMBOLICATES the imp — it prints `-[Class sel]` in place of the address.
# Before the fix, the address column held that name fragment, not a 0x address; now the
# address comes from `otool -o` (raw), paired by imp-line order, with the correct +/- kind
# from the class/metaclass method list.
@test "objc-methods yields a real address for a symbolicated framework binary" {
  classic_only
  run "$RE" objc-methods IOBluetoothUI initImageDictionaries
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -qE '^0x[0-9a-f]+[[:space:]]+\+\[IOBluetoothDeviceImageVault initImageDictionaries\]'
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

# --- platform backend (portable) ----------------------------------------------

@test "re_cache_only distinguishes a cache-only path from a real file" {
  load_re
  re_cache_only "/no/such/binary/anywhere"      # absent -> shared cache (true)
  run re_cache_only "$RE"                        # the script itself is on disk
  [ "$status" -ne 0 ]                            # on-disk -> false
}

@test "arch default tracks the host machine" {
  load_re
  case "$(uname -m)" in
    arm64)  [ "$arch" = arm64e ] ;;
    x86_64) [ "$arch" = x86_64 ] ;;
  esac
}

# --- modern backend: same operations via dyld_info on the shared cache ---------
# These mirror the classic-target tests above. dyld_info resolves a framework's
# install-name straight into the cache; the re subcommands normalize its output
# to the same shapes, so the assertions are the logical twins of the x86_64 ones.

@test "strings reads a cstring literal out of the dyld cache" {
  modern_only
  run "$RE" strings IOBluetooth 'ForceReadDeviceName'
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'ForceReadDeviceName'
}

@test "syms reads the export trie out of the dyld cache" {
  modern_only
  run "$RE" syms IOBluetooth IOBluetoothIsBluetoothSecured
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q '_IOBluetoothIsBluetoothSecured'
}

@test "disasm slices a function out of the cache and shows its arm64 prologue" {
  modern_only
  run "$RE" disasm IOBluetooth IOBluetoothIsBluetoothSecured
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q '# function: _IOBluetoothIsBluetoothSecured'
  printf '%s\n' "$output" | grep -q 'pacibsp'          # arm64 signed-prologue
}

@test "objc-methods recovers a selector from cache disasm labels" {
  modern_only
  # dyld_info -objc refuses on cache dylibs; the imp is recovered from the
  # disassembler's own +[Class sel]: labels instead.
  run "$RE" objc-methods IOBluetooth sharedDeviceManager
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -qE '^0x[0-9a-f]+[[:space:]]+\+\[BluetoothDeviceManager sharedDeviceManager\]'
}

@test "calls finds arm64 bl call sites by symbol name" {
  modern_only
  run "$RE" calls IOBluetooth _objc_release
  [ "$status" -eq 0 ]
  # enclosing-function lines, each showing the bl to the target
  n=$(printf '%s\n' "$output" | grep -c ' in ')
  [ "$n" -ge 1 ]
}

@test "xref resolves arm64 adrp+ldr references to an absolute address" {
  modern_only
  # GOT slot for _OBJC_CLASS_$_BluetoothDeviceManager; _IOBluetoothIsBluetoothSecured loads it.
  run "$RE" xref IOBluetooth 0x1e6658580
  [ "$status" -eq 0 ]
  printf '%s\n' "$output" | grep -q 'IOBluetoothIsBluetoothSecured'
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
