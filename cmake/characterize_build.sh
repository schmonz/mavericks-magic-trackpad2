#!/bin/sh
# Cross-build equivalence gate.
#
# Characterize the TOOLCHAIN-INVARIANT, source/header-determined properties of a
# build's artifacts (the kext above all), so a modern GitHub-Actions cross-build
# can be proven identical -- in every load-bearing respect -- to the trusted
# native 10.9 build BEFORE a release publishes. Deliberately ignores codegen-
# variant details (addresses, sizes, bytes, UUID, local symbols) that legitimately
# differ between the native-10.9 clang and the modern cross clang.
#
# FAIL-CLOSED: if any GATED input cannot be measured (tool missing/errors, the
# kext binary absent, or a mandatory measurement comes back empty), emit AND
# compare abort non-zero rather than risk a silent empty-vs-empty "match".
#
# The KEXT is the gated artifact (undefined + defined external symbol sets, KPI
# deps, arch) -- it's the one that can't be re-verified without 10.9 hardware.
# Userland post-10.9 leaks are covered separately by cmake/compat_guard.sh.
#
# NOTE: POSIX sh has no `local`; every function uses uniquely-prefixed variable
# names so helpers never clobber a caller's state.
#
# Usage:
#   characterize_build.sh emit    <build-dir> <out-dir>
#   characterize_build.sh compare <reference-dir> <build-dir>
#
# Gated artifact under <build-dir>:
#   MT2Gesture.kext/Contents/MacOS/MT2Gesture   (+ Contents/Info.plist)
set -eu

die() { echo "CANNOT MEASURE (fail-closed): $*" >&2; exit 4; }

# Run a measurement to a file; fail if the tool errors or the result is empty.
measure_nonempty() {   # <out-file> <what> <cmd> [args...]
  mn_out=$1; mn_what=$2; shift 2
  "$@" > "$mn_out" 2>/dev/null || die "$mn_what ($* failed)"
  [ -s "$mn_out" ] || die "$mn_what produced no output"
}

emit_binary() {   # <mach-o> <out-prefix> <want-defined:0|1>
  eb_bin=$1; eb_pfx=$2; eb_wd=${3:-0}
  [ -f "$eb_bin" ] || die "missing binary $eb_bin"
  measure_nonempty "$eb_pfx.arch" "lipo -archs $eb_bin" lipo -archs "$eb_bin"
  # Undefined external symbols = the kxld/dyld inputs the loader must resolve.
  nm -u "$eb_bin" > "$eb_pfx.undefined.raw" 2>/dev/null || die "nm -u $eb_bin"
  sed 's/^[[:space:]]*//' "$eb_pfx.undefined.raw" | awk 'NF{print $NF}' | sort -u > "$eb_pfx.undefined"
  rm -f "$eb_pfx.undefined.raw"
  [ -s "$eb_pfx.undefined" ] || die "no undefined symbols measured for $eb_bin"
  # Defined external symbols = what we export (classes, vtables, C entry points).
  # Gated for the kext only (executables' exports aren't a meaningful contract).
  if [ "$eb_wd" = 1 ]; then
    nm -gU "$eb_bin" > "$eb_pfx.defined.raw" 2>/dev/null || die "nm -gU $eb_bin"
    awk 'NF{print $NF}' "$eb_pfx.defined.raw" | sort -u > "$eb_pfx.defined"
    rm -f "$eb_pfx.defined.raw"
    [ -s "$eb_pfx.defined" ] || die "no defined external symbols measured for $eb_bin"
  fi
  # Advisory only (NOT gated -- otool wording can differ across toolchains).
  {
    printf 'filetype=%s\n' "$(otool -hv "$eb_bin" 2>/dev/null | grep -oE '(EXECUTE|KEXT_BUNDLE|BUNDLE|DYLIB|OBJECT)' | head -1)"
    printf 'minos=%s\n' "$(otool -l "$eb_bin" 2>/dev/null | awk '/LC_VERSION_MIN_MACOSX/{f=1} f&&/version/{print $2; exit}')"
  } > "$eb_pfx.info" 2>/dev/null || true
}

emit() {   # <build-dir> <out-dir>
  em_bd=$1; em_out=$2
  mkdir -p "$em_out/kext"
  em_kbin="$em_bd/MT2Gesture.kext/Contents/MacOS/MT2Gesture"
  em_kplist="$em_bd/MT2Gesture.kext/Contents/Info.plist"
  emit_binary "$em_kbin" "$em_out/kext/macho" 1
  # Kext KPI dependencies + bundle id = the load-time contract (GATED, mandatory).
  [ -f "$em_kplist" ] || die "missing kext Info.plist $em_kplist"
  em_bid=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$em_kplist" 2>/dev/null) \
    || die "cannot read CFBundleIdentifier from $em_kplist"
  [ -n "$em_bid" ] || die "empty CFBundleIdentifier in $em_kplist"
  em_kpi=$(/usr/libexec/PlistBuddy -c 'Print :OSBundleLibraries' "$em_kplist" 2>/dev/null) \
    || die "cannot read OSBundleLibraries from $em_kplist"
  {
    printf 'CFBundleIdentifier=%s\n' "$em_bid"
    printf '%s\n' "$em_kpi" | grep '=' | sed 's/^[[:space:]]*//' | sort
  } > "$em_out/kext/kpideps"
  [ -s "$em_out/kext/kpideps" ] || die "no KPI dependencies measured from $em_kplist"
  # Kext-only by design: it is THE artifact that can't be re-verified without 10.9
  # hardware, so it's where cross-toolchain divergence must be gated. Userland
  # post-10.9 leaks are covered separately by cmake/compat_guard.sh. Keeping this
  # kext-only also lets the "always run after a kext build" POST_BUILD hook fire on
  # a kext-only build without churning the reference on whether tools were built.
}

# Every gated file that MUST exist + be non-empty in a valid characterization.
assert_gated_present() {   # <dir>
  agp_d=$1
  for agp_f in "$agp_d"/kext/macho.arch "$agp_d"/kext/macho.undefined \
               "$agp_d"/kext/macho.defined "$agp_d"/kext/kpideps; do
    [ -s "$agp_f" ] || die "reference/candidate missing or empty gated file: $agp_f"
  done
}

case "${1:-}" in
  emit)
    [ $# -eq 3 ] || { echo "usage: $0 emit <build-dir> <out-dir>" >&2; exit 64; }
    emit "$2" "$3"; assert_gated_present "$3"; echo "characterization written to $3" >&2 ;;
  compare)
    [ $# -eq 3 ] || { echo "usage: $0 compare <reference-dir> <build-dir>" >&2; exit 64; }
    cmp_ref=$2; cmp_bd=$3
    [ -d "$cmp_ref" ] || die "missing reference characterization dir: $cmp_ref"
    assert_gated_present "$cmp_ref"      # a corrupt/partial committed reference fails closed
    cmp_tmp=$(mktemp -d "${TMPDIR:-/tmp}/mt2-char.XXXXXX")
    emit "$cmp_bd" "$cmp_tmp"            # dies if the candidate can't be fully measured
    assert_gated_present "$cmp_tmp"
    if diff -ru -x '*.info' "$cmp_ref" "$cmp_tmp" > "$cmp_tmp.diff" 2>&1; then
      echo "EQUIVALENT: $cmp_bd matches native reference $cmp_ref"
    else
      echo "DIVERGENCE: $cmp_bd differs from native reference $cmp_ref" >&2
      cat "$cmp_tmp.diff" >&2
      exit 1
    fi ;;
  *) echo "usage: $0 emit|compare ..." >&2; exit 64 ;;
esac
